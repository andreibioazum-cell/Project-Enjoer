"""Reference interpreter for DimScript.

The interpreter is intentionally a development tool: it validates a script and
makes it possible to exercise gameplay callbacks without a native build.  The
shipping path is the C compiler in :mod:`dimscript.compiler`; both consume the
same AST, so a script can be prototyped here and then compiled to native C.
"""

from __future__ import annotations

from dataclasses import dataclass, field
from typing import Any, Dict, List, Optional, Sequence

from .ast import (
    Assign,
    Binary,
    Call,
    Delete,
    Expr,
    ExprStmt,
    FunctionDecl,
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
from .compiler import Analyzer
from .lexer import DimScriptError
from .parser import parse


class RuntimeError(DimScriptError):
    pass


@dataclass
class StructValue:
    type_name: str
    fields: Dict[str, Any] = field(default_factory=dict)


@dataclass
class TextCommand:
    text: str
    x: float
    y: float
    scale: float
    color: tuple[float, float, float]


class RenderRecorder:
    """Font-less render API used by the interpreter.

    ``text`` is recorded rather than drawn.  That makes the lack of a font
    explicit while still allowing tests and tools to inspect what a script
    requested from the future Vulkan text backend.
    """

    def __init__(self) -> None:
        self.color_value = (1.0, 1.0, 1.0)
        self.text_commands: List[TextCommand] = []
        self.text_calls = 0

    def reset_frame(self) -> None:
        self.text_commands.clear()

    def color(self, red: Any, green: Any, blue: Any) -> None:
        self.color_value = (float(red), float(green), float(blue))

    def text(self, text: Any, x: Any, y: Any, scale: Any) -> None:
        self.text_calls += 1
        self.text_commands.append(
            TextCommand(str(text), float(x), float(y), float(scale), self.color_value)
        )


@dataclass
class _ReturnSignal:
    value: Any = None


class Interpreter:
    def __init__(self, program: Program, filename: str = "<string>") -> None:
        self.program = program
        self.filename = filename
        # Run the same static checks used by native compilation.  This catches
        # a typo before a callback mutates state at runtime.
        self.model = Analyzer(program).analyze()
        self.structs = {struct.name for struct in program.structs}
        self.functions = {function.name: function for function in program.functions}
        self.globals: Dict[str, Any] = {}
        self.render = RenderRecorder()
        self.initialized = False
        self.loaded = False

    @classmethod
    def from_source(cls, source: str, filename: str = "<string>") -> "Interpreter":
        return cls(parse(source, filename=filename), filename=filename)

    def initialize(self) -> None:
        if self.initialized:
            return
        for declaration in self.program.globals:
            self.globals[declaration.name] = self.evaluate(declaration.value, {})
        self.initialized = True

    def shutdown(self) -> None:
        if not self.initialized:
            return
        if "quit" in self.functions:
            self.call("quit", [])
        self.globals.clear()
        self.initialized = False
        self.loaded = False

    def load(self) -> None:
        self.initialize()
        if not self.loaded:
            self.call("load", [])
            self.loaded = True

    def update(self, delta_time: float) -> None:
        self.load()
        if "update" in self.functions:
            self.call("update", [float(delta_time)])

    def draw(self) -> List[TextCommand]:
        self.load()
        self.render.reset_frame()
        if "draw" in self.functions:
            self.call("draw", [])
        return list(self.render.text_commands)

    def touchpressed(self, pointer_id: int, x: float, y: float) -> None:
        self.load()
        if "touchpressed" in self.functions:
            self.call("touchpressed", [int(pointer_id), float(x), float(y)])

    def call(self, name: str, args: Sequence[Any]) -> Any:
        function = self.functions.get(name)
        if function is None:
            return None
        if len(args) != len(function.params):
            raise RuntimeError(
                f"{self.filename}:{function.line}:{function.column}: {name} ожидал "
                f"{len(function.params)} аргументов, получено {len(args)}"
            )
        local = {parameter.name: value for parameter, value in zip(function.params, args)}
        try:
            self.execute_block(function.body, local)
        except _ReturnSignal as signal:
            return signal.value
        return None

    def execute_block(self, statements: Sequence[Stmt], local: Dict[str, Any]) -> None:
        for statement in statements:
            self.execute(statement, local)

    def execute(self, statement: Stmt, local: Dict[str, Any]) -> None:
        if isinstance(statement, Assign):
            value = self.evaluate(statement.value, local)
            self.assign(statement.target, value, local)
            return
        if isinstance(statement, ExprStmt):
            self.evaluate(statement.expression, local)
            return
        if isinstance(statement, Delete):
            target = statement.expression
            if isinstance(target, Name):
                self.globals[target.name] = None
            else:
                self.assign(target, None, local)
            return
        if isinstance(statement, If):
            branch = statement.then_body if self.truthy(self.evaluate(statement.condition, local)) else statement.else_body
            self.execute_block(branch, local)
            return
        if isinstance(statement, Return):
            raise _ReturnSignal(None if statement.expression is None else self.evaluate(statement.expression, local))
        raise RuntimeError(f"неизвестное выражение {statement!r}")

    def evaluate(self, expression: Expr, local: Dict[str, Any]) -> Any:
        if isinstance(expression, Literal):
            return expression.value
        if isinstance(expression, Name):
            if expression.name in local:
                return local[expression.name]
            if expression.name in self.globals:
                return self.globals[expression.name]
            if expression.name == "render":
                return self.render
            raise RuntimeError(f"{self.filename}:{expression.line}:{expression.column}: неизвестное имя {expression.name!r}")
        if isinstance(expression, Member):
            value = self.evaluate(expression.object, local)
            if isinstance(value, StructValue):
                try:
                    return value.fields[expression.name]
                except KeyError as exc:
                    raise RuntimeError(f"поле {expression.name!r} отсутствует в {value.type_name}") from exc
            if isinstance(value, RenderRecorder):
                return getattr(value, expression.name)
            raise RuntimeError(f"нельзя получить поле {expression.name!r}")
        if isinstance(expression, New):
            if expression.type_name not in self.structs:
                raise RuntimeError(f"неизвестная структура {expression.type_name!r}")
            fields = next(struct.fields for struct in self.program.structs if struct.name == expression.type_name)
            return StructValue(expression.type_name, {field.name: self.default_value(field.type_name) for field in fields})
        if isinstance(expression, Unary):
            value = self.evaluate(expression.operand, local)
            if expression.operator in ("-", "+"):
                return -value if expression.operator == "-" else +value
            if expression.operator in ("!", "not"):
                return not self.truthy(value)
            raise RuntimeError(f"неизвестный унарный оператор {expression.operator!r}")
        if isinstance(expression, Binary):
            # Keep short-circuiting for conditions, just like generated C.
            if expression.operator in ("and", "&&"):
                left = self.evaluate(expression.left, local)
                return self.truthy(left) and self.truthy(self.evaluate(expression.right, local))
            if expression.operator in ("or", "||"):
                left = self.evaluate(expression.left, local)
                return self.truthy(left) or self.truthy(self.evaluate(expression.right, local))
            left = self.evaluate(expression.left, local)
            right = self.evaluate(expression.right, local)
            operator = expression.operator
            if operator == "..":
                return self.to_string(left) + self.to_string(right)
            if operator == "+":
                return left + right
            if operator == "-":
                return left - right
            if operator == "*":
                return left * right
            if operator == "/":
                return left / right
            if operator == "%":
                return left % right
            if operator in ("==",):
                return left == right
            if operator in ("!=", "~="):
                return left != right
            if operator == ">":
                return left > right
            if operator == "<":
                return left < right
            if operator == ">=":
                return left >= right
            if operator == "<=":
                return left <= right
            raise RuntimeError(f"неизвестный оператор {operator!r}")
        if isinstance(expression, Call):
            return self.evaluate_call(expression, local)
        raise RuntimeError(f"неизвестное выражение {expression!r}")

    def evaluate_call(self, expression: Call, local: Dict[str, Any]) -> Any:
        if isinstance(expression.callee, Member) and isinstance(expression.callee.object, Name) and expression.callee.object.name == "render":
            method = getattr(self.render, expression.callee.name, None)
            if method is None:
                raise RuntimeError(f"неизвестная функция render.{expression.callee.name}")
            return method(*(self.evaluate(argument, local) for argument in expression.args))
        if isinstance(expression.callee, Name) and expression.callee.name in {"print", "log"}:
            values = [self.evaluate(argument, local) for argument in expression.args]
            print(" ".join(self.to_string(value) for value in values))
            return None
        if isinstance(expression.callee, Name):
            return self.call(expression.callee.name, [self.evaluate(argument, local) for argument in expression.args])
        raise RuntimeError("вызвать можно только функцию")

    def assign(self, target: Expr, value: Any, local: Dict[str, Any]) -> None:
        if isinstance(target, Name):
            if target.name in self.globals and target.name not in local:
                self.globals[target.name] = value
            else:
                local[target.name] = value
            return
        if isinstance(target, Member):
            owner = self.evaluate(target.object, local)
            if isinstance(owner, StructValue):
                owner.fields[target.name] = value
                return
            raise RuntimeError(f"объект не поддерживает поле {target.name!r}")
        raise RuntimeError("слева от '=' должно быть имя или поле")

    @staticmethod
    def default_value(type_name: str) -> Any:
        if type_name in {"float"}:
            return 0.0
        if type_name in {"int", "bool", "boolean"}:
            return 0
        if type_name in {"string", "str"}:
            return ""
        return None

    @staticmethod
    def truthy(value: Any) -> bool:
        return bool(value)

    @staticmethod
    def to_string(value: Any) -> str:
        if value is None:
            return "nil"
        if value is True:
            return "true"
        if value is False:
            return "false"
        if isinstance(value, float) and value.is_integer():
            return str(int(value))
        return str(value)


def run_source(source: str, clicks: int = 0, frames: int = 1, dt: float = 1.0 / 60.0,
               filename: str = "<string>") -> Interpreter:
    """Run a script through its lifecycle and return the interpreter state."""

    interpreter = Interpreter.from_source(source, filename=filename)
    interpreter.initialize()
    interpreter.load()
    for _ in range(max(0, clicks)):
        interpreter.touchpressed(0, 0.0, 0.0)
    for _ in range(max(0, frames)):
        interpreter.update(dt)
        interpreter.draw()
    return interpreter
