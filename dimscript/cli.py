"""Command line interface for the DimScript compiler and interpreter.

    dimscriptc examples/clicker.ds --check          # one script
    dimscriptc games/brick --run --frames 3          # a whole game folder
    dimscriptc games/brick -o game.c --header game.h

A directory argument is a *game*: ``game.manifest`` plus ``.ds`` files, linked
in ``require`` order and handed to the same frontend as a single file.
"""

from __future__ import annotations

import argparse
import json
import sys
from pathlib import Path
from typing import Optional, Sequence

from .compiler import CCompiler, compile_program, compile_source
from .interpreter import Interpreter
from .lexer import DimScriptError
from .parser import parse
from .project import load_project, project_from_sources


def _read_source(path: str) -> tuple[str, str]:
    if path == "-":
        return sys.stdin.read(), "<stdin>"
    file_path = Path(path)
    return file_path.read_text(encoding="utf-8"), str(file_path)


def _write(path: str, text: str) -> None:
    if path == "-":
        sys.stdout.write(text)
    else:
        output = Path(path)
        output.parent.mkdir(parents=True, exist_ok=True)
        output.write_text(text, encoding="utf-8")


def build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(
        prog="dimscriptc",
        description="DimScript — translate a game script (or a whole game folder) "
                    "to native C, or run its reference interpreter.",
    )
    parser.add_argument("source", help=".ds файл, папка игры или - для stdin")
    mode = parser.add_mutually_exclusive_group()
    mode.add_argument("--check", action="store_true", help="разобрать и проверить типы, ничего не записывая")
    mode.add_argument("--run", action="store_true", help="прогнать колбэки в эталонном интерпретаторе")
    mode.add_argument("--manifest", action="store_true", help="показать game.manifest в JSON")
    parser.add_argument("--emit-c", action="store_true", help="сгенерировать C (режим по умолчанию)")
    parser.add_argument("-o", "--output", default=None, help="куда положить C; '-' печатает в stdout")
    parser.add_argument("--header", default=None, help="и тоже записать заголовок ABI")
    parser.add_argument("--print-c", action="store_true", help="напечатать сгенерированный C")
    parser.add_argument("--click", type=int, default=0, help="тапов в режиме --run")
    parser.add_argument("--frames", type=int, default=1, help="кадров update/draw в режиме --run")
    parser.add_argument("--dt", type=float, default=1.0 / 60.0, help="delta time для --run")
    parser.add_argument("--width", type=float, default=640.0, help="ширина экрана для --run")
    parser.add_argument("--height", type=float, default=360.0, help="высота экрана для --run")
    return parser


def _load(source_argument: str) -> tuple[object, str]:
    """Return ``(program, filename)``, reading a folder as one linked program."""

    path = Path(source_argument)
    if source_argument != "-" and path.is_dir():
        project = load_project(path)
        return project, str(path)
    source, filename = _read_source(source_argument)
    if source_argument == "-":
        # stdin has no folder to read a manifest from, so the stream is one
        # module named `main`, which is what `require "main"` expects.
        project = project_from_sources([("main", source)])
        return project, filename
    from .parser import parse as _parse

    return _parse(source, filename=filename), filename


def _run_interpreter(program, filename: str, clicks: int, frames: int, dt: float,
                     width: float, height: float) -> int:
    interpreter = Interpreter(program, filename=filename)
    interpreter.engine.resize(int(width), int(height))
    interpreter.initialize()
    interpreter.load()
    interpreter.resized(width, height)
    for index in range(max(0, clicks)):
        interpreter.touchpressed(index % 4, width * 0.5, height * 0.5)
    commands = []
    for _ in range(max(0, frames)):
        interpreter.update(dt)
        commands = interpreter.draw()
    result = {
        "mode": "interpreter",
        "font_renderer": "disabled",
        "clicks": max(0, clicks),
        "frames": max(0, frames),
        "vertices": len(interpreter.frame.vertices),
        "text_commands": [
            {
                "text": command.text,
                "x": command.x,
                "y": command.y,
                "scale": command.scale,
                "color": list(command.color),
            }
            for command in commands
        ],
    }
    # Expose simple scalar state without imposing a reflection API on scripts.
    game = interpreter.globals.get("game")
    if getattr(game, "fields", None) is not None:
        result["game"] = {name: _plain(value) for name, value in game.fields.items()}
    print(json.dumps(result, ensure_ascii=False, indent=2))
    interpreter.shutdown()
    return 0


def _plain(value: object) -> object:
    if isinstance(value, (int, float, str, bool)) or value is None:
        return value
    if isinstance(value, list):
        return [_plain(item) for item in value]
    return Interpreter.to_string(value)


def main(argv: Optional[Sequence[str]] = None) -> int:
    args = build_parser().parse_args(argv)
    try:
        loaded, filename = _load(args.source)
        manifest = getattr(loaded, "manifest", None)
        if args.manifest:
            if manifest is None:
                print("dimscriptc: error: заголовок game.manifest есть только у папки игры", file=sys.stderr)
                return 2
            print(json.dumps(manifest.as_dict(), ensure_ascii=False, indent=2))
            return 0
        program = loaded if hasattr(loaded, "functions") else loaded.program

        if args.run:
            return _run_interpreter(program, filename, args.click, args.frames, args.dt,
                                     args.width, args.height)

        if args.check:
            from .compiler import Analyzer

            model = Analyzer(program).analyze()
            note = " (папка игры)" if manifest is not None else ""
            print(f"OK: {filename}{note} ({len(model.structs)} struct(s), "
                  f"{len(model.function_params)} function(s))")
            return 0

        if manifest is not None:
            c_source, model = compile_program(program, filename=filename)
        else:
            source, filename = _read_source(args.source)
            c_source, model = compile_source(source, filename=filename)
        if args.output:
            _write(args.output, c_source)
        elif not args.print_c:
            # A useful default for shell pipelines: no hidden generated file,
            # just C on stdout.
            sys.stdout.write(c_source)
        if args.header:
            _write(args.header, CCompiler(program, filename=filename).header())
        if args.print_c:
            sys.stdout.write(c_source)
        return 0
    except (OSError, UnicodeError, DimScriptError, ValueError) as error:
        print(f"dimscriptc: error: {error}", file=sys.stderr)
        return 2


if __name__ == "__main__":
    raise SystemExit(main())
