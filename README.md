# Enjoer — 3D-плейс

Вместо мессенджера: **заходишь в игру — сразу 3D**, как в Roblox.
Написано **целиком на C** поверх того же софтверного рендера, что раньше
питал Cubic Battle 4 / DimScript:

- программный рендер `graphics.c` (HUD: круги, текст, прямоугольники);
- собственный **3D-растеризатор** в `rbx/rbx_render.c` (кубы, z-буфер,
  перспектива, туман, освещение граней);
- TTF-шрифт `ttf_font.c` и `game/assets/fonts/ChillRoundGothic_Heavy.ttf`;
- рантайм `runtime.c` и звук `sound.c`.

## Что внутри

Короткий экран «Заходим в игру…», и ты уже на зелёной baseplate:

- классический **нуб** (жёлтая голова, синий торс, зелёные ноги);
- дом, радужный обби, башня, деревья, бассейн, лава, батут;
- другие игроки гуляют по карте;
- монеты, джойстик слева, прыжок справа;
- камера от третьего лица: тяни палец / мышь, на ПК ещё WASD и пробел.

## Структура

```text
main.c                  главный цикл Android (native activity)
runtime.c / runtime.h   рантайм
graphics.c              2D HUD (+ ttf_font.c)
rbx/
├── rbx.h               вход (rbx_key)
├── rbx_render.c        софтверный 3D
└── rbx_game.c          мир, физика, аватар, управление
game/
├── AndroidManifest.xml
├── java/com/cb4/…      Java-мост клавиатуры
├── assets/fonts/…
└── sounds/…
tools/preview/          превью на ПК в браузере
```

## Превью на ПК (без Android)

```sh
gcc -O2 -std=c99 -Itools/preview/compat -I. \
    runtime.c graphics.c rbx/rbx_render.c rbx/rbx_game.c \
    tools/preview/host_compat.c tools/preview/host_main.c \
    -lm -lpthread -o preview
./preview --port 8090 --assets game/assets
# открой http://localhost:8090
```

`W A S D` — ходьба, пробел — прыжок, стрелки или перетаскивание — камера.

## Сборка APK

Каждый push собирает APK в GitHub Actions (`build-android`): CMake + NDK
(`arm64-v8a` и `armeabi-v7a`, API 29). Артефакт — **Enjoer-APK-Android10-14-arm64-arm32**.

## Шрифт

`ChillRoundGothic_Heavy` — SIL OFL (Warren2060/ChillRoundGothic).
