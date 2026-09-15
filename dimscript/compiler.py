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
    Break,
    Call,
    Continue,
    Delete,
    Expr,
    ExprStmt,
    For,
    FunctionDecl,
    GlobalDecl,
    If,
    Index,
    ListLiteral,
    Literal,
    Member,
    Name,
    New,
    Program,
    Return,
    Stmt,
    Unary,
    While,
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
MATH = DType("namespace:math")
ENGINE = DType("namespace:engine")
INPUT = DType("namespace:input")
LIST = DType("list")

#: Everything the VM offers on a list value; only `count` is a property.
LIST_METHODS = frozenset({"push", "insert", "delete", "remove_at", "clear", "index_of", "join",
                          "count"})

#: What the ahead-of-time backend knows about.  Anything outside it is a clear
#: error rather than silently missing code — `list` and indexing are the two
#: constructs the interpreter (src/ds_vm.c) has and compiled games do not.
NAMESPACE_TYPES = {"render": RENDER, "math": MATH, "engine": ENGINE, "input": INPUT}

#: namespace -> callable name -> (argument count, C call template)
BUILTIN_CALLS = {
    "render": {
        # -1 means "any count the VM accepts": render.color takes three or four
        # components, the fourth one being alpha.
        "color": (-1, "ds_render_color({0}, {1}, {2})"),
        "color_alpha": (4, "ds_render_color_alpha({0}, {1}, {2}, {3})"),
        "clear": (3, "ds_render_clear({0}, {1}, {2})"),
        "rect": (4, "ds_render_rect({0}, {1}, {2}, {3})"),
        "frame": (5, "ds_render_frame({0}, {1}, {2}, {3}, {4})"),
        "circle": (3, "ds_render_circle({0}, {1}, {2})"),
        "ring": (4, "ds_render_ring({0}, {1}, {2}, {3})"),
        "line": (5, "ds_render_line({0}, {1}, {2}, {3}, {4})"),
        "tri": (6, "ds_render_triangle({0}, {1}, {2}, {3}, {4}, {5})"),
        "text": (4, "ds_render_text({0}, {1}, {2}, {3})"),
    },
    "math": {
        "floor": (1, "ds_math_floor({0})"),
        "ceil": (1, "ds_math_ceil({0})"),
        "round": (1, "ds_math_round({0})"),
        "abs": (1, "ds_math_abs({0})"),
        "sign": (1, "ds_math_sign({0})"),
        "sqrt": (1, "ds_math_sqrt({0})"),
        "sin": (1, "ds_math_sin({0})"),
        "cos": (1, "ds_math_cos({0})"),
        "tan": (1, "ds_math_tan({0})"),
        "min": (2, "ds_math_min({0}, {1})"),
        "max": (2, "ds_math_max({0}, {1})"),
        "mod": (2, "ds_math_mod({0}, {1})"),
        "pow": (2, "ds_math_pow({0}, {1})"),
        "lerp": (3, "ds_math_lerp({0}, {1}, {2})"),
        "random": (-1, "ds_engine_random()"),
        "pi": (0, "ds_math_pi()"),
        "e": (0, "ds_math_e()"),
    },
    "engine": {
        "width": (0, "ds_engine_width()"),
        "height": (0, "ds_engine_height()"),
        "time": (0, "ds_engine_time()"),
        "delta": (0, "ds_engine_delta()"),
        "fps": (0, "ds_engine_fps()"),
        "frame": (0, "ds_engine_frame()"),
        "quit": (0, "ds_engine_quit()"),
    },
    "input": {
        "touches": (0, "ds_engine_touch_count()"),
        "touch_x": (1, "ds_engine_touch_x((int)({0}))"),
        "touch_y": (1, "ds_engine_touch_y((int)({0}))"),
        "touch_down": (1, "ds_engine_touch_down((int)({0}))"),
        "key": (1, "ds_engine_key_down({0})"),
    },
}

#: What a builtin *call* evaluates to.  Anything drawing has no result.
BUILTIN_RETURNS = {
    ("math", "floor"): FLOAT,
    ("math", "ceil"): FLOAT,
    ("math", "round"): FLOAT,
    ("math", "abs"): FLOAT,
    ("math", "sign"): INT,
    ("math", "sqrt"): FLOAT,
    ("math", "sin"): FLOAT,
    ("math", "cos"): FLOAT,
    ("math", "tan"): FLOAT,
    ("math", "min"): FLOAT,
    ("math", "max"): FLOAT,
    ("math", "mod"): FLOAT,
    ("math", "pow"): FLOAT,
    ("math", "lerp"): FLOAT,
    ("math", "random"): FLOAT,
    ("math", "pi"): FLOAT,
    ("math", "e"): FLOAT,
    ("engine", "width"): FLOAT,
    ("engine", "height"): FLOAT,
    ("engine", "time"): FLOAT,
    ("engine", "delta"): FLOAT,
    ("engine", "fps"): FLOAT,
    ("engine", "frame"): INT,
    ("engine", "quit"): VOID,
    ("input", "touches"): INT,
    ("input", "touch_x"): FLOAT,
    ("input", "touch_y"): FLOAT,
    ("input", "touch_down"): BOOL,
    ("input", "key"): BOOL,
}

#: Names that read like a field (`engine.width`) but are really getters.
BUILTIN_PROPERTIES = {
    "math": {
        "pi": "ds_math_pi()",
        "e": "ds_math_e()",
    },
    "engine": {
        "width": "ds_engine_width()",
        "height": "ds_engine_height()",
        "time": "ds_engine_time()",
        "delta": "ds_engine_delta()",
        "fps": "ds_engine_fps()",
        "frame": "ds_engine_frame()",
    },
    "input": {
        "touches": "ds_engine_touch_count()",
    },
}

_TYPE_NAMES = {
    "list": LIST,
    "int": INT,
    "float": FLOAT,
    "string": STRING,
    "str": STRING,
    "bool": BOOL,
    "boolean": BOOL,
    "void": VOID,
}


@dataclass
class SemanticModel:
    structs: Dict[str, Dict[str, DType]] = field(default_factory=dict)
    global_types: Dict[str, DType] = field(default_factory=dict)
    function_params: Dict[str, List[DType]] = field(default_factory=dict)
    function_returns: Dict[str, List[DType]] = field(default_factory=dict)
    function_locals: Dict[str, Dict[str, DType]] = field(default_factory=dict)
    expression_types: Dict[int, DType] = field(default_factory=dict)
    target_types: Dict[int, DType] = field(default_factory=dict)
    loop_variables: Dict[int, str] = field(default_factory=dict)
    list_types: Dict[int, DType] = field(default_factory=dict)
    list_expressions: set = field(default_factory=set)


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
    #: Engine callbacks and how many parameters they receive.  A script may
    #: implement any subset of them; the rest simply never run.
    CALLBACK_ARITY = {
        "load": 0,
        "resized": 2,
        "touchpressed": 3,
        "touchmoved": 3,
        "touchreleased": 3,
        "keypressed": 1,
        "keyreleased": 1,
        "update": 1,
        "draw": 0,
        "quit": 0,
    }

    def __init__(self, program: Program) -> None:
        self.program = program
        self.model = SemanticModel()
        self.functions = {function.name: function for function in program.functions}
        self._current_function: Optional[FunctionDecl] = None
        self._locals: Dict[str, DType] = {}
        self._loop_depth = 0

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
            callback_arity = self.CALLBACK_ARITY.get(function.name)
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
            if not dtype.is_struct and dtype != LIST:
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
        if isinstance(statement, While):
            condition = self._expression_type(statement.condition, self._locals)
            if condition not in (BOOL, INT, FLOAT, UNKNOWN):
                raise _error(statement.condition, "условие while должно быть bool или числом")
            self._analyze_loop_body(statement.body)
            return
        if isinstance(statement, For):
            start = self._expression_type(statement.start, self._locals)
            stop = self._expression_type(statement.stop, self._locals)
            if not _is_numeric(start) or not _is_numeric(stop):
                raise _error(statement, "границы for должны быть числами")
            if statement.step is not None:
                step = self._expression_type(statement.step, self._locals)
                if not _is_numeric(step):
                    raise _error(statement.step, "шаг for должен быть числом")
            self._locals[statement.variable] = INT
            self.model.loop_variables[id(statement)] = statement.variable
            self._analyze_loop_body(statement.body)
            return
        if isinstance(statement, (Break, Continue)):
            if not self._loop_depth:
                raise _error(statement, "break/continue возможны только внутри while или for")
            return
        raise _error(statement, "неизвестная конструкция")

    def _analyze_loop_body(self, body: List[Stmt]) -> None:
        saved = dict(self._locals)
        self._loop_depth += 1
        for child in body:
            self._analyze_statement(child)
        self._loop_depth -= 1
        # A loop variable stays visible as a C local, everything the body
        # introduced stays in the map because the C function declares it too.
        del saved

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
            if object_type in (RENDER, MATH, ENGINE, INPUT):
                namespace = object_type.name.split(":", 1)[1]
                properties = BUILTIN_PROPERTIES.get(namespace, {})
                if expression.name in properties:
                    dtype = FLOAT
                elif expression.name in BUILTIN_CALLS.get(namespace, {}):
                    dtype = DType(f"builtin:{expression.name}")
                else:
                    raise _error(expression, f"у {namespace} нет свойства {expression.name!r}")
            elif object_type == LIST:
                # Elements of a list are only reachable at runtime, so they get
                # the permissive type; `count` is the one real property.
                if expression.name == "count":
                    dtype = INT
                elif expression.name in LIST_METHODS:
                    dtype = DType(f"builtin:{expression.name}")
                else:
                    raise _error(expression, f"у списка нет свойства {expression.name!r}")
            elif object_type == UNKNOWN:
                dtype = UNKNOWN
            elif object_type == STRING:
                # The interpreter has a few string properties; compiled games
                # keep the language subset that maps to plain C.
                raise _error(expression, f"строка не имеет поля {expression.name!r}; length доступен только в интерпретаторе")
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
        elif isinstance(expression, Index):
            # `[i]` reads a number out of a list or a character out of a string;
            # both are interpreter features, so the checker only types them and
            # leaves the rejection to the code generator.
            object_type = self._expression_type(expression.object, locals_)
            self._expression_type(expression.index, locals_)
            # Unknown: the element type of a list is only known at runtime, and
            # the code generator refuses to reach this far anyway.
            dtype = STRING if object_type == STRING else UNKNOWN
        elif isinstance(expression, ListLiteral):
            for item in expression.items:
                self._expression_type(item, locals_)
            dtype = LIST
        elif isinstance(expression, Call):
            dtype = self._call_type(expression, locals_)
        else:
            raise _error(expression, "неизвестное выражение")

        self.model.expression_types[id(expression)] = dtype
        return dtype

    def _call_type(self, expression: Call, locals_: Dict[str, DType]) -> DType:
        callee = expression.callee
        if isinstance(callee, Member) and isinstance(callee.object, Name) and callee.object.name in NAMESPACE_TYPES:
            namespace = callee.object.name
            table = BUILTIN_CALLS.get(namespace, {})
            if callee.name not in table:
                raise _error(callee, f"нет функции {namespace}.{callee.name}")
            expected_count, _ = table[callee.name]
            if expected_count < 0:
                # Variadic: render.color(r, g, b) and render.color(r, g, b, a).
                if not 1 <= len(expression.args) <= 6:
                    raise _error(expression, f"{namespace}.{callee.name} ожидает от 1 до 6 аргументов")
            elif len(expression.args) != expected_count:
                plural = "" if expected_count == 1 else "а"
                raise _error(expression, f"{namespace}.{callee.name} ожидает {expected_count} аргумент{plural}")
            for index, argument in enumerate(expression.args):
                dtype = self._expression_type(argument, locals_)
                if namespace == "render" and callee.name == "text" and index == 0:
                    if dtype not in (STRING, INT, FLOAT, BOOL, UNKNOWN):
                        raise _error(argument, "первый аргумент render.text должен быть текстом или числом")
                elif not _is_numeric(dtype) and dtype not in (STRING, UNKNOWN):
                    raise _error(argument, f"аргумент {namespace}.{callee.name} должен быть числом")
            return BUILTIN_RETURNS.get((namespace, callee.name), VOID)
        if isinstance(callee, Name) and callee.name in self.functions:
            params = self.model.function_params[callee.name]
            if len(params) != len(expression.args):
                raise _error(expression, f"{callee.name} ожидает {len(params)} аргументов")
            for expected, argument in zip(params, expression.args):
                actual = self._expression_type(argument, locals_)
                if not _compatible(expected, actual):
                    raise _error(argument, f"ожидался {_display_type(expected)}, получено {_display_type(actual)}")
            return self.model.function_returns[callee.name]
        if isinstance(callee, Name) and callee.name in {"print", "log", "str", "len", "num"}:
            if callee.name == "str":
                expected = 1
            elif callee.name in {"len", "num"}:
                expected = 1
            else:
                expected = -1
            if expected > 0 and len(expression.args) != expected:
                raise _error(expression, f"{callee.name} ожидает {expected} аргумент")
            if expected < 0 and not expression.args:
                raise _error(expression, f"{callee.name} ожидает аргументы")
            for argument in expression.args:
                self._expression_type(argument, locals_)
            return STRING if callee.name == "str" else (FLOAT if callee.name == "num" else
                                                        (INT if callee.name == "len" else VOID))
        if isinstance(callee, Member) and isinstance(callee.object, (Name, Member, Index)):
            owner = self._expression_type(callee.object, locals_)
            if owner == LIST:
                # `list.push(x)` and friends exist in the VM; the expression is
                # legal, the C backend simply cannot emit it.
                for argument in expression.args:
                    self._expression_type(argument, locals_)
                self.model.list_expressions.add(id(expression))
                return VOID
        raise _error(callee, "вызвать можно функцию скрипта или builtin (render./math./engine./input.)")


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
    #: name -> (return type, C parameter types) of the `dimscript_*` wrapper
    #: that the host calls.  A wrapper is emitted for every one of them, so a
    #: game that implements only `update` still links against the engine.
    CALLBACKS = {
        "load": ("void", []),
        "resized": ("void", ["float", "float"]),
        "touchpressed": ("void", ["int", "float", "float"]),
        "touchmoved": ("void", ["int", "float", "float"]),
        "touchreleased": ("void", ["int", "float", "float"]),
        "keypressed": ("void", ["const char *"]),
        "keyreleased": ("void", ["const char *"]),
        "update": ("void", ["float"]),
        "draw": ("void", []),
        "quit": ("void", []),
    }

    WRAPPER_PARAMS = {
        "load": "void",
        "resized": "float width, float height",
        "touchpressed": "int id, float touch_x, float touch_y",
        "touchmoved": "int id, float touch_x, float touch_y",
        "touchreleased": "int id, float touch_x, float touch_y",
        "keypressed": "const char *name",
        "keyreleased": "const char *name",
        "update": "float dt",
        "draw": "void",
        "quit": "void",
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
        self.reject_unsupported()
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
            *[f"void dimscript_{name}({params});" for name, params in self.WRAPPER_PARAMS.items()],
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

    def reject_unsupported(self) -> None:
        """Lists are a runtime feature: an ahead-of-time build must say so.

        The check runs before any C is emitted, so a game never ends up with a
        half generated file.  Everything the VM has and the C ABI does not is
        listed here.
        """

        from .ast import Node  # local import: the module already imports its children

        def walk(node: object):
            yield node
            for child in getattr(node, "__dict__", {}).values() if hasattr(node, "__dict__") else []:
                if isinstance(child, Node):
                    yield from walk(child)
                elif isinstance(child, (list, tuple)):
                    for item in child:
                        if isinstance(item, Node):
                            yield from walk(item)

        roots: List[Node] = list(self.program.functions) + list(self.program.structs)
        roots += list(self.program.globals)
        for root in roots:
            for node in walk(root):
                if isinstance(node, Index):
                    raise SemanticError(
                        f"{self.filename}:{node.line}:{node.column}: список по индексу (list[i]) "
                        "работает только в интерпретаторе (src/ds_vm.c); для ahead-of-time сборки "
                        "храните данные в полях struct")
                if isinstance(node, ListLiteral):
                    raise SemanticError(
                        f"{self.filename}:{node.line}:{node.column}: список [..] работает только "
                        "в интерпретаторе (src/ds_vm.c); для ahead-of-time сборки используйте struct")

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
        # `for i = a, b do` declares its counter as a plain C local so that a
        # `break` or a later read of the loop variable behaves as in the VM.
        for statement in function.body:
            for loop in self._collect_loops(statement):
                if loop not in local_types and loop not in parameter_names:
                    self.emit(f"int32_t {_sanitize(loop)} = 0;")
        for statement in function.body:
            self.emit_statement(statement)
        if self.model.function_returns[function.name] == VOID:
            self.emit("return;")
        self.indent_level -= 1
        self.emit("}")
        self.current_function = None

    def emit_lifecycle_wrappers(self) -> None:
        for name in self.CALLBACKS:
            function = self.functions.get(name)
            declaration = self.wrapper_params(name)
            self.emit(f"void dimscript_{name}({declaration}) {{")
            self.indent_level += 1
            if function is None:
                # A game may implement any subset of the engine callbacks, so a
                # wrapper always exists; when there is nothing to call it simply
                # ignores the event.
                for parameter in _parameter_names(declaration):
                    self.emit(f"(void){parameter};")
            else:
                self.emit(f"if (!ds_program_initialized) return;")
                self.emit(f"ds_fn_{_sanitize(name)}({self.wrapper_args(name, function)});")
            self.indent_level -= 1
            self.emit("}")

    def wrapper_params(self, name: str) -> str:
        return self.WRAPPER_PARAMS.get(name, "void")

    def wrapper_args(self, name: str, function: FunctionDecl) -> str:
        # The canonical callbacks have the parameter count the wrapper passes;
        # a mismatch is reported by semantic analysis before this runs.
        return ", ".join(_sanitize(parameter.name) for parameter in function.params)

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
            self.emit(f"if ({self.truthy(statement.condition)}) {{")
            self.indent_level += 1
            for child in statement.then_body:
                self.emit_statement(child)
            self.indent_level -= 1
            if statement.else_body:
                # `else if` parses to a single nested If, which keeps the chain
                # flat in the C output as well.
                nested = statement.else_body[0] if len(statement.else_body) == 1 else None
                if isinstance(nested, If):
                    self.emit(f"}} else if ({self.truthy(nested.condition)}) {{")
                    self.indent_level += 1
                    for child in nested.then_body:
                        self.emit_statement(child)
                    self.indent_level -= 1
                    self._emit_else_chain(nested)
                else:
                    self.emit("} else {")
                    self.indent_level += 1
                    for child in statement.else_body:
                        self.emit_statement(child)
                    self.indent_level -= 1
            self.emit("}")
        elif isinstance(statement, While):
            self.emit(f"while ({self.truthy(statement.condition)}) {{")
            self.indent_level += 1
            self.emit_body(statement.body)
            self.indent_level -= 1
            self.emit("}")
        elif isinstance(statement, For):
            variable = _sanitize(statement.variable)
            start = self.expression(statement.start)
            stop = self.expression(statement.stop)
            step = self.expression(statement.step) if statement.step is not None else "1"
            self.emit(f"for ({variable} = {start}; {variable} <= {stop}; {variable} += ({step})) {{")
            self.indent_level += 1
            self.emit_body(statement.body)
            self.indent_level -= 1
            self.emit("}")
        elif isinstance(statement, Break):
            self.emit("break;")
        elif isinstance(statement, Continue):
            self.emit("continue;")
        elif isinstance(statement, Return):
            if statement.expression is None:
                self.emit("return;")
            else:
                self.emit(f"return {self.expression(statement.expression)};")
        else:
            raise TypeError(f"unsupported statement {statement!r}")

    def _emit_else_chain(self, statement: If) -> None:
        if not statement.else_body:
            return
        nested = statement.else_body[0] if len(statement.else_body) == 1 else None
        if isinstance(nested, If):
            self.emit(f"}} else if ({self.truthy(nested.condition)}) {{")
            self.indent_level += 1
            for child in nested.then_body:
                self.emit_statement(child)
            self.indent_level -= 1
            self._emit_else_chain(nested)
        else:
            self.emit("} else {")
            self.indent_level += 1
            for child in statement.else_body:
                self.emit_statement(child)
            self.indent_level -= 1

    def _collect_loops(self, statement: Stmt) -> List[str]:
        """Loop counter names declared by this function (top level only)."""

        if isinstance(statement, For):
            return [statement.variable]
        return []

    def emit_body(self, body: List[Stmt]) -> None:
        for statement in body:
            self.emit_statement(statement)

    def truthy(self, expression: Expr) -> str:
        """DimScript truthiness: a number counts when it is not zero."""

        dtype = self.expression_type(expression)
        value = self.expression(expression)
        if dtype == BOOL:
            # `if ((a == b))` is what the expression emitter produces; Clang
            # reads the doubled parentheses as a mistake, so peel one layer.
            stripped = _strip_outer_parentheses(value)
            return stripped if stripped is not None else value
        return f"({value}) != 0"

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
        object_type = self.expression_type(expression.object)
        if object_type.name.startswith("namespace:"):
            namespace = object_type.name.split(":", 1)[1]
            getter = BUILTIN_PROPERTIES.get(namespace, {}).get(expression.name)
            if getter is not None:
                return getter
            raise SemanticError(f"{self.filename}: {expression.line}:{expression.column}: "
                                f"{namespace}.{expression.name} нельзя читать как поле, вызовите функцию")
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
            callee = expression.callee
            if isinstance(callee, Member) and isinstance(callee.object, Name) and callee.object.name in NAMESPACE_TYPES:
                namespace = callee.object.name
                entry = BUILTIN_CALLS.get(namespace, {}).get(callee.name)
                if entry is None:
                    raise SemanticError(f"{self.filename}:{expression.line}: нет функции {namespace}.{callee.name}")
                template = entry[1]
                if namespace == "render" and callee.name == "color" and len(expression.args) >= 4:
                    template = "ds_render_color_alpha({0}, {1}, {2}, {3})"
                # Text and key names cross the C boundary as `const char *`,
                # every other builtin argument is a double.
                rendered = []
                for index, argument in enumerate(expression.args):
                    if namespace == "render" and callee.name == "text" and index == 0:
                        rendered.append(self.as_string(argument))
                    elif namespace == "input" and callee.name == "key":
                        rendered.append(self.as_string(argument))
                    else:
                        rendered.append(f"(double)({self.expression(argument)})")
                return template.format(*rendered)
            if isinstance(expression.callee, Name) and expression.callee.name in {"print", "log"}:
                parts = ", ".join(self.as_string(argument) for argument in expression.args)
                if len(expression.args) == 1:
                    return f"ds_log({parts})"
                return f"ds_log(ds_text_join({len(expression.args)}, {parts}))"
            if isinstance(expression.callee, Name) and expression.callee.name == "str":
                return self.as_string(expression.args[0])
            if isinstance(expression.callee, Name) and expression.callee.name == "num":
                return f"ds_number_of_text({self.as_string(expression.args[0])})"
            if isinstance(expression.callee, Name) and expression.callee.name == "len":
                return f"ds_length_of({self.as_string(expression.args[0])})"
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


def _strip_outer_parentheses(text: str) -> Optional[str]:
    """`(a == b)` -> `a == b`, or None when the parentheses are not a pair."""

    if not text.startswith("(") or not text.endswith(")"):
        return None
    depth = 0
    for index, character in enumerate(text):
        if character == "(":
            depth += 1
        elif character == ")":
            depth -= 1
            if depth == 0:
                return text[1:-1].strip() if index == len(text) - 1 else None
    return None


def _parameter_names(declaration: str) -> List[str]:
    if declaration.strip() == "void":
        return []
    names = []
    for part in declaration.split(","):
        match = re.search(r"([A-Za-z_][A-Za-z0-9_]*)\s*$", part.strip())
        if match:
            names.append(match.group(1))
    return names


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
