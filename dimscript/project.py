"""Multi-file DimScript games.

A game is a folder: ``game.manifest`` plus one or more ``.ds`` files.  Each file
is a *module* whose top level may ``require "other"``.  Modules are linked in
dependency order and merged into a single :class:`~dimscript.ast.Program`, which
is what the checker, the reference interpreter and the C backend consume — the
same rule the runtime interpreter in :file:`src/ds_vm.c` follows, so a game does
not change shape when it is compiled ahead of time.
"""

from __future__ import annotations

from dataclasses import dataclass, field
from pathlib import Path
from typing import Dict, Iterable, List, Optional, Sequence, Tuple

from .ast import GlobalDecl, Node, Program, Require
from .lexer import DimScriptError
from .manifest import MANIFEST_NAME, GameManifest, game_files, read_manifest
from .parser import parse


class ProjectError(DimScriptError):
    """Raised when a game folder cannot be assembled into one program."""


@dataclass
class ScriptFile:
    """One ``.ds`` file of a game."""

    name: str
    source: str
    path: Optional[Path] = None
    program: Program = field(default_factory=lambda: Program(line=1, column=1))

    @property
    def filename(self) -> str:
        return str(self.path) if self.path else f"{self.name}.ds"


@dataclass
class GameProject:
    """A loaded, linked game."""

    directory: Optional[Path]
    manifest: GameManifest
    scripts: List[ScriptFile] = field(default_factory=list)
    program: Program = field(default_factory=lambda: Program(line=1, column=1))

    @property
    def name(self) -> str:
        if self.directory:
            return self.directory.name
        return self.manifest.title or "game"

    def sources(self) -> List[Tuple[str, str]]:
        return [(script.name, script.source) for script in self.scripts]


def _walk(node: object) -> Iterable[Node]:
    yield node
    for value in getattr(node, "__dict__", {}).values():
        if isinstance(value, Node):
            yield from _walk(value)
        elif isinstance(value, (list, tuple)):
            for item in value:
                if isinstance(item, Node):
                    yield from _walk(item)


def _parse(script: ScriptFile) -> None:
    try:
        script.program = parse(script.source, filename=script.filename)
    except DimScriptError as error:
        raise ProjectError(str(error)) from error


def _order(scripts: Sequence[ScriptFile], entry: str = "main") -> List[ScriptFile]:
    """Dependency order: requires first, `main` (when present) as the root."""

    by_name = {script.name: script for script in scripts}
    unknown = [script for script in scripts if _missing_require(script, by_name)]
    if unknown:
        script = unknown[0]
        raise ProjectError(
            f"{script.filename}: require \"{_missing_require(script, by_name)}\": "
            f"нет файла {_missing_require(script, by_name)}.ds в игре"
        )

    roots: List[ScriptFile] = []
    if entry in by_name:
        roots.append(by_name[entry])
    roots.extend(script for script in scripts if script not in roots)

    ordered: List[ScriptFile] = []
    visited: Dict[str, bool] = {}

    def visit(script: ScriptFile, stack: Tuple[str, ...] = ()) -> None:
        if script.name in visited:
            return
        if script.name in stack:
            chain = " -> ".join(list(stack) + [script.name])
            raise ProjectError(f"циклический require: {chain}")
        for module in script.program.requires:
            visit(by_name[module.module], stack + (script.name,))
        visited[script.name] = True
        ordered.append(script)

    for script in roots:
        visit(script)
    return ordered


def _missing_require(script: ScriptFile, by_name: Dict[str, ScriptFile]) -> str:
    for module in script.program.requires:
        if module.module not in by_name:
            return module.module
    return ""


def link_scripts(scripts: Sequence[ScriptFile]) -> Program:
    """Merge parsed scripts into one program, checking for name collisions."""

    program = Program(line=1, column=1)
    seen_structs: Dict[str, str] = {}
    seen_functions: Dict[str, str] = {}
    seen_globals: Dict[str, str] = {}

    for script in _order(list(scripts)):
        _parse(script)
        # Every node remembers its file, so a compiled error or an out-of-range
        # index reports `blocks.ds:47` and not just a line number.
        for node in _walk(script.program):
            setattr(node, "file", script.filename)
        for module in script.program.requires:
            program.requires.append(Require(module=module.module, line=module.line, column=module.column))
        for struct in script.program.structs:
            if struct.name in seen_structs:
                raise ProjectError(
                    f"{script.filename}: struct {struct.name!r} объявлена дважды "
                    f"(второй раз; первый — {seen_structs[struct.name]})"
                )
            seen_structs[struct.name] = script.filename
            program.structs.append(struct)
        for function in script.program.functions:
            if function.name in seen_functions:
                raise ProjectError(
                    f"{script.filename}: функция {function.name!r} объявлена дважды "
                    f"(первый раз в {seen_functions[function.name]})"
                )
            seen_functions[function.name] = script.filename
            program.functions.append(function)
        for declaration in script.program.globals:
            if declaration.name in seen_globals:
                raise ProjectError(
                    f"{script.filename}: глобальная переменная {declaration.name!r} объявлена "
                    f"дважды (первый раз в {seen_globals[declaration.name]})"
                )
            seen_globals[declaration.name] = script.filename
            program.globals.append(declaration)
    return program


def project_from_sources(sources: Iterable[Tuple[str, str]],
                         manifest: Optional[GameManifest] = None,
                         directory: Optional[Path] = None) -> GameProject:
    """Build a project from ``(module name, source)`` pairs.

    Used by the CLI for stdin, by the tests and by anything that already has the
    text of a game in memory.
    """

    scripts = [ScriptFile(name=name, source=source) for name, source in sources]
    if not scripts:
        raise ProjectError("в игре нет ни одного .ds файла")
    project = GameProject(directory=directory, manifest=manifest or GameManifest(), scripts=scripts)
    project.program = link_scripts(scripts)
    return project


def load_project(directory: Path) -> GameProject:
    """Load ``game.manifest`` and every ``.ds`` file of *directory*."""

    directory = Path(directory)
    if not directory.is_dir():
        raise ProjectError(f"{directory}: это не папка игры")
    if not (directory / MANIFEST_NAME).exists():
        raise ProjectError(f"{directory}: нет {MANIFEST_NAME} (папка игры должна описывать себя)")
    manifest = read_manifest(directory)
    paths = game_files(directory, manifest.scripts)
    scripts = [
        ScriptFile(name=path.stem, source=path.read_text(encoding="utf-8"), path=path)
        for path in paths
    ]
    project = GameProject(directory=directory, manifest=manifest, scripts=scripts)
    project.program = link_scripts(scripts)
    listed = set(manifest.scripts)
    unlisted = [script.path.name for script in scripts if script.path and script.path.name not in listed]
    if listed and unlisted:
        # The engine loads these too (after the listed ones); warn, do not fail.
        project.manifest.scripts = list(manifest.scripts) + unlisted
    return project


def game_title(project: GameProject) -> str:
    return project.manifest.title
