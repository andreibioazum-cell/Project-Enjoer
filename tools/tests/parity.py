#!/usr/bin/env python3
"""The two DimScript interpreters must not disagree.

``src/ds_vm.c`` is the runtime that ships; :mod:`dimscript.interpreter` is the
reference used by tooling and by ``dimscriptc --run``.  This test plays the same
game on both and diffs the recorded frame — every vertex of every shape and
every ``render.text`` call — so a change to one implementation that the other
does not follow fails here instead of on a device.

Run through ``tools/tests/run.sh``, which builds ``build-tests/frame_dump``
first.  Without that binary the test reports SKIP rather than silently passing.
"""

from pathlib import Path
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT))

from dimscript.interpreter import Interpreter  # noqa: E402
from dimscript.project import load_project  # noqa: E402

FRAMES = 4
DT = 1.0 / 60.0
DUMP = ROOT / "build-tests" / "frame_dump"
GAME = ROOT / "games" / "brick"


def fixed(value: float) -> int:
    return int(round(float(value) * 1000.0))


def python_dump(game_dir: Path) -> str:
    project = load_project(game_dir)
    interpreter = Interpreter(project.program, filename=str(game_dir))
    interpreter.engine.resize(640, 360)
    interpreter.initialize()
    interpreter.load()
    interpreter.resized(640.0, 360.0)
    for _ in range(FRAMES):
        interpreter.update(DT)
        interpreter.draw()
    frame = interpreter.frame
    lines = [
        f"title {project.manifest.title}",
        "size 640 360",
        f"frames {FRAMES}",
        f"vertices {len(frame.vertices)}",
        "clear {} {} {}".format(*(fixed(value) for value in frame.clear_color)),
    ]
    for index, vertex in enumerate(frame.vertices):
        lines.append("v {} {} {} {} {} {}".format(
            index, fixed(vertex.x), fixed(vertex.y), fixed(vertex.r), fixed(vertex.g), fixed(vertex.b)))
    lines.append(f"texts {len(frame.texts)}")
    for index, text in enumerate(frame.texts):
        lines.append("t {} {} {} {} {} {} {} {}".format(
            index, fixed(text.x), fixed(text.y), fixed(text.scale), fixed(text.color[0]),
            fixed(text.color[1]), fixed(text.color[2]), text.text))
    digest = 2166136261
    for vertex in frame.vertices:
        for value in (vertex.x, vertex.y, vertex.r, vertex.g, vertex.b):
            digest = ((digest ^ fixed(value)) * 16777619) & 0xFFFFFFFFFFFFFFFF
    lines.append(f"hash {digest}")
    lines.append("error ")
    return "\n".join(lines)


def main() -> int:
    if not DUMP.exists():
        print(f"SKIP DimScript parity: {DUMP.relative_to(ROOT)} not built (run tools/tests/run.sh)")
        return 0
    native = subprocess.run(
        [str(DUMP), str(GAME), str(FRAMES), repr(DT)],
        cwd=ROOT, capture_output=True, text=True,
    )
    if native.returncode != 0:
        print(f"FAIL DimScript parity: frame_dump exited {native.returncode}\n{native.stderr}", file=sys.stderr)
        return 1
    reference = python_dump(GAME)
    native_lines = [line for line in native.stdout.splitlines() if line.strip()]
    reference_lines = [line for line in reference.splitlines() if line.strip()]
    if native_lines != reference_lines:
        print("FAIL DimScript parity: the native VM and the reference interpreter differ", file=sys.stderr)
        shown = 0
        for native_line, reference_line in zip(native_lines, reference_lines):
            if native_line != reference_line:
                print(f"  vm:  {native_line}\n  py:  {reference_line}", file=sys.stderr)
                shown += 1
                if shown >= 8:
                    break
        print(f"  ({len(native_lines)} lines from the VM, {len(reference_lines)} from python)", file=sys.stderr)
        return 1
    vertices = next(line for line in native_lines if line.startswith("vertices ")).split(" ")[1]
    print(f"PASS DimScript parity: VM and reference interpreter drew the same {vertices} vertices")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
