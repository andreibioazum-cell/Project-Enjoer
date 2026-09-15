#!/usr/bin/env python3
"""Small dependency-free regression test for the DimScript frontend."""

from pathlib import Path
import os
import sys

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT))
# The generated C embeds script paths, so the project loads through the same
# relative name the regen command uses (see below), from anywhere.
os.chdir(ROOT)

from dimscript.compiler import CCompiler  # noqa: E402
from dimscript.interpreter import Interpreter  # noqa: E402
from dimscript.project import load_project  # noqa: E402


# games/clicker is the single source of the shipped game: the same folder the
# APK, the preview and --run execute, and src/generated/clicker.c is a byte
# for byte copy of what the compiler below produces.  Regen with:
#   python3 tools/dimscriptc.py games/clicker --emit-c \
#       -o src/generated/clicker.c --header src/generated/clicker.h
project = load_project(Path("games/clicker"))
backend = CCompiler(project.program, filename="games/clicker")
c_source = backend.compile()
c_header = backend.header()
assert "typedef struct ClickerGame" in c_source
# Fresh strings consumed in place are hoisted into statement temps (__ds_tN)
# declared above the call and released right below it.
assert "= ds_concat(" in c_source
assert "ds_release((void*)__ds_t" in c_source
assert "ds_font_load(" in c_source
assert "render.font" not in c_source and "ds_render_font(" in c_source
assert "void dimscript_touchpressed" in c_source
assert "void dimscript_keypressed" in c_source
assert (ROOT / "src" / "generated" / "clicker.c").read_text(encoding="utf-8") == c_source
assert (ROOT / "src" / "generated" / "clicker.h").read_text(encoding="utf-8") == c_header

interpreter = Interpreter.from_game(Path("games/clicker"))
interpreter.load()
assert interpreter.globals["game"].fields["font"] == 0
interpreter.touchpressed(3, 10.0, 20.0)
assert interpreter.globals["game"].fields["score"] == 1
assert interpreter.globals["game"].fields["best"] == 1
interpreter.update(1.0 / 60.0)
commands = interpreter.draw()
assert any(command.text == "Счет: 1" for command in commands)
assert any(command.text == "Рекорд: 1" for command in commands)
assert all(command.font == 0 for command in commands)
assert interpreter.globals["game"].fields["text_scale"] < 1.6
print("PASS DimScript parser + interpreter + C compiler")
