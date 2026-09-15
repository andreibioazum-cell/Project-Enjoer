"""Command line interface for the DimScript compiler and interpreter."""

from __future__ import annotations

import argparse
import json
import sys
from pathlib import Path
from typing import Optional, Sequence

from .compiler import CCompiler, compile_source
from .interpreter import Interpreter
from .lexer import DimScriptError
from .parser import parse


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
        description="DimScript — translate a small game script to native C or run its reference interpreter.",
    )
    parser.add_argument("source", help=".ds source file, or - for stdin")
    mode = parser.add_mutually_exclusive_group()
    mode.add_argument("--check", action="store_true", help="parse and type-check without writing C")
    mode.add_argument("--run", action="store_true", help="run callbacks in the Python reference interpreter")
    parser.add_argument("--emit-c", action="store_true", help="emit native C (the default compile mode)")
    parser.add_argument("-o", "--output", default=None, help="output C file; '-' writes to stdout")
    parser.add_argument("--header", default=None, help="also write the generated C ABI header")
    parser.add_argument("--print-c", action="store_true", help="print generated C after compiling")
    parser.add_argument("--click", type=int, default=0, help="touches to send in --run mode (default: 0)")
    parser.add_argument("--frames", type=int, default=1, help="update/draw frames in --run mode")
    parser.add_argument("--dt", type=float, default=1.0 / 60.0, help="delta time used by --run")
    return parser


def _run_interpreter(source: str, filename: str, clicks: int, frames: int, dt: float) -> int:
    interpreter = Interpreter.from_source(source, filename=filename)
    interpreter.initialize()
    interpreter.load()
    for _ in range(max(0, clicks)):
        interpreter.touchpressed(0, 0.0, 0.0)
    for _ in range(max(0, frames)):
        interpreter.update(dt)
        commands = interpreter.draw()
    result = {
        "mode": "interpreter",
        "font_renderer": "disabled",
        "clicks": max(0, clicks),
        "frames": max(0, frames),
        "text_commands": [
            {
                "text": command.text,
                "x": command.x,
                "y": command.y,
                "scale": command.scale,
                "color": list(command.color),
            }
            for command in (commands if frames > 0 else interpreter.draw())
        ],
    }
    # Expose simple scalar state without imposing a reflection API on scripts.
    game = interpreter.globals.get("game")
    if getattr(game, "fields", None) is not None:
        result["game"] = dict(game.fields)
    print(json.dumps(result, ensure_ascii=False, indent=2))
    interpreter.shutdown()
    return 0


def main(argv: Optional[Sequence[str]] = None) -> int:
    args = build_parser().parse_args(argv)
    try:
        source, filename = _read_source(args.source)
        if args.run:
            return _run_interpreter(source, filename, args.click, args.frames, args.dt)

        c_source, model = compile_source(source, filename=filename)
        if args.check:
            print(f"OK: {filename} ({len(model.structs)} struct(s), {len(model.function_params)} function(s))")
            return 0

        if args.output:
            _write(args.output, c_source)
        elif not args.print_c:
            # A useful default for shell pipelines: no hidden generated file,
            # just C on stdout.
            sys.stdout.write(c_source)
        if args.header:
            program = parse(source, filename=filename)
            _write(args.header, CCompiler(program, filename=filename).header())
        if args.print_c:
            sys.stdout.write(c_source)
        return 0
    except (OSError, UnicodeError, DimScriptError, ValueError) as error:
        print(f"dimscriptc: error: {error}", file=sys.stderr)
        return 2


if __name__ == "__main__":
    raise SystemExit(main())
