#!/usr/bin/env python3
"""APK asset packing: images + fonts from the game folder into assets/game.

Builds a tiny game with a real PNG, a subfolder sprite and a font, packs it
with tools/gamepack.py, then AOT-compiles a script that loads them and runs
the result against the *staged* folder — proving the packer, the compiler
builtins, the PNG decoder and the registries all agree.  Also covers the
failure paths: a missing file, a fake PNG, the removed 3D cube key and a
launcher icon tree.
"""

from __future__ import annotations

import os
import shlex
import shutil
import struct
import subprocess
import sys
import tempfile
import zlib
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
OUT = ROOT / "build-tests" / "pack"
sys.path.insert(0, str(ROOT))

from dimscript.compiler import CCompiler  # noqa: E402
from dimscript.lexer import DimScriptError  # noqa: E402
from dimscript.parser import parse  # noqa: E402
from tools.gamepack import pack  # noqa: E402

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

GAME_MANIFEST = """title = "Assets"
package = "com.cb4.assets"
scripts = ["main.ds"]
images = ["hero.png"]
fonts = ["font.ttf"]
"""

GAME_SCRIPT = """logo = 0
fnt = 0
load() {
    logo = image.load("hero.png")
    fnt = font.load("font.ttf")
}
draw() {
    render.font(fnt)
    render.text("hi", 10.0, 10.0, 1.0)
    render.image(logo, 0.0, 0.0, 4.0, 3.0)
}
quit() {
}
"""

DRIVER = """/* pack.py driver: runs the staged game, reports what the loaders found. */
#include "assets_game.h"
#include "dimscript_runtime.h"
#include "ds_files.h"
#include "ds_font.h"
#include "ds_image.h"
#include "enjoer_draw.h"
#include "engine.h"
#include <stdio.h>

int main(int argc, char **argv) {
    if (argc < 2) return 2;
    ds_files_set_root(argv[1]);
    screen_w = 64;
    screen_h = 64;
    ds_runtime_init();
    ds_engine_reset(screen_w, screen_h);
    dimscript_init();
    dimscript_load();
    printf("images %d\\n", (int)ds_image_count());
    printf("image_size %d %d\\n", (int)ds_image_width(0), (int)ds_image_height(0));
    printf("fonts %d\\n", (int)ds_font_count());
    enjoer_frame_begin(screen_w, screen_h);
    dimscript_draw();
    enjoer_frame_end();
    printf("vertices %d\\n", enjoer_frame()->vertex_count);
    printf("texts %d font %d\\n", enjoer_frame()->text_count,
           enjoer_frame()->text_count ? (int)enjoer_frame()->texts[0].font : -9);
    dimscript_quit();
    dimscript_shutdown();
    ds_runtime_shutdown();
    return 0;
}
"""


class Failure(Exception):
    pass


def check(condition: bool, message: str) -> None:
    if not condition:
        raise Failure(message)


def write_png(path: Path, width: int, height: int, rgb: tuple = (255, 128, 0)) -> None:
    raw = b"".join(b"\x00" + bytes(rgb) * width for _ in range(height))

    def chunk(tag: bytes, data: bytes) -> bytes:
        body = tag + data
        return struct.pack(">I", len(data)) + body + struct.pack(">I", zlib.crc32(body))

    path.write_bytes(
        b"\x89PNG\r\n\x1a\n"
        + chunk(b"IHDR", struct.pack(">IIBBBBB", width, height, 8, 2, 0, 0, 0))
        + chunk(b"IDAT", zlib.compress(raw))
        + chunk(b"IEND", b"")
    )


def make_game(directory: Path) -> Path:
    (directory / "sub").mkdir(parents=True, exist_ok=True)
    (directory / "game.manifest").write_text(GAME_MANIFEST, encoding="utf-8")
    (directory / "main.ds").write_text(GAME_SCRIPT, encoding="utf-8")
    write_png(directory / "hero.png", 4, 3)
    write_png(directory / "sub" / "coin.png", 2, 2, (0, 255, 0))
    (directory / "font.ttf").write_bytes(b"\x00\x01\x00\x00" + b"\x00" * 64)
    return directory


def compiler() -> list:
    from_env = os.environ.get("CC") or os.environ.get("ENJOER_CC")
    if from_env:
        return shlex.split(from_env)
    if shutil.which("clang"):
        return ["clang"]
    probe = subprocess.run([sys.executable, "-m", "ziglang", "cc", "--version"], capture_output=True)
    if probe.returncode == 0:
        return [sys.executable, "-m", "ziglang", "cc"]
    return ["cc"]


def run(command: list) -> str:
    process = subprocess.run(command, cwd=ROOT, capture_output=True, text=True)
    if process.returncode != 0:
        tail = (process.stderr or process.stdout).strip().splitlines()[-25:]
        raise Failure(" ".join(shlex.quote(part) for part in command) + "\n" + "\n".join(tail))
    return process.stdout


def expect_error(game: Path, staging: Path, needle: str) -> None:
    try:
        pack(game, staging, check_only=True)
    except DimScriptError as error:
        check(needle in str(error), f"{game.name}: ждали {needle!r}, получили {error}")
        return
    raise Failure(f"{game.name}: pack прошёл, а ждали ошибку {needle!r}")


def main() -> int:
    OUT.mkdir(parents=True, exist_ok=True)
    work = Path(tempfile.mkdtemp(prefix="enjoer-pack-"))

    try:
        game = make_game(work / "game")
        staging = work / "staging"
        manifest = pack(game, staging)
        staged = staging
        check(manifest.images == ["hero.png", "sub/coin.png"], f"images: {manifest.images}")
        check(manifest.fonts == ["font.ttf"], f"fonts: {manifest.fonts}")
        assets = staged / "assets" / "game"
        for name in ("main.ds", "game.manifest", "hero.png", "sub/coin.png", "font.ttf"):
            check((assets / name).is_file(), f"нет {name} в staging")
        packaged = (assets / "game.manifest").read_text(encoding="utf-8")
        check('images = ["hero.png", "sub/coin.png"]' in packaged, packaged)
        check('fonts = ["font.ttf"]' in packaged, packaged)
        check((staged / "AndroidManifest.xml").is_file(), "нет AndroidManifest.xml")

        # Failure paths.
        ghost = make_game(work / "ghost")
        (ghost / "game.manifest").write_text(
            GAME_MANIFEST.replace('images = ["hero.png"]', 'images = ["ghost.png"]'),
            encoding="utf-8",
        )
        expect_error(ghost, work / "staging-ghost", "'ghost.png' из 'images' не найден")

        fake = make_game(work / "fake")
        (fake / "hero.png").write_bytes(b"definitely not a png")
        expect_error(fake, work / "staging-fake", "не PNG")

        # PNG only: a listed JPEG fails the manifest suffix rule, and JPEG
        # bytes under a .png name fail the magic check with the format named.
        jpeg = b"\xff\xd8\xff\xe0\x00\x10JFIF\x00" + b"\x00" * 16
        listed_jpg = make_game(work / "listed-jpg")
        (listed_jpg / "pic.jpg").write_bytes(jpeg)
        (listed_jpg / "game.manifest").write_text(
            GAME_MANIFEST.replace('images = ["hero.png"]', 'images = ["pic.jpg"]'),
            encoding="utf-8",
        )
        expect_error(listed_jpg, work / "staging-listed-jpg", "должен заканчиваться на")

        sniffed_jpg = make_game(work / "sniffed-jpg")
        (sniffed_jpg / "hero.png").write_bytes(jpeg)
        expect_error(sniffed_jpg, work / "staging-sniffed-jpg", "в images только PNG (это JPEG")

        cube = make_game(work / "cube")
        with open(cube / "game.manifest", "a", encoding="utf-8") as stream:
            stream.write("cube = true\n")
        expect_error(cube, work / "staging-cube", "только 2D")

        icons = make_game(work / "icons")
        mipmap = icons / "res" / "mipmap-xhdpi"
        mipmap.mkdir(parents=True)
        (mipmap / "ic.png").write_bytes(b"x")
        expect_error(icons, work / "staging-icons", "иконки лаунчера")

        # A plain icon.png is an ordinary sprite, not a launcher icon.
        write_png(game / "icon.png", 1, 1)
        manifest = pack(game, staging, check_only=True)
        check("icon.png" in manifest.images, f"icon.png должны упаковать: {manifest.images}")

        # The shipped game still packs.
        real_game = pack(ROOT / "game", work / "staging-game", check_only=True)
        check(real_game.title == "Cubic Battle 4", real_game.title)
        check(real_game.package == "com.cb4.cubicbattle", real_game.package)
        check(real_game.scripts[0] == "main.ds", str(real_game.scripts))
        check("battle.ds" in real_game.scripts and "ui.ds" in real_game.scripts, str(real_game.scripts))

        # AOT end to end: compile the game, run it against the staged assets.
        program = parse((game / "main.ds").read_text(encoding="utf-8"),
                        filename=str(game / "main.ds"))
        backend = CCompiler(program, filename=str(game / "main.ds"))
        (OUT / "assets_game.c").write_text(backend.compile(), encoding="utf-8")
        (OUT / "assets_game.h").write_text(backend.header("ASSETS_GAME_H"), encoding="utf-8")
        (OUT / "assets_driver.c").write_text(DRIVER, encoding="utf-8")
        cc = compiler()
        flags = ["-std=c99", "-O2", "-Wall", "-Wextra", "-Werror", "-I./src", f"-I{OUT}"]
        flags += shlex.split(os.environ.get("ENJOER_SANITIZE", ""))
        run([*cc, *flags, "-o", str(OUT / "assets_run"), str(OUT / "assets_driver.c"),
             str(OUT / "assets_game.c"), *ENGINE_C, "-lm"])
        output = run([str(OUT / "assets_run"), str(assets)])
        values = dict(
            line.split(" ", 1) for line in output.splitlines() if " " in line
        )
        check(values.get("images") == "1", output)
        check(values.get("image_size") == "4 3", output)
        check(values.get("fonts") == "1", output)
        check(values.get("vertices") == "6", output)
        check(values.get("texts") == "1 font 0", output)
    finally:
        shutil.rmtree(work, ignore_errors=True)

    print("PASS gamepack: images + fonts staged, AOT loaders agree (4x3 PNG, 1 font, 6 vertices)")
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except Failure as error:
        print(f"FAIL gamepack: {error}", file=sys.stderr)
        raise SystemExit(1)
