"""Recursive-descent / Pratt parser for DimScript."""

from __future__ import annotations

from typing import List, Optional

from .ast import (
    Assign,
    Binary,
    Call,
    Delete,
    Expr,
    ExprStmt,
    FieldDecl,
    FunctionDecl,
    GlobalDecl,
    If,
    Literal,
    Member,
    Name,
    New,
    Param,
    Program,
    Return,
    Stmt,
    StructDecl,
    Unary,
)
from .lexer import DimScriptError, Token, lex


class ParseError(DimScriptError):
    pass


_BINARY_PRECEDENCE = {
    "or": 1,
    "||": 1,
    "and": 2,
    "&&": 2,
    "==": 3,
    "!=": 3,
    "~=": 3,
    ">": 4,
    "<": 4,
    ">=": 4,
    "<=": 4,
    "..": 5,
    "+": 6,
    "-": 6,
    "*": 7,
    "/": 7,
    "%": 7,
}


class Parser:
    def __init__(self, tokens: List[Token], filename: str = "<string>") -> None:
        self.tokens = tokens
        self.index = 0
        self.filename = filename

    @property
    def current(self) -> Token:
        return self.tokens[self.index]

    def peek(self, offset: int = 1) -> Token:
        position = min(self.index + offset, len(self.tokens) - 1)
        return self.tokens[position]

    def error(self, message: str, token: Optional[Token] = None) -> ParseError:
        token = token or self.current
        return ParseError(f"{self.filename}:{token.line}:{token.column}: {message}")

    def advance(self) -> Token:
        token = self.current
        if self.index < len(self.tokens) - 1:
            self.index += 1
        return token

    def at(self, value: str) -> bool:
        return self.current.value == value

    def accept(self, value: str) -> Optional[Token]:
        if self.at(value):
            return self.advance()
        return None

    def expect(self, value: str) -> Token:
        if not self.at(value):
            raise self.error(f"ожидалось {value!r}, получено {self.current.describe()}")
        return self.advance()

    def expect_identifier(self, what: str = "имя") -> Token:
        if self.current.kind not in ("IDENT", "KEYWORD") or self.current.value in {
            "struct",
            "new",
            "delete",
            "if",
            "then",
            "else",
            "return",
            "true",
            "false",
            "nil",
            "and",
            "or",
            "not",
        }:
            raise self.error(f"ожидалось {what}, получено {self.current.describe()}")
        return self.advance()

    def skip_separators(self) -> None:
        while self.current.kind == "NEWLINE" or self.at(";"):
            self.advance()

    def parse(self) -> Program:
        program = Program(line=1, column=1)
        self.skip_separators()
        while self.current.kind != "EOF":
            if self.at("struct"):
                program.structs.append(self.parse_struct())
            elif self.current.kind in ("IDENT", "KEYWORD") and self.peek().value == "(":
                program.functions.append(self.parse_function())
            elif self.current.kind in ("IDENT", "KEYWORD"):
                program.globals.append(self.parse_global())
            else:
                raise self.error("ожидался struct, функция или глобальное присваивание")
            self.skip_separators()
        return program

    def parse_struct(self) -> StructDecl:
        start = self.expect("struct")
        name = self.expect_identifier("имя структуры")
        self.expect("{")
        fields: List[FieldDecl] = []
        self.skip_separators()
        while not self.at("}"):
            field = self.expect_identifier("имя поля")
            self.expect(":")
            type_name = self.expect_identifier("тип поля")
            fields.append(FieldDecl(name=field.value, type_name=type_name.value,
                                    line=field.line, column=field.column))
            # Commas are optional; line breaks are the normal separator.
            self.accept(",")
            self.accept(";")
            self.skip_separators()
            if self.current.kind == "EOF":
                raise self.error("незакрытая структура, ожидалась '}'")
        self.expect("}")
        return StructDecl(name=name.value, fields=fields, line=start.line, column=start.column)

    def parse_global(self) -> GlobalDecl:
        name = self.expect_identifier("имя глобальной переменной")
        self.expect("=")
        value = self.parse_expression()
        return GlobalDecl(name=name.value, value=value, line=name.line, column=name.column)

    def parse_function(self) -> FunctionDecl:
        name = self.expect_identifier("имя функции")
        self.expect("(")
        params: List[Param] = []
        self.skip_separators()
        if not self.at(")"):
            while True:
                param = self.expect_identifier("имя параметра")
                type_name: Optional[str] = None
                if self.accept(":"):
                    type_name = self.expect_identifier("тип параметра").value
                params.append(Param(name=param.value, type_name=type_name,
                                    line=param.line, column=param.column))
                if not self.accept(","):
                    break
                self.skip_separators()
        self.expect(")")
        return_type: Optional[str] = None
        if self.accept(":"):
            return_type = self.expect_identifier("тип результата").value
        body = self.parse_block()
        return FunctionDecl(name=name.value, params=params, body=body,
                            return_type=return_type, line=name.line, column=name.column)

    def parse_block(self) -> List[Stmt]:
        self.expect("{")
        body: List[Stmt] = []
        self.skip_separators()
        while not self.at("}"):
            if self.current.kind == "EOF":
                raise self.error("незакрытый блок, ожидалась '}'")
            body.append(self.parse_statement())
            self.skip_separators()
        self.expect("}")
        return body

    def parse_statement(self) -> Stmt:
        if self.at("if"):
            return self.parse_if()
        if self.at("delete"):
            start = self.advance()
            expression = self.parse_expression()
            return Delete(expression=expression, line=start.line, column=start.column)
        if self.at("return"):
            start = self.advance()
            if self.current.kind in ("NEWLINE", "EOF") or self.at("}"):
                return Return(expression=None, line=start.line, column=start.column)
            return Return(expression=self.parse_expression(), line=start.line, column=start.column)

        expression = self.parse_expression()
        if self.accept("="):
            value = self.parse_expression()
            return Assign(target=expression, value=value, line=expression.line, column=expression.column)
        return ExprStmt(expression=expression, line=expression.line, column=expression.column)

    def parse_if(self) -> If:
        start = self.expect("if")
        condition = self.parse_expression()
        self.accept("then")
        self.skip_separators()
        # Braces are optional after ``then``.  The compact form used by the
        # original DimScript example closes an implicit block with ``}``:
        # ``if ready then\n    draw()\n}``.  Accepting an explicit ``{`` as
        # well makes nested game logic easier to read.
        then_body = self.parse_block() if self.at("{") else self.parse_implicit_block()
        else_body: List[Stmt] = []
        self.skip_separators()
        if self.accept("else"):
            # Both ``else {}`` and ``else if ...`` are useful.  The latter is
            # represented as one nested If statement in the else body.
            self.skip_separators()
            if self.at("if"):
                else_body = [self.parse_if()]
            elif self.at("{"):
                else_body = self.parse_block()
            else:
                else_body = self.parse_implicit_block()
        return If(condition=condition, then_body=then_body, else_body=else_body,
                  line=start.line, column=start.column)

    def parse_implicit_block(self) -> List[Stmt]:
        body: List[Stmt] = []
        self.skip_separators()
        while not self.at("}"):
            if self.current.kind == "EOF":
                raise self.error("незакрытый if, ожидалась '}'")
            body.append(self.parse_statement())
            self.skip_separators()
        self.expect("}")
        return body

    # Pratt expression parser -------------------------------------------------
    def parse_expression(self, minimum_precedence: int = 0) -> Expr:
        expression = self.parse_prefix()
        while True:
            # Calls and member access bind more tightly than every binary op.
            if self.accept("."):
                name = self.expect_identifier("имя поля или метода")
                expression = Member(object=expression, name=name.value,
                                    line=expression.line, column=expression.column)
                continue
            if self.accept("("):
                args: List[Expr] = []
                self.skip_separators()
                if not self.at(")"):
                    while True:
                        args.append(self.parse_expression())
                        if not self.accept(","):
                            break
                        self.skip_separators()
                self.expect(")")
                expression = Call(callee=expression, args=args,
                                  line=expression.line, column=expression.column)
                continue

            operator = self.current.value
            precedence = _BINARY_PRECEDENCE.get(operator)
            if precedence is None or precedence < minimum_precedence:
                break
            self.advance()
            # All current binary operators are left associative.  Adding one
            # to the minimum precedence makes the right side stop at an op of
            # equal priority.
            right = self.parse_expression(precedence + 1)
            expression = Binary(operator=operator, left=expression, right=right,
                                 line=expression.line, column=expression.column)
        return expression

    def parse_prefix(self) -> Expr:
        token = self.current
        if token.value in ("-", "+", "!", "not"):
            self.advance()
            return Unary(operator=token.value, operand=self.parse_expression(8),
                         line=token.line, column=token.column)
        if token.kind == "NUMBER":
            self.advance()
            is_float = any(marker in token.value for marker in (".", "e", "E"))
            value: object = float(token.value) if is_float else int(token.value)
            return Literal(value=value, literal_type="float" if is_float else "int",
                           line=token.line, column=token.column)
        if token.kind == "STRING":
            self.advance()
            return Literal(value=token.value, literal_type="string",
                           line=token.line, column=token.column)
        if token.value == "true" or token.value == "false":
            self.advance()
            return Literal(value=token.value == "true", literal_type="bool",
                           line=token.line, column=token.column)
        if token.value == "nil":
            self.advance()
            return Literal(value=None, literal_type="nil", line=token.line, column=token.column)
        if token.value == "(":
            self.advance()
            expression = self.parse_expression()
            self.expect(")")
            return expression
        if token.value == "new":
            self.advance()
            type_name = self.expect_identifier("тип после new")
            return New(type_name=type_name.value, line=token.line, column=token.column)
        if token.kind in ("IDENT", "KEYWORD") and token.value not in {
            "struct", "delete", "if", "then", "else", "return",
        }:
            self.advance()
            return Name(name=token.value, line=token.line, column=token.column)
        raise self.error(f"неожиданный токен {token.describe()}")


def parse(source: str, filename: str = "<string>") -> Program:
    """Lex and parse DimScript source."""

    return Parser(lex(source), filename=filename).parse()
