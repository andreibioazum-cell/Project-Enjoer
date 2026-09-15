"""Recursive-descent / Pratt parser for DimScript.

The grammar is small on purpose.  A program is a list of `require`, `struct`,
global assignment and function declarations; a function body is a list of
statements; and blocks are opened by ``{`` (or implicitly by ``then``/``do``)
and always closed by ``}`` — ``end`` is accepted as an alias.  Newlines separate
statements, so a game script needs no semicolons.
"""

from __future__ import annotations

from typing import List, Optional

from .ast import (
    Assign,
    Binary,
    Break,
    Call,
    Continue,
    Delete,
    Expr,
    ExprStmt,
    FieldDecl,
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
    Param,
    Program,
    Require,
    Return,
    Stmt,
    StructDecl,
    Unary,
    While,
)
from .lexer import DimScriptError, Token, lex

# Keywords that can never be used as a name.  Everything else (`do`, `end`,
# `then`, `count`, `for`) stays available to game code.
HARD_KEYWORDS = frozenset(
    {
        "new",
        "delete",
        "if",
        "else",
        "return",
        "true",
        "false",
        "nil",
        "and",
        "or",
        "not",
        "while",
        "break",
        "continue",
    }
)


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
        return self.current.value == value and self.current.kind in ("IDENT", "KEYWORD", "SYMBOL")

    def at_symbol(self, value: str) -> bool:
        return self.current.kind == "SYMBOL" and self.current.value == value

    def accept(self, value: str) -> Optional[Token]:
        if self.at_symbol(value):
            return self.advance()
        return None

    def accept_keyword(self, value: str) -> Optional[Token]:
        if self.current.kind == "KEYWORD" and self.current.value == value:
            return self.advance()
        return None

    def expect_keyword(self, value: str) -> Token:
        if self.current.kind != "KEYWORD" or self.current.value != value:
            raise self.error(f"ожидалось ключевое слово {value!r}, получено {self.current.describe()}")
        return self.advance()

    def expect(self, value: str) -> Token:
        if not self.at_symbol(value):
            raise self.error(f"ожидалось {value!r}, получено {self.current.describe()}")
        return self.advance()

    def expect_identifier(self, what: str = "имя") -> Token:
        # Soft keywords (`do`, `end`, `count`-like names) may be used freely so
        # that a game never has to rename a field because of the grammar.
        if self.current.kind not in ("IDENT", "KEYWORD") or self.current.value in HARD_KEYWORDS:
            raise self.error(f"ожидалось {what}, получено {self.current.describe()}")
        return self.advance()

    def skip_separators(self) -> None:
        while self.current.kind == "NEWLINE" or self.at_symbol(";"):
            self.advance()

    # --- program -----------------------------------------------------------
    def parse(self) -> Program:
        program = Program(line=1, column=1)
        self.skip_separators()
        while self.current.kind != "EOF":
            if self.at_symbol("}"):
                raise self.error(
                    "лишняя '}' — блок уже закрыт; одна '}' закрывает if/while/for и тело функции"
                )
            if self.current.value == "require" and self.current.kind == "KEYWORD":
                program.requires.append(self.parse_require())
            elif self.current.value == "struct" and self.current.kind == "KEYWORD":
                program.structs.append(self.parse_struct())
            elif self.current.kind in ("IDENT", "KEYWORD") and self.peek().value == "(":
                program.functions.append(self.parse_function())
            elif self.current.kind in ("IDENT", "KEYWORD"):
                program.globals.append(self.parse_global())
            else:
                raise self.error("ожидался struct, функция, require или глобальное присваивание")
            self.skip_separators()
        return program

    def parse_require(self) -> Require:
        start = self.advance()  # require
        token = self.current
        if token.kind != "STRING":
            raise self.error('require ожидает строку: require "ui"', token)
        self.advance()
        module = token.value.strip()
        if module.endswith(".ds"):
            module = module[: -len(".ds")]
        if not module:
            raise self.error("require ожидает имя модуля", token)
        return Require(module=module, line=start.line, column=start.column)

    def parse_struct(self) -> StructDecl:
        start = self.expect_keyword("struct")
        name = self.expect_identifier("имя структуры")
        self.expect("{")
        fields: List[FieldDecl] = []
        self.skip_separators()
        while not self.at_symbol("}") and not self.at_end():
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
        if self.at_end():
            self.advance()
        else:
            self.expect("}")
        return StructDecl(name=name.value, fields=fields, line=start.line, column=start.column)

    def at_end(self) -> bool:
        return self.current.kind == "KEYWORD" and self.current.value == "end"

    def parse_global(self) -> GlobalDecl:
        name = self.expect_identifier("имя глобальной переменной")
        self.expect("=")
        self.skip_separators()
        value = self.parse_expression()
        return GlobalDecl(name=name.value, value=value, line=name.line, column=name.column)

    def parse_function(self) -> FunctionDecl:
        name = self.expect_identifier("имя функции")
        self.expect("(")
        params: List[Param] = []
        self.skip_separators()
        if not self.at_symbol(")"):
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
        self.skip_separators()
        body = self.parse_block()
        return FunctionDecl(name=name.value, params=params, body=body,
                            return_type=return_type, line=name.line, column=name.column)

    def parse_block(self) -> List[Stmt]:
        self.expect("{")
        return self.parse_block_body()

    def parse_block_body(self) -> List[Stmt]:
        """Statements up to the closing `}` (or `end`), which is consumed."""

        body: List[Stmt] = []
        self.skip_separators()
        while not self.at_symbol("}") and not self.at_end():
            if self.current.kind == "EOF":
                raise self.error("незакрытый блок, ожидалась '}'")
            body.append(self.parse_statement())
            self.skip_separators()
        if self.at_end():
            self.advance()
        else:
            self.expect("}")
        return body

    def parse_block_open(self) -> None:
        """`then`/`do`, an optional newline and an optional `{`."""

        self.accept_keyword("then")
        self.accept_keyword("do")
        self.skip_separators()
        self.accept("{")

    def parse_statement(self) -> Stmt:
        if self.current.value == "if" and self.current.kind == "KEYWORD":
            return self.parse_if()
        if self.current.value == "while" and self.current.kind == "KEYWORD":
            return self.parse_while()
        if self.current.value == "for" and self.current.kind in ("KEYWORD", "IDENT"):
            if self.peek().kind in ("IDENT", "KEYWORD") and self.peek(2).value == "=":
                return self.parse_for()
        token = self.accept_keyword("break")
        if token is not None:
            return Break(line=token.line, column=token.column)
        token = self.accept_keyword("continue")
        if token is not None:
            return Continue(line=token.line, column=token.column)
        if self.current.value == "delete" and self.current.kind == "KEYWORD":
            start = self.advance()
            expression = self.parse_expression()
            return Delete(expression=expression, line=start.line, column=start.column)
        if self.current.value == "return" and self.current.kind == "KEYWORD":
            start = self.advance()
            if self.current.kind in ("NEWLINE", "EOF") or self.at_symbol("}") or self.at_end():
                return Return(expression=None, line=start.line, column=start.column)
            return Return(expression=self.parse_expression(), line=start.line, column=start.column)

        explicit_local = bool(self.accept_keyword("local"))
        start = self.current
        expression = self.parse_expression()
        if self.accept("="):
            self.skip_separators()
            value = self.parse_expression()
            if not isinstance(expression, (Name, Member, Index)):
                raise self.error("слева от '=' должно быть имя, поле или элемент списка", start)
            return Assign(target=expression, value=value, line=start.line, column=start.column,
                          explicit_local=explicit_local)
        if explicit_local:
            raise self.error("после local ожидалось 'имя = значение'", start)
        return ExprStmt(expression=expression, line=start.line, column=start.column)

    def parse_if(self) -> If:
        start = self.expect_keyword("if")
        condition = self.parse_expression()
        self.parse_block_open()
        then_body = self.parse_block_body()
        else_body: List[Stmt] = []
        checkpoint = self.index
        self.skip_separators()
        if self.current.value == "else" and self.current.kind == "KEYWORD":
            self.advance()
            self.skip_separators()
            if self.current.value == "if" and self.current.kind == "KEYWORD":
                # `else if` is one nested If statement in the else body.
                else_body = [self.parse_if()]
            else:
                self.parse_block_open()
                else_body = self.parse_block_body()
        else:
            self.index = checkpoint
        return If(condition=condition, then_body=then_body, else_body=else_body,
                  line=start.line, column=start.column)

    def parse_while(self) -> While:
        start = self.expect_keyword("while")
        condition = self.parse_expression()
        self.parse_block_open()
        body = self.parse_block_body()
        return While(condition=condition, body=body, line=start.line, column=start.column)

    def parse_for(self) -> For:
        start = self.expect_keyword("for")
        variable = self.expect_identifier("имя переменной цикла")
        self.expect("=")
        first = self.parse_expression()
        self.expect(",")
        last = self.parse_expression()
        step: Optional[Expr] = None
        if self.accept(","):
            step = self.parse_expression()
        self.parse_block_open()
        body = self.parse_block_body()
        return For(variable=variable.value, start=first, stop=last, step=step, body=body,
                   line=start.line, column=start.column)

    # Pratt expression parser -------------------------------------------------
    def parse_expression(self, minimum_precedence: int = 0) -> Expr:
        expression = self.parse_prefix()
        while True:
            # Calls, member access and indexing bind more tightly than any
            # binary operator.
            if self.accept("."):
                name = self.expect_identifier("имя поля или метода")
                expression = Member(object=expression, name=name.value,
                                    line=expression.line, column=expression.column)
                continue
            if self.accept("("):
                args: List[Expr] = []
                self.skip_separators()
                if not self.at_symbol(")"):
                    while True:
                        args.append(self.parse_expression())
                        if not self.accept(","):
                            break
                        self.skip_separators()
                self.expect(")")
                expression = Call(callee=expression, args=args,
                                  line=expression.line, column=expression.column)
                continue
            if self.accept("["):
                self.skip_separators()
                index = self.parse_expression()
                self.skip_separators()
                self.expect("]")
                expression = Index(object=expression, index=index,
                                   line=expression.line, column=expression.column)
                continue

            operator = self.current.value
            precedence = _BINARY_PRECEDENCE.get(operator)
            if precedence is None or precedence < minimum_precedence:
                break
            if self.current.kind not in ("SYMBOL", "KEYWORD"):
                break
            self.advance()
            # A line may end after an operator, never before one: that is what
            # makes the newline a statement separator.
            self.skip_separators()
            right = self.parse_expression(precedence + 1)
            expression = Binary(operator=operator, left=expression, right=right,
                                line=expression.line, column=expression.column)
        return expression

    def parse_prefix(self) -> Expr:
        token = self.current
        if token.kind == "SYMBOL" and token.value in ("-", "+", "!"):
            self.advance()
            return Unary(operator=token.value, operand=self.parse_expression(8),
                         line=token.line, column=token.column)
        if token.kind == "KEYWORD" and token.value == "not":
            self.advance()
            return Unary(operator="not", operand=self.parse_expression(8),
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
        if token.kind == "SYMBOL" and token.value == "(":
            self.advance()
            self.skip_separators()
            expression = self.parse_expression()
            self.skip_separators()
            self.expect(")")
            return expression
        if token.kind == "SYMBOL" and token.value == "[":
            self.advance()
            items: List[Expr] = []
            self.skip_separators()
            while not self.at_symbol("]"):
                if self.current.kind == "EOF":
                    raise self.error("незакрытый список, ожидалась ']'")
                items.append(self.parse_expression())
                self.skip_separators()
                if not self.accept(","):
                    break
                self.skip_separators()
            self.expect("]")
            return ListLiteral(items=items, line=token.line, column=token.column)
        if token.kind == "KEYWORD" and token.value == "new":
            self.advance()
            type_name = self.expect_identifier("тип после new")
            return New(type_name=type_name.value, line=token.line, column=token.column)
        if token.kind in ("IDENT", "KEYWORD") and token.value not in HARD_KEYWORDS:
            self.advance()
            return Name(name=token.value, line=token.line, column=token.column)
        raise self.error(f"неожиданный токен {token.describe()}")


def parse(source: str, filename: str = "<string>") -> Program:
    """Lex and parse DimScript source."""

    return Parser(lex(source), filename=filename).parse()
