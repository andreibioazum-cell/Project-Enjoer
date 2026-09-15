#!/usr/bin/env python3
"""String-temps torture test: every ownership path of the C compiler.

A script that nests concats, converts, compares, stores, returns, passes,
loops and lists fresh strings is compiled to C99, built with the engine and
run — under AddressSanitizer + UBSan when the host compiler accepts them, so
a use-after-free fails loudly and any leak fails LeakSanitizer.  The driver
also checks the recorded texts, which proves the freed-too-early shapes never
corrupt a value a text still needs.
"""

from __future__ import annotations

import os
import shlex
import shutil
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
OUT = ROOT / "build-tests" / "temps"

ENGINE_C = [
    "src/enjoer_draw.c",
    "src/dimscript_runtime.c",
    "src/ds_manifest.c",
    "src/ds_files.c",
    "src/ds_image.c",
    "src/ds_png.c",
    "src/ds_font.c",
    "src/ds_ttf.c",
    "src/core/log.c",
    "src/core/state.c",
]

TORTURE_DS = """\
struct TempsBox {
    kept: string
    count: int
}

box = new TempsBox
tag = "v" .. "1"
tags = ["n0"]
word = "hey"
pair = ["a", "b"]
extras = []

describe(value: string): string {
    return "got=" .. value
}

shout(value: string) {
    log("shout", value .. "!")
}

load() {
    box.kept = "k" .. "ept"
    box.count = 0
    tags.push("n" .. "1")
    tags.insert(0, "i" .. "0")
    extras.push("p" .. "ush")
    shout(tag)
    shout("a" .. "b")
    log("one", 2, "t" .. "hree")
    log(str(num("12" .. "3")))
    local i = 0
    local n = 0
    local w = 0
    local guard = 0
    local acc = 0
    while i < 3 do {
        i = i + 1
    }
    box.count = i
    if ("a" .. "b") == "ab" then {
        box.count = box.count + 10
    } else {
        box.count = -1
    }
    if word[1] == "e" then {
        box.count = box.count + 100
    }
    while ("x" .. n) != "x2" do {
        n = n + 1
    }
    box.count = box.count + n
    if ("z" .. "z") == "ab" then {
        box.count = -999
    } else {
        if ("q" .. "q") == "qq" then {
            box.count = box.count + 10000
        }
    }
    while ("y" .. w) != "done" do {
        w = w + 1
        guard = guard + 1
        if w == 2 then {
            continue
        }
        if guard > 10 then {
            break
        }
    }
    for k = num("1" .. "0"), 12 do {
        acc = acc + k
    }
    box.count = box.count + acc
}

keypressed(name: string) {
    if name == "space" then {
        box.count = box.count + 1000
    }
    log("key", name)
}

touchpressed(id, touch_x, touch_y) {
    box.count = box.count + 1
}

update(dt: float) {
}

draw() {
    render.text("cat=" .. ("a" .. "b"), 10.0, 10.0, 1.0)
    render.text("n=" .. box.count, 10.0, 30.0, 1.0)
    render.text("len=" .. len("abcd" .. "ef"), 10.0, 50.0, 1.0)
    render.text("s=" .. str(7), 10.0, 70.0, 1.0)
    render.text("j=" .. pair.join(","), 10.0, 90.0, 1.0)
    render.text("c=" .. word[1], 10.0, 110.0, 1.0)
    render.text("k=" .. box.kept, 10.0, 130.0, 1.0)
    render.text("t=" .. tag, 10.0, 150.0, 1.0)
    render.text("d=" .. describe("Z"), 10.0, 170.0, 1.0)
    render.text("li=" .. tags[0] .. tags[1], 10.0, 190.0, 1.0)
    render.text("idx=" .. tags.index_of("n" .. "1"), 10.0, 210.0, 1.0)
    tags[2] = "s" .. "et"
    render.text("set=" .. tags[2], 10.0, 230.0, 1.0)
    render.text("fl=" .. extras.join("+"), 10.0, 250.0, 1.0)
}

quit() {
    delete box
}
"""

# box.count: 3 (i-loop) + 10 ("ab") + 100 (char) + 2 (x-loop) + 10000 ("qq")
# + 33 (for k = 10..12) + 1000 (space) + 1 (tap) = 11149.  The driver below
# asserts every text, so the count above is checked, not just hoped for.
DRIVER_C = """\
/* String-temps driver: runs the torture callbacks and checks the texts. */
#include "enjoer_draw.h"
#include "dimscript_runtime.h"
#include "temps_game.h"

#include <stdio.h>
#include <string.h>

static int failures = 0;

static void check_text(const char *needle) {
    const EnjoerFrame *frame = enjoer_frame();
    int index = 0;
    for (index = 0; index < frame->text_count; ++index)
        if (!strcmp(frame->texts[index].text, needle)) return;
    fprintf(stderr, "temps: missing text '%s'\\n", needle);
    failures = 1;
}

int main(void) {
    ds_runtime_init();
    dimscript_init();
    dimscript_load();
    enjoer_frame_begin(320, 240);
    dimscript_keypressed("space");
    dimscript_keyreleased("space");
    dimscript_touchpressed(1, 10.0f, 20.0f);
    dimscript_touchreleased(1, 10.0f, 20.0f);
    dimscript_update(0.016f);
    dimscript_draw();
    dimscript_update(0.016f);
    dimscript_draw();
    check_text("cat=ab");
    check_text("n=11149");
    check_text("len=6");
    check_text("s=7");
    check_text("j=a,b");
    check_text("c=e");
    check_text("k=kept");
    check_text("t=v1");
    check_text("d=got=Z");
    check_text("li=i0n0");
    check_text("idx=2");
    check_text("set=set");
    check_text("fl=push");
    dimscript_quit();
    dimscript_shutdown();
    if (!failures) puts("TEMPS-OK");
    return failures;
}
"""


class Failure(Exception):
    pass


def run(command: list[str]) -> str:
    process = subprocess.run(command, cwd=ROOT, capture_output=True, text=True)
    if process.returncode != 0:
        tail = (process.stderr or process.stdout).strip().splitlines()[-25:]
        raise Failure(f"{' '.join(shlex.quote(part) for part in command)}\n" + "\n".join(tail))
    return process.stdout


def compiler() -> list[str]:
    from_env = os.environ.get("ENJOER_CC") or os.environ.get("CC")
    if from_env:
        return shlex.split(from_env)
    if shutil.which("clang"):
        return ["clang"]
    probe = subprocess.run([sys.executable, "-m", "ziglang", "cc", "--version"], capture_output=True)
    if probe.returncode == 0:
        return [sys.executable, "-m", "ziglang", "cc"]
    return ["cc"]


def sanitize_flags(cc: list[str]) -> list[str]:
    """Sanitizers from run.sh (ASAN=1), else probed, else plain with a note."""
    forced = os.environ.get("ENJOER_SANITIZE")
    if forced is not None:
        return shlex.split(forced)
    probe = subprocess.run(
        [*cc, "-fsanitize=address,undefined", "-x", "c", "-", "-o", os.devnull],
        input="int main(void) { return 0; }\n",
        capture_output=True,
        text=True,
        cwd=ROOT,
    )
    if probe.returncode == 0:
        return ["-fsanitize=address,undefined", "-fno-omit-frame-pointer"]
    print("temps: host compiler has no sanitizers, running plain", file=sys.stderr)
    return []


def main() -> int:
    sys.path.insert(0, str(ROOT))
    OUT.mkdir(parents=True, exist_ok=True)

    from dimscript.compiler import CCompiler
    from dimscript.parser import parse

    program = parse(TORTURE_DS, filename="temps_game.ds")
    backend = CCompiler(program, filename="temps_game.ds")
    c_source = backend.compile()
    (OUT / "temps_game.c").write_text(c_source, encoding="utf-8")
    (OUT / "temps_game.h").write_text(backend.header("TEMPS_GAME_H"), encoding="utf-8")
    (OUT / "temps_driver.c").write_text(DRIVER_C, encoding="utf-8")

    # Codegen shape: every statement temp is declared once and freed once, the
    # materialized checks fired, and both while shapes coexist.
    declarations = c_source.count("DsString *__ds_t")
    releases = c_source.count("ds_release((void*)__ds_t")
    if declarations == 0 or declarations != releases:
        raise Failure(f"temps are unbalanced: {declarations} declared, {releases} freed")
    for shape in ("for (;;)", "int __ds_c", "while (", "ds_string_dup(",
                  "ds_string_replace(", "ds_string_dtor"):
        if shape not in c_source:
            raise Failure(f"missing codegen shape: {shape!r}")

    cc = compiler()
    flags = ["-std=c99", "-O1", "-g", "-Wall", "-Wextra", "-Werror", "-I./src", f"-I{OUT}"]
    sanitizers = sanitize_flags(cc)
    flags += sanitizers
    run([*cc, *flags, "-o", str(OUT / "temps_run"), str(OUT / "temps_driver.c"),
         str(OUT / "temps_game.c"), *ENGINE_C, "-lm"])
    output = run([str(OUT / "temps_run")])
    if "TEMPS-OK" not in output.splitlines():
        raise Failure(f"driver failed:\n{output}")
    mode = "ASan+UBSan-clean" if sanitizers else "plain build, 13 texts exact"
    print(f"PASS string temps torture ({declarations} temps, {mode})")
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except Failure as failure:
        print(f"FAIL temps: {failure}", file=sys.stderr)
        raise SystemExit(1)
