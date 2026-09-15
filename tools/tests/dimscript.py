#!/usr/bin/env python3
"""Small dependency-free regression test for the DimScript frontend."""

from pathlib import Path
import sys

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT))

from dimscript.compiler import compile_source  # noqa: E402
from dimscript.interpreter import Interpreter  # noqa: E402


source = (ROOT / "examples" / "clicker.ds").read_text(encoding="utf-8")
c_source, model = compile_source(source, filename="examples/clicker.ds")
assert "typedef struct ClickerGame" in c_source
assert "ds_render_text(ds_concat(\"Счет: \"" in c_source
assert "void dimscript_touchpressed" in c_source
assert model.global_types["game"].is_struct
assert (ROOT / "src" / "generated" / "clicker.c").read_text(encoding="utf-8") == c_source

interpreter = Interpreter.from_source(source, filename="examples/clicker.ds")
interpreter.load()
interpreter.touchpressed(3, 10.0, 20.0)
assert interpreter.globals["game"].fields["score"] == 1
interpreter.update(1.0 / 60.0)
commands = interpreter.draw()
assert any(command.text == "Счет: 1" for command in commands)
assert interpreter.globals["game"].fields["text_scale"] < 1.6
print("PASS DimScript parser + interpreter + C compiler")
