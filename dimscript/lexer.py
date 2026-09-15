"""Lexer for the DimScript source language."""

from __future__ import annotations

from dataclasses import dataclass
from typing import List


class DimScriptError(Exception):
    """Base class for user-facing compiler errors."""


class LexError(DimScriptError):
    pass


@dataclass(frozen=True)
class Token:
    kind: str
    value: str
    line: int
    column: int

    def describe(self) -> str:
        if self.kind == "EOF":
            return "конец файла"
        return repr(self.value)


# Longest operators must be checked first.  ``..`` is DimScript's string
# concatenation operator and is intentionally not a range operator.
_OPERATORS = (
    ">=",
    "<=",
    "==",
    "!=",
    "~=",
    "..",
    "&&",
    "||",
    "=>",
)
_SINGLE = set("{}()[]:,.=+-*/%<>!")
# `count`, `end`, `do` and friends stay usable as names: only the keywords the
# grammar actually needs are hard reserved, which keeps game code readable.
_KEYWORDS = {
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
    "while",
    "for",
    "do",
    "end",
    "break",
    "continue",
    "require",
    "local",
}

# Keywords that may still be used as identifiers or field names.
SOFT_KEYWORDS = {"then", "do", "end", "require", "local", "for", "struct"}


def _decode_entities(source: str) -> str:
    """Accept HTML-escaped comparison operators copied from a web page.

    The example in the original request contains ``&amp;gt;``/``&amp;lt;``
    after one extra HTML escaping pass.  These replacements are done before
    lexing so both the written example and normal source behave identically.
    They are deliberately limited to operator entities rather than applying
    HTML decoding to string literals.
    """

    replacements = (
        ("&amp;gt;", ">"),
        ("&amp;lt;", "<"),
        ("&gt;", ">"),
        ("&lt;", "<"),
    )
    for old, new in replacements:
        source = source.replace(old, new)
    return source


def _decode_string(raw: str, line: int, column: int) -> str:
    result: List[str] = []
    index = 0
    escapes = {
        "n": "\n",
        "r": "\r",
        "t": "\t",
        "0": "\0",
        "\\": "\\",
        '"': '"',
        "'": "'",
    }
    while index < len(raw):
        char = raw[index]
        if char != "\\":
            result.append(char)
            index += 1
            continue
        index += 1
        if index >= len(raw):
            raise LexError(f"{line}:{column}: незакрытая escape-последовательность в строке")
        escaped = raw[index]
        if escaped in escapes:
            result.append(escapes[escaped])
            index += 1
        elif escaped == "u" and index + 4 < len(raw):
            digits = raw[index + 1 : index + 5]
            try:
                result.append(chr(int(digits, 16)))
            except ValueError as exc:
                raise LexError(f"{line}:{column}: неверный unicode escape \\u{digits}") from exc
            index += 5
        else:
            # Keeping unknown escapes literal makes the language friendly for
            # paths and future syntax while still retaining the user's text.
            result.append(escaped)
            index += 1
    return "".join(result)


def lex(source: str) -> List[Token]:
    """Tokenize *source* and return a list ending in an EOF token."""

    source = _decode_entities(source)
    tokens: List[Token] = []
    index = 0
    line = 1
    column = 1
    length = len(source)

    def advance(count: int = 1) -> None:
        nonlocal index, line, column
        for _ in range(count):
            if index >= length:
                return
            if source[index] == "\n":
                line += 1
                column = 1
            else:
                column += 1
            index += 1

    while index < length:
        char = source[index]

        if char in " \t\r\f\v":
            advance()
            continue
        if char == "\n":
            tokens.append(Token("NEWLINE", "\n", line, column))
            advance()
            continue

        # Lua-style comments are part of the public syntax.  A C++-style
        # comment is accepted as a small convenience for generated examples.
        if source.startswith("--", index) or source.startswith("//", index):
            while index < length and source[index] != "\n":
                advance()
            continue

        start_line, start_column = line, column

        if char in "\"'":
            quote = char
            advance()
            raw: List[str] = []
            closed = False
            while index < length:
                current = source[index]
                if current == quote:
                    closed = True
                    advance()
                    break
                if current == "\n":
                    raise LexError(
                        f"{start_line}:{start_column}: строковый литерал не может переноситься на новую строку"
                    )
                if current == "\\" and index + 1 < length:
                    raw.append(current)
                    advance()
                    raw.append(source[index])
                    advance()
                else:
                    raw.append(current)
                    advance()
            if not closed:
                raise LexError(f"{start_line}:{start_column}: незакрытая строка")
            tokens.append(Token("STRING", _decode_string("".join(raw), start_line, start_column), start_line, start_column))
            continue

        if char.isdigit() or (char == "." and index + 1 < length and source[index + 1].isdigit()):
            start = index
            saw_dot = char == "."
            advance()
            while index < length and source[index].isdigit():
                advance()
            if index < length and source[index] == "." and not saw_dot and not source.startswith("..", index):
                saw_dot = True
                advance()
                while index < length and source[index].isdigit():
                    advance()
            if index < length and source[index] in "eE":
                exponent_index = index
                advance()
                if index < length and source[index] in "+-":
                    advance()
                exponent_digits = index
                while index < length and source[index].isdigit():
                    advance()
                if exponent_digits == index:
                    # Leave the e in the input to get a useful identifier
                    # error rather than silently accepting a malformed number.
                    index = exponent_index
                    column -= index - exponent_index
            value = source[start:index]
            tokens.append(Token("NUMBER", value, start_line, start_column))
            continue

        if char.isalpha() or char == "_":
            start = index
            advance()
            while index < length and (source[index].isalnum() or source[index] == "_"):
                advance()
            value = source[start:index]
            tokens.append(Token("KEYWORD" if value in _KEYWORDS else "IDENT", value, start_line, start_column))
            continue

        matched = False
        for operator in _OPERATORS:
            if source.startswith(operator, index):
                tokens.append(Token("SYMBOL", operator, start_line, start_column))
                advance(len(operator))
                matched = True
                break
        if matched:
            continue
        if char in _SINGLE:
            tokens.append(Token("SYMBOL", char, start_line, start_column))
            advance()
            continue

        raise LexError(f"{start_line}:{start_column}: неизвестный символ {char!r}")

    tokens.append(Token("EOF", "", line, column))
    return tokens
