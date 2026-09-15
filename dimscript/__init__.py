"""DimScript language frontend.

Use :func:`compile_source` for native C generation or
:class:`dimscript.interpreter.Interpreter` for a quick development run.
"""

from .compiler import CCompiler, SemanticError, compile_header, compile_program, compile_source
from .interpreter import Interpreter, run_source
from .parser import ParseError, parse

__all__ = [
    "CCompiler",
    "Interpreter",
    "ParseError",
    "SemanticError",
    "compile_header",
    "compile_program",
    "compile_source",
    "parse",
    "run_source",
]
