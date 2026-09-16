#!/usr/bin/env python3
"""Regression test for the Cubic Battle DimScript frontend and AOT output."""

from pathlib import Path
import sys

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT))
os.chdir(ROOT)

from dimscript.compiler import CCompiler  # noqa: E402
from dimscript.interpreter import Interpreter  # noqa: E402
from dimscript.project import load_project  # noqa: E402


# The repository has one game. Its source, interpreter preview, APK assets and
# checked-in AOT output all come from this same directory. Regenerate with:
#   python3 tools/dimscriptc.py game --emit-c \
#       -o src/generated/cubicbattle.c --header src/generated/cubicbattle.h
project = load_project(Path("game"))
backend = CCompiler(project.program, filename="game")
c_source = backend.compile()
c_header = backend.header()
assert "typedef struct Palette" in c_source
assert "typedef struct Player" in c_source
assert "ds_font_load(" in c_source
assert "render.font" not in c_source and "ds_render_font(" in c_source
assert "void dimscript_touchpressed" in c_source
assert "void dimscript_keypressed" in c_source
assert (ROOT / "src" / "generated" / "cubicbattle.c").read_text(encoding="utf-8") == c_source
assert (ROOT / "src" / "generated" / "cubicbattle.h").read_text(encoding="utf-8") == c_header

interpreter = Interpreter.from_game(Path("game"))
interpreter.load()
assert interpreter.globals["game_font"] == 0
commands = interpreter.draw()
assert any(command.text == "Cubic Battle 4" for command in commands)
assert any(command.text == "WARNING" for command in commands)
assert all(command.font == 0 for command in commands)

# Input and resize are accepted while the warning is on screen. The warning is
# deliberately modal, so a tap cannot accidentally start a battle underneath it.
interpreter.touchpressed(3, 10.0, 20.0)
assert interpreter.globals["game_state"] == 0
interpreter.resized(960.0, 540.0)
resized = interpreter.draw()
assert any(command.text == "Cubic Battle 4" for command in resized)
print("PASS DimScript parser + interpreter + Cubic Battle AOT compiler")
