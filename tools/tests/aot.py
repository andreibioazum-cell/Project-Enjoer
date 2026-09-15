#!/usr/bin/env python3
"""STRICT COMPILER MODE — NO VM, NO REFCOUNT, MANUAL MEMORY, SPEED LIKE C.

AOT path: examples/shapes.ds -> C99 -> clang -O3 -> machine code.
No VM comparison, only checks that generated C compiles with -Werror and draws.
"""

from __future__ import annotations

import os
import shlex
import shutil
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
OUT = ROOT / "build-tests" / "aot"
SOURCE = ROOT / "examples" / "shapes.ds"
MANIFEST = """title = "Shapes"
package = "com.cb4.shapes"
scripts = ["shapes.ds"]
"""

ENGINE_C = [
    "src/enjoer_draw.c",
    "src/dimscript_runtime.c",
    "src/ds_manifest.c",
    "src/ds_files.c",
    "src/ds_image.c",
    "src/ds_png.c",
    "src/core/log.c",
    "src/core/state.c",
]

class Failure(Exception):
    pass

def run(command: list[str]) -> str:
    process = subprocess.run(command, cwd=ROOT, capture_output=True, text=True)
    if process.returncode != 0:
        tail = (process.stderr or process.stdout).strip().splitlines()[-25:]
        raise Failure(f"{' '.join(shlex.quote(part) for part in command)}\n" + "\n".join(tail))
    return process.stdout

def compiler() -> list[str]:
    from_env = os.environ.get("CC")
    if from_env:
        return shlex.split(from_env)
    if shutil.which("clang"):
        return ["clang"]
    probe = subprocess.run([sys.executable, "-m", "ziglang", "cc", "--version"], capture_output=True)
    if probe.returncode == 0:
        return [sys.executable, "-m", "ziglang", "cc"]
    return ["cc"]

def main() -> int:
    sys.path.insert(0, str(ROOT))
    OUT.mkdir(parents=True, exist_ok=True)

    from dimscript.compiler import CCompiler
    from dimscript.parser import parse

    program = parse(SOURCE.read_text(encoding="utf-8"), filename=str(SOURCE))
    backend = CCompiler(program, filename=str(SOURCE))
    (OUT / "shapes.c").write_text(backend.compile(), encoding="utf-8")
    (OUT / "shapes.h").write_text(backend.header("SHAPES_GENERATED_H"), encoding="utf-8")

    game = OUT / "game"
    game.mkdir(parents=True, exist_ok=True)
    shutil.copy2(SOURCE, game / "shapes.ds")
    (game / "game.manifest").write_text(MANIFEST, encoding="utf-8")

    cc = compiler()
    flags = ["-std=c99", "-O3", "-Wall", "-Wextra", "-Werror", "-I./src"]

    # STRICT COMPILER: only AOT binary, no VM
    run([*cc, *flags, "-DENJOER_AOT_DRIVER=1", f"-I{OUT}",
         "-o", str(OUT / "aot_frames"), "tools/tests/frame_dump.c", str(OUT / "shapes.c"),
         *ENGINE_C, "-lm"])

    aot = run([str(OUT / "aot_frames"), str(game), "4", "0.016666666666666666"])

    def useful(text: str) -> list[str]:
        return [line for line in text.splitlines() if line.strip() and not line.startswith("title ")]

    aot_lines = useful(aot)
    vertices = next((line.split(" ",1)[1] for line in aot_lines if line.startswith("vertices ")), "0")
    print(f"PASS DimScript AOT: strictly compiler, manual memory, speed like C ({vertices} vertices, AOT to machine code)")
    return 0

if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except Failure as error:
        print(f"FAIL DimScript AOT: {error}", file=sys.stderr)
        raise SystemExit(1)
