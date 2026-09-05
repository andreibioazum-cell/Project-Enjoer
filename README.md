# Enjoer — 3D-плейс

Вместо мессенджера: **заходишь в игру — сразу 3D**, как в Roblox.
Написано **целиком на C** поверх того же софтверного рендера, что раньше
питал Cubic Battle 4 / DimScript:

- программный рендер `graphics/` (HUD: круги, текст, прямоугольники);
- собственный **3D-растеризатор** в `rbx/rbx_render.c` (кубы, z-буфер,
  перспектива, туман, освещение граней, плавный апскейл);
- TTF-шрифт `graphics/ttf/` и `game/assets/fonts/ChillRoundGothic_Heavy.ttf`;
- рантайм в `core/` и звук в `sound/`.

Каждый `.c`-файл — отдельная единица компиляции (без «include .c»),
в каждом не больше 500 строк.

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
runtime.h               общий публичный API
core/
├── log.c               консоль (кольцевой буфер) + логирование
├── state.c             глобальное состояние, ошибки, protected-вызовы
├── strings.c           пул строк + строковые хелперы
├── arrays.c            динамические массивы (DSArray)
└── keyboard_android.c  мост к системной клавиатуре Android (JNI)
graphics/
├── gfx_internal.h      внутренние объявления модуля
├── gfx_frame.c         очередь команд, жизненный цикл, ассеты
├── gfx_draw.c          2D-примитивы (rect/circle/ring/line/tri)
├── gfx_texture.c       PNG-текстуры (stb) и их отрисовка
├── gfx_text.c          текст, метрики, экран ошибки
└── ttf/
    ├── ttf_internal.h  интерфейс минимального TTF-движка
    ├── ttf_outline.c   чтение таблиц и контуров глифов
    └── ttf_font.c      запекание атласа, cmap, метрики
sound/
├── sound_internal.h    интерфейс звукового модуля
├── sound.c             WAV-декодер, микшер, API звуков
└── sound_android.c     вывод через AudioTrack (JNI)
rbx/
├── rbx.h               публичный вход (rbx_key)
├── rbx_internal.h      внутренние связи модуля
├── rbx_render.c        софтверный 3D (z-буфер, туман, апскейл)
├── rbx_world.c         мир, монеты, боты
├── rbx_player.c        физика игрока, камера, клавиатура
├── rbx_input.c         тач, джойстик, кнопка прыжка
├── rbx_scene.c         отрисовка 3D-сцены
├── rbx_hud.c           2D-интерфейс поверх 3D
└── rbx_game.c          хуки init/update/draw/touch
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
    core/log.c core/state.c core/strings.c core/arrays.c \
    graphics/gfx_frame.c graphics/gfx_draw.c graphics/gfx_texture.c \
    graphics/gfx_text.c graphics/ttf/ttf_outline.c graphics/ttf/ttf_font.c \
    rbx/rbx_render.c rbx/rbx_world.c rbx/rbx_player.c rbx/rbx_input.c \
    rbx/rbx_scene.c rbx/rbx_hud.c rbx/rbx_game.c \
    tools/preview/host_compat.c tools/preview/host_main.c \
    -lm -lpthread -o preview
./preview --port 8090 --assets game/assets
# открой http://localhost:8090
```

`W A S D` — ходьба, пробел — прыжок, стрелки или перетаскивание — камера.
Слева джойстик, справа прыжок. Собери монеты на радужном обби.

## Качество картинки

- 3D рендерится в **полном разрешении** на экранах до ~1.5 Мп; на более
  крупных — в половинном, но с **плавным (билинейным) апскейлом**, а не
  дублированием пикселей блоками;
- текст запекается в атлас на 48px и сэмплируется билинейно — гладкие
  глифы при любом масштабе интерфейса.

## Сборка APK

Каждый push собирает APK в GitHub Actions (`build-android`): CMake + NDK
(`arm64-v8a` и `armeabi-v7a`, API 29). Артефакт — **Enjoer-APK-Android10-14-arm64-arm32**.

## Шрифт

`ChillRoundGothic_Heavy` — SIL OFL (Warren2060/ChillRoundGothic).
