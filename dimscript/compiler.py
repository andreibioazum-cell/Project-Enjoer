"""DimScript front end: semantic checks and native C99 code generation.

STRICT COMPILER MODE — NO VM, NO REFCOUNT, MANUAL MEMORY, SPEED LIKE C.

A game is *compiled* ahead of time, never interpreted.  ``tools/aot.py`` loads a
game folder, merges every ``.ds`` file into one program and hands it to
:class:`CCompiler`, whose output is plain C99 against
:file:`src/dimscript_runtime.h`.  clang (Android NDK is clang) at -O3 then
produces machine code that ships inside the APK — no bytecode, no dispatch
loop, no runtime type checks, no GC in hot path.

Memory model — manual, like in C, with strings as immutable values:
* ``new X``, ``[]``, string literals, ``..`` are raw malloc via ``ds_alloc``.
  Caller owns the pointer.
* Strings are values: storing one (global, local, struct field, list element)
  duplicates via ``ds_string_replace``/``ds_list_set_string`` and frees the old
  one, and ``return`` duplicates too — so a stored string is always owned and a
  borrowed one (literal, param, getter) can never be freed out from under its
  owner.  Duplication is unobservable: strings have no mutation API.
* Lists and structs have identity: pushing a struct or a list stores the
  pointer raw (sharing like in C, user must not double-free), while string
  lists own private duplicates freed by ``ds_string_dtor``.
* Assignment of non-strings is raw ``a = b`` — no keep/release, no overhead.
* ``delete x`` emits ``ds_release_slot(&x)`` which is ``free + NULL``.
* Getters return BORROWED pointers; function parameters are borrowed (a callee
  that wants to keep one duplicates it with ``"" .. param``).
* ``ds_concat``, ``ds_text_join``, ``ds_*_to_string`` allocate fresh owned string.
* Struct dtors free owned fields via ``ds_release`` (which is free + dtor).
* No ``ds_keep``, ``ds_move``, ``ds_assign`` in generated code — speed like C.
* AOT to machine code: ``tools/aot.py`` -> ``game.c`` -> ``clang -O3`` -> ``.so``.
* Fresh strings consumed in place (``..`` operands, ``log``/``num``/``len``
  arguments, ``==`` operands, builtin and user-function string arguments, list
  push/insert/set items) live in a per-statement temp frame (``__ds_tN``) freed
  with LIFO ``ds_release`` at the ``;`` — the statement still reads like C, and
  ASan stays clean.  Only a provably consumed value may be released.
* Conditions with temps materialize (``int __ds_cN = (cond);``) and free before
  the branches run; a ``while`` with temps becomes ``for (;;)`` with the check
  on top, so every pass pays for its own strings, and ``for`` start bounds
  materialize the same way.  Only ``for`` stop/step bounds stay inline (they
  re-evaluate per pass, and hoisting them would change side-effect counts) —
  a fresh string there is pathological and leaks a bounded few per trip.
* Function parameters are borrowed: a callee that wants to keep a string
  argument must copy it (``kept = "" .. param``).  The ``dimscript_*`` wrappers
  free the ``DsString`` they allocate for ``const char *`` parameters.
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
    Node,
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
    """A script type.  `element` is set for ``list`` and makes the type of
    ``list[i]`` statically known, which is what lets the C backend store bricks
    in a list without boxing them."""

    name: str
    element: Optional["DType"] = None

    @property
    def is_struct(self) -> bool:
        return self.name.startswith("struct:")

    @property
    def struct_name(self) -> str:
        return self.name.split(":", 1)[1] if self.is_struct else ""

    @property
    def is_handle(self) -> bool:
        """True for types that own a reference on the runtime heap."""
        return self.is_struct or self.name in ("string", "list")


INT = DType("int")
FLOAT = DType("float")
STRING = DType("string")
BOOL = DType("bool")
VOID = DType("void")
NIL = DType("nil")
UNKNOWN = DType("unknown")
LIST = DType("list")
RENDER = DType("namespace:render")
MATH = DType("namespace:math")
ENGINE = DType("namespace:engine")
INPUT = DType("namespace:input")
IMAGE = DType("namespace:image")
FONT = DType("namespace:font")


def list_of(element: DType) -> DType:
    return DType("list", element)


LIST_METHODS = frozenset({"push", "insert", "delete", "remove_at", "clear", "index_of", "join",
                          "count"})

NAMESPACE_TYPES = {"render": RENDER, "math": MATH, "engine": ENGINE, "input": INPUT,
                   "image": IMAGE, "font": FONT}


@dataclass(frozen=True)
class Builtin:
    """One ``namespace.name`` call — STRICT COMPILER, MANUAL MEMORY.

    ``c`` is a C template; ``{n}`` is n-th arg, already converted.
    ``strings`` are DsString* args — now BORROWED, not consumed (no ds_keep).
    ``where`` appends script location for errors.
    Speed like C, no refcount.
    """

    args: int
    returns: DType
    c: str
    strings: Tuple[int, ...] = ()
    where: bool = False
    numbers: str = "double"  # numeric cast used for the remaining arguments


BUILTINS: Dict[str, Dict[str, Builtin]] = {
    "render": {
        "color": Builtin(-1, VOID, "ds_render_color((float)({0}), (float)({1}), (float)({2}))",
                         numbers="float"),
        "color_alpha": Builtin(4, VOID,
                               "ds_render_color_alpha((float)({0}), (float)({1}), (float)({2}), (float)({3}))",
                               numbers="float"),
        "clear": Builtin(3, VOID, "ds_render_clear((float)({0}), (float)({1}), (float)({2}))",
                         numbers="float"),
        "rect": Builtin(4, VOID, "ds_render_rect({0}, {1}, {2}, {3})", numbers="float"),
        "frame": Builtin(5, VOID, "ds_render_frame({0}, {1}, {2}, {3}, {4})", numbers="float"),
        "circle": Builtin(3, VOID, "ds_render_circle({0}, {1}, {2})", numbers="float"),
        "ring": Builtin(4, VOID, "ds_render_ring({0}, {1}, {2}, {3})", numbers="float"),
        "line": Builtin(5, VOID, "ds_render_line({0}, {1}, {2}, {3}, {4})", numbers="float"),
        "tri": Builtin(6, VOID, "ds_render_triangle({0}, {1}, {2}, {3}, {4}, {5})", numbers="float"),
        "text": Builtin(4, VOID, "ds_render_text({0}, {1}, {2}, {3})", strings=(0,), numbers="float"),
        "image": Builtin(5, VOID, "ds_render_image((int32_t)({0}), {1}, {2}, {3}, {4})",
                         numbers="float"),
        "image_region": Builtin(
            9, VOID, "ds_render_image_region((int32_t)({0}), {1}, {2}, {3}, {4}, {5}, {6}, {7}, {8})",
            numbers="float"),
        "font": Builtin(1, VOID, "ds_render_font((int32_t)({0}))"),
    },
    "image": {
        "load": Builtin(1, INT, "ds_image_load({0})", strings=(0,)),
        "width": Builtin(1, INT, "ds_image_width((int32_t)({0}))"),
        "height": Builtin(1, INT, "ds_image_height((int32_t)({0}))"),
        "count": Builtin(0, INT, "ds_image_count()"),
    },
    "font": {
        "load": Builtin(1, INT, "ds_font_load({0})", strings=(0,)),
        "count": Builtin(0, INT, "ds_font_count()"),
    },
    "math": {
        "floor": Builtin(1, FLOAT, "ds_math_floor({0})"),
        "ceil": Builtin(1, FLOAT, "ds_math_ceil({0})"),
        "round": Builtin(1, FLOAT, "ds_math_round({0})"),
        "abs": Builtin(1, FLOAT, "ds_math_abs({0})"),
        "sign": Builtin(1, INT, "ds_math_sign({0})"),
        "sqrt": Builtin(1, FLOAT, "ds_math_sqrt({0})"),
        "sin": Builtin(1, FLOAT, "ds_math_sin({0})"),
        "cos": Builtin(1, FLOAT, "ds_math_cos({0})"),
        "tan": Builtin(1, FLOAT, "ds_math_tan({0})"),
        "min": Builtin(2, FLOAT, "ds_math_min({0}, {1})"),
        "max": Builtin(2, FLOAT, "ds_math_max({0}, {1})"),
        "mod": Builtin(2, FLOAT, "ds_mod({0}, {1}, {2})", where=True),
        "pow": Builtin(2, FLOAT, "ds_math_pow({0}, {1})"),
        "lerp": Builtin(3, FLOAT, "ds_math_lerp({0}, {1}, {2})"),
        "random": Builtin(0, FLOAT, "ds_engine_random()"),
        "pi": Builtin(0, FLOAT, "ds_math_pi()"),
        "e": Builtin(0, FLOAT, "ds_math_e()"),
    },
    "engine": {
        "width": Builtin(0, FLOAT, "ds_engine_width()"),
        "height": Builtin(0, FLOAT, "ds_engine_height()"),
        "time": Builtin(0, FLOAT, "ds_engine_time()"),
        "delta": Builtin(0, FLOAT, "ds_engine_delta()"),
        "fps": Builtin(0, FLOAT, "ds_engine_fps()"),
        "frame": Builtin(0, INT, "ds_engine_frame()"),
        "quit": Builtin(0, VOID, "ds_engine_quit()"),
    },
    "input": {
        "touches": Builtin(0, INT, "ds_engine_touch_count()"),
        "touch_x": Builtin(1, FLOAT, "ds_engine_touch_x((int)({0}))"),
        "touch_y": Builtin(1, FLOAT, "ds_engine_touch_y((int)({0}))"),
        "touch_down": Builtin(1, BOOL, "ds_engine_touch_down((int)({0}))"),
        "key": Builtin(1, BOOL, "ds_engine_key_down({0})", strings=(0,)),
    },
}

#: Names that read like a field (`engine.width`) but are really getters.
BUILTIN_PROPERTIES = {
    "math": {"pi": "ds_math_pi()", "e": "ds_math_e()"},
    "engine": {
        "width": "ds_engine_width()",
        "height": "ds_engine_height()",
        "time": "ds_engine_time()",
        "delta": "ds_engine_delta()",
        "fps": "ds_engine_fps()",
        "frame": "ds_engine_frame()",
    },
    "input": {"touches": "ds_engine_touch_count()"},
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


def _type_from_name(name: Optional[str], structs: Dict[str, Dict[str, DType]]) -> DType:
    if not name:
        return UNKNOWN
    if name in _TYPE_NAMES:
        return _TYPE_NAMES[name]
    if name in structs:
        return DType(f"struct:{name}")
    return DType(f"unresolved:{name}")


def _display_type(dtype: DType) -> str:
    if dtype.name == "list" and dtype.element is not None:
        return f"list[{_display_type(dtype.element)}]"
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
    if expected.name == "list" and actual.name == "list":
        if expected.element is None or actual.element is None:
            return True
        return _compatible(expected.element, actual.element)
    if expected == FLOAT and actual == INT:
        return True
    if expected.is_struct and actual == NIL:
        return True
    if expected in (STRING, LIST) and actual == NIL:
        return True
    return False


def _error(node: object, message: str) -> SemanticError:
    location = _location(node)
    return SemanticError(f"{location}: {message}")


def _location(node: object) -> str:
    line = getattr(node, "line", 0)
    column = getattr(node, "column", 0)
    return f"{line}:{column}"


def _default_param_type(function_name: str, parameter_name: str) -> DType:
    """Give untyped event parameters a predictable C ABI type."""

    lower = parameter_name.lower()
    if lower == "id" or lower.endswith("_id"):
        return INT
    if lower in {"x", "y", "dt", "time", "delta"} or lower.endswith("_x") or lower.endswith("_y"):
        return FLOAT
    if function_name.lower() == "touchpressed" and lower in {"touch_x", "touch_y"}:
        return FLOAT
    return FLOAT


def _walk(node: object) -> Iterable[Node]:
    yield node
    for value in getattr(node, "__dict__", {}).values():
        if isinstance(value, Node):
            yield from _walk(value)
        elif isinstance(value, (list, tuple)):
            for item in value:
                if isinstance(item, Node):
                    yield from _walk(item)


@dataclass
class SemanticModel:
    structs: Dict[str, Dict[str, DType]] = field(default_factory=dict)
    global_types: Dict[str, DType] = field(default_factory=dict)
    function_params: Dict[str, List[DType]] = field(default_factory=dict)
    function_returns: Dict[str, DType] = field(default_factory=dict)
    function_locals: Dict[str, Dict[str, DType]] = field(default_factory=dict)
    expression_types: Dict[int, DType] = field(default_factory=dict)
    target_types: Dict[int, DType] = field(default_factory=dict)
    #: element type of every list variable, field, parameter and return value
    list_elements: Dict[Tuple[str, str], DType] = field(default_factory=dict)
    literal_types: Dict[int, DType] = field(default_factory=dict)


# ---------------------------------------------------------------------------
# List element inference
# ---------------------------------------------------------------------------

#: Key of the "list that a function returns".
RETURN_KEY = "<return>"


class ListInference:
    """Finds what every list holds.

    DimScript lists are homogeneous: a list of bricks stays a list of bricks, so
    one pass over the program — literals, ``push``/``insert`` calls, element
    assignments, arguments and return values, iterated to a fixed point —
    decides the element type of every list variable, field, parameter and return
    value.  The C backend needs that type to store values inline (``Brick *``
    elements, not boxed cells), and a list whose element type cannot be derived
    is a compile error instead of a surprise at runtime.
    """

    def __init__(self, program: Program, structs: Dict[str, Dict[str, DType]]) -> None:
        self.program = program
        self.structs = structs
        self.elements: Dict[Tuple[str, str], DType] = {}
        self.functions = {function.name: function for function in program.functions}
        self.function_returns: Dict[str, DType] = {}

    def run(self, global_types: Dict[str, DType], function_returns: Dict[str, DType],
            function_params: Dict[str, List[DType]]) -> Dict[Tuple[str, str], DType]:
        self.function_returns = function_returns
        self.model_function_params = function_params
        for _ in range(64):
            changed = False
            for declaration in self.program.globals:
                changed |= self._element_of(declaration.value, ("", declaration.name), "", {},
                                            global_types)
            for function in self.program.functions:
                seed = {parameter.name: dtype for parameter, dtype in
                        zip(function.params, self.model_function_params[function.name])}
                changed |= self._visit_statements(function.body, function.name, seed, global_types)
            if not changed:
                break
        return self.elements

    # -- helpers ----------------------------------------------------------

    def _merge(self, key: Tuple[str, str], element: DType) -> bool:
        if element is None or element == UNKNOWN or element.name in ("nil", "void"):
            return False
        if element.name == "list" and element.element is None:
            # A bare `list` (an empty literal, an untyped value) says nothing
            # about the element type, so it never widens an inference result.
            return False
        if element.name.startswith("unresolved:"):
            return False
        current = self.elements.get(key)
        if current is None:
            self.elements[key] = element
            return True
        if current == element:
            return False
        if current == UNKNOWN:
            self.elements[key] = element
            return True
        if element == UNKNOWN:
            return False
        if _is_numeric(current) and _is_numeric(element):
            merged = FLOAT if FLOAT in (current, element) else INT
            if merged != current:
                self.elements[key] = merged
                return True
            return False
        raise SemanticError(
            f"список хранит и {_display_type(current)}, и {_display_type(element)}: "
            "элементы одного списка должны быть одного типа"
        )

    def _key_for(self, target: Expr, function: str,
                 locals_: Optional[Dict[str, DType]] = None) -> Optional[Tuple[str, str]]:
        if isinstance(target, Name):
            if target.name not in (locals_ or {}) and target.name in self.model_global_types:
                return ("", target.name)
            return (function, target.name)
        if isinstance(target, Member):
            owner = self._owner_name(target.object)
            if owner is not None:
                return (f"field:{owner}", target.name)
        return None

    def _owner_name(self, expression: Expr) -> Optional[str]:
        if isinstance(expression, Name):
            if expression.name in self.model_global_types:
                return _display_struct(self.model_global_types[expression.name])
            return expression.name
        if isinstance(expression, Member):
            parent = self._owner_name(expression.object)
            return f"{parent}.{expression.name}" if parent else None
        return None

    model_global_types: Dict[str, DType] = {}
    model_function_params: Dict[str, List[DType]] = {}

    def _type(self, expression: Expr, function: str, locals_: Dict[str, DType],
              global_types: Dict[str, DType]) -> DType:
        """The type of an expression as far as list inference needs it."""

        if isinstance(expression, Literal):
            return _TYPE_NAMES.get(expression.literal_type,
                                   NIL if expression.literal_type == "nil" else UNKNOWN)
        if isinstance(expression, Name):
            return locals_.get(expression.name, global_types.get(expression.name, UNKNOWN))
        if isinstance(expression, Member):
            owner = self._type(expression.object, function, locals_, global_types)
            if owner in (RENDER, MATH, ENGINE, INPUT, IMAGE, FONT):
                return UNKNOWN
            if owner.name == "list":
                return INT if expression.name == "count" else UNKNOWN
            if owner.is_struct:
                return self.structs.get(owner.struct_name, {}).get(expression.name, UNKNOWN)
            return UNKNOWN
        if isinstance(expression, Index):
            owner = self._type(expression.object, function, locals_, global_types)
            if owner.name == "list":
                return self._lookup_element(expression.object, function, locals_)
            return STRING if owner == STRING else UNKNOWN
        if isinstance(expression, New):
            return _type_from_name(expression.type_name, self.structs)
        if isinstance(expression, Unary):
            return self._type(expression.operand, function, locals_, global_types)
        if isinstance(expression, Binary):
            if expression.operator == "..":
                return STRING
            left = self._type(expression.left, function, locals_, global_types)
            right = self._type(expression.right, function, locals_, global_types)
            if expression.operator in ("+", "-", "*", "/", "%"):
                if FLOAT in (left, right) or expression.operator == "/":
                    return FLOAT
                return INT
            return BOOL
        if isinstance(expression, ListLiteral):
            return LIST
        if isinstance(expression, Call):
            callee = expression.callee
            if isinstance(callee, Name) and callee.name in self.function_returns:
                return self.function_returns[callee.name]
            return UNKNOWN
        return UNKNOWN

    def _lookup_element(self, expression: Expr, function: str,
                        locals_: Optional[Dict[str, DType]] = None) -> DType:
        """Element type of a list-valued expression."""

        if isinstance(expression, Name):
            key = self._key_for(expression, function, locals_)
            return self.elements.get(key, UNKNOWN) if key else UNKNOWN
        if isinstance(expression, Member):
            key = self._key_for(expression, function, locals_)
            if key:
                return self.elements.get(key, UNKNOWN)
        if isinstance(expression, Call):
            callee = expression.callee
            if isinstance(callee, Name):
                return self.elements.get((callee.name, RETURN_KEY), UNKNOWN)
        if isinstance(expression, Index):
            outer = self._lookup_element(expression.object, function, locals_)
            if outer.name == "list" and outer.element is not None:
                return outer.element
        return UNKNOWN

    def _element_of(self, expression: Expr, key: Tuple[str, str], function: str,
                    locals_: Dict[str, DType], global_types: Dict[str, DType]) -> bool:
        """Merge the element type carried by `expression` into `key`."""

        changed = False
        if isinstance(expression, ListLiteral):
            for item in expression.items:
                changed |= self._merge(key, self._type(item, function, locals_, global_types))
            return changed
        if isinstance(expression, Index):
            # `x = other[i]` where other is a list of lists
            outer = self._lookup_element(expression.object, function, locals_)
            if outer.name == "list" and outer.element is not None and outer.element.name == "list":
                changed |= self._merge(key, self._lookup_element(expression, function, locals_))
        if isinstance(expression, Name) or isinstance(expression, Member):
            source = self._lookup_element(expression, function, locals_)
            if source != UNKNOWN:
                changed |= self._merge(key, source)
        if isinstance(expression, Call):
            callee = expression.callee
            if isinstance(callee, Name) and callee.name in self.functions:
                returned = self._lookup_element(expression, function, locals_)
                if returned != UNKNOWN:
                    changed |= self._merge(key, returned)
                # arguments flow into parameters
                parameters = list(self.functions[callee.name].params)
                for parameter, argument in zip(parameters, expression.args):
                    argument_element = self._lookup_element(argument, function, locals_)
                    if argument_element != UNKNOWN:
                        changed |= self._merge((callee.name, parameter.name), argument_element)
        return changed

    def _visit_statements(self, statements: Sequence[Stmt], function: str,
                          locals_: Dict[str, DType], global_types: Dict[str, DType]) -> bool:
        changed = False
        for statement in statements:
            if isinstance(statement, Assign):
                target_key = self._key_for(statement.target, function, locals_)
                if target_key:
                    changed |= self._element_of(statement.value, target_key, function, locals_,
                                                global_types)
                if isinstance(statement.target, Name):
                    declared = self._type(statement.value, function, locals_, global_types)
                    if declared != UNKNOWN and declared.name != "nil":
                        previous = locals_.get(statement.target.name)
                        locals_[statement.target.name] = declared
                        if previous != declared:
                            changed = True
                changed |= self._visit_expression(statement.value, function, locals_, global_types)
                continue
            if isinstance(statement, ExprStmt):
                changed |= self._visit_expression(statement.expression, function, locals_,
                                                  global_types)
                continue
            if isinstance(statement, Delete):
                changed |= self._visit_expression(statement.expression, function, locals_,
                                                  global_types)
                continue
            if isinstance(statement, If):
                changed |= self._visit_expression(statement.condition, function, locals_, global_types)
                changed |= self._visit_statements(statement.then_body, function, locals_, global_types)
                changed |= self._visit_statements(statement.else_body, function, locals_, global_types)
                continue
            if isinstance(statement, While):
                changed |= self._visit_expression(statement.condition, function, locals_, global_types)
                changed |= self._visit_statements(statement.body, function, locals_, global_types)
                continue
            if isinstance(statement, For):
                locals_[statement.variable] = INT
                for part in (statement.start, statement.stop, statement.step):
                    if part is not None:
                        changed |= self._visit_expression(part, function, locals_, global_types)
                changed |= self._visit_statements(statement.body, function, locals_, global_types)
                continue
            if isinstance(statement, Return):
                if statement.expression is not None:
                    changed |= self._element_of(statement.expression, (function, RETURN_KEY),
                                                function, locals_, global_types)
                    changed |= self._visit_expression(statement.expression, function, locals_,
                                                      global_types)
                continue
        return changed

    def _visit_expression(self, expression: Expr, function: str, locals_: Dict[str, DType],
                          global_types: Dict[str, DType]) -> bool:
        changed = False
        if isinstance(expression, Call):
            callee = expression.callee
            if (isinstance(callee, Member) and isinstance(callee.object, (Name, Member)) and
                    callee.name in ("push", "insert")):
                value = expression.args[-1]
                key = self._key_for(callee.object, function, locals_)
                if key:
                    changed |= self._merge(key, self._type(value, function, locals_, global_types))
                    changed |= self._element_of(value, key, function, locals_, global_types)
            # Propagate list element types through function calls, e.g. draw_blocks(game.blocks)
            if isinstance(callee, Name) and callee.name in self.functions:
                parameters = list(self.functions[callee.name].params)
                for parameter, argument in zip(parameters, expression.args):
                    argument_element = self._lookup_element(argument, function, locals_)
                    if argument_element != UNKNOWN:
                        changed |= self._merge((callee.name, parameter.name), argument_element)
            for argument in expression.args:
                changed |= self._visit_expression(argument, function, locals_, global_types)
            return changed
        if isinstance(expression, Binary):
            changed |= self._visit_expression(expression.left, function, locals_, global_types)
            changed |= self._visit_expression(expression.right, function, locals_, global_types)
            return changed
        if isinstance(expression, Unary):
            return self._visit_expression(expression.operand, function, locals_, global_types)
        if isinstance(expression, Index):
            changed |= self._visit_expression(expression.object, function, locals_, global_types)
            changed |= self._visit_expression(expression.index, function, locals_, global_types)
            return changed
        if isinstance(expression, Member):
            return self._visit_expression(expression.object, function, locals_, global_types)
        if isinstance(expression, ListLiteral):
            for item in expression.items:
                changed |= self._visit_expression(item, function, locals_, global_types)
            return changed
        return changed


def _display_struct(dtype: DType) -> str:
    return dtype.struct_name if dtype.is_struct else "?"


class Analyzer:
    """Types every expression and records what the code generator needs."""

    #: Engine callbacks and how many parameters they receive.
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
        inference = ListInference(self.program, self.model.structs)
        inference.model_global_types = self.model.global_types
        self.model.list_elements = inference.run(self.model.global_types,
                                                 self.model.function_returns,
                                                 self.model.function_params)
        for function in self.program.functions:
            self._analyze_function(function)
        # `blocks = []` can only be typed once every push is known, so the
        # element type inferred above is attached to the literal afterwards.
        self._apply_inferred_elements()
        return self.model

    # -- collection -------------------------------------------------------

    def _collect_structs(self) -> None:
        for struct in self.program.structs:
            if struct.name in self.model.structs:
                raise _error(struct, f"структура {struct.name!r} объявлена дважды")
            fields: Dict[str, DType] = {}
            for field_decl in struct.fields:
                if field_decl.name in fields:
                    raise _error(field_decl, f"поле {field_decl.name!r} объявлено дважды")
                dtype = _type_from_name(field_decl.type_name, self.model.structs)
                if dtype.name.startswith("unresolved:"):
                    raise _error(field_decl, f"неизвестный тип {field_decl.type_name!r}")
                fields[field_decl.name] = dtype
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
            self._apply_inferred_element(declaration.value, ("", declaration.name))
            self.model.global_types[declaration.name] = dtype

    def _analyze_function(self, function: FunctionDecl) -> None:
        self._current_function = function
        self._locals = {
            parameter.name: dtype
            for parameter, dtype in zip(function.params, self.model.function_params[function.name])
        }
        for statement in function.body:
            self._analyze_statement(statement)
        self.model.function_locals[function.name] = dict(self._locals)
        self._current_function = None
        self._locals = {}

    # -- statements -------------------------------------------------------

    def _analyze_statement(self, statement: Stmt) -> None:
        if isinstance(statement, Assign):
            value_type = self._expression_type(statement.value, self._locals)
            target_type = self._target_type(statement.target, value_type)
            self._apply_inferred_element(statement.value, self._target_key(statement.target))
            if not _compatible(target_type, value_type):
                raise _error(statement, f"нельзя присвоить {_display_type(value_type)} переменной типа "
                                        f"{_display_type(target_type)}")
            self.model.target_types[id(statement.target)] = target_type
            return
        if isinstance(statement, ExprStmt):
            self._expression_type(statement.expression, self._locals)
            return
        if isinstance(statement, Delete):
            dtype = self._expression_type(statement.expression, self._locals)
            if not dtype.is_handle or isinstance(statement.expression, Index):
                raise _error(statement, "delete применим к переменной или полю объекта")
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
                raise _error(statement, f"функция возвращает {_display_type(expected)}, а получено "
                                        f"{_display_type(actual)}")
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
            self._analyze_loop_body(statement.body)
            return
        if isinstance(statement, (Break, Continue)):
            if not self._loop_depth:
                raise _error(statement, "break/continue возможны только внутри while или for")
            return
        raise _error(statement, "неизвестная конструкция")

    def _analyze_loop_body(self, body: List[Stmt]) -> None:
        self._loop_depth += 1
        for child in body:
            self._analyze_statement(child)
        self._loop_depth -= 1

    def _target_type(self, target: Expr, value_type: DType) -> DType:
        if isinstance(target, Name):
            if target.name in self._locals:
                return self._locals[target.name]
            if target.name in self.model.global_types:
                return self.model.global_types[target.name]
            # Assignment introduces a local variable.
            self._locals[target.name] = value_type
            return value_type
        if isinstance(target, (Member, Index)):
            return self._expression_type(target, self._locals)
        raise _error(target, "слева от '=' должно быть имя, поле объекта или элемент списка")

    # -- expressions ------------------------------------------------------

    def _element_type(self, name: str, object_expression: Optional[Expr] = None) -> DType:
        """Element type of a list variable, field or return value."""

        function = self._current_function.name if self._current_function else ""
        if isinstance(object_expression, Member):
            owner = self._owner_name(object_expression.object)
            key = (f"field:{owner}", object_expression.name) if owner else None
        else:
            key = (function, name) if name else None
        if key is None:
            return UNKNOWN
        return self.model.list_elements.get(key, UNKNOWN)

    def _owner_name(self, expression: Expr) -> Optional[str]:
        if isinstance(expression, Name):
            dtype = self.model.global_types.get(expression.name) or self._locals.get(expression.name)
            if dtype is None:
                return expression.name
            return _display_struct(dtype) if dtype.is_struct else "?"
        if isinstance(expression, Member):
            parent = self._owner_name(expression.object)
            return f"{parent}.{expression.name}" if parent else None
        return None

    def _expression_type(self, expression: Expr, locals_: Dict[str, DType]) -> DType:
        cached = self.model.expression_types.get(id(expression))
        if cached is not None:
            return cached

        dtype = UNKNOWN
        if isinstance(expression, Literal):
            dtype = _TYPE_NAMES.get(expression.literal_type,
                                    NIL if expression.literal_type == "nil" else UNKNOWN)
            if expression.literal_type == "list":
                dtype = LIST
        elif isinstance(expression, Name):
            if expression.name in locals_:
                dtype = locals_[expression.name]
            elif expression.name in self.model.global_types:
                dtype = self.model.global_types[expression.name]
            elif expression.name in NAMESPACE_TYPES:
                dtype = NAMESPACE_TYPES[expression.name]
            elif expression.name in self.functions:
                dtype = DType(f"function:{expression.name}")
            else:
                raise _error(expression, f"неизвестное имя {expression.name!r}")
        elif isinstance(expression, Member):
            dtype = self._member_type(expression, locals_)
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
            dtype = self._binary_type(expression, locals_)
        elif isinstance(expression, Index):
            object_type = self._expression_type(expression.object, locals_)
            self._expression_type(expression.index, locals_)
            if object_type == STRING:
                dtype = STRING
            elif object_type.name == "list":
                element = self._element_for(expression.object)
                if element == UNKNOWN:
                    raise _error(expression,
                                 "не удалось вывести тип элементов списка: добавьте элемент через "
                                 "push(...) или задайте список литералом [..]")
                dtype = element
            else:
                raise _error(expression, f"индекс применим к списку или строке, а не к "
                                         f"{_display_type(object_type)}")
        elif isinstance(expression, ListLiteral):
            element = UNKNOWN
            for item in expression.items:
                item_type = self._expression_type(item, locals_)
                if element == UNKNOWN:
                    element = item_type
                elif item_type != UNKNOWN and not _compatible(element, item_type):
                    raise _error(item, f"элементы списка должны быть одного типа: {_display_type(element)} "
                                       f"и {_display_type(item_type)}")
            dtype = list_of(element) if element != UNKNOWN else LIST
        elif isinstance(expression, Call):
            dtype = self._call_type(expression, locals_)
        else:
            raise _error(expression, "неизвестное выражение")

        # A list whose elements were inferred carries them in its type: that is
        # what lets the C backend store bricks in the list without boxing.
        if dtype.name == "list" and dtype.element is None:
            element = self._element_for(expression)
            if element != UNKNOWN:
                dtype = list_of(element)
        self.model.expression_types[id(expression)] = dtype
        return dtype

    def _apply_inferred_elements(self) -> None:
        for declaration in self.program.globals:
            self._apply_inferred_element(declaration.value, ("", declaration.name))
        for function in self.program.functions:
            self._current_function = function
            self._locals = dict(self.model.function_locals.get(function.name, {}))
            self._apply_in_statements(function.body)
            self._current_function = None
            self._locals = {}

    def _apply_in_statements(self, statements: Sequence[Stmt]) -> None:
        for statement in statements:
            if isinstance(statement, Assign):
                self._apply_inferred_element(statement.value, self._target_key(statement.target))
                continue
            if isinstance(statement, If):
                self._apply_in_statements(statement.then_body)
                self._apply_in_statements(statement.else_body)
                continue
            if isinstance(statement, While):
                self._apply_in_statements(statement.body)
                continue
            if isinstance(statement, For):
                self._apply_in_statements(statement.body)

    def _target_key(self, target: Expr) -> Optional[Tuple[str, str]]:
        """Where the element type of an assignment target lives."""

        function = self._current_function.name if self._current_function else ""
        if isinstance(target, Name):
            if target.name in self._locals:
                return (function, target.name)
            if target.name in self.model.global_types:
                return ("", target.name)
            return None
        if isinstance(target, Member):
            owner = self._owner_name(target.object)
            return (f"field:{owner}", target.name) if owner else None
        return None

    def _apply_inferred_element(self, value: Expr, key: Optional[Tuple[str, str]]) -> None:
        """Give `[]` the element type its variable was inferred to hold."""

        if key is None or not isinstance(value, ListLiteral) or value.items:
            return
        element = self.model.list_elements.get(key)
        if element is None or element == UNKNOWN:
            return
        self.model.expression_types[id(value)] = list_of(element)

    def _element_for(self, expression: Expr) -> DType:
        """Element type behind a list-valued expression."""

        function = self._current_function.name if self._current_function else ""
        if isinstance(expression, Name):
            if expression.name in self._locals:
                key = (function, expression.name)
            elif expression.name in self.model.global_types:
                key = ("", expression.name)
            else:
                return UNKNOWN
            return self.model.list_elements.get(key, UNKNOWN)
        if isinstance(expression, Member):
            owner = self._owner_name(expression.object)
            return self.model.list_elements.get((f"field:{owner}", expression.name), UNKNOWN) if owner else UNKNOWN
        if isinstance(expression, Call):
            callee = expression.callee
            if isinstance(callee, Name):
                return self.model.list_elements.get((callee.name, RETURN_KEY), UNKNOWN)
        if isinstance(expression, Index):
            outer = self._element_for(expression.object)
            if outer.name == "list" and outer.element is not None:
                return outer.element
        if isinstance(expression, ListLiteral) and expression.items:
            return self._expression_type(expression.items[0], self._locals)
        return UNKNOWN

    def _member_type(self, expression: Member, locals_: Dict[str, DType]) -> DType:
        object_type = self._expression_type(expression.object, locals_)
        if object_type in (RENDER, MATH, ENGINE, INPUT, IMAGE, FONT):
            namespace = object_type.name.split(":", 1)[1]
            properties = BUILTIN_PROPERTIES.get(namespace, {})
            if expression.name in properties:
                return FLOAT
            if expression.name in BUILTINS.get(namespace, {}):
                return DType(f"builtin:{namespace}.{expression.name}")
            raise _error(expression, f"у {namespace} нет свойства {expression.name!r}")
        if object_type.name == "list":
            if expression.name == "count":
                return INT
            if expression.name in LIST_METHODS:
                return DType(f"builtin:list.{expression.name}")
            raise _error(expression, f"у списка нет свойства {expression.name!r}")
        if object_type == UNKNOWN:
            return UNKNOWN
        if object_type == STRING:
            raise _error(expression, f"строка не имеет поля {expression.name!r}")
        if object_type.is_struct:
            fields = self.model.structs.get(object_type.struct_name, {})
            if expression.name not in fields:
                raise _error(expression, f"у {object_type.struct_name} нет поля {expression.name!r}")
            return fields[expression.name]
        raise _error(expression, f"тип {_display_type(object_type)} не имеет полей")

    def _binary_type(self, expression: Binary, locals_: Dict[str, DType]) -> DType:
        left = self._expression_type(expression.left, locals_)
        right = self._expression_type(expression.right, locals_)
        operator = expression.operator
        if operator == "..":
            return STRING
        if operator in ("+", "-", "*", "/", "%"):
            if not _is_numeric(left) or not _is_numeric(right):
                raise _error(expression, f"оператор {operator} работает только с числами")
            return FLOAT if FLOAT in (left, right) or operator == "/" else INT
        if operator in ("==", "!=", "~=", ">", "<", ">=", "<="):
            return BOOL
        if operator in ("and", "or", "&&", "||"):
            return BOOL
        raise _error(expression, f"неизвестный оператор {operator!r}")

    def _call_type(self, expression: Call, locals_: Dict[str, DType]) -> DType:
        callee = expression.callee
        if isinstance(callee, Member) and isinstance(callee.object, Name) and callee.object.name in NAMESPACE_TYPES:
            namespace = callee.object.name
            table = BUILTINS.get(namespace, {})
            if callee.name not in table:
                raise _error(callee, f"нет функции {namespace}.{callee.name}")
            builtin = table[callee.name]
            if builtin.args < 0:
                if not 3 <= len(expression.args) <= 4:
                    raise _error(expression, f"{namespace}.{callee.name} ожидает 3 или 4 аргумента")
            elif len(expression.args) != builtin.args:
                plural = "" if builtin.args == 1 else "а"
                raise _error(expression, f"{namespace}.{callee.name} ожидает {builtin.args} аргумент{plural}")
            for index, argument in enumerate(expression.args):
                dtype = self._expression_type(argument, locals_)
                if index in builtin.strings:
                    if dtype not in (STRING, UNKNOWN):
                        raise _error(argument, f"аргумент {namespace}.{callee.name} должен быть строкой")
                elif not _is_numeric(dtype) and dtype != UNKNOWN:
                    raise _error(argument, f"аргумент {namespace}.{callee.name} должен быть числом")
            return builtin.returns
        if isinstance(callee, Member) and isinstance(callee.object, (Name, Member, Index)):
            owner = self._expression_type(callee.object, locals_)
            if owner.name == "list":
                return self._list_call_type(expression, callee, owner, locals_)
        if isinstance(callee, Name) and callee.name in self.functions:
            params = self.model.function_params[callee.name]
            if len(params) != len(expression.args):
                raise _error(expression, f"{callee.name} ожидает {len(params)} аргументов")
            for expected, argument in zip(params, expression.args):
                actual = self._expression_type(argument, locals_)
                if not _compatible(expected, actual):
                    raise _error(argument, f"ожидался {_display_type(expected)}, получено "
                                           f"{_display_type(actual)}")
            return self.model.function_returns[callee.name]
        if isinstance(callee, Name) and callee.name in {"print", "log", "str", "len", "num"}:
            if callee.name in {"str", "len", "num"} and len(expression.args) != 1:
                raise _error(expression, f"{callee.name} ожидает 1 аргумент")
            if callee.name in {"print", "log"} and not expression.args:
                raise _error(expression, f"{callee.name} ожидает аргументы")
            for argument in expression.args:
                self._expression_type(argument, locals_)
            if callee.name == "str":
                return STRING
            if callee.name == "num":
                return FLOAT
            if callee.name == "len":
                dtype = self._expression_type(expression.args[0], locals_)
                if dtype != STRING and dtype.name != "list":
                    raise _error(expression.args[0], "len работает со строкой или списком")
                return INT
            return VOID
        raise _error(callee, "вызвать можно функцию скрипта или builtin (render./math./engine./input./image./font.)")

    def _list_call_type(self, expression: Call, callee: Member, owner: DType,
                        locals_: Dict[str, DType]) -> DType:
        element = self._element_for(callee.object)
        name = callee.name
        if name in ("push", "insert"):
            expected = 1 if name == "push" else 2
            if len(expression.args) != expected:
                raise _error(expression, f"list.{name} ожидает {expected} аргумент(а)")
            value = expression.args[-1]
            value_type = self._expression_type(value, locals_)
            if element == UNKNOWN:
                raise _error(value, "тип элементов списка не выведен: добавьте элемент литералом [..] "
                                    "или через push(...)")
            if not _compatible(element, value_type):
                raise _error(value, f"список хранит {_display_type(element)}, а добавлено "
                                    f"{_display_type(value_type)}")
            if name == "insert":
                index_type = self._expression_type(expression.args[0], locals_)
                if not _is_numeric(index_type):
                    raise _error(expression.args[0], "индекс должен быть числом")
            return VOID
        if name in ("delete", "remove_at"):
            if len(expression.args) != 1:
                raise _error(expression, "list.delete ожидает индекс")
            index_type = self._expression_type(expression.args[0], locals_)
            if not _is_numeric(index_type):
                raise _error(expression.args[0], "индекс должен быть числом")
            return VOID
        if name == "clear":
            if expression.args:
                raise _error(expression, "list.clear не принимает аргументов")
            return VOID
        if name == "index_of":
            if len(expression.args) != 1:
                raise _error(expression, "list.index_of ожидает значение")
            value_type = self._expression_type(expression.args[0], locals_)
            if element != UNKNOWN and not _compatible(element, value_type):
                raise _error(expression.args[0], f"список хранит {_display_type(element)}")
            return INT
        if name == "join":
            if len(expression.args) != 1:
                raise _error(expression, "list.join ожидает разделитель")
            separator = self._expression_type(expression.args[0], locals_)
            if separator not in (STRING, UNKNOWN):
                raise _error(expression.args[0], "разделитель должен быть строкой")
            if element != UNKNOWN and element.is_handle and element.name == "list":
                raise _error(expression, "список списков нельзя объединить в строку")
            return STRING
        raise _error(callee, f"у списка нет метода {name!r}")


# ---------------------------------------------------------------------------
# C code generator
# ---------------------------------------------------------------------------


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


def _c_string(value: str) -> str:
    """A C string literal for the bytes of a script string."""

    return json.dumps(value, ensure_ascii=False)


def _strip_outer_parentheses(text: str) -> Optional[str]:
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


class CCompiler:
    """Turns a merged program into one C99 translation unit."""

    #: name -> C parameter types of the `dimscript_*` wrapper the host calls.
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
        self.structs_by_name = {struct.name: struct for struct in program.structs}
        self.global_c_names = {name: f"g_{_sanitize(name)}" for name in self.model.global_types}
        self.lines: List[str] = []
        self.indent_level = 0
        self.current_function: Optional[FunctionDecl] = None
        self.fresh_expression: Optional[int] = None
        self.literals: Dict[str, int] = {}
        self.literal_order: List[str] = []
        self.temp_frames: List[List[Tuple[str, str]]] = []
        self.temp_counter = 0
        self.temps_enabled = True

    # -- output helpers ---------------------------------------------------

    def emit(self, text: str) -> None:
        if text:
            self.lines.append("    " * self.indent_level + text)
        else:
            self.lines.append("")

    # -- temporary string frames ------------------------------------------
    #
    # A fresh string consumed in place (an operand of `..`, an argument of
    # `log`/`num`/`len`, a `==` operand, a builtin string argument) is bound to
    # a `__ds_tN` variable declared just above the statement and released with
    # LIFO `ds_release` right below it.  Stored values, `return` results, list
    # elements and function arguments never enter a frame.

    def temp_push(self) -> None:
        """Open a frame collecting fresh strings consumed by one statement."""
        self.temp_frames.append([])

    def temp_pop(self) -> List[Tuple[str, str]]:
        """Close the current frame and return its (variable, code) pairs."""
        return self.temp_frames.pop()

    def emit_temp_frame(self, frame: List[Tuple[str, str]]) -> None:
        for variable, code in frame:
            self.emit(f"DsString *{variable} = {code};")

    def emit_temp_release(self, frame: List[Tuple[str, str]]) -> None:
        for variable, _ in reversed(frame):
            self.emit(f"ds_release((void*){variable});")

    def emit_line_with_temps(self, line: str) -> None:
        """Emit one statement, freeing the fresh strings it consumed in place."""
        frame = self.temp_pop()
        self.emit_temp_frame(frame)
        self.emit(line)
        self.emit_temp_release(frame)

    def build_check(self, expression: Expr) -> Tuple[str, List[Tuple[str, str]]]:
        """Build a condition, returning its code plus the temps it consumed.

        The caller materializes the check (``int __ds_cN = (cond);``) and frees
        the frame before the branches run, so every iteration pays for exactly
        the strings it built: an `if` frees before the body, a `while` frees
        on every pass through a desugared loop.  An empty frame keeps the
        plain `if (cond)` / `while (cond)` shape.
        """
        self.temp_push()
        code = self.truthy(expression)
        return code, self.temp_pop()

    def alloc_scratch(self) -> str:
        """A unique `__ds_cN` name for a materialized check or loop bound."""
        variable = f"__ds_c{self.temp_counter}"
        self.temp_counter += 1
        return variable

    def emit_check(self, variable: str, code: str, frame: List[Tuple[str, str]]) -> None:
        """Emit a materialized check: temps, the cached value, the release."""
        self.emit_temp_frame(frame)
        self.emit(f"int {variable} = ({code});")
        self.emit_temp_release(frame)

    def compile(self) -> str:
        self.emit("/* Generated by DimScript (tools/aot.py). Do not edit by hand. */")
        self.emit(f"/* Source: {self.filename} */")
        self.emit('#include "dimscript_runtime.h"')
        self.emit("#include <string.h>")
        self.emit("")
        self.emit_structs()
        self.emit_struct_dtors()
        self.emit_globals()
        self.emit("")
        self.emit("static int ds_program_initialized;")
        self.emit("")
        # The literal pool is emitted last: the initialisers generated below are
        # what tell us which literals the program actually uses.
        literal_slot = len(self.lines)
        self.emit("")
        self.emit_function_declarations()
        for function in self.program.functions:
            self.emit_user_function(function)
            self.emit("")
        self.emit_lifecycle()
        self.lines[literal_slot:literal_slot + 1] = self.literal_lines()
        return "\n".join(self.lines).rstrip() + "\n"

    def header(self, guard: str = "DIMSCRIPT_GAME_H") -> str:
        name = _sanitize(guard).upper()
        lines = [
            "/* Generated by DimScript (tools/aot.py). */",
            f"#ifndef {name}",
            f"#define {name}",
            '#include "dimscript_runtime.h"',
            "#ifdef __cplusplus",
            'extern "C" {',
            "#endif",
            "void dimscript_init(void);",
            "void dimscript_shutdown(void);",
            *[f"void dimscript_{callback}({params});" for callback, params in self.WRAPPER_PARAMS.items()],
            "#ifdef __cplusplus",
            "}",
            "#endif",
            f"#endif /* {name} */",
            "",
        ]
        return "\n".join(lines)

    # -- declaration helpers ----------------------------------------------

    def c_type(self, dtype: DType) -> str:
        if dtype == INT:
            return "int32_t"
        if dtype == FLOAT:
            return "float"
        if dtype == BOOL:
            return "int32_t"
        if dtype == STRING:
            return "DsString *"
        if dtype.name == "list":
            return "DsList *"
        if dtype.is_struct:
            return f"{self.struct_names.get(dtype.struct_name, _sanitize(dtype.struct_name))} *"
        if dtype == VOID:
            return "void"
        return "void *"

    def default_value(self, dtype: DType) -> str:
        if dtype == FLOAT:
            return "0.0f"
        if dtype in (INT, BOOL):
            return "0"
        return "NULL"

    def require_element(self, element: Optional[DType], node: object) -> DType:
        if element is None or element == UNKNOWN:
            raise SemanticError(
                f"{getattr(node, 'file', self.filename)}:{getattr(node, 'line', 0)}: "
                "тип элементов списка не выведен: добавьте элемент через push(...) или задайте "
                "список литералом [..]")
        return element

    def element_kind(self, element: DType) -> str:
        if element == FLOAT or element == NIL:
            return "DS_ELEM_FLOAT"
        if element == INT:
            return "DS_ELEM_INT"
        if element == BOOL:
            return "DS_ELEM_BOOL"
        if element == STRING:
            return "DS_ELEM_STRING"
        if element.name == "list":
            return "DS_ELEM_LIST"
        if element.is_struct or element == UNKNOWN:
            return "DS_ELEM_OBJECT"
        return "DS_ELEM_OBJECT"

    def element_size(self, element: DType) -> str:
        if element.is_struct:
            return f"sizeof({self.struct_names[element.struct_name]})"
        return "0"

    def element_dtor(self, element: DType) -> str:
        if element == STRING:
            return "ds_string_dtor"
        if not element.is_struct:
            return "NULL"
        struct = self.structs_by_name[element.struct_name]
        return f"ds_dtor_{self.struct_names[struct.name]}" if self.struct_has_handles(struct) else "NULL"

    def where(self, node: object) -> str:
        filename = getattr(node, "file", None) or self.filename
        return f'"{filename}:{getattr(node, "line", 0)}"'

    # -- literals ---------------------------------------------------------

    def literal(self, text: str) -> str:
        index = self.literals.get(text)
        if index is None:
            index = len(self.literal_order)
            self.literals[text] = index
            self.literal_order.append(text)
        return f"ds_literals[{index}]"

    def literal_lines(self) -> List[str]:
        lines = [f"/* {len(self.literal_order)} script literal(s), interned once at load time. */",
                 "static const DsLiteralSource ds_literal_sources[] = {"]
        for text in self.literal_order:
            length = len(text.encode("utf-8"))
            lines.append(f"    {{{_c_string(text)}, {length}}},")
        if not self.literal_order:
            lines.append("    {NULL, 0},")
        lines.append("};")
        lines.append("static DsString **ds_literals;")
        return lines

    # -- structs ----------------------------------------------------------

    def struct_has_handles(self, struct) -> bool:
        return any(_type_from_name(field.type_name, self.model.structs).is_handle
                   for field in struct.fields)

    def emit_structs(self) -> None:
        for struct in self.program.structs:
            self.emit(f"typedef struct {self.struct_names[struct.name]} {{")
            self.indent_level += 1
            for field_decl in struct.fields:
                dtype = _type_from_name(field_decl.type_name, self.model.structs)
                self.emit(f"{self.c_type(dtype)} {_sanitize(field_decl.name)};")
            self.indent_level -= 1
            self.emit(f"}} {self.struct_names[struct.name]};")
        self.emit("")

    def emit_struct_dtors(self) -> None:
        """Manual memory: dtor frees owned handle fields via ds_release (free+dtor). No refcount."""

        for struct in self.program.structs:
            handle_fields = [field for field in struct.fields
                             if _type_from_name(field.type_name, self.model.structs).is_handle]
            name = self.struct_names[struct.name]
            if not handle_fields:
                self.emit(f"/* {name}: only plain values, no dtor. */")
                continue
            self.emit(f"static void ds_dtor_{name}(void *value) {{")
            self.indent_level += 1
            self.emit(f"{name} *self = ({name} *)value;")
            for field_decl in handle_fields:
                # manual free
                self.emit(f"if (self->{_sanitize(field_decl.name)}) ds_release((void*)self->{_sanitize(field_decl.name)});")
            self.indent_level -= 1
            self.emit("}")
        self.emit("")

    def emit_globals(self) -> None:
        for declaration in self.program.globals:
            dtype = self.model.global_types[declaration.name]
            self.emit(f"static {self.c_type(dtype)} {self.global_c_names[declaration.name]} = "
                      f"{self.default_value(dtype)};")

    def emit_function_declarations(self) -> None:
        for function in self.program.functions:
            params = self.model.function_params[function.name]
            parameter_text = ", ".join(
                f"{self.c_type(dtype)} {_sanitize(parameter.name)}"
                for parameter, dtype in zip(function.params, params)) or "void"
            # A game may intentionally keep helper functions that are only used
            # by one optional screen.  Keep -Werror enabled for the native build
            # without making those valid, generated helpers a build failure.
            self.emit(f"static {self.c_type(self.model.function_returns[function.name])} "
                      f"ds_fn_{_sanitize(function.name)}({parameter_text}) __attribute__((unused));")
        self.emit("")

    # -- lifecycle --------------------------------------------------------

    def emit_lifecycle(self) -> None:
        self.temp_counter = 0
        self.emit("void dimscript_init(void) {")
        self.indent_level += 1
        self.emit("if (ds_program_initialized) return;")
        # The count is derived from the pool array itself, not frozen here:
        # the global initialisers below register their own literals, so any
        # len() taken now would undercount and ds_literals[N] would read out
        # of bounds (an empty pool still emits one {NULL, 0} row, hence the
        # division stays exact in every case).
        self.emit("ds_literals = ds_intern_literals(ds_literal_sources, (int)(sizeof(ds_literal_sources) / "
                  "sizeof(ds_literal_sources[0])));")
        for declaration in self.program.globals:
            self.emit_assignment(self.global_c_names[declaration.name], declaration.value,
                                 self.model.global_types[declaration.name])
        self.emit("ds_program_initialized = 1;")
        self.indent_level -= 1
        self.emit("}")
        self.emit("")
        self.emit("void dimscript_shutdown(void) {")
        self.indent_level += 1
        self.emit("if (!ds_program_initialized) return;")
        # manual: free globals if needed
        for declaration in self.program.globals:
            dtype = self.model.global_types[declaration.name]
            if dtype.is_handle:
                self.emit(f"if ({self.global_c_names[declaration.name]}) ds_release((void*){self.global_c_names[declaration.name]});")
                self.emit(f"{self.global_c_names[declaration.name]} = {self.default_value(dtype)};")
        self.emit("ds_program_initialized = 0;")
        self.indent_level -= 1
        self.emit("}")
        self.emit("")
        for callback, declaration in self.WRAPPER_PARAMS.items():
            function = self.functions.get(callback)
            self.emit(f"void dimscript_{callback}({declaration}) {{")
            self.indent_level += 1
            if function is None:
                for parameter in _parameter_names(declaration):
                    self.emit(f"(void){parameter};")
            else:
                self.emit("if (!ds_program_initialized) return;")
                # MANUAL MEMORY: convert const char* -> DsString* when needed, speed like C
                args = []
                converted: List[str] = []
                for param, dtype in zip(function.params, self.model.function_params[function.name]):
                    cname = _sanitize(param.name)
                    if dtype == STRING:
                        # wrapper has const char* name, convert to DsString*
                        self.emit(f"DsString *{cname}_s = ds_string_new({cname}, {cname} ? strlen({cname}) : 0);")
                        args.append(f"{cname}_s")
                        converted.append(f"{cname}_s")
                    else:
                        args.append(cname)
                self.emit(f"ds_fn_{_sanitize(callback)}({', '.join(args)});")
                # Parameters are borrowed: the callee must copy a string it
                # wants to keep, and the wrapper frees its conversions.
                for cname in converted:
                    self.emit(f"ds_release((void*){cname});")
            self.indent_level -= 1
            self.emit("}")
        self.emit("")

    # -- functions --------------------------------------------------------

    def owned_locals(self, function: FunctionDecl) -> List[str]:
        """STRICT COMPILER: no auto releases, manual memory like C."""
        return []

    def emit_releases(self, function: FunctionDecl) -> None:
        """Release owned local strings before leaving a function.

        Lists and structs are identity values whose ownership is explicit in
        the script. String locals are different: every assignment duplicates
        into the local slot, so the generated function owns that slot and must
        release it on every return path. This is still manual memory — there
        is no per-frame refcount or garbage collector — but it prevents a HUD
        string assembled every frame from leaking forever.
        """
        parameters = {parameter.name for parameter in function.params}
        for name, dtype in self.model.function_locals.get(function.name, {}).items():
            if name in parameters or dtype != STRING:
                continue
            self.emit(f"ds_release_slot((void **)&{_sanitize(name)});")

    def emit_user_function(self, function: FunctionDecl) -> None:
        self.current_function = function
        self.temp_counter = 0
        params = self.model.function_params[function.name]
        return_type = self.model.function_returns[function.name]
        parameter_text = ", ".join(
            f"{self.c_type(dtype)} {_sanitize(parameter.name)}"
            for parameter, dtype in zip(function.params, params)) or "void"
        self.emit(f"static __attribute__((unused)) {self.c_type(return_type)} "
                  f"ds_fn_{_sanitize(function.name)}({parameter_text}) {{")
        self.indent_level += 1
        for parameter in function.params:
            self.emit(f"(void){_sanitize(parameter.name)};")
        parameter_names = {_sanitize(parameter.name) for parameter in function.params}
        local_types = self.model.function_locals.get(function.name, {})
        for name, dtype in local_types.items():
            if name in parameter_names:
                continue
            self.emit(f"{self.c_type(dtype)} {_sanitize(name)} = {self.default_value(dtype)};")
        for statement in function.body:
            if isinstance(statement, For) and statement.variable not in local_types:
                self.emit(f"int32_t {_sanitize(statement.variable)} = 0;")
        for statement in function.body:
            self.emit_statement(statement)
        self.emit_releases(function)
        self.emit(self.return_statement(return_type, None))
        self.indent_level -= 1
        self.emit("}")
        self.current_function = None

    def default_return(self, dtype: DType) -> str:
        if dtype == VOID:
            return ""
        return self.default_value(dtype)

    def return_statement(self, dtype: DType, value: Optional[str]) -> str:
        if dtype == VOID:
            return "return;"
        return f"return {value if value is not None else self.default_value(dtype)};"

    # -- statements -------------------------------------------------------

    def emit_statement(self, statement: Stmt) -> None:
        if isinstance(statement, Assign):
            self.emit_assignment_target(statement.target, statement.value)
            return
        if isinstance(statement, ExprStmt):
            self.temp_push()
            line = f"{self.value(statement.expression)[0]};"
            self.emit_line_with_temps(line)
            return
        if isinstance(statement, Delete):
            self.emit_delete(statement.expression)
            return
        if isinstance(statement, If):
            code, frame = self.build_check(statement.condition)
            if frame:
                scratch = self.alloc_scratch()
                self.emit_check(scratch, code, frame)
                self.emit(f"if ({scratch}) {{")
            else:
                self.emit(f"if ({code}) {{")
            self.indent_level += 1
            for child in statement.then_body:
                self.emit_statement(child)
            self.indent_level -= 1
            if statement.else_body:
                nested = statement.else_body[0] if len(statement.else_body) == 1 else None
                if isinstance(nested, If):
                    code, frame = self.build_check(nested.condition)
                    if frame:
                        # Desugared: the check runs inside the else block and
                        # frees before the nested if; the "}" below then closes
                        # this else block, whatever the chain opened inside.
                        scratch = self.alloc_scratch()
                        self.emit("} else {")
                        self.indent_level += 1
                        self.emit_check(scratch, code, frame)
                        self.emit(f"if ({scratch}) {{")
                        self.indent_level += 1
                        for child in nested.then_body:
                            self.emit_statement(child)
                        self.indent_level -= 1
                        self._emit_else_chain(nested)
                        self.emit("}")
                        self.indent_level -= 1
                    else:
                        self.emit(f"}} else if ({code}) {{")
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
            return
        if isinstance(statement, While):
            code, frame = self.build_check(statement.condition)
            if frame:
                # Desugared: the check runs on top of every pass and frees
                # before the body, so each iteration pays for its own strings.
                # A body's break/continue keeps working: both land back on the
                # check, exactly like a plain while.
                scratch = self.alloc_scratch()
                self.emit("for (;;) {")
                self.indent_level += 1
                self.emit_check(scratch, code, frame)
                self.emit(f"if (!{scratch}) break;")
                for child in statement.body:
                    self.emit_statement(child)
                self.indent_level -= 1
                self.emit("}")
            else:
                self.emit(f"while ({code}) {{")
                self.indent_level += 1
                for child in statement.body:
                    self.emit_statement(child)
                self.indent_level -= 1
                self.emit("}")
            return
        if isinstance(statement, For):
            variable = _sanitize(statement.variable)
            # The start bound runs once, so its temps materialize eagerly; the
            # stop/step bounds re-evaluate every iteration and stay inline —
            # hoisting them would either leak across passes or change how many
            # times a bound with side effects runs.  A fresh string in a
            # numeric bound means num()/len() of a fresh value, which is
            # pathological; this is the one place the compiler knowingly
            # leaves such values inline (bounded by the trip count).
            self.temp_push()
            start = self.value(statement.start)[0]
            start_frame = self.temp_pop()
            enabled, self.temps_enabled = self.temps_enabled, False
            try:
                stop = self.value(statement.stop)[0]
                step = self.value(statement.step)[0] if statement.step is not None else "1"
            finally:
                self.temps_enabled = enabled
            if start_frame:
                scratch = self.alloc_scratch()
                self.emit_temp_frame(start_frame)
                self.emit(f"int32_t {scratch} = (int32_t)({start});")
                self.emit_temp_release(start_frame)
                start = scratch
            self.emit(f"for ({variable} = (int32_t)({start}); {variable} <= (int32_t)({stop}); "
                      f"{variable} += (int32_t)({step})) {{")
            self.indent_level += 1
            for child in statement.body:
                self.emit_statement(child)
            self.indent_level -= 1
            self.emit("}")
            return
        if isinstance(statement, Break):
            self.emit("break;")
            return
        if isinstance(statement, Continue):
            self.emit("continue;")
            return
        if isinstance(statement, Return):
            function = self.current_function
            assert function is not None
            return_type = self.model.function_returns[function.name]
            if statement.expression is None:
                self.emit_releases(function)
                self.emit(self.return_statement(return_type, None))
                return
            self.temp_push()
            code, _fresh = self.value(statement.expression)
            frame = self.temp_pop()
            if return_type == STRING:
                # String returns are always owned: the caller frees or stores
                # them.  A fresh result is materialized into a temp so the
                # original joins the frame (declared last, released first —
                # its operands are the earlier temps), and the duplicate is
                # what the caller receives.  A borrowed one (param, global,
                # literal, member) is duplicated as-is.
                if self.is_fresh_owned(statement.expression):
                    dup = self.alloc_scratch()
                    original = f"__ds_t{self.temp_counter}"
                    self.temp_counter += 1
                    frame.append((original, code))
                    self.emit_temp_frame(frame)
                    self.emit(f"DsString *{dup} = ds_string_dup({original});")
                    self.emit_temp_release(frame)
                    self.emit_releases(function)
                    self.emit(f"return {dup};")
                else:
                    self.emit_temp_frame(frame)
                    self.emit_temp_release(frame)
                    self.emit_releases(function)
                    self.emit(f"return ds_string_dup({code});")
            elif return_type == VOID:
                # `return f()` with a void call: run it, free its temps, leave.
                self.emit_temp_frame(frame)
                if frame:
                    self.emit(f"{code};")
                    self.emit_temp_release(frame)
                self.emit_releases(function)
                self.emit("return;")
            elif frame:
                # The value must survive its own temps: materialize, free, hand
                # over.  (Releasing first would use the temps after freeing.)
                scratch = self.alloc_scratch()
                self.emit_temp_frame(frame)
                self.emit(f"{self.c_type(return_type)} {scratch} = ({code});")
                self.emit_temp_release(frame)
                self.emit_releases(function)
                self.emit(f"return {scratch};")
            else:
                self.emit_releases(function)
                self.emit(f"return {code};")
            return
        raise TypeError(f"unsupported statement {statement!r}")

    def _emit_else_chain(self, statement: If) -> None:
        if not statement.else_body:
            return
        nested = statement.else_body[0] if len(statement.else_body) == 1 else None
        if isinstance(nested, If):
            code, frame = self.build_check(nested.condition)
            if frame:
                scratch = self.alloc_scratch()
                self.emit("} else {")
                self.indent_level += 1
                self.emit_check(scratch, code, frame)
                self.emit(f"if ({scratch}) {{")
                self.indent_level += 1
                for child in nested.then_body:
                    self.emit_statement(child)
                self.indent_level -= 1
                self._emit_else_chain(nested)
                self.emit("}")
                self.indent_level -= 1
            else:
                self.emit(f"}} else if ({code}) {{")
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

    def emit_delete(self, expression: Expr) -> None:
        self.temp_push()
        slot = self.slot(expression)
        # manual memory like C: free + NULL
        line = f"ds_release_slot((void **)&{slot});"
        self.emit_line_with_temps(line)

    def emit_assignment_target(self, target: Expr, value: Expr) -> None:
        # STRICT COMPILER, MANUAL MEMORY, SPEED LIKE C — no ds_keep/move/assign
        if isinstance(target, Index):
            self.temp_push()
            list_type = self.expression_type(target.object)
            element = self.require_element(list_type.element, target)
            list_code = self.value(target.object)[0]
            index = self.value(target.index)[0]
            # ds_list_set_string duplicates: a fresh value is freed with the frame.
            if element == STRING:
                val_code = self.owned_string(value)
            else:
                val_code = self.value(value)[0]
            setter = {
                "DS_ELEM_FLOAT": "ds_list_set_float",
                "DS_ELEM_INT": "ds_list_set_int",
                "DS_ELEM_BOOL": "ds_list_set_bool",
                "DS_ELEM_STRING": "ds_list_set_string",
                "DS_ELEM_LIST": "ds_list_set_list",
                "DS_ELEM_OBJECT": "ds_list_set_object",
            }[self.element_kind(element)]
            cast = "" if element in (FLOAT, INT, BOOL) else f"({self.c_type(element)})"
            line = f"{setter}({list_code}, (int64_t)({index}), {cast}{val_code});"
            self.emit_line_with_temps(line)
            return
        if isinstance(target, Name):
            name = self.assignable_name(target)
            dtype = self.expression_type(target)
            # The semantic model records value expressions, while an
            # assignment target is also a slot. Recover a global's declared
            # type here so string globals use ds_string_replace rather than
            # leaking their previous value on every toast update.
            if dtype == UNKNOWN and target.name in self.model.global_types:
                dtype = self.model.global_types[target.name]
            self.emit_assignment(name, value, dtype)
            return
        if isinstance(target, Member):
            self.temp_push()
            slot = self.member(target)
            if self.expression_type(target) == STRING:
                # Struct fields own their strings (the struct dtor frees them).
                line = f"ds_string_replace((DsString **)&{slot}, {self.owned_string(value)});"
            else:
                line = f"{slot} = {self.value(value)[0]};"
            self.emit_line_with_temps(line)
            return
        raise TypeError(f"unsupported assignment target {target!r}")

    def emit_assignment(self, target: str, value: Expr, dtype: DType) -> None:
        self.temp_push()
        if dtype == STRING and not self.is_param_slot(target):
            # Strings are immutable values: storing duplicates and frees the
            # old one, so globals and locals always own their strings.  Params
            # stay raw — the incoming value is borrowed, never owned.
            line = f"ds_string_replace((DsString **)&{target}, {self.owned_string(value)});"
        else:
            line = f"{target} = {self.value(value)[0]};"
        self.emit_line_with_temps(line)

    def is_param_slot(self, target: str) -> bool:
        if self.current_function is None:
            return False
        return target in {_sanitize(parameter.name) for parameter in self.current_function.params}

    def assignable_name(self, expression: Name) -> str:
        return self.name(expression)

    def slot(self, expression: Expr) -> str:
        if isinstance(expression, Name):
            return self.name(expression)
        if isinstance(expression, Member):
            return self.member(expression)
        raise TypeError(f"not a slot: {expression!r}")

    def truthy(self, expression: Expr) -> str:
        dtype = self.expression_type(expression)
        if dtype == STRING:
            return f"(ds_string_length({self.owned_string(expression)}) != 0)"
        code = self.value(expression)[0]
        if dtype == BOOL:
            stripped = _strip_outer_parentheses(code)
            return stripped if stripped is not None else code
        if dtype.is_handle:
            return f"({code} != NULL)"
        return f"({code}) != 0"

    # -- values (code + ownership) ----------------------------------------

    def keep(self, expression: Expr, code: str) -> str:
        # NO REFCOUNT: identity, speed like C
        return code

    def owned(self, expression: Expr) -> str:
        # MANUAL: direct code, no keep
        code, fresh = self.value(expression)
        return code

    def value(self, expression: Expr) -> Tuple[str, bool]:
        """Return ``(C code, owns_a_reference)`` for an expression."""

        if isinstance(expression, Literal):
            if expression.literal_type == "string":
                return self.literal(str(expression.value)), False
            if expression.literal_type == "int":
                return str(int(expression.value)), False
            if expression.literal_type == "float":
                return _c_float(float(expression.value)), False
            if expression.literal_type == "bool":
                return ("1" if expression.value else "0"), False
            if expression.literal_type == "nil":
                return "NULL", False
        if isinstance(expression, Name):
            return self.name(expression), False
        if isinstance(expression, Member):
            dtype = self.expression_type(expression)
            if dtype.name.startswith("builtin:"):
                return self.builtin_property(expression), False
            return self.member(expression), False
        if isinstance(expression, New):
            dtype = _type_from_name(expression.type_name, self.model.structs)
            name = self.struct_names[expression.type_name]
            struct = next(s for s in self.program.structs if s.name == expression.type_name)
            dtor = f"ds_dtor_{name}" if self.struct_has_handles(struct) else "NULL"
            return f"({self.c_type(dtype)})ds_alloc(sizeof({name}), {dtor})", True
        if isinstance(expression, Unary):
            operator = "!" if expression.operator == "not" else expression.operator
            return f"({operator}{self.value(expression.operand)[0]})", False
        if isinstance(expression, Binary):
            return self.binary(expression)
        if isinstance(expression, Index):
            return self.index_read(expression)
        if isinstance(expression, ListLiteral):
            return self.list_literal(expression), True
        if isinstance(expression, Call):
            return self.call(expression)
        raise TypeError(f"unsupported expression {expression!r}")

    def binary(self, expression: Binary) -> Tuple[str, bool]:
        operator = expression.operator
        if operator == "..":
            left = self.owned_string(expression.left)
            right = self.owned_string(expression.right)
            return f"ds_concat({left}, {right})", True
        left_type = self.expression_type(expression.left)
        right_type = self.expression_type(expression.right)
        if operator in ("==", "!=", "~=", ">", "<", ">=", "<="):
            if left_type == STRING or right_type == STRING:
                comparison = {"~=": "!="}.get(operator, operator)
                left = self.owned_string(expression.left)
                right = self.owned_string(expression.right)
                return f"(ds_string_compare({left}, {right}) {comparison} 0)", False
        c_operator = {"and": "&&", "or": "||", "~=": "!="}.get(operator, operator)
        if c_operator == "/":
            # `/` always yields a float, and dividing by zero is a script error,
            # so a constant non-zero divisor is the only case without a check.
            left = self.number(expression.left, left_type)
            right = self.number(expression.right, right_type)
            return f"ds_div({left}, {right}, {self.where(expression)})", False
        if c_operator == "%":
            if left_type in (INT, BOOL) and right_type in (INT, BOOL):
                left = self.value(expression.left)[0]
                right = self.value(expression.right)[0]
                return (f"ds_imod((int64_t)({left}), (int64_t)({right}), {self.where(expression)})",
                        False)
            left = self.number(expression.left, left_type)
            right = self.number(expression.right, right_type)
            return f"ds_mod({left}, {right}, {self.where(expression)})", False
        left = self.value(expression.left)[0]
        right = self.value(expression.right)[0]
        return f"({left} {c_operator} {right})", False

    def number(self, expression: Expr, dtype: DType) -> str:
        code = self.value(expression)[0]
        return f"(double)({code})"

    def is_fresh_string(self, expression: Expr) -> bool:
        """True when the expression provably allocates a new string value.

        Only these shapes are ever released after the statement: assignment
        targets, ``return`` results, list elements and function arguments are
        handled by other paths and never touch this predicate.
        """
        if isinstance(expression, Binary) and expression.operator == "..":
            return True
        if isinstance(expression, Index):
            return self.expression_type(expression.object) == STRING
        if isinstance(expression, Call):
            callee = expression.callee
            if isinstance(callee, Name) and callee.name == "str" and expression.args:
                argument = expression.args[0]
                dtype = self.expression_type(argument)
                return dtype != STRING or self.is_fresh_string(argument)
            if (isinstance(callee, Member) and callee.name == "join"
                    and self.expression_type(callee.object).name == "list"):
                return True
            # String returns are always duplicated at the `return`, so every
            # user call that yields a string hands over an owned value.
            if isinstance(callee, Name) and callee.name in self.functions:
                return self.model.function_returns[callee.name] == STRING
        return False

    def owned_string_raw(self, expression: Expr) -> str:
        """Inline string value with no temp tracking (escapes or converts)."""
        dtype = self.expression_type(expression)
        code, _fresh = self.value(expression)
        if dtype == STRING:
            return code
        if dtype == INT:
            return f"ds_int_to_string((int64_t)({code}))"
        if dtype == FLOAT:
            return f"ds_float_to_string((double)({code}))"
        if dtype == BOOL:
            return f"ds_bool_to_string(({code}) != 0)"
        return code

    def is_fresh_owned(self, expression: Expr) -> bool:
        """True when :meth:`owned_string_raw` returns a fresh allocation."""
        dtype = self.expression_type(expression)
        if dtype != STRING:
            return dtype in (INT, FLOAT, BOOL)
        return self.is_fresh_string(expression)

    def owned_string(self, expression: Expr) -> str:
        """A string consumed in place: fresh values join the statement temps."""
        code = self.owned_string_raw(expression)
        if (self.temps_enabled and self.temp_frames
                and self.is_fresh_owned(expression)):
            variable = f"__ds_t{self.temp_counter}"
            self.temp_counter += 1
            self.temp_frames[-1].append((variable, code))
            return variable
        return code

    def index_read(self, expression: Index) -> Tuple[str, bool]:
        # MANUAL: getters return BORROWED, no keep, speed like C
        object_type = self.expression_type(expression.object)
        code = self.value(expression.object)[0]
        index = self.value(expression.index)[0]
        if object_type == STRING:
            return f"ds_string_char_at({code}, (int64_t)({index}), {self.where(expression)})", True
        element = self.require_element(object_type.element, expression)
        getter = {
            "DS_ELEM_FLOAT": "ds_list_get_float",
            "DS_ELEM_INT": "ds_list_get_int",
            "DS_ELEM_BOOL": "ds_list_get_bool",
            "DS_ELEM_STRING": "ds_list_get_string",
            "DS_ELEM_LIST": "ds_list_get_list",
            "DS_ELEM_OBJECT": "ds_list_get_object",
        }[self.element_kind(element)]
        call = f"{getter}({code}, (int64_t)({index}), {self.where(expression)})"
        references = element in (STRING,) or element.name == "list" or element.is_struct
        if references:
            # borrowed, not owned
            return f"({self.c_type(element)}){call}", False
        return call, False

    def list_literal(self, expression: ListLiteral) -> str:
        # MANUAL: no keep, direct values, speed like C
        dtype = self.expression_type(expression)
        element = dtype.element if dtype.element is not None else UNKNOWN
        if not expression.items:
            element = self.require_element(element, expression)
        kind = self.element_kind(element)
        if not expression.items:
            return f"ds_list_new({kind}, {self.element_size(element)}, {self.element_dtor(element)}, 0)"
        values = []
        for item in expression.items:
            # String literals duplicate inside ds_list_of_strings; fresh items
            # are the statement's temps.
            code = self.owned_string(item) if element == STRING else self.value(item)[0]
            values.append(code)
        if element in (FLOAT,):
            casted = ", ".join(f"(double)({value})" for value in values)
            return f"ds_list_of_floats({len(values)}, (const double[]){{{casted}}})"
        if element == INT:
            casted = ", ".join(f"(int64_t)({value})" for value in values)
            return f"ds_list_of_ints({len(values)}, (const int64_t[]){{{casted}}})"
        if element == BOOL:
            casted = ", ".join(f"(int)({value}) != 0" for value in values)
            return f"ds_list_of_bools({len(values)}, (const int[]){{{casted}}})"
        if element == STRING:
            casted = ", ".join(f"(DsString *)({value})" for value in values)
            return f"ds_list_of_strings({len(values)}, (DsString *const[]){{{casted}}})"
        if element.name == "list":
            casted = ", ".join(f"(DsList *)({value})" for value in values)
            return f"ds_list_of_lists({len(values)}, (DsList *const[]){{{casted}}})"
        casted = ", ".join(f"(void *)({value})" for value in values)
        return (f"ds_list_of_objects({len(values)}, (void *const[]){{{casted}}}, "
                f"{self.element_size(element)}, {self.element_dtor(element)})")

    # -- calls -------------------------------------------------------------

    def call(self, expression: Call) -> Tuple[str, bool]:
        callee = expression.callee
        if isinstance(callee, Member) and isinstance(callee.object, Name) and callee.object.name in NAMESPACE_TYPES:
            return self.builtin_call(callee.object.name, callee, expression), self.builtin_is_fresh(
                callee.object.name, callee.name)
        if isinstance(callee, Member) and isinstance(callee.object, (Name, Member, Index)):
            owner = self.expression_type(callee.object)
            if owner.name == "list":
                return self.list_call(callee, expression), self.builtin_is_fresh("list", callee.name)
        if isinstance(callee, Name) and callee.name in self.functions:
            # Parameters are borrowed: a fresh string argument is the caller's
            # temp and joins the statement frame (a callee that wants to keep
            # one duplicates it with `"" .. param`).
            params = self.model.function_params[callee.name]
            assert len(params) == len(expression.args)
            arguments = [
                self.owned_string(argument) if dtype == STRING else self.value(argument)[0]
                for argument, dtype in zip(expression.args, params)
            ]
            call_code = f"ds_fn_{_sanitize(callee.name)}({', '.join(arguments)})"
            # fresh if returns handle (allocates)
            return call_code, self.model.function_returns[callee.name].is_handle
        if isinstance(callee, Name) and callee.name in {"print", "log"}:
            parts = [self.owned_string(argument) for argument in expression.args]
            if len(parts) == 1:
                return f"ds_log({parts[0]})", False
            joined = f"ds_text_join({len(parts)}, {', '.join(parts)})"
            if self.temps_enabled and self.temp_frames:
                variable = f"__ds_t{self.temp_counter}"
                self.temp_counter += 1
                self.temp_frames[-1].append((variable, joined))
                return f"ds_log({variable})", False
            return f"ds_log({joined})", False
        if isinstance(callee, Name) and callee.name == "str":
            # The caller owns the result, so it stays raw and never joins temps.
            return self.owned_string_raw(expression.args[0]), True
        if isinstance(callee, Name) and callee.name == "num":
            return f"ds_number_of_text({self.owned_string(expression.args[0])})", False
        if isinstance(callee, Name) and callee.name == "len":
            argument = expression.args[0]
            dtype = self.expression_type(argument)
            if dtype == STRING:
                return f"ds_string_length({self.owned_string(argument)})", False
            return f"ds_list_count({self.value(argument)[0]})", False
        raise TypeError(f"unsupported call {expression!r}")

    def builtin_is_fresh(self, namespace: str, name: str) -> bool:
        """True when a builtin hands back a reference the caller owns."""

        if namespace == "render":
            return False
        if namespace in ("math", "engine", "input", "image", "font"):
            return False
        if namespace == "list":
            return name == "join"
        return False

    def builtin_property(self, expression: Member) -> str:
        object_type = self.expression_type(expression.object)
        namespace = object_type.name.split(":", 1)[1]
        getter = BUILTIN_PROPERTIES.get(namespace, {}).get(expression.name)
        if getter:
            return getter
        raise SemanticError(f"{self.filename}: {getattr(expression, 'line', 0)}: "
                            f"{namespace}.{expression.name} нельзя читать как поле")

    def builtin_call(self, namespace: str, callee: Member, expression: Call) -> str:
        builtin = BUILTINS[namespace][callee.name]
        arguments: List[str] = []
        if namespace == "render" and callee.name == "color" and len(expression.args) == 4:
            builtin = BUILTINS["render"]["color_alpha"]
        for index, argument in enumerate(expression.args):
            if index in builtin.strings:
                arguments.append(self.owned_string(argument))
            elif builtin.numbers == "float":
                arguments.append(f"(float)({self.value(argument)[0]})")
            else:
                arguments.append(f"(double)({self.value(argument)[0]})")
        if builtin.where:
            arguments.append(self.where(expression))
        return builtin.c.format(*arguments)

    def list_call(self, callee: Member, expression: Call) -> str:
        list_type = self.expression_type(callee.object)
        element = self.require_element(list_type.element, callee)
        kind = self.element_kind(element)
        code = self.value(callee.object)[0]
        suffix = {
            "DS_ELEM_FLOAT": "float",
            "DS_ELEM_INT": "int",
            "DS_ELEM_BOOL": "bool",
            "DS_ELEM_STRING": "string",
            "DS_ELEM_LIST": "list",
            "DS_ELEM_OBJECT": "object",
        }[kind]
        name = callee.name
        if name == "push":
            # String lists own private duplicates; a fresh value is the
            # statement's temp and is freed after the push duplicated it.
            if element == STRING:
                value = self.owned_string(expression.args[0])
            else:
                value = self.value(expression.args[0])[0] if element.is_handle else self.element_value(
                    expression.args[0], element)
            return f"ds_list_push_{suffix}({code}, {value})"
        if name == "insert":
            index = self.value(expression.args[0])[0]
            if element == STRING:
                value = self.owned_string(expression.args[1])
            else:
                value = self.value(expression.args[1])[0] if element.is_handle else self.element_value(
                    expression.args[1], element)
            return f"ds_list_insert_{suffix}({code}, (int64_t)({index}), {value})"
        if name in ("delete", "remove_at"):
            index = self.value(expression.args[0])[0]
            return f"ds_list_remove_at({code}, (int64_t)({index}), {self.where(callee)})"
        if name == "clear":
            return f"ds_list_clear({code})"
        if name == "index_of":
            argument = expression.args[0]
            if element == STRING:
                value = self.owned_string(argument)
                return f"ds_list_index_of_string({code}, {value})"
            if element.is_struct or element.name == "list":
                value = self.value(argument)[0]
                return f"ds_list_index_of_object({code}, (const void *)({value}))"
            value = self.value(argument)[0]
            return f"ds_list_index_of_{suffix}({code}, {self.element_value(argument, element)})"
        if name == "join":
            separator = self.owned_string(expression.args[0])
            return f"ds_list_join({code}, {separator})"
        raise TypeError(f"unsupported list method {callee.name}")

    def element_value(self, expression: Expr, element: DType) -> str:
        code = self.value(expression)[0]
        if element == FLOAT:
            return f"(double)({code})"
        if element == INT:
            return f"(int64_t)({code})"
        if element == BOOL:
            return f"(({code}) != 0)"
        return code

    # -- names and members -------------------------------------------------

    def name(self, expression: Name) -> str:
        if self.current_function is not None:
            local_types = self.model.function_locals.get(self.current_function.name, {})
            if expression.name in local_types:
                return _sanitize(expression.name)
        if expression.name in self.global_c_names:
            return self.global_c_names[expression.name]
        return _sanitize(expression.name)

    def member(self, expression: Member) -> str:
        dtype = self.expression_type(expression)
        if dtype.name.startswith("builtin:"):
            raise SemanticError(f"{self.filename}: {getattr(expression, 'line', 0)}: "
                                f"{dtype.name.split(':', 1)[1]} — это метод, его нужно вызвать")
        object_type = self.expression_type(expression.object)
        if object_type.name == "list":
            # `list.count`
            return f"ds_list_count({self.value(expression.object)[0]})"
        code = self.value(expression.object)[0]
        pointer = self.c_type(object_type)
        return f"(({pointer})ds_require((void *)({code}), {self.where(expression)}))->{_sanitize(expression.name)}"

    def expression_type(self, expression: Expr) -> DType:
        return self.model.expression_types.get(id(expression), UNKNOWN)


def compile_program(program: Program, filename: str = "<string>") -> Tuple[str, SemanticModel]:
    compiler = CCompiler(program, filename=filename)
    return compiler.compile(), compiler.model


def compile_header(program: Optional[Program] = None, filename: str = "<string>") -> str:
    program = program or Program(line=1, column=1)
    return CCompiler(program, filename=filename).header()


def compile_source(source: str, filename: str = "<string>") -> Tuple[str, SemanticModel]:
    from .parser import parse

    return compile_program(parse(source, filename=filename), filename=filename)


def compile_project(project, name: Optional[str] = None) -> Tuple[str, SemanticModel]:
    """Compile a loaded game folder (see dimscript.project.load_project)."""

    filenames = ", ".join(script.name for script in getattr(project, "scripts", []))
    return compile_program(project.program, filename=name or filenames or "<game>")


def compile_program(program: Program, filename: str = "<string>") -> Tuple[str, SemanticModel]:
    compiler = CCompiler(program, filename=filename)
    return compiler.compile(), compiler.model


def compile_header(program: Optional[Program] = None, filename: str = "<string>") -> str:
    program = program or Program(line=1, column=1)
    return CCompiler(program, filename=filename).header()


def compile_source(source: str, filename: str = "<string>") -> Tuple[str, SemanticModel]:
    from .parser import parse

    return compile_program(parse(source, filename=filename), filename=filename)


def compile_project(project, name: Optional[str] = None) -> Tuple[str, SemanticModel]:
    """Compile a loaded game folder (see dimscript.project.load_project)."""

    filenames = ", ".join(script.name for script in getattr(project, "scripts", []))
    return compile_program(project.program, filename=name or filenames or "<game>")
