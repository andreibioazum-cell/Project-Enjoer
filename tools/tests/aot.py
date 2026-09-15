#!/usr/bin/env python3
"""The ahead-of-time path must draw exactly what the interpreter draws.

    python3 tools/tests/aot.py

`examples/shapes.ds` is written in the subset the C backend supports (no lists),
so the same source can be:

  * linked into a binary that runs it on the VM in `src/ds_vm.c`, and
  * compiled to C99 and linked into a binary that runs the generated code.

Both binaries print the frame they recorded; this script diffs them.  A missing
runtime symbol, a renamed builtin or an argument that reaches the batch at a
different coordinate fails here rather than on a device.
"""

from __future__ import annotations

import os
import shlex
import shutil
import subprocess
import sys
import tempfile
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
    "src/core/log.c",
    "src/core/state.c",
]
VM_C = ["src/ds_vm.c", "src/ds_vm_heap.c", "src/ds_vm_lang.c", "src/ds_vm_exec.c"]


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
    if not OUT.exists():
        OUT.mkdir(parents=True)

    from dimscript.compiler import CCompiler, SemanticModel  # noqa: F401
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
    # The generated code is compiled with the same strictness the engine uses,
    # so a backend that emits an unused variable or a wrong prototype fails.
    flags = ["-std=c99", "-O3", "-Wall", "-Wextra", "-Werror", "-I./src"]

    # 1. the interpreter binary (the VM reads the folder like the engine does)
    run([*cc, *flags, "-o", str(OUT / "vm_frames"), "tools/tests/frame_dump.c", *VM_C, *ENGINE_C, "-lm"])
    # 2. the generated-C binary: same recording path, no VM at all
    run([*cc, *flags, "-DENJOER_AOT_DRIVER=1", f"-I{OUT}",
         "-o", str(OUT / "aot_frames"), "tools/tests/frame_dump.c", str(OUT / "shapes.c"),
         *ENGINE_C, "-lm"])

    vm = run([str(OUT / "vm_frames"), str(game), "4", "0.016666666666666666"])
    aot = run([str(OUT / "aot_frames"), str(game), "4", "0.016666666666666666"])

    def useful(text: str) -> list[str]:
        return [line for line in text.splitlines() if line.strip() and not line.startswith("title ")]

    vm_lines, aot_lines = useful(vm), useful(aot)
    if vm_lines != aot_lines:
        print("FAIL DimScript AOT: generated C and the VM drew different frames", file=sys.stderr)
        shown = 0
        for left, right in zip(vm_lines, aot_lines):
            if left != right:
                print(f"  vm: {left}\n  c:  {right}", file=sys.stderr)
                shown += 1
                if shown >= 6:
                    break
        print(f"  ({len(vm_lines)} vs {len(aot_lines)} lines; see build-tests/aot/shapes.c)", file=sys.stderr)
        return 1
    vertices = next(line.split(" ", 1)[1] for line in vm_lines if line.startswith("vertices "))
    print(f"PASS DimScript AOT: generated C matched the VM ({vertices} vertices, lists excluded by design)")
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except Failure as error:
        print(f"FAIL DimScript AOT: {error}", file=sys.stderr)
        raise SystemExit(1)
