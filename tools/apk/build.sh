#!/bin/sh
# Fast APK build for an Enjoer game — no Gradle, no daemon, no full rebuilds.
#
#   tools/apk/build.sh                        # game -> build-apk/dist/*.apk
#   tools/apk/build.sh --game game --abi all --install
#
# The pipeline is the same tools Gradle would call, minus the minutes of
# overhead: gamepack stages assets/game, CMake+NDK builds libds_game.so,
# javac+d8 make classes.dex, aapt2 links the APK, zipalign+apksigner finish it.
#
# Every slow step is skipped when its outputs are newer than its inputs, so a
# second run with no changes finishes in seconds.  What makes it fast:
#
#   * Ninja when available (falls back to Unix Makefiles), -j$(nproc);
#   * ccache when available (CMake picks it up automatically);
#   * a single ABI by default (arm64-v8a) — pass --abi all for both;
#   * CMake-side asset packing is disabled: this script stages assets once and
#     passes -DENJOER_GAME_DIR= so the native build never repacks.
#
# Needs: ANDROID_SDK_ROOT (or ANDROID_HOME), an NDK (ANDROID_NDK_ROOT or
# $SDK/ndk/<version>), a JDK (javac, keytool) and python3.
set -eu

GAME=game
BUILD=build-apk
DIST=""
ABI=arm64-v8a
CLEAN=0
FORCE=0
INSTALL=0
LAUNCH=0
JOBS=""
KEYSTORE=""
KEY_ALIAS="androiddebugkey"
STORE_PASS="android"
KEY_PASS="android"

while [ $# -gt 0 ]; do
    case "$1" in
        --game) GAME=$2; shift 2;;
        --build) BUILD=$2; shift 2;;
        --dist) DIST=$2; shift 2;;
        --abi) ABI=$2; shift 2;;
        --clean) CLEAN=1; shift;;
        --force) FORCE=1; shift;;
        --install) INSTALL=1; shift;;
        --launch) INSTALL=1; LAUNCH=1; shift;;
        --keystore) KEYSTORE=$2; shift 2;;
        --alias) KEY_ALIAS=$2; shift 2;;
        --storepass) STORE_PASS=$2; shift 2;;
        --keypass) KEY_PASS=$2; shift 2;;
        -j) JOBS=$2; shift 2;;
        -h|--help)
            sed -n '2,24p' "$0"
            echo "Options: --game DIR --build DIR --dist DIR --abi arm64-v8a|armeabi-v7a|all"
            echo "         --clean --force --install --launch -j N"
            echo "         --keystore PATH --alias NAME --storepass P --keypass P"
            exit 0;;
        *) echo "build.sh: неизвестный флаг $1 (см. --help)" >&2; exit 2;;
    esac
done

cd "$(dirname "$0")/../.."
ROOT=$(pwd)
[ -n "$DIST" ] || DIST="$BUILD/dist"

if [ "$CLEAN" = 1 ]; then
    rm -rf "$BUILD"
fi
mkdir -p "$BUILD" "$DIST"

# --- SDK / NDK / JDK -------------------------------------------------------
SDK=${ANDROID_SDK_ROOT:-${ANDROID_HOME:-}}
[ -n "$SDK" ] || { echo "build.sh: задайте ANDROID_SDK_ROOT" >&2; exit 2; }
BT=$(ls -d "$SDK"/build-tools/* 2>/dev/null | sort -V | tail -n 1)
[ -n "$BT" ] && [ -x "$BT/aapt2" ] || { echo "build.sh: в $SDK нет build-tools с aapt2" >&2; exit 2; }
PLATFORM_JAR=$(ls "$SDK"/platforms/android-*/android.jar 2>/dev/null | sort -V | tail -n 1)
[ -n "$PLATFORM_JAR" ] || { echo "build.sh: в $SDK нет platforms/android-*/android.jar" >&2; exit 2; }
NDK=${ANDROID_NDK_ROOT:-$(ls -d "$SDK"/ndk/* 2>/dev/null | sort -V | tail -n 1)}
[ -n "$NDK" ] && [ -f "$NDK/build/cmake/android.toolchain.cmake" ] || {
    echo "build.sh: задайте ANDROID_NDK_ROOT (в $SDK/ndk тоже пусто)" >&2; exit 2; }
command -v javac >/dev/null 2>&1 || { echo "build.sh: нет javac — поставьте JDK" >&2; exit 2; }
command -v cmake >/dev/null 2>&1 || { echo "build.sh: нет cmake" >&2; exit 2; }
command -v python3 >/dev/null 2>&1 || { echo "build.sh: нет python3" >&2; exit 2; }
if ! command -v ninja >/dev/null 2>&1 && ! command -v ccache >/dev/null 2>&1; then
    echo "build.sh: совет — поставьте ninja и ccache, сборка станет заметно быстрее" >&2
fi

if [ -z "$JOBS" ]; then
    if command -v nproc >/dev/null 2>&1; then JOBS=$(nproc)
    elif command -v sysctl >/dev/null 2>&1; then JOBS=$(sysctl -n hw.ncpu 2>/dev/null || echo 4)
    else JOBS=4; fi
fi

# --- helpers ---------------------------------------------------------------
# needs_rebuild <stamp> <src...>: true when the stamp is missing, --force is
# set, or any source (file or tree) is newer than the stamp.
needs_rebuild() {
    stamp=$1; shift
    if [ "$FORCE" = 1 ] || [ ! -f "$stamp" ]; then return 0; fi
    for src in "$@"; do
        [ -e "$src" ] || continue
        if [ -d "$src" ]; then
            if find "$src" -newer "$stamp" -type f -print -quit 2>/dev/null | grep -q .; then
                return 0
            fi
        elif [ "$src" -nt "$stamp" ]; then
            return 0
        fi
    done
    return 1
}

GAME_ABS=$(cd "$GAME" 2>/dev/null && pwd) || { echo "build.sh: нет папки игры $GAME" >&2; exit 2; }

# --- 1. assets -------------------------------------------------------------
STAGING="$BUILD/staging"
STAMP_ASSETS="$BUILD/staging.stamp"
if needs_rebuild "$STAMP_ASSETS" "$GAME_ABS" tools/gamepack.py dimscript/manifest.py dimscript/project.py; then
    echo "== gamepack: $GAME -> assets/game (+ картинки, шрифты)"
    rm -rf "$STAGING"
    python3 tools/gamepack.py "$GAME_ABS" --staging "$STAGING" \
        --manifest-out "$BUILD/apk-manifest/AndroidManifest.xml"
    touch "$STAMP_ASSETS"
else
    echo "== gamepack: без изменений, пропускаю"
fi

PACKAGE=$(python3 -c "import sys; sys.path.insert(0, '.'); from dimscript.manifest import read_manifest; from pathlib import Path; print(read_manifest(Path('$GAME_ABS')).package)")
TITLE=$(python3 -c "import sys; sys.path.insert(0, '.'); from dimscript.manifest import read_manifest; from pathlib import Path; print(read_manifest(Path('$GAME_ABS')).title)")

# --- 2. native library -----------------------------------------------------
case "$ABI" in
    all) ABIS="arm64-v8a armeabi-v7a";;
    arm64-v8a|armeabi-v7a) ABIS="$ABI";;
    *) echo "build.sh: --abi ждёт arm64-v8a, armeabi-v7a или all" >&2; exit 2;;
esac

if command -v ninja >/dev/null 2>&1; then GENERATOR="Ninja"; else GENERATOR="Unix Makefiles"; fi

for CURRENT_ABI in $ABIS; do
    NATIVE_DIR="$BUILD/native-$CURRENT_ABI"
    mkdir -p "$NATIVE_DIR"
    LIB="$NATIVE_DIR/libds_game.so"
    ARGS_FILE="$NATIVE_DIR/enjoer-args.txt"
    ARGS="$GENERATOR|$CURRENT_ABI|$(cd "$ROOT" && pwd)"
    if [ -f "$NATIVE_DIR/CMakeCache.txt" ] && [ -f "$ARGS_FILE" ] && \
       [ "$(cat "$ARGS_FILE")" = "$ARGS" ] && [ "$FORCE" != 1 ]; then
        echo "== cmake ($CURRENT_ABI): конфигурация свежая, пропускаю"
    else
        echo "== cmake ($CURRENT_ABI): конфигурация [$GENERATOR]"
        cmake -S "$ROOT" -B "$NATIVE_DIR" -G "$GENERATOR" \
            -DCMAKE_TOOLCHAIN_FILE="$NDK/build/cmake/android.toolchain.cmake" \
            -DANDROID_ABI="$CURRENT_ABI" -DANDROID_PLATFORM=android-29 \
            -DCMAKE_BUILD_TYPE=Release -DENJOER_GAME_DIR=
        printf '%s' "$ARGS" >"$ARGS_FILE"
    fi
    echo "== ninja ($CURRENT_ABI): сборка libds_game.so [-j$JOBS]"
    cmake --build "$NATIVE_DIR" -j "$JOBS"
    [ -f "$LIB" ] || { echo "build.sh: не собрался $LIB" >&2; exit 1; }
done

# --- 3. dex ------------------------------------------------------------------
DEX_DIR="$BUILD/dex"
CLASSES_DIR="$BUILD/classes"
DEX="$DEX_DIR/classes.dex"
STAMP_DEX="$BUILD/dex.stamp"
if needs_rebuild "$STAMP_DEX" game/java/com/cb4/GameActivity.java "$PLATFORM_JAR"; then
    echo "== javac+d8: classes.dex"
    rm -rf "$DEX_DIR" "$CLASSES_DIR"
    mkdir -p "$DEX_DIR" "$CLASSES_DIR"
    if javac --release 8 -version >/dev/null 2>&1; then RELEASE_FLAG="--release 8"; else RELEASE_FLAG=""; fi
    # shellcheck disable=SC2086
    javac $RELEASE_FLAG -cp "$PLATFORM_JAR" -d "$CLASSES_DIR" game/java/com/cb4/GameActivity.java
    "$BT/d8" --release --min-api 29 --lib "$PLATFORM_JAR" \
        --output "$DEX_DIR" "$CLASSES_DIR/com/cb4/GameActivity.class"
    touch "$STAMP_DEX"
else
    echo "== javac+d8: без изменений, пропускаю"
fi

# --- 4. package --------------------------------------------------------------
UNSIGNED="$BUILD/app-unsigned.apk"
ALIGNED="$BUILD/app-aligned.apk"
APK="$DIST/$PACKAGE-debug.apk"
echo "== aapt2: компоновка APK"
rm -f "$UNSIGNED" "$ALIGNED"
"$BT/aapt2" link -o "$UNSIGNED" -I "$PLATFORM_JAR" \
    --manifest "$BUILD/apk-manifest/AndroidManifest.xml" \
    -A "$STAGING/assets" --min-sdk-version 29 --target-sdk-version 34

LIB_ARGS=""
for CURRENT_ABI in $ABIS; do
    LIB_ARGS="$LIB_ARGS $CURRENT_ABI=$BUILD/native-$CURRENT_ABI/libds_game.so"
done
# shellcheck disable=SC2086
python3 - "$UNSIGNED" "$DEX" $LIB_ARGS <<'EOF'
import sys, zipfile
apk, dex, libs = sys.argv[1], sys.argv[2], sys.argv[3:]
with zipfile.ZipFile(apk, "a", zipfile.ZIP_DEFLATED) as archive:
    archive.write(dex, "classes.dex")
    for item in libs:  # .so хранится без сжатия — так надо для mmap на устройстве
        abi, path = item.split("=", 1)
        archive.write(path, "lib/%s/libds_game.so" % abi, compress_type=zipfile.ZIP_STORED)
EOF

"$BT/zipalign" -f -p 4 "$UNSIGNED" "$ALIGNED"

if [ -z "$KEYSTORE" ]; then
    KEYSTORE="$BUILD/debug.keystore"
    if [ ! -f "$KEYSTORE" ]; then
        echo "== keytool: новый отладочный ключ (один раз)"
        keytool -genkeypair -keystore "$KEYSTORE" -alias "$KEY_ALIAS" \
            -storepass "$STORE_PASS" -keypass "$KEY_PASS" \
            -keyalg RSA -keysize 2048 -validity 10950 \
            -dname "CN=Android Debug,O=Enjoer" >/dev/null 2>&1
    fi
fi
rm -f "$APK"
"$BT/apksigner" sign --ks "$KEYSTORE" --ks-key-alias "$KEY_ALIAS" \
    --ks-pass:"$STORE_PASS" --key-pass:"$KEY_PASS" --out "$APK" "$ALIGNED"
"$BT/apksigner" verify "$APK" >/dev/null && echo "== apksigner: подпись в порядке"

SIZE=$(du -h "$APK" | cut -f1)
echo ""
echo "Готово: $APK ($SIZE) — $TITLE [$PACKAGE], ABI: $ABIS"

if [ "$INSTALL" = 1 ]; then
    ADB="$SDK/platform-tools/adb"
    [ -x "$ADB" ] || { echo "build.sh: нет $ADB" >&2; exit 2; }
    "$ADB" install -r "$APK"
    if [ "$LAUNCH" = 1 ]; then
        "$ADB" shell am start -n "$PACKAGE/com.cb4.GameActivity"
    fi
fi
