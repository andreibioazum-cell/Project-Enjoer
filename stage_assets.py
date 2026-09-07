#!/usr/bin/env python3
"""Copy the game's assets (assets/) into the directory aapt packages into the APK."""

from __future__ import annotations

import argparse
import os
import shutil
import subprocess
import sys
from pathlib import Path


def stage_assets(source: Path, destination: Path) -> list[Path]:
    """Replace *destination* with a clean copy of *source*.

    The source tree already contains every runtime asset the engine opens by
    name: textures/*.png, fonts/*.ttf and sounds/*.wav. Older CI invocations
    still pass the pre-restructure path game/assets; accept it as an alias.
    """
    if not source.is_dir() and source.name == "assets":
        source = Path(__file__).resolve().parent / "assets"
    source = source.resolve()
    destination = destination.resolve()

    if not source.is_dir():
        raise ValueError(f"asset directory not found: {source}")
    if (source == destination
        or source in destination.parents
        or destination in source.parents):
        raise ValueError("the asset and staging directories must not overlap")

    shutil.rmtree(destination, ignore_errors=True)
    destination.mkdir(parents=True)

    staged: list[Path] = []
    for path in sorted(source.rglob("*")):
        relative_path = path.relative_to(source)
        target = destination / relative_path

        if path.is_symlink():
            raise ValueError(f"asset symlinks are not supported: {relative_path}")
        if path.is_dir():
            target.mkdir(parents=True, exist_ok=True)
        elif path.name not in {".gitkeep", "README.md"}:
            target.parent.mkdir(parents=True, exist_ok=True)
            shutil.copy2(path, target)
            staged.append(relative_path)

    return staged


def stage_projects(source: Path, destination: Path) -> list[Path]:
    """Copy the engine projects into the APK asset tree.

    Only what a phone can actually run is shipped: the ``project.eng``
    manifest and the ``.escn`` scenes (plus script sources for reference;
    devices use the engine's built-in scripts). A ``index.txt`` is written so
    the APK can enumerate projects without a directory walk, which the Android
    asset manager does not support for subdirectories.
    """
    source = source.resolve()
    destination = destination.resolve()
    if not source.is_dir():
        return []
    shutil.rmtree(destination, ignore_errors=True)
    destination.mkdir(parents=True)

    staged: list[Path] = []
    names: list[str] = []
    for project_dir in sorted(p for p in source.iterdir() if p.is_dir()):
        manifest = project_dir / "project.eng"
        if not manifest.is_file():
            continue
        names.append(project_dir.name)
        target_dir = destination / project_dir.name
        target_dir.mkdir(parents=True, exist_ok=True)
        shutil.copy2(manifest, target_dir / "project.eng")
        staged.append(Path(project_dir.name) / "project.eng")
        for sub, patterns in (("scenes", ("*.escn",)), ("scripts", ("*.c",))):
            src_sub = project_dir / sub
            if not src_sub.is_dir():
                continue
            (target_dir / sub).mkdir(parents=True, exist_ok=True)
            for file in sorted(src_sub.rglob("*")):
                if file.is_file() and any(file.match(p) for p in patterns):
                    rel = file.relative_to(src_sub)
                    target = target_dir / sub / rel
                    target.parent.mkdir(parents=True, exist_ok=True)
                    shutil.copy2(file, target)
                    staged.append(Path(project_dir.name) / sub / rel)

    index = destination / "index.txt"
    index.write_text("\n".join(names) + "\n", encoding="utf-8")
    staged.append(Path("index.txt"))
    return staged


def build_android_activity(source: Path, apk_root: Path) -> Path | None:
    """Compile the optional Java NativeActivity bridge into ``classes.dex``.

    The repository's Android workflow already calls this staging script after
    installing the SDK, so keeping the Java/Dex step here also makes manual APK
    staging reproducible without a Gradle project. On development hosts without
    an Android SDK, asset-only staging remains available.
    """
    sources = sorted(source.rglob("*.java")) if source.is_dir() else []
    if not sources:
        return None

    sdk_value = os.environ.get("ANDROID_HOME") or os.environ.get("ANDROID_SDK_ROOT")
    if not sdk_value:
        print("Android SDK not found; skipped Java activity compilation")
        return None

    sdk = Path(sdk_value).resolve()
    android_jar = sdk / "platforms" / "android-34" / "android.jar"
    d8 = sdk / "build-tools" / "34.0.0" / "d8"
    javac = shutil.which("javac")
    if not javac:
        raise ValueError("javac not found while staging the Android activity")
    if not android_jar.is_file():
        raise ValueError(f"Android API 34 platform not found: {android_jar}")
    if not d8.is_file():
        raise ValueError(f"Android build-tools 34.0.0 d8 not found: {d8}")

    apk_root = apk_root.resolve()
    classes_dir = apk_root / ".java-classes"
    dex_dir = apk_root / ".java-dex"
    shutil.rmtree(classes_dir, ignore_errors=True)
    shutil.rmtree(dex_dir, ignore_errors=True)
    classes_dir.mkdir(parents=True)
    dex_dir.mkdir(parents=True)
    try:
        subprocess.run([
            javac, "-source", "8", "-target", "8",
            "-bootclasspath", str(android_jar),
            "-d", str(classes_dir),
            *(str(path) for path in sources),
        ], check=True)
        class_files = sorted(classes_dir.rglob("*.class"))
        if not class_files:
            raise ValueError("javac produced no Android activity classes")
        subprocess.run([
            str(d8), "--min-api", "29", "--output", str(dex_dir),
            *(str(path) for path in class_files),
        ], check=True)
        generated = dex_dir / "classes.dex"
        if not generated.is_file():
            raise ValueError("d8 did not produce classes.dex")
        destination = apk_root / "classes.dex"
        shutil.copy2(generated, destination)
        return destination
    finally:
        shutil.rmtree(classes_dir, ignore_errors=True)
        shutil.rmtree(dex_dir, ignore_errors=True)


def parse_args(argv: list[str] | None = None) -> argparse.Namespace:
    parser = argparse.ArgumentParser(description="Copy game assets into an APK staging directory.")
    parser.add_argument("source", nargs="?", default="assets")
    parser.add_argument("destination", nargs="?", default="staging/assets")
    return parser.parse_args(argv)


def main(argv: list[str] | None = None) -> int:
    args = parse_args(argv)
    try:
        destination = Path(args.destination)
        staged = stage_assets(Path(args.source), destination)
        projects = stage_projects(
            Path(__file__).resolve().parent / "projects", destination / "projects")
        java_source = Path(__file__).resolve().parent / "game" / "java"
        dex = build_android_activity(java_source, destination.parent)
    except (OSError, ValueError, subprocess.CalledProcessError) as error:
        print(f"Error: {error}", file=sys.stderr)
        return 1

    print(f"Staged {len(staged)} asset(s) and {len(projects)} project file(s) in {args.destination}")
    for path in staged + projects:
        print(f"  {path.as_posix()}")
    if dex:
        print(f"Built Android activity: {dex}")
    return 0


if __name__ == '__main__':
    sys.exit(main())
