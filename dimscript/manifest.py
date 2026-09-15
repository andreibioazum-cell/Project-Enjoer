"""`game.manifest` — what an Enjoer game tells the engine and the packer.

The format is a flat ``key = value`` file with ``--`` comments, mirroring
:file:`src/ds_manifest.c`, which is what the engine reads on a device.  Keeping
both parsers in sync is a deliberate trade-off: the language has no dependency
on a config library, and a game folder stays readable without any tooling.

Icons are intentionally not part of the format yet — :func:`parse_manifest`
raises on an ``icon`` key instead of ignoring it, so nobody ships a half wired
asset.
"""

from __future__ import annotations

import re
from dataclasses import dataclass, field
from pathlib import Path
from typing import Dict, List, Sequence, Tuple

from .lexer import DimScriptError


class ManifestError(DimScriptError):
    """Raised for a malformed or unsupported manifest."""


MANIFEST_NAME = "game.manifest"

ORIENTATIONS = ("portrait", "landscape", "sensor", "sensorLandscape", "sensorPortrait")
STRING_KEYS = ("title", "author", "package", "version", "orientation")
INT_KEYS = ("version_code", "target_fps")
BOOL_KEYS = ("resizeable", "show_fps")
LIST_KEYS = {"scripts": (".ds", 32), "images": (".png", 32), "fonts": ((".ttf", ".otf"), 8)}
KNOWN_KEYS = STRING_KEYS + INT_KEYS + BOOL_KEYS + ("clear_color",) + tuple(LIST_KEYS)

_PACKAGE = re.compile(r"^[a-z_][a-zA-Z0-9_]*(\.[a-z_][a-zA-Z0-9_]*)+$")
_KEY = re.compile(r"^[a-z_]+$")


@dataclass
class GameManifest:
    """A resolved manifest: explicit keys plus the documented defaults."""

    title: str = "Enjoer Game"
    author: str = "unknown"
    package: str = "com.cb4.game"
    version: str = "1.0"
    version_code: int = 1
    orientation: str = "sensorLandscape"
    target_fps: int = 60
    resizeable: bool = True
    show_fps: bool = False
    clear_color: Tuple[float, float, float] = (0.05, 0.08, 0.15)
    scripts: List[str] = field(default_factory=list)
    images: List[str] = field(default_factory=list)
    fonts: List[str] = field(default_factory=list)
    directory: Path = None  # type: ignore[assignment]

    @property
    def android_orientation(self) -> str:
        return self.orientation

    @property
    def activity_name(self) -> str:
        return f"{self.package}.GameActivity"

    def as_dict(self) -> Dict[str, object]:
        return {
            "title": self.title,
            "author": self.author,
            "package": self.package,
            "version": self.version,
            "versionCode": self.version_code,
            "orientation": self.orientation,
            "targetFps": self.target_fps,
            "resizeable": self.resizeable,
            "showFps": self.show_fps,
            "clearColor": list(self.clear_color),
            "scripts": list(self.scripts),
            "images": list(self.images),
            "fonts": list(self.fonts),
        }


def _strip_comment(line: str) -> str:
    quote = ""
    index = 0
    while index < len(line):
        character = line[index]
        if quote:
            if character == "\\" and index + 1 < len(line):
                index += 2
                continue
            if character == quote:
                quote = ""
        elif character in "\"'":
            quote = character
        elif line.startswith("--", index) or line.startswith("//", index):
            return line[:index]
        index += 1
    return line


def _unquote(text: str) -> str:
    if len(text) >= 2 and text[0] == text[-1] and text[0] in "\"'":
        body = text[1:-1]
        return body.replace("\\n", "\n").replace("\\t", "\t").replace('\\"', '"').replace("\\\\", "\\")
    return text


def _parse_color(text: str, line: int) -> Tuple[float, float, float]:
    text = text.strip()
    if text.startswith("#") and len(text) == 7:
        try:
            return tuple(int(text[index : index + 2], 16) / 255.0 for index in (1, 3, 5))  # type: ignore[return-value]
        except ValueError as error:  # pragma: no cover - defensive
            raise ManifestError(f"game.manifest:{line}: не цвет {text!r}") from error
    parts = [part for part in re.split(r"[\s,]+", text.strip("[] ")) if part]
    if len(parts) != 3:
        raise ManifestError(f"game.manifest:{line}: clear_color ожидает '#rrggbb' или три числа 0..1")
    values: List[float] = []
    for part in parts:
        try:
            values.append(float(part))
        except ValueError as error:
            raise ManifestError(f"game.manifest:{line}: не число в clear_color: {part!r}") from error
        if not 0.0 <= values[-1] <= 1.0:
            raise ManifestError(f"game.manifest:{line}: компоненты clear_color в диапазоне 0..1")
    return (values[0], values[1], values[2])


def parse_manifest(text: str, source: str = "game.manifest") -> GameManifest:
    """Parse *text* and return the resolved :class:`GameManifest`."""

    manifest = GameManifest()
    for number, raw_line in enumerate(text.splitlines(), start=1):
        line = _strip_comment(raw_line).strip()
        if not line:
            continue
        if "=" not in line:
            raise ManifestError(f"{source}:{number}: ожидается 'key = value'")
        key, _, value = line.partition("=")
        key = key.strip()
        value = value.strip()
        if not _KEY.match(key):
            raise ManifestError(f"{source}:{number}: имя '{key}' недопустимо (только строчные буквы и '_')")
        if key == "icon" or key == "icons" or key.startswith("icon_"):
            raise ManifestError(
                f"{source}:{number}: иконки пока не поддерживаются, уберите ключ '{key}'"
            )
        if key == "cube":
            raise ManifestError(
                f"{source}:{number}: 3D-куб удалён — Enjoer теперь только 2D, уберите ключ 'cube'"
            )
        if key not in KNOWN_KEYS:
            raise ManifestError(
                f"{source}:{number}: неизвестный ключ '{key}' "
                f"(можно: {', '.join(sorted(KNOWN_KEYS))})"
            )
        if key == "title":
            manifest.title = _unquote(value)
        elif key == "author":
            manifest.author = _unquote(value)
        elif key == "package":
            manifest.package = _unquote(value)
            if not _PACKAGE.match(manifest.package):
                raise ManifestError(f"{source}:{number}: package='{manifest.package}' не похоже на com.name.game")
        elif key == "version":
            manifest.version = _unquote(value)
        elif key == "orientation":
            manifest.orientation = _unquote(value)
            if manifest.orientation not in ORIENTATIONS:
                raise ManifestError(
                    f"{source}:{number}: orientation='{manifest.orientation}', ожидалось "
                    + "/".join(ORIENTATIONS)
                )
        elif key == "version_code":
            try:
                manifest.version_code = int(value)
            except ValueError as error:
                raise ManifestError(f"{source}:{number}: version_code ожидает целое число") from error
            if manifest.version_code < 1:
                raise ManifestError(f"{source}:{number}: version_code должно быть >= 1")
        elif key == "target_fps":
            try:
                manifest.target_fps = int(value)
            except ValueError as error:
                raise ManifestError(f"{source}:{number}: target_fps ожидает целое число") from error
            if not 20 <= manifest.target_fps <= 240:
                raise ManifestError(f"{source}:{number}: target_fps в диапазоне 20..240")
        elif key in BOOL_KEYS:
            if value not in {"true", "false", "0", "1"}:
                raise ManifestError(f"{source}:{number}: '{key}' принимает true или false")
            flag = value in {"true", "1"}
            if key == "resizeable":
                manifest.resizeable = flag
            else:
                manifest.show_fps = flag
        elif key == "clear_color":
            manifest.clear_color = _parse_color(value, number)
        elif key in LIST_KEYS:
            wanted, limit = LIST_KEYS[key]
            if isinstance(wanted, str):
                wanted = (wanted,)
            names = [item for item in re.split(r"[\s,]+", value.strip("[] ")) if item]
            parsed = [_unquote(name) for name in names]
            for name in parsed:
                if not name.endswith(wanted):
                    raise ManifestError(
                        f"{source}:{number}: {name!r} в '{key}' должен заканчиваться на "
                        + "/".join(wanted)
                    )
                if ".." in Path(name).parts or Path(name).is_absolute():
                    raise ManifestError(
                        f"{source}:{number}: {name!r} в '{key}' — путь внутри папки игры, без '..'"
                    )
            if len(parsed) > limit:
                raise ManifestError(f"{source}:{number}: больше {limit} имён в '{key}'")
            setattr(manifest, key, parsed)
    return manifest


def read_manifest(directory: Path) -> GameManifest:
    """Load ``<directory>/game.manifest``; missing files fall back to defaults."""

    path = Path(directory) / MANIFEST_NAME
    manifest = GameManifest()
    manifest.directory = Path(directory)
    if not path.exists():
        return manifest
    manifest = parse_manifest(path.read_text(encoding="utf-8"), source=str(path))
    manifest.directory = Path(directory)
    return manifest


ANDROID_TEMPLATE = """<?xml version="1.0" encoding="utf-8"?>
<!-- Generated by `python3 tools/gamepack.py` from {game}/game.manifest.
     Enjoer games have no launcher icon yet, so the application tag intentionally
     omits android:icon and Android shows the default one. -->
<manifest xmlns:android="http://schemas.android.com/apk/res/android"
    package="{package}"
    android:versionCode="{version_code}"
    android:versionName="{version}">

    <uses-sdk android:minSdkVersion="29" android:targetSdkVersion="34" />
    <uses-feature android:name="android.hardware.touchscreen" android:required="true" />
    <uses-feature android:name="android.hardware.vulkan.version" android:version="0x400003" android:required="true" />

    <application
        android:label="{title}"
        android:hasCode="true"
        android:allowBackup="false"
        android:theme="@android:style/Theme.Black.NoTitleBar.Fullscreen">

        <activity
            android:name="{activity}"
            android:label="{title}"
            android:configChanges="orientation|keyboardHidden|keyboard|screenSize|smallestScreenSize|screenLayout|uiMode"
            android:screenOrientation="{orientation}"
            android:resizeableActivity="{resizeable}"
            android:windowSoftInputMode="adjustResize|stateHidden"
            android:exported="true">
            <meta-data android:name="android.app.lib_name" android:value="ds_game" />
            <intent-filter>
                <action android:name="android.intent.action.MAIN" />
                <category android:name="android.intent.category.LAUNCHER" />
            </intent-filter>
        </activity>
    </application>
</manifest>
"""


def android_manifest_xml(manifest: GameManifest) -> str:
    """Render the Android manifest for *manifest*.

    Only metadata that Enjoer can honour today is emitted: label, package,
    version, orientation and resizeability.  Icons are skipped on purpose.
    """

    def escape(value: str) -> str:
        return (
            value.replace("&", "&amp;")
            .replace("<", "&lt;")
            .replace(">", "&gt;")
            .replace('"', "&quot;")
        )

    return ANDROID_TEMPLATE.format(
        game=manifest.directory.name if manifest.directory else "game",
        package=manifest.package,
        version_code=manifest.version_code,
        version=escape(manifest.version),
        title=escape(manifest.title),
        activity=manifest.activity_name,
        orientation=manifest.android_orientation,
        resizeable="true" if manifest.resizeable else "false",
    )


def game_files(directory: Path, scripts: Sequence[str]) -> List[Path]:
    """All ``.ds`` files of a game: the manifest order first, then the rest."""

    directory = Path(directory)
    ordered: List[Path] = []
    for name in scripts:
        path = directory / name
        if not path.exists():
            raise ManifestError(f"{name} отсутствует в {directory}")
        ordered.append(path)
    for path in sorted(directory.glob("*.ds")):
        if path not in ordered:
            ordered.append(path)
    if not ordered:
        raise ManifestError(f"в {directory} нет ни одного .ds файла")
    return ordered
