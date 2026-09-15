"""Semantic checks and native C99 code generation for DimScript.

The compiler intentionally performs no interpretation while emitting a game:
DimScript values become ordinary C values, structs become C structs and the
callbacks become C functions.  The only support code is the tiny
``dimscript_runtime`` ABI used for allocation, string concatenation and the
font-less render API.
"""

from __future__ import annotations

import json
import re
from dataclasses import dataclass, field
from typing import Dict, Iterable, List, Optional, Sequence, Tuple

from .ast import (
    Assign,
    Binary,
    Call,
    Delete,
    Expr,
    ExprStmt,
    FunctionDecl,
    GlobalDecl,
    If,
    Literal,
    Member,
    Name,
    New,
    Program,
    Return,
    Stmt,
    Unary,
)
from .lexer import DimScriptError


class SemanticError(DimScriptError):
    pass


@dataclass(frozen=True)
class DType:
    """A small nominal type used by the checker and code generator."""

    name: str

    @property
    def is_struct(self) -> bool:
        return self.name.startswith("struct:")

    @property
    def struct_name(self) -> str:
        return self.name.split(":", 1)[1] if self.is_struct else ""


INT = DType("int")
FLOAT = DType("float")
STRING = DType("string")
BOOL = DType("bool")
VOID = DType("void")
NIL = DType("nil")
UNKNOWN = DType("unknown")
RENDER = DType("namespace:render")


@dataclass
class SemanticModel:
    structs: Dict[str, Dict[str, DType]] = field(default_factory=dict)
    global_types: Dict[str, DType] = field(default_factory=dict)
    function_params: Dict[str, List[DType]] = field(default_factory=dict)
    function_returns: Dict[str, DType] = field(default_factory=dict)
    function_locals: Dict[str, Dict[str, DType]] = field(default_factory=dict)
    expression_types: Dict[int, DType] = field(default_factory=dict)
    target_types: Dict[int, DType] = field(default_factory=dict)


_TYPE_NAMES = {
    "int": INT,
    "float": FLOAT,
    "string": STRING,
    "str": STRING,
    "bool": BOOL,
    "boolean": BOOL,
    "void": VOID,
}


def _type_from_name(name: Optional[str], structs: Dict[str, Dict[str, DType]]) -> DType:
    if not name:
        return UNKNOWN
    if name in _TYPE_NAMES:
        return _TYPE_NAMES[name]
    if name in structs:
        return DType(f"struct:{name}")
    return DType(f"unresolved:{name}")


def _display_type(dtype: DType) -> str:
    if dtype.is_struct:
        return dtype.struct_name
    if dtype.name.startswith("unresolved:"):
        return dtype.name.split(":", 1)[1]
    return dtype.name


def _is_numeric(dtype: DType) -> bool:
    return dtype in (INT, FLOAT, UNKNOWN)


def _compatible(expected: DType, actual: DType) -> bool:
    if expected == UNKNOWN or actual == UNKNOWN or expected == actual:
        return True
    if expected == FLOAT and actual == INT:
        return True
    if expected.is_struct and actual == NIL:
        return True
    if expected == STRING and actual == NIL:
        return True
    return False


def _error(node: object, message: str) -> SemanticError:
    line = getattr(node, "line", 0)
    column = getattr(node, "column", 0)
    return SemanticError(f"{line}:{column}: {message}")


def _default_param_type(function_name: str, parameter_name: str) -> DType:
    """Give untyped event parameters a predictable C ABI type.

    The syntax permits concise callback declarations such as
    ``touchpressed(id, touch_x, touch_y)``.  Event ids are integers and
    coordinates/time values are floats; other untyped parameters default to
    float so they remain useful in animation callbacks.
    """

    lower = parameter_name.lower()
    if lower == "id" or lower.endswith("_id"):
        return INT
    if lower in {"x", "y", "dt", "time", "delta"} or lower.endswith("_x") or lower.endswith("_y"):
        return FLOAT
    if function_name.lower() == "touchpressed" and lower in {"touch_x", "touch_y"}:
        return FLOAT
    return FLOAT


class Analyzer:
    def __init__(self, program: Program) -> None:
        self.program = program
        self.model = SemanticModel()
        self.functions = {function.name: function for function in program.functions}
        self._current_function: Optional[FunctionDecl] = None
        self._locals: Dict[str, DType] = {}

    def analyze(self) -> SemanticModel:
        self._collect_structs()
        self._collect_function_signatures()
        self._collect_globals()
        for function in self.program.functions:
            self._analyze_function(function)
        return self.model

    def _collect_structs(self) -> None:
        for struct in self.program.structs:
            if struct.name in self.model.structs:
                raise _error(struct, f"структура {struct.name!r} объявлена дважды")
            fields: Dict[str, DType] = {}
            for field in struct.fields:
                if field.name in fields:
                    raise _error(field, f"поле {field.name!r} объявлено дважды")
                dtype = _type_from_name(field.type_name, self.model.structs)
                if dtype.name.startswith("unresolved:"):
                    raise _error(field, f"неизвестный тип {field.type_name!r}")
                fields[field.name] = dtype
            self.model.structs[struct.name] = fields

    def _collect_function_signatures(self) -> None:
        for function in self.program.functions:
            if function.name in self.model.function_params:
                raise _error(function, f"функция {function.name!r} объявлена дважды")
            params: List[DType] = []
            names = set()
            for parameter in function.params:
                if parameter.name in names:
                    raise _error(parameter, f"параметр {parameter.name!r} повторяется")
                names.add(parameter.name)
                dtype = _type_from_name(parameter.type_name, self.model.structs)
                if dtype == UNKNOWN:
                    dtype = _default_param_type(function.name, parameter.name)
                if dtype.name.startswith("unresolved:"):
                    raise _error(parameter, f"неизвестный тип {parameter.type_name!r}")
                params.append(dtype)
            return_type = _type_from_name(function.return_type, self.model.structs)
            if return_type == UNKNOWN:
                return_type = VOID
            if return_type.name.startswith("unresolved:"):
                raise _error(function, f"неизвестный тип результата {function.return_type!r}")
            callback_arity = {
                "load": 0,
                "touchpressed": 3,
                "update": 1,
                "draw": 0,
                "quit": 0,
            }.get(function.name)
            if callback_arity is not None and len(params) != callback_arity:
                raise _error(function, f"callback {function.name!r} должен иметь {callback_arity} параметр(а)")
            self.model.function_params[function.name] = params
            self.model.function_returns[function.name] = return_type

    def _collect_globals(self) -> None:
        for declaration in self.program.globals:
            if declaration.name in self.model.global_types:
                raise _error(declaration, f"глобальная переменная {declaration.name!r} объявлена дважды")
            dtype = self._expression_type(declaration.value, {})
            if dtype == VOID:
                raise _error(declaration, "нельзя сохранить значение void")
            self.model.global_types[declaration.name] = dtype

    def _analyze_function(self, function: FunctionDecl) -> None:
        self._current_function = function
        self._locals = {
            parameter.name: dtype
            for parameter, dtype in zip(function.params, self.model.function_params[function.name])
        }
        for statement in function.body:
            self._analyze_statement(statement)
        # Parameters are not locals for declaration purposes, but retaining
        # the map makes the result useful to tools inspecting the compiler.
        self.model.function_locals[function.name] = dict(self._locals)
        self._current_function = None
        self._locals = {}

    def _analyze_statement(self, statement: Stmt) -> None:
        if isinstance(statement, Assign):
            value_type = self._expression_type(statement.value, self._locals)
            target_type = self._target_type(statement.target, value_type)
            if not _compatible(target_type, value_type):
                raise _error(statement, f"нельзя присвоить {_display_type(value_type)} переменной типа {_display_type(target_type)}")
            self.model.target_types[id(statement.target)] = target_type
            return
        if isinstance(statement, ExprStmt):
            self._expression_type(statement.expression, self._locals)
            return
        if isinstance(statement, Delete):
            dtype = self._expression_type(statement.expression, self._locals)
            if not dtype.is_struct:
                raise _error(statement, "delete применим только к объекту struct")
            return
        if isinstance(statement, If):
            condition = self._expression_type(statement.condition, self._locals)
            if condition not in (BOOL, INT, FLOAT, UNKNOWN):
                raise _error(statement.condition, "условие if должно быть bool или числом")
            for child in statement.then_body:
                self._analyze_statement(child)
            for child in statement.else_body:
                self._analyze_statement(child)
            return
        if isinstance(statement, Return):
            actual = VOID if statement.expression is None else self._expression_type(statement.expression, self._locals)
            expected = self.model.function_returns[self._current_function.name]  # type: ignore[union-attr]
            if not _compatible(expected, actual):
                raise _error(statement, f"функция возвращает {_display_type(expected)}, а получено {_display_type(actual)}")
            return
        raise _error(statement, "неизвестная конструкция")

    def _target_type(self, target: Expr, value_type: DType) -> DType:
        if isinstance(target, Name):
            if target.name in self._locals:
                return self._locals[target.name]
            if target.name in self.model.global_types:
                return self.model.global_types[target.name]
            # Assignment introduces a local variable.  This keeps small game
            # scripts concise while still producing a statically typed C local.
            self._locals[target.name] = value_type
            return value_type
        if isinstance(target, Member):
            return self._expression_type(target, self._locals)
        raise _error(target, "слева от '=' должно быть имя или поле объекта")

    def _expression_type(self, expression: Expr, locals_: Dict[str, DType]) -> DType:
        cached = self.model.expression_types.get(id(expression))
        if cached is not None:
            return cached

        dtype = UNKNOWN
        if isinstance(expression, Literal):
            dtype = _TYPE_NAMES.get(expression.literal_type, NIL if expression.literal_type == "nil" else UNKNOWN)
        elif isinstance(expression, Name):
            if expression.name in locals_:
                dtype = locals_[expression.name]
            elif expression.name in self.model.global_types:
                dtype = self.model.global_types[expression.name]
            elif expression.name == "render":
                dtype = RENDER
            elif expression.name in self.functions:
                dtype = DType(f"function:{expression.name}")
            else:
                raise _error(expression, f"неизвестное имя {expression.name!r}")
        elif isinstance(expression, Member):
            object_type = self._expression_type(expression.object, locals_)
            if object_type == RENDER:
                dtype = DType(f"builtin:{expression.name}")
            elif object_type.is_struct:
                fields = self.model.structs.get(object_type.struct_name, {})
                if expression.name not in fields:
                    raise _error(expression, f"у {object_type.struct_name} нет поля {expression.name!r}")
                dtype = fields[expression.name]
            else:
                raise _error(expression, f"тип {_display_type(object_type)} не имеет полей")
        elif isinstance(expression, New):
            dtype = _type_from_name(expression.type_name, self.model.structs)
            if not dtype.is_struct:
                raise _error(expression, f"new ожидает имя struct, получено {expression.type_name!r}")
        elif isinstance(expression, Unary):
            operand = self._expression_type(expression.operand, locals_)
            if expression.operator in ("-", "+"):
                if not _is_numeric(operand):
                    raise _error(expression, "унарный +/- применим только к числу")
                dtype = operand
            elif expression.operator in ("!", "not"):
                dtype = BOOL
            else:
                raise _error(expression, f"неизвестный унарный оператор {expression.operator!r}")
        elif isinstance(expression, Binary):
            left = self._expression_type(expression.left, locals_)
            right = self._expression_type(expression.right, locals_)
            operator = expression.operator
            if operator == "..":
                dtype = STRING
            elif operator in ("+", "-", "*", "/", "%"):
                if not _is_numeric(left) or not _is_numeric(right):
                    raise _error(expression, f"оператор {operator} работает только с числами")
                dtype = FLOAT if FLOAT in (left, right) or operator == "/" else INT
            elif operator in ("==", "!=", "~=", ">", "<", ">=", "<="):
                dtype = BOOL
            elif operator in ("and", "or", "&&", "||"):
                dtype = BOOL
            else:
                raise _error(expression, f"неизвестный оператор {operator!r}")
        elif isinstance(expression, Call):
            dtype = self._call_type(expression, locals_)
        else:
            raise _error(expression, "неизвестное выражение")

        self.model.expression_types[id(expression)] = dtype
        return dtype

    def _call_type(self, expression: Call, locals_: Dict[str, DType]) -> DType:
        callee = expression.callee
        if isinstance(callee, Member) and isinstance(callee.object, Name) and callee.object.name == "render":
            if callee.name not in {"color", "text"}:
                raise _error(callee, f"неизвестная функция render.{callee.name}")
            expected_count = 3 if callee.name == "color" else 4
            if len(expression.args) != expected_count:
                raise _error(expression, f"render.{callee.name} ожидает {expected_count} аргумента")
            for argument in expression.args:
                self._expression_type(argument, locals_)
            if callee.name == "color":
                for argument in expression.args:
                    if not _is_numeric(self._expression_type(argument, locals_)):
                        raise _error(argument, "компоненты цвета должны быть числами")
            else:
                text_type = self._expression_type(expression.args[0], locals_)
                if text_type not in (STRING, INT, FLOAT, BOOL, UNKNOWN):
                    raise _error(expression.args[0], "первый аргумент render.text должен быть текстом или числом")
                for argument in expression.args[1:]:
                    if not _is_numeric(self._expression_type(argument, locals_)):
                        raise _error(argument, "координаты и масштаб текста должны быть числами")
            return VOID
        if isinstance(callee, Name) and callee.name in self.functions:
            params = self.model.function_params[callee.name]
            if len(params) != len(expression.args):
                raise _error(expression, f"{callee.name} ожидает {len(params)} аргументов")
            for expected, argument in zip(params, expression.args):
                actual = self._expression_type(argument, locals_)
                if not _compatible(expected, actual):
                    raise _error(argument, f"ожидался {_display_type(expected)}, получено {_display_type(actual)}")
            return self.model.function_returns[callee.name]
        if isinstance(callee, Name) and callee.name in {"print", "log"}:
            if len(expression.args) != 1:
                raise _error(expression, f"{callee.name} ожидает один аргумент")
            self._expression_type(expression.args[0], locals_)
            return VOID
        raise _error(callee, "вызвать можно функцию или render.color/render.text")


# C code generator -------------------------------------------------------------


def _sanitize(name: str) -> str:
    value = re.sub(r"[^a-zA-Z0-9_]", "_", name)
    if not value or value[0].isdigit():
        value = "_" + value
    return value


def _c_float(value: float) -> str:
    text = repr(float(value))
    if text in {"nan", "inf", "-inf"}:
        raise ValueError("DimScript не поддерживает NaN/Infinity как литералы")
    if "." not in text and "e" not in text.lower():
        text += ".0"
    return text + "f"


class CCompiler:
    CALLBACKS = {
        "load": ("void", []),
        "touchpressed": ("void", ["int", "float", "float"]),
        "update": ("void", ["float"]),
        "draw": ("void", []),
        "quit": ("void", []),
    }

    def __init__(self, program: Program, filename: str = "<string>") -> None:
        self.program = program
        self.filename = filename
        self.model = Analyzer(program).analyze()
        self.functions = {function.name: function for function in program.functions}
        self.struct_names = {struct.name: _sanitize(struct.name) for struct in program.structs}
        self.global_c_names = {name: _sanitize(name) for name in self.model.global_types}
        self.lines: List[str] = []
        self.indent_level = 0
        self.current_function: Optional[FunctionDecl] = None

    def compile(self) -> str:
        self.emit("/* Generated by DimScript. Do not edit by hand. */")
        self.emit(f"/* Source: {self.filename} */")
        self.emit('#include "dimscript_runtime.h"')
        self.emit("#include <stdint.h>")
        self.emit("#include <stddef.h>")
        self.emit("#include <string.h>")
        self.emit("")
        self.emit_structs()
        self.emit_globals()
        self.emit("")
        self.emit("static int ds_program_initialized;")
        self.emit("")
        self.emit_global_init()
        self.emit("")
        for function in self.program.functions:
            self.emit_user_function(function)
            self.emit("")
        self.emit_lifecycle_wrappers()
        return "\n".join(self.lines).rstrip() + "\n"

    def header(self, guard: str = "DIMSCRIPT_GENERATED_H") -> str:
        guard = _sanitize(guard).upper()
        lines = [
            "/* Generated by DimScript. */",
            f"#ifndef {guard}",
            f"#define {guard}",
            "#ifdef __cplusplus",
            'extern "C" {',
            "#endif",
            "void dimscript_init(void);",
            "void dimscript_shutdown(void);",
            "void dimscript_load(void);",
            "void dimscript_touchpressed(int id, float touch_x, float touch_y);",
            "void dimscript_update(float dt);",
            "void dimscript_draw(void);",
            "void dimscript_quit(void);",
            "#ifdef __cplusplus",
            "}",
            "#endif",
            f"#endif /* {guard} */",
            "",
        ]
        return "\n".join(lines)

    def emit(self, text: str) -> None:
        if text:
            self.lines.append("    " * self.indent_level + text)
        else:
            self.lines.append("")

    def emit_structs(self) -> None:
        for struct in self.program.structs:
            self.emit(f"typedef struct {self.struct_names[struct.name]} {{")
            self.indent_level += 1
            for field in struct.fields:
                dtype = _type_from_name(field.type_name, self.model.structs)
                self.emit(f"{self.c_type(dtype)} {_sanitize(field.name)};")
            self.indent_level -= 1
            self.emit(f"}} {self.struct_names[struct.name]};")
            self.emit("")

    def emit_globals(self) -> None:
        for declaration in self.program.globals:
            dtype = self.model.global_types[declaration.name]
            self.emit(f"static {self.c_type(dtype)} {self.global_c_names[declaration.name]} = {self.default_value(dtype)};")

    def emit_global_init(self) -> None:
        self.emit("void dimscript_init(void) {")
        self.indent_level += 1
        self.emit("if (ds_program_initialized) return;")
        for declaration in self.program.globals:
            self.emit_assignment(self.global_c_names[declaration.name], declaration.value, global_scope=True)
        self.emit("ds_program_initialized = 1;")
        self.indent_level -= 1
        self.emit("}")
        self.emit("")
        self.emit("void dimscript_shutdown(void) {")
        self.indent_level += 1
        self.emit("if (!ds_program_initialized) return;")
        for declaration in self.program.globals:
            dtype = self.model.global_types[declaration.name]
            if dtype.is_struct:
                self.emit(f"ds_delete((void **)&{self.global_c_names[declaration.name]});")
        self.emit("ds_program_initialized = 0;")
        self.indent_level -= 1
        self.emit("}")

    def emit_user_function(self, function: FunctionDecl) -> None:
        self.current_function = function
        params = self.model.function_params[function.name]
        parameter_text = ", ".join(
            f"{self.c_type(dtype)} {_sanitize(parameter.name)}"
            for parameter, dtype in zip(function.params, params)
        ) or "void"
        self.emit(f"static {self.c_type(self.model.function_returns[function.name])} ds_fn_{_sanitize(function.name)}({parameter_text}) {{")
        self.indent_level += 1
        # Explicit casts to void make generated callback parameters warning-free
        # under -Wall -Wextra -Werror, even when the script intentionally does
        # not use all event fields.
        for parameter in function.params:
            self.emit(f"(void){_sanitize(parameter.name)};")
        local_types = self.model.function_locals.get(function.name, {})
        parameter_names = {parameter.name for parameter in function.params}
        for name, dtype in local_types.items():
            if name not in parameter_names:
                self.emit(f"{self.c_type(dtype)} {_sanitize(name)} = {self.default_value(dtype)};")
        for statement in function.body:
            self.emit_statement(statement)
        if self.model.function_returns[function.name] == VOID:
            self.emit("return;")
        self.indent_level -= 1
        self.emit("}")
        self.current_function = None

    def emit_lifecycle_wrappers(self) -> None:
        for name, (return_c, callback_types) in self.CALLBACKS.items():
            function = self.functions.get(name)
            self.emit(f"void dimscript_{name}(" + self.wrapper_params(name) + ") {")
            self.indent_level += 1
            if function is not None:
                args = self.wrapper_args(name, function)
                self.emit(f"ds_fn_{_sanitize(name)}({args});")
            self.indent_level -= 1
            self.emit("}")

    def wrapper_params(self, name: str) -> str:
        if name == "touchpressed":
            return "int id, float touch_x, float touch_y"
        if name == "update":
            return "float dt"
        return "void"

    def wrapper_args(self, name: str, function: FunctionDecl) -> str:
        if name == "touchpressed":
            # The canonical callback has these three parameters.  If a script
            # gives it another count, semantic analysis still describes the
            # error clearly before code generation reaches here.
            names = [_sanitize(parameter.name) for parameter in function.params]
            return ", ".join(names)
        if name == "update":
            return ", ".join(_sanitize(parameter.name) for parameter in function.params)
        return ""

    def c_type(self, dtype: DType) -> str:
        if dtype == INT:
            return "int32_t"
        if dtype == FLOAT:
            return "float"
        if dtype == STRING:
            return "const char *"
        if dtype == BOOL:
            return "int"
        if dtype == VOID:
            return "void"
        if dtype.is_struct:
            return f"{self.struct_names.get(dtype.struct_name, _sanitize(dtype.struct_name))} *"
        if dtype == NIL or dtype == UNKNOWN:
            return "const char *"
        # Analyzer rejects unresolved types, but this fallback makes an error
        # in a future extension fail at C compile time rather than Python time.
        return "void *"

    def default_value(self, dtype: DType) -> str:
        if dtype == FLOAT:
            return "0.0f"
        if dtype in (INT, BOOL):
            return "0"
        return "NULL"

    def emit_statement(self, statement: Stmt) -> None:
        if isinstance(statement, Assign):
            self.emit_assignment(self.target(statement.target), statement.value)
        elif isinstance(statement, ExprStmt):
            self.emit(f"{self.expression(statement.expression)};")
        elif isinstance(statement, Delete):
            if isinstance(statement.expression, Name):
                name = self.name(statement.expression)
                self.emit(f"ds_delete((void **)&{name});")
            else:
                self.emit(f"ds_delete((void **)&{self.expression(statement.expression)});")
        elif isinstance(statement, If):
            self.emit(f"if ({self.expression(statement.condition)}) {{")
            self.indent_level += 1
            for child in statement.then_body:
                self.emit_statement(child)
            self.indent_level -= 1
            if statement.else_body:
                self.emit("} else {")
                self.indent_level += 1
                for child in statement.else_body:
                    self.emit_statement(child)
                self.indent_level -= 1
            self.emit("}")
        elif isinstance(statement, Return):
            if statement.expression is None:
                self.emit("return;")
            else:
                self.emit(f"return {self.expression(statement.expression)};")
        else:
            raise TypeError(f"unsupported statement {statement!r}")

    def target(self, target: Expr) -> str:
        if isinstance(target, Name):
            return self.name(target)
        if isinstance(target, Member):
            return self.member(target)
        raise TypeError(f"unsupported assignment target {target!r}")

    def emit_assignment(self, target: str, value: Expr, global_scope: bool = False) -> None:
        self.emit(f"{target} = {self.expression(value)};")

    def name(self, expression: Name) -> str:
        if self.current_function is not None:
            local_types = self.model.function_locals.get(self.current_function.name, {})
            if expression.name in local_types:
                return _sanitize(expression.name)
        if expression.name in self.global_c_names:
            return self.global_c_names[expression.name]
        return _sanitize(expression.name)

    def member(self, expression: Member) -> str:
        return f"{self.expression(expression.object)}->{_sanitize(expression.name)}"

    def expression_type(self, expression: Expr) -> DType:
        return self.model.expression_types.get(id(expression), UNKNOWN)

    def expression(self, expression: Expr) -> str:
        if isinstance(expression, Literal):
            if expression.literal_type == "string":
                return json.dumps(str(expression.value), ensure_ascii=False)
            if expression.literal_type == "int":
                return str(int(expression.value))
            if expression.literal_type == "float":
                return _c_float(float(expression.value))
            if expression.literal_type == "bool":
                return "1" if expression.value else "0"
            if expression.literal_type == "nil":
                return "NULL"
        if isinstance(expression, Name):
            return self.name(expression)
        if isinstance(expression, Member):
            return self.member(expression)
        if isinstance(expression, New):
            dtype = _type_from_name(expression.type_name, self.model.structs)
            c_name = self.struct_names[expression.type_name]
            return f"({self.c_type(dtype)})ds_alloc(sizeof({c_name}))"
        if isinstance(expression, Unary):
            operator = "!" if expression.operator == "not" else expression.operator
            return f"({operator}{self.expression(expression.operand)})"
        if isinstance(expression, Binary):
            if expression.operator == "..":
                left = self.as_string(expression.left)
                right = self.as_string(expression.right)
                return f"ds_concat({left}, {right})"
            operator = {"and": "&&", "or": "||", "~=": "!="}.get(expression.operator, expression.operator)
            left_type = self.expression_type(expression.left)
            right_type = self.expression_type(expression.right)
            if expression.operator in {"==", "!=", "~=", ">", "<", ">=", "<="} and (
                left_type == STRING or right_type == STRING
            ):
                comparison = {"==": "==", "!=": "!=", "~=": "!=", ">": ">", "<": "<", ">=": ">=", "<=": "<="}[expression.operator]
                return f"(strcmp({self.as_string(expression.left)}, {self.as_string(expression.right)}) {comparison} 0)"
            return f"({self.expression(expression.left)} {operator} {self.expression(expression.right)})"
        if isinstance(expression, Call):
            if isinstance(expression.callee, Member) and isinstance(expression.callee.object, Name) and expression.callee.object.name == "render":
                if expression.callee.name == "color":
                    args = ", ".join(f"(float)({self.expression(argument)})" for argument in expression.args)
                    return f"ds_render_color({args})"
                if expression.callee.name == "text":
                    text = self.as_string(expression.args[0])
                    coordinates = ", ".join(f"(float)({self.expression(argument)})" for argument in expression.args[1:])
                    return f"ds_render_text({text}, {coordinates})"
            if isinstance(expression.callee, Name) and expression.callee.name in {"print", "log"}:
                return f"ds_log({self.as_string(expression.args[0])})"
            if isinstance(expression.callee, Name):
                return f"ds_fn_{_sanitize(expression.callee.name)}({', '.join(self.expression(argument) for argument in expression.args)})"
        raise TypeError(f"unsupported expression {expression!r}")

    def as_string(self, expression: Expr) -> str:
        dtype = self.expression_type(expression)
        value = self.expression(expression)
        if dtype == STRING or dtype == UNKNOWN:
            return value
        if dtype == INT:
            return f"ds_int_to_string((int64_t)({value}))"
        if dtype == FLOAT:
            return f"ds_float_to_string((double)({value}))"
        if dtype == BOOL:
            return f"ds_bool_to_string(({value}) != 0)"
        return value


def compile_source(source: str, filename: str = "<string>") -> Tuple[str, SemanticModel]:
    """Compile source text to C and return ``(c_source, semantic_model)``."""

    from .parser import parse

    program = parse(source, filename=filename)
    compiler = CCompiler(program, filename=filename)
    return compiler.compile(), compiler.model


def compile_program(program: Program, filename: str = "<string>") -> Tuple[str, SemanticModel]:
    compiler = CCompiler(program, filename=filename)
    return compiler.compile(), compiler.model


def compile_header(source: str, filename: str = "<string>") -> str:
    from .parser import parse

    program = parse(source, filename=filename)
    return CCompiler(program, filename=filename).header()
