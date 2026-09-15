"""DimScript language frontend.

Use :func:`compile_source` for native C generation or
:class:`dimscript.interpreter.Interpreter` for a quick development run.
"""

from .compiler import (CCompiler, SemanticError, compile_header, compile_program,
                       compile_source)
from .interpreter import Interpreter, run_source
from .manifest import ManifestError, read_manifest
from .parser import ParseError, parse
from .project import GameProject, link_scripts, load_project, project_from_sources

__all__ = [
    "CCompiler",
    "GameProject",
    "Interpreter",
    "ManifestError",
    "ParseError",
    "SemanticError",
    "compile_header",
    "compile_program",
    "compile_source",
    "link_scripts",
    "run_source",
    "load_project",
    "parse",
    "project_from_sources",
    "read_manifest",
]
