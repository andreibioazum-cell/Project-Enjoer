"""Abstract syntax tree nodes for DimScript.

The tree is deliberately small.  DimScript is a game scripting language, not
an attempt to expose all of C's syntax: statements are assignments, calls,
conditionals and lifecycle functions, while expressions cover the values that
are useful in a frame callback.
"""

from __future__ import annotations

from dataclasses import dataclass, field
from typing import List, Optional, Union


@dataclass
class Node:
    line: int = 0
    column: int = 0


@dataclass
class TypeRef(Node):
    name: str = "unknown"


@dataclass
class FieldDecl(Node):
    name: str = ""
    type_name: str = "unknown"


@dataclass
class StructDecl(Node):
    name: str = ""
    fields: List[FieldDecl] = field(default_factory=list)


@dataclass
class Param(Node):
    name: str = ""
    type_name: Optional[str] = None


@dataclass
class GlobalDecl(Node):
    name: str = ""
    value: "Expr" = None  # type: ignore[assignment]


@dataclass
class FunctionDecl(Node):
    name: str = ""
    params: List[Param] = field(default_factory=list)
    body: List["Stmt"] = field(default_factory=list)
    return_type: Optional[str] = None


@dataclass
class Require(Node):
    """`require "module"` — another .ds file of the same game folder."""

    module: str = ""


@dataclass
class Program(Node):
    requires: List[Require] = field(default_factory=list)
    structs: List[StructDecl] = field(default_factory=list)
    globals: List[GlobalDecl] = field(default_factory=list)
    functions: List[FunctionDecl] = field(default_factory=list)


# Expressions -----------------------------------------------------------------


@dataclass
class Expr(Node):
    pass


@dataclass
class Literal(Expr):
    value: object = None
    literal_type: str = "unknown"


@dataclass
class Name(Expr):
    name: str = ""


@dataclass
class Member(Expr):
    object: Expr = None  # type: ignore[assignment]
    name: str = ""


@dataclass
class Index(Expr):
    object: Expr = None  # type: ignore[assignment]
    index: Expr = None  # type: ignore[assignment]


@dataclass
class ListLiteral(Expr):
    items: List[Expr] = field(default_factory=list)


@dataclass
class Call(Expr):
    callee: Expr = None  # type: ignore[assignment]
    args: List[Expr] = field(default_factory=list)


@dataclass
class New(Expr):
    type_name: str = ""


@dataclass
class Unary(Expr):
    operator: str = ""
    operand: Expr = None  # type: ignore[assignment]


@dataclass
class Binary(Expr):
    operator: str = ""
    left: Expr = None  # type: ignore[assignment]
    right: Expr = None  # type: ignore[assignment]


# Statements ------------------------------------------------------------------


@dataclass
class Stmt(Node):
    pass


@dataclass
class Assign(Stmt):
    target: Expr = None  # type: ignore[assignment]
    value: Expr = None  # type: ignore[assignment]
    explicit_local: bool = False


@dataclass
class ExprStmt(Stmt):
    expression: Expr = None  # type: ignore[assignment]


@dataclass
class If(Stmt):
    condition: Expr = None  # type: ignore[assignment]
    then_body: List[Stmt] = field(default_factory=list)
    else_body: List[Stmt] = field(default_factory=list)


@dataclass
class While(Stmt):
    condition: Expr = None  # type: ignore[assignment]
    body: List["Stmt"] = field(default_factory=list)


@dataclass
class For(Stmt):
    variable: str = ""
    start: Expr = None  # type: ignore[assignment]
    stop: Expr = None  # type: ignore[assignment]
    step: Optional[Expr] = None
    body: List["Stmt"] = field(default_factory=list)


@dataclass
class Break(Stmt):
    pass


@dataclass
class Continue(Stmt):
    pass


@dataclass
class Delete(Stmt):
    expression: Expr = None  # type: ignore[assignment]


@dataclass
class Return(Stmt):
    expression: Optional[Expr] = None


AnyExpr = Union[Literal, Name, Member, Index, ListLiteral, Call, New, Unary, Binary]
AnyStmt = Union[Assign, ExprStmt, If, While, For, Break, Continue, Delete, Return]
