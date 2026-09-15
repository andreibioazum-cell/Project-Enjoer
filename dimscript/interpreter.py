"""Reference interpreter for DimScript — the twin of the native VM in ``src/ds_vm.c``.

The interpreter is a development tool with one strict rule: it must behave
*exactly* like the runtime that ships.  Arithmetic, string conversion, list
indices, the shape of every ``render.`` primitive and the meaning of every
callback are shared with :file:`src/ds_vm_exec.c`, so a script that runs here
runs the same on Vulkan.  That is also what makes the parity test
:file:`tools/tests/dimscript.py` meaningful: it compares the two frame
batches vertex by vertex.
"""

from __future__ import annotations

import math
from dataclasses import dataclass, field
from typing import Any, Dict, List, Optional, Sequence, Tuple

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
from .compiler import Analyzer
from .lexer import DimScriptError
from .parser import parse

MAX_TEXT = 64
TEXT_LENGTH = 160
MAX_VERTICES = 16 * 1024


class RuntimeError(DimScriptError):
    pass


class BreakSignal(Exception):
    pass


class ContinueSignal(Exception):
    pass


class ReturnSignal(Exception):
    def __init__(self, value: Any = None) -> None:
        super().__init__("return")
        self.value = value


@dataclass
class StructValue:
    type_name: str
    fields: Dict[str, Any] = field(default_factory=dict)

    def get(self, name: str) -> Any:
        if name not in self.fields:
            raise RuntimeError(f"у {self.type_name} нет поля {name!r}")
        return self.fields[name]


class ListValue(list):
    """A DimScript list.  A Python list subclass, because that is what it is."""

    @property
    def count(self) -> int:  # mirrors `list.count` in the VM (own property)
        return list.__len__(self)


@dataclass
class Vertex:
    x: float
    y: float
    r: float
    g: float
    b: float


@dataclass
class TextCommand:
    text: str
    x: float
    y: float
    scale: float
    color: Tuple[float, float, float]


@dataclass
class Frame:
    """The same triangle batch the C runtime fills, minus the GPU."""

    vertices: List[Vertex] = field(default_factory=list)
    texts: List[TextCommand] = field(default_factory=list)
    clear_color: Tuple[float, float, float] = (0.02, 0.03, 0.05)
    has_clear: bool = False
    overflow: bool = False

    def begin(self) -> None:
        self.vertices.clear()
        self.texts.clear()
        self.has_clear = False
        self.overflow = False

    def triangle(self, x0, y0, x1, y1, x2, y2, r, g, b) -> None:
        # Positions are raw pixels (the renderer owns the orthographic
        # transform); only the colour is clamped, exactly like push_vertex() in
        # src/enjoer_draw.c.
        color = (_clamp01(r), _clamp01(g), _clamp01(b))
        for x, y in ((x0, y0), (x1, y1), (x2, y2)):
            if len(self.vertices) >= MAX_VERTICES:
                self.overflow = True
                return
            self.vertices.append(Vertex(x, y, *color))

    def summary(self) -> Dict[str, Any]:
        return {
            "vertex_count": len(self.vertices),
            "text_count": len(self.texts),
            "texts": [{"text": t.text, "x": t.x, "y": t.y, "scale": t.scale} for t in self.texts],
        }


def _clamp01(value: float) -> float:
    if value != value:
        return 0.0
    return 0.0 if value < 0.0 else 1.0 if value > 1.0 else value


class RenderRecorder:
    """Font-less render API; the tessellation matches ``src/enjoer_draw.c``."""

    def __init__(self, frame: Frame) -> None:
        self.frame = frame
        self.color_value = (1.0, 1.0, 1.0)
        self.text_calls = 0

    @property
    def text_commands(self) -> List[TextCommand]:
        return self.frame.texts

    def reset_frame(self) -> None:
        self.frame.begin()

    def clear(self, red: Any, green: Any, blue: Any) -> None:
        self.color(float(red), float(green), float(blue))
        self.frame.clear_color = (float(red), float(green), float(blue))
        self.frame.has_clear = True

    def color(self, red: Any, green: Any, blue: Any, alpha: Any = None) -> None:
        values = [float(red), float(green), float(blue)]
        if alpha is not None:
            a = max(0.0, min(1.0, float(alpha)))
            values = [value * a for value in values]
        self.color_value = (values[0], values[1], values[2])

    def color_alpha(self, red: Any, green: Any, blue: Any, alpha: Any) -> None:
        self.color(red, green, blue, alpha)

    def rect(self, x: Any, y: Any, width: Any, height: Any) -> None:
        x, y, width, height = map(float, (x, y, width, height))
        if width < 0:
            x, width = x + width, -width
        if height < 0:
            y, height = y + height, -height
        if width <= 0 or height <= 0:
            return
        self._quad(x, y, x + width, y, x + width, y + height, x, y + height)

    def frame_rect(self, x: Any, y: Any, width: Any, height: Any, thickness: Any) -> None:
        x, y, width, height, t = map(float, (x, y, width, height, thickness))
        if width <= 0 or height <= 0:
            return
        t = t if t > 0 else 1.0
        if t * 2 > width:
            t = width * 0.5
        if t * 2 > height:
            t = height * 0.5
        self.rect(x, y, width, t)
        self.rect(x, y + height - t, width, t)
        self.rect(x, y + t, t, height - t - t)
        self.rect(x + width - t, y + t, t, height - t - t)

    # `frame` is a keyword-ish name in the language, but as a builtin member it
    # is spelled exactly like that in scripts.
    frame_rect_alias = frame_rect

    def circle(self, x: Any, y: Any, radius: Any, segments: int = 0) -> None:
        x, y, radius = float(x), float(y), float(radius)
        if radius <= 0:
            return
        count = 24 if segments < 3 else min(segments, 64)
        step = 2 * math.pi / count
        for index in range(count):
            a0, a1 = step * index, step * (index + 1)
            self._triangle(
                x,
                y,
                x + math.cos(a0) * radius,
                y + math.sin(a0) * radius,
                x + math.cos(a1) * radius,
                y + math.sin(a1) * radius,
            )

    def ring(self, x: Any, y: Any, radius: Any, thickness: Any, segments: int = 0) -> None:
        x, y, radius, t = float(x), float(y), float(radius), float(thickness)
        if radius <= 0:
            return
        t = min(t, radius) if t > 0 else 1.0
        inner = radius - t
        count = 24 if segments < 3 else min(segments, 64)
        step = 2 * math.pi / count
        for index in range(count):
            a0, a1 = step * index, step * (index + 1)
            c0, s0, c1, s1 = math.cos(a0), math.sin(a0), math.cos(a1), math.sin(a1)
            self._quad(
                x + c0 * radius, y + s0 * radius,
                x + c1 * radius, y + s1 * radius,
                x + c1 * inner, y + s1 * inner,
                x + c0 * inner, y + s0 * inner,
            )

    def line(self, x0: Any, y0: Any, x1: Any, y1: Any, thickness: Any) -> None:
        x0, y0, x1, y1, t = map(float, (x0, y0, x1, y1, thickness))
        dx, dy = x1 - x0, y1 - y0
        length = math.hypot(dx, dy)
        if length <= 0.0001:
            if t > 0:
                self.circle(x0, y0, t * 0.5, 10)
            return
        t = t if t > 0 else 1.0
        nx, ny = -dy / length * (t * 0.5), dx / length * (t * 0.5)
        self._quad(x0 + nx, y0 + ny, x1 + nx, y1 + ny, x1 - nx, y1 - ny, x0 - nx, y0 - ny)

    def tri(self, x0: Any, y0: Any, x1: Any, y1: Any, x2: Any, y2: Any) -> None:
        self._triangle(float(x0), float(y0), float(x1), float(y1), float(x2), float(y2))

    def triangle(self, *args: Any) -> None:
        self.tri(*args)

    def text(self, text: Any, x: Any, y: Any, scale: Any) -> None:
        self.text_calls += 1
        if len(self.frame.texts) >= MAX_TEXT:
            return
        self.frame.texts.append(
            TextCommand(
                Interpreter.to_string(text)[:TEXT_LENGTH],
                float(x),
                float(y),
                float(scale),
                self.color_value,
            )
        )

    def _triangle(self, x0, y0, x1, y1, x2, y2) -> None:
        r, g, b = self.color_value
        self.frame.triangle(x0, y0, x1, y1, x2, y2, r, g, b)

    def _quad(self, x0, y0, x1, y1, x2, y2, x3, y3) -> None:
        self._triangle(x0, y0, x1, y1, x2, y2)
        self._triangle(x0, y0, x2, y2, x3, y3)


LIST_METHODS = {"push", "insert", "delete", "remove_at", "clear", "index_of", "join"}
RENDER_MEMBERS = {"color_value", "text_calls", "clear", "color", "color_alpha", "rect", "frame",
                  "circle", "ring", "line", "tri", "triangle", "text"}


class Interpreter:
    def __init__(self, program: Program, filename: str = "<string>") -> None:
        self.program = program
        self.filename = filename
        # Run the same static checks used by native compilation.  This catches
        # a typo before a callback mutates state at runtime.
        self.model = Analyzer(program).analyze()
        self.structs = {struct.name: struct for struct in program.structs}
        self.functions = {function.name: function for function in program.functions}
        self.globals: Dict[str, Any] = {}
        self.frame = Frame()
        self.render = RenderRecorder(self.frame)
        self.engine = EngineState()
        self.text_log: List[str] = []
        self.initialized = False
        self.loaded = False
        self.quit_requested = False

    @classmethod
    def from_source(cls, source: str, filename: str = "<string>") -> "Interpreter":
        return cls(parse(source, filename=filename), filename=filename)

    @classmethod
    def from_game(cls, directory, manifest=None) -> "Interpreter":
        """Build an interpreter for a game folder (``game.manifest`` + ``.ds``)."""

        from .project import load_project

        project = load_project(directory) if manifest is None else load_project(directory)
        return cls(project.program, filename=str(project.directory))

    # --- lifecycle ---------------------------------------------------------
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

    def resized(self, width: float, height: float) -> None:
        self.load()
        self.engine.resize(int(width), int(height))
        self.call("resized", [float(width), float(height)])

    def update(self, delta_time: float) -> None:
        self.load()
        self.engine.new_frame(delta_time)
        if "update" in self.functions:
            self.call("update", [float(delta_time)])

    def draw(self) -> List[TextCommand]:
        self.load()
        self.render.reset_frame()
        if "draw" in self.functions:
            self.call("draw", [])
        return list(self.frame.texts)

    def touch(self, pointer_id: int, x: float, y: float, down: bool) -> None:
        self.load()
        self.engine.touch(pointer_id, x, y, down)
        name = "touchpressed" if down else "touchreleased"
        if name in self.functions:
            self.call(name, [int(pointer_id), float(x), float(y)])

    def touchpressed(self, pointer_id: int, x: float, y: float) -> None:
        self.load()
        self.engine.touch(pointer_id, x, y, True)
        if "touchpressed" in self.functions:
            self.call("touchpressed", [int(pointer_id), float(x), float(y)])

    def touchmoved(self, pointer_id: int, x: float, y: float) -> None:
        self.load()
        self.engine.touch(pointer_id, x, y, True)
        if "touchmoved" in self.functions:
            self.call("touchmoved", [int(pointer_id), float(x), float(y)])

    def touchreleased(self, pointer_id: int, x: float, y: float) -> None:
        self.load()
        self.engine.touch(pointer_id, x, y, False)
        if "touchreleased" in self.functions:
            self.call("touchreleased", [int(pointer_id), float(x), float(y)])

    def key(self, name: str, down: bool) -> None:
        self.load()
        self.engine.key(name, down)
        callback = "keypressed" if down else "keyreleased"
        if callback in self.functions:
            self.call(callback, [name])

    # --- calls --------------------------------------------------------------
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
        except ReturnSignal as signal:
            return signal.value
        return None

    def execute_block(self, statements: Sequence[Stmt], local: Dict[str, Any]) -> None:
        for statement in statements:
            self.execute(statement, local)

    def execute(self, statement: Stmt, local: Dict[str, Any]) -> None:
        if isinstance(statement, Assign):
            value = self.evaluate(statement.value, local)
            self.assign(statement.target, value, local, statement.explicit_local)
            return
        if isinstance(statement, ExprStmt):
            self.evaluate(statement.expression, local)
            return
        if isinstance(statement, Delete):
            target = statement.expression
            if isinstance(target, Name):
                if target.name not in self.globals and target.name not in local:
                    raise RuntimeError(f"неизвестное имя {target.name!r}")
                self.assign(target, None, local)
            else:
                self.assign(target, None, local)
            return
        if isinstance(statement, If):
            branch = statement.then_body if self.truthy(self.evaluate(statement.condition, local)) else statement.else_body
            self.execute_block(branch, local)
            return
        if isinstance(statement, While):
            while self.truthy(self.evaluate(statement.condition, local)):
                try:
                    self.execute_block(statement.body, local)
                except BreakSignal:
                    break
                except ContinueSignal:
                    continue
            return
        if isinstance(statement, For):
            start = self._as_int(self.evaluate(statement.start, local))
            stop = self._as_int(self.evaluate(statement.stop, local))
            step = 1 if statement.step is None else self._as_int(self.evaluate(statement.step, local))
            if step == 0:
                raise RuntimeError("шаг for не может быть нулём")
            counter = start
            while (step > 0 and counter <= stop) or (step < 0 and counter >= stop):
                local[statement.variable] = counter
                try:
                    self.execute_block(statement.body, local)
                except BreakSignal:
                    break
                except ContinueSignal:
                    pass
                counter += step
            return
        if isinstance(statement, Break):
            raise BreakSignal()
        if isinstance(statement, Continue):
            raise ContinueSignal()
        if isinstance(statement, Return):
            raise ReturnSignal(None if statement.expression is None else self.evaluate(statement.expression, local))
        raise RuntimeError(f"неизвестное выражение {statement!r}")

    # --- expressions --------------------------------------------------------
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
            if expression.name == "math":
                return _Namespace("math")
            if expression.name in {"engine", "input"}:
                return _Namespace(expression.name)
            raise RuntimeError(f"{self.filename}:{expression.line}:{expression.column}: неизвестное имя {expression.name!r}")
        if isinstance(expression, Member):
            value = self.evaluate(expression.object, local)
            return self.member_value(value, expression.name, expression, local)
        if isinstance(expression, Index):
            owner = self.evaluate(expression.object, local)
            index = self._as_int(self.evaluate(expression.index, local))
            return self.get_index(owner, index)
        if isinstance(expression, ListLiteral):
            return ListValue(self.evaluate(item, local) for item in expression.items)
        if isinstance(expression, New):
            if expression.type_name not in self.structs:
                raise RuntimeError(f"неизвестная структура {expression.type_name!r}")
            fields = self.structs[expression.type_name].fields
            return StructValue(expression.type_name, {field.name: self.default_value(field.type_name) for field in fields})
        if isinstance(expression, Unary):
            value = self.evaluate(expression.operand, local)
            if expression.operator in ("-", "+"):
                number = self._as_float(value)
                return -number if expression.operator == "-" else number
            if expression.operator in ("!", "not"):
                return not self.truthy(value)
            raise RuntimeError(f"неизвестный унарный оператор {expression.operator!r}")
        if isinstance(expression, Binary):
            return self.binary(expression, local)
        if isinstance(expression, Call):
            return self.evaluate_call(expression, local)
        raise RuntimeError(f"неизвестное выражение {expression!r}")

    def binary(self, expression: Binary, local: Dict[str, Any]) -> Any:
        operator = expression.operator
        if operator in ("and", "&&"):
            left = self.evaluate(expression.left, local)
            return self.truthy(left) and self.truthy(self.evaluate(expression.right, local))
        if operator in ("or", "||"):
            left = self.evaluate(expression.left, local)
            return self.truthy(left) or self.truthy(self.evaluate(expression.right, local))
        left = self.evaluate(expression.left, local)
        right = self.evaluate(expression.right, local)
        if operator == "..":
            return self.to_string(left) + self.to_string(right)
        if operator == "==":
            return self.equals(left, right)
        if operator in ("!=", "~="):
            return not self.equals(left, right)
        if isinstance(left, str) or isinstance(right, str):
            if operator in ("<", "<=", ">", ">="):
                return _compare(self.to_string(left), self.to_string(right), operator)
            raise RuntimeError("строку можно только склеить оператором '..'")
        if operator in ("<", "<=", ">", ">="):
            if _is_number(left) and _is_number(right):
                both_int = _is_int(left) and _is_int(right)
                if both_int:
                    return _compare(int(left), int(right), operator)
                return _compare(self._as_float(left), self._as_float(right), operator)
            return _compare(self._as_float(left), self._as_float(right), operator)
        left_int = _is_int(left)
        right_int = _is_int(right)
        both_int = left_int and right_int
        if operator == "+":
            if both_int:
                return int(left) + int(right)
            return self._as_float(left) + self._as_float(right)
        if operator == "-":
            if both_int:
                return int(left) - int(right)
            return self._as_float(left) - self._as_float(right)
        if operator == "*":
            if both_int:
                return int(left) * int(right)
            return self._as_float(left) * self._as_float(right)
        if operator == "/":
            divisor = self._as_float(right)
            if divisor == 0.0:
                raise RuntimeError("деление на ноль")
            return self._as_float(left) / divisor
        if operator == "%":
            divisor = self._as_float(right)
            if divisor == 0.0:
                raise RuntimeError("остаток от нуля")
            if both_int:
                value = int(left) % int(right)
                return int(value)
            value = self._as_float(left)
            return value - math.floor(value / divisor) * divisor
        raise RuntimeError(f"неизвестный оператор {operator!r}")

    @staticmethod
    def equals(left: Any, right: Any) -> bool:
        if isinstance(left, StructValue) or isinstance(right, StructValue):
            return left is right
        if isinstance(left, list) or isinstance(right, list):
            return left is right
        if isinstance(left, str) or isinstance(right, str):
            return Interpreter.to_string(left) == Interpreter.to_string(right)
        if _is_number(left) and _is_number(right):
            return float(left) == float(right)
        if left is None or right is None:
            return left is None and right is None
        return bool(left) == bool(right)

    def member_value(self, value: Any, name: str, expression: Expr, local: Dict[str, Any]) -> Any:
        if isinstance(value, StructValue):
            return value.get(name)
        if isinstance(value, RenderRecorder):
            if name == "color_value":
                return value.color_value
            if name not in RENDER_MEMBERS:
                raise RuntimeError(f"нет свойства render.{name}")
            return value.frame_rect if name == "frame" else getattr(value, name)
        if isinstance(value, _Namespace):
            return _Bound(f"{value.name}.{name}", None, (value.name, name))
        if isinstance(value, list):
            if name == "count":
                return len(value)
            raise RuntimeError(f"у списка нет свойства {name!r}")
        if isinstance(value, str):
            if name == "length":
                return len(value)
            raise RuntimeError(f"у строки нет свойства {name!r}")
        if isinstance(value, StructValue):
            return value.get(name)
        if value is None:
            raise RuntimeError(f"нельзя прочитать поле {name!r} у nil")
        raise RuntimeError(f"нельзя получить поле {name!r}")

    def get_index(self, owner: Any, index: int) -> Any:
        if isinstance(owner, list):
            position = index + len(owner) if index < 0 else index
            if position < 0 or position >= len(owner):
                raise RuntimeError(f"индекс {index} вне списка (длина {len(owner)})")
            return owner[position]
        if isinstance(owner, str):
            position = index + len(owner) if index < 0 else index
            if position < 0 or position >= len(owner):
                raise RuntimeError(f"индекс {index} вне строки (длина {len(owner)})")
            return owner[position]
        raise RuntimeError("индекс [..] возможен только у списка или строки")

    def set_index(self, owner: Any, index: int, value: Any) -> None:
        if not isinstance(owner, list):
            raise RuntimeError("изменять по индексу можно только элементы списка")
        position = index + len(owner) if index < 0 else index
        if position < 0 or position >= len(owner):
            raise RuntimeError(f"индекс {index} вне списка (длина {len(owner)})")
        owner[position] = value

    def evaluate_call(self, expression: Call, local: Dict[str, Any]) -> Any:
        callee = expression.callee
        if isinstance(callee, Member) and isinstance(callee.object, Name):
            namespace = callee.object.name
            if namespace in {"render", "math", "engine", "input"} or namespace in self.globals:
                pass
        if isinstance(callee, Member):
            owner = self.evaluate(callee.object, local)
            if isinstance(owner, list):
                return self.list_method(owner, callee.name, expression, local)
            if isinstance(owner, _Namespace):
                arguments = [self.evaluate(argument, local) for argument in expression.args]
                return self.builtin(owner.name, callee.name, arguments, expression)
            if isinstance(owner, RenderRecorder):
                arguments = [self.evaluate(argument, local) for argument in expression.args]
                return self.builtin("render", callee.name, arguments, expression)
        if isinstance(callee, Name):
            if callee.name in self.functions:
                arguments = [self.evaluate(argument, local) for argument in expression.args]
                return self.call(callee.name, arguments)
            arguments = [self.evaluate(argument, local) for argument in expression.args]
            if callee.name in {"print", "log", "str", "len", "num", "sqrt", "floor", "ceil", "abs"}:
                return self.builtin("", callee.name, arguments, expression)
            raise RuntimeError(f"нет функции {callee.name!r}")
        raise RuntimeError("вызвать можно только функцию или builtin")

    def list_method(self, owner: List[Any], name: str, expression: Call, local: Dict[str, Any]) -> Any:
        arguments = [self.evaluate(argument, local) for argument in expression.args]
        if name == "push":
            owner.extend(arguments or [None])
            return None
        if name == "insert":
            if len(arguments) != 2:
                raise RuntimeError("insert(индекс, значение) ждёт 2 аргумента")
            index = self._as_int(arguments[0])
            index = min(max(index, 0), len(owner))
            owner.insert(index, arguments[1])
            return None
        if name in {"delete", "remove_at"}:
            if len(arguments) != 1:
                raise RuntimeError(f"{name}(индекс) ждёт 1 аргумент")
            index = self._as_int(arguments[0])
            position = index + len(owner) if index < 0 else index
            if position < 0 or position >= len(owner):
                raise RuntimeError(f"индекс {index} вне списка (длина {len(owner)})")
            return owner.pop(position)
        if name == "clear":
            owner.clear()
            return None
        if name == "index_of":
            value = arguments[0] if arguments else None
            for index, item in enumerate(owner):
                if self.equals(item, value):
                    return index
            return -1
        if name == "join":
            separator = self.to_string(arguments[0]) if arguments else ""
            return separator.join(self.to_string(item) for item in owner)
        raise RuntimeError(f"у списка нет метода {name!r}")

    # --- builtins -----------------------------------------------------------
    def builtin(self, namespace: str, name: str, arguments: List[Any], expression: Expr) -> Any:
        # Only the numeric namespaces coerce their arguments; render keeps the
        # raw values because `render.text` takes text first.
        numbers = (
            [] if namespace in ("render", "")
            # `input.key("a")` takes text; everything else is numeric, and a
            # string reaching the arithmetic below raises like it does in the VM.
            else [argument if isinstance(argument, str) else self._as_float(argument)
                  for argument in arguments]
        )
        if namespace in ("render", ""):
            if name in {"print", "log"}:
                line = " ".join(self.to_string(argument) for argument in arguments)
                self.text_log.append(line)
                print(line)
                return None
            if name == "str":
                return self.to_string(arguments[0])
            if name == "num":
                return self._as_float(arguments[0])
            if name == "len":
                value = arguments[0]
                if isinstance(value, (list, str)):
                    return len(value)
                if value is None:
                    return 0
                return int(self._as_float(value))
            if namespace == "":
                raise RuntimeError(f"нет функции {name!r}")
        if namespace == "render":
            return self.render_builtin(name, arguments)
        if namespace == "math":
            return self.math_builtin(name, arguments, numbers, expression)
        if namespace == "engine":
            return self.engine_builtin(name, numbers)
        if namespace == "input":
            return self.input_builtin(name, arguments, numbers)
        raise RuntimeError(f"нет builtin {namespace}.{name}")

    def render_builtin(self, name: str, arguments: List[Any]) -> Any:
        method = {"frame": "frame_rect"}.get(name, name)
        if name not in {"clear", "color", "color_alpha", "rect", "frame", "circle", "ring", "line",
                        "tri", "triangle", "text"}:
            raise RuntimeError(f"неизвестная функция render.{name}")
        arity = {"clear": 3, "color": 3, "color_alpha": 4, "rect": 4, "frame": 5, "circle": 3,
                 "ring": 4, "line": 5, "tri": 6, "triangle": 6, "text": 4}[name]
        if len(arguments) != arity:
            raise RuntimeError(f"render.{name} ожидает {arity} аргумента")
        getattr(self.render, method)(*arguments)
        return None

    def math_builtin(self, name: str, arguments: List[Any], numbers: List[float], expression: Expr) -> Any:
        if name not in {"floor", "ceil", "round", "abs", "sign", "sqrt", "sin", "cos", "tan", "min",
                        "max", "mod", "pow", "lerp", "random", "pi", "e", "tau", "inf"}:
            raise RuntimeError(f"нет функции math.{name}")
        both_int = all(_is_int(argument) for argument in arguments)
        if name == "floor":
            return int(math.floor(numbers[0])) if both_int else math.floor(numbers[0])
        if name == "ceil":
            return int(math.ceil(numbers[0])) if both_int else math.ceil(numbers[0])
        if name == "round":
            return int(math.floor(numbers[0] + 0.5)) if both_int else math.floor(numbers[0] + 0.5)
        if name == "abs":
            return abs(int(numbers[0])) if both_int else abs(numbers[0])
        if name == "sign":
            return 1 if numbers[0] > 0 else -1 if numbers[0] < 0 else 0
        if name == "sqrt":
            if numbers[0] < 0:
                raise RuntimeError("sqrt ожидает число >= 0")
            value = math.sqrt(numbers[0])
            return int(value) if both_int and value.is_integer() else value
        if name == "sin":
            return math.sin(numbers[0])
        if name == "cos":
            return math.cos(numbers[0])
        if name == "tan":
            return math.tan(numbers[0])
        if name == "min":
            return arguments[0] if numbers[0] <= numbers[1] else arguments[1]
        if name == "max":
            return arguments[0] if numbers[0] >= numbers[1] else arguments[1]
        if name == "mod":
            if numbers[1] == 0.0:
                raise RuntimeError("остаток от нуля")
            if both_int:
                return int(numbers[0]) % int(numbers[1])
            return numbers[0] - math.floor(numbers[0] / numbers[1]) * numbers[1]
        if name == "pow":
            return math.pow(numbers[0], numbers[1])
        if name == "lerp":
            return numbers[0] + (numbers[1] - numbers[0]) * numbers[2]
        if name == "random":
            if len(arguments) == 0:
                return self.engine.random()
            if len(arguments) == 1:
                return self.engine.random() * numbers[0]
            return numbers[0] + self.engine.random() * (numbers[1] - numbers[0])
        return {"pi": math.pi, "e": math.e, "tau": math.tau, "inf": math.inf}[name]

    def engine_builtin(self, name: str, numbers: List[float]) -> Any:
        engine = self.engine
        if name == "width":
            return float(engine.width)
        if name == "height":
            return float(engine.height)
        if name == "time":
            return engine.time
        if name == "delta":
            return engine.delta_time
        if name == "fps":
            return engine.fps
        if name == "frame":
            return engine.frame
        if name == "quit":
            self.quit_requested = True
            return None
        raise RuntimeError(f"нет функции engine.{name}")

    def input_builtin(self, name: str, arguments: List[Any], numbers: List[float]) -> Any:
        engine = self.engine
        if name == "touches":
            return len(engine.touches)
        if name in {"touch_x", "touch_y", "touch_down"}:
            if len(arguments) != 1:
                raise RuntimeError(f"input.{name} ожидает 1 аргумент")
            touch = engine.touch_by_id(int(numbers[0]))
            if touch is None:
                return 0.0 if name != "touch_down" else False
            return {"touch_x": float(touch[1]), "touch_y": float(touch[2]), "touch_down": bool(touch[3])}[name]
        if name == "key":
            if len(arguments) != 1:
                raise RuntimeError("input.key ожидает 1 аргумент")
            return self.to_string(arguments[0]) in engine.keys_down
        raise RuntimeError(f"нет функции input.{name}")

    # --- assignment ---------------------------------------------------------
    def assign(self, target: Expr, value: Any, local: Dict[str, Any], explicit_local: bool = False) -> None:
        if isinstance(target, Name):
            if explicit_local or (target.name not in self.globals):
                local[target.name] = value
            else:
                self.globals[target.name] = value
            return
        if isinstance(target, Index):
            owner = self.evaluate(target.object, local)
            self.set_index(owner, self._as_int(self.evaluate(target.index, local)), value)
            return
        if isinstance(target, Member):
            owner = self.evaluate(target.object, local)
            if isinstance(owner, StructValue):
                if target.name not in owner.fields:
                    raise RuntimeError(f"у {owner.type_name} нет поля {target.name!r}")
                owner.fields[target.name] = value
                return
            if isinstance(owner, RenderRecorder):
                self.render_setter(target.name, value)
                return
            raise RuntimeError(f"объект не поддерживает поле {target.name!r}")
        raise RuntimeError("слева от '=' должно быть имя, поле или элемент списка")

    def render_setter(self, name: str, value: Any) -> None:
        if name in {"clear_color", "clear"}:
            self.render.clear(*self.color_tuple(value))
            return
        if name == "color":
            self.render.color(*self.color_tuple(value))
            return
        raise RuntimeError(f"в render нельзя присвоить {name!r}")

    def color_tuple(self, value: Any) -> Tuple[float, float, float]:
        if isinstance(value, (list, tuple)):
            parts = [float(part) for part in value]
            if len(parts) == 3:
                return (parts[0], parts[1], parts[2])
        raise RuntimeError("цвет — три числа")

    @staticmethod
    def default_value(type_name: str) -> Any:
        if type_name in {"float"}:
            return 0.0
        if type_name in {"int", "bool", "boolean"}:
            return 0
        if type_name in {"string", "str"}:
            return ""
        if type_name in {"list", "array"}:
            return ListValue()
        return None

    @staticmethod
    def truthy(value: Any) -> bool:
        if isinstance(value, str):
            return len(value) > 0
        if isinstance(value, (StructValue, list)):
            return value is not None
        return bool(value)

    @staticmethod
    def _as_float(value: Any) -> float:
        if isinstance(value, str):
            raise RuntimeError("арифметика ждёт числа, получено string")
        if value is None:
            return 0.0
        return float(value)

    @staticmethod
    def _as_int(value: Any) -> int:
        if isinstance(value, bool):
            return int(value)
        if isinstance(value, int):
            return value
        if isinstance(value, float):
            return int(value)
        if isinstance(value, str):
            try:
                return int(float(value))
            except ValueError as exc:
                raise RuntimeError(f"{value!r} не число") from exc
        raise RuntimeError("ожидалось целое число")

    @staticmethod
    def to_string(value: Any) -> str:
        if value is None:
            return "nil"
        if value is True:
            return "true"
        if value is False:
            return "false"
        if isinstance(value, int):
            return str(value)
        if isinstance(value, float):
            if value.is_integer() and abs(value) < 1e15:
                return f"{value:.1f}"
            return repr(value)
        if isinstance(value, list):
            return "[" + ", ".join(Interpreter.to_string(item) for item in value) + "]"
        if isinstance(value, StructValue):
            return f"{value.type_name}@{id(value) % 100000}"
        return str(value)


def _is_int(value: Any) -> bool:
    return isinstance(value, int) and not isinstance(value, bool)


def _is_number(value: Any) -> bool:
    return isinstance(value, (int, float)) and not isinstance(value, bool)


def _compare(left: Any, right: Any, operator: str) -> bool:
    if operator == "<":
        return left < right
    if operator == "<=":
        return left <= right
    if operator == ">":
        return left > right
    return left >= right


@dataclass
class _Namespace:
    name: str


@dataclass
class _Bound:
    name: str
    function: Any = None
    path: Tuple[str, str] = ("", "")


class EngineState:
    """Mirror of ``DsEngineState`` in the native runtime."""

    def __init__(self, width: int = 1024, height: int = 768) -> None:
        self.width = width
        self.height = height
        self.time = 0.0
        self.delta_time = 1.0 / 60.0
        self.fps = 60.0
        self.frame = 0
        self.touches: List[Tuple[int, float, float, int]] = []
        self.keys_down: set = set()
        self._random = 0.5

    def resize(self, width: int, height: int) -> None:
        if width > 0:
            self.width = width
        if height > 0:
            self.height = height

    def new_frame(self, delta_time: float) -> None:
        self.delta_time = float(delta_time)
        self.time += self.delta_time
        self.fps = (1.0 / self.delta_time) if self.delta_time > 0 else self.fps
        self.frame += 1
        self._random = (self._random * 1103515245 + 12345) % 2147483648 / 2147483648.0

    def touch(self, pointer_id: int, x: float, y: float, down: bool) -> None:
        for index, touch in enumerate(self.touches):
            if touch[0] == pointer_id:
                if down:
                    self.touches[index] = (pointer_id, float(x), float(y), 1)
                else:
                    del self.touches[index]
                return
        if down:
            self.touches.append((pointer_id, float(x), float(y), 1))

    def touch_by_id(self, pointer_id: int) -> Optional[Tuple[int, float, float, int]]:
        for touch in self.touches:
            if touch[0] == pointer_id:
                return touch
        for index, touch in enumerate(self.touches):
            if index == pointer_id:
                return touch
        return None

    def key(self, name: str, down: bool) -> None:
        if down:
            self.keys_down.add(name)
        else:
            self.keys_down.discard(name)

    def random(self) -> float:
        self._random = (self._random * 2147483647 + 1.0) % 1.0
        if self._random <= 0.0:
            self._random = 0.12345
        return self._random


def run_source(source: str, clicks: int = 0, frames: int = 1, dt: float = 1.0 / 60.0,
               filename: str = "<string") -> Interpreter:
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
