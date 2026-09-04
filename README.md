# Enjoer Messenger

Мессенджер с интерфейсом «как у Телеграма», написанный **целиком на C** поверх
технологий, которые раньше питали игру Cubic Battle 4 / DimScript:

- тот же **программный рендер** `graphics.c` (командный буфер: `rect`,
  `roundrect`, `circle`, `ring`, `line`, `tri`, `text`, PNG-текстуры);
- тот же **собственный TTF-растеризатор** `ttf_font.c` (атлас глифов,
  scanline-заливка, без FreeType);
- тот же **шрифт** — `game/assets/fonts/ChillRoundGothic_Heavy.ttf`
  (Chill Round Gothic, OFL);
- тот же рантайм `runtime.c` (строки, массивы, защита от падений, мост к
  системной клавиатуре Android) и звук `sound.c` (WAV → AudioTrack).

Скриптового слоя больше нет: раньше `gen.py` + `ds_compiler.py` компилировали
`.ds`-логику в C, теперь все хуки `init/update/draw/touch/reset` реализованы
напрямую в `messenger/*.c`. DimScript-компилятор и игровая сетевая логика
(Firebase) остались только в истории git.

## Что умеет

**Список чатов** (главный экран):

- шапка с меню и поиском: плашка «Поиск», живой фильтр по имени, курсор, крестик;
- строки в стиле Telegram: цветные аватарки с инициалами (у «Избранного» —
  закладка), индикатор «в сети», превью последнего сообщения («Вы: …», в
  группах — с именем автора), время («07:35», «Вчера», «Пн», «5 марта»),
  счётчики непрочитанных (серые, если чат замьючен);
- скролл с инерцией и резиной, подсветка нажатия, FAB-карандаш.

**Разговор**:

- обои с мягким зелёным градиентом и плывущим узором, как в Телеграме;
- пузыри с хвостиками: белые входящие, светло-зелёные исходящие; время и
  статусы внутри пузыря — часики → ✓ → зелёные ✓✓;
- разделители дат («Сегодня» / «Вчера» / «5 марта»);
- в группах имена авторов над сообщениями своим цветом;
- индикатор «печатает…» (прыгающие точки в пузыре и в шапке);
- автоответчик: собеседник «печатает» и отвечает, непрочитанные растут,
  чаты сохраняются на диск (`enjoer_chats.dat`);
- поле ввода как в Телеграме: смайл, скрепка, курсор, кнопка отправки
  (самолётик) / микрофон, кнопка «вниз» при прокрутке.

**Звук**: `game/sounds/send.wav` и `notify.wav` (отправка / уведомление).

## Структура

```text
main.c                  главный цикл Android (native activity)
runtime.c / runtime.h   рантайм: лог, строки, массивы, клавиатура (JNI)
graphics.c              софтверный рендер (+ ttf_font.c внутри)
ttf_font.c              парсер TrueType и запекание атласа глифов
sound.c                 WAV-декодер и вывод звука (AudioTrack)
messenger/
├── msg.h               общий заголовок: модель, палитра, API экранов
├── msg_app.c           хуки init/update/draw/touch/reset, переходы экранов
├── msg_data.c          чаты/сообщения, демо-данные, сохранение, автоответы
├── msg_ui.c            аватарки, иконки примитивами, текст, время
├── msg_chats.c         экран «список чатов»
└── msg_chat.c          экран разговора: пузыри, раскладка, прокрутка
game/
├── AndroidManifest.xml манифест APK (NativeActivity com.cb4.GameActivity)
├── java/com/cb4/…      Java-мост к системной клавиатуре (IME)
├── assets/fonts/…      ChillRoundGothic_Heavy.ttf (сабсет OFL-шрифта)
└── sounds/…            send.wav, notify.wav
tools/preview/          превью на ПК: те же C-файлы + HTTP-сервер для браузера
third_party/            stb_image.h / stb_image_write.h (public domain)
```

## Превью на ПК (без Android)

Тот же код приложения собирается обычным `gcc` и открывается в браузере —
сервер отдаёт кадры и принимает касания/клавиатуру:

```sh
gcc -O2 -std=c99 -Itools/preview/compat -I. -Imessenger \
    runtime.c graphics.c messenger/msg_*.c messenger/msg_app.c \
    tools/preview/host_compat.c tools/preview/host_main.c \
    -lm -lpthread -o preview
./preview --port 8090 --assets game/assets --data /tmp/enjoer-data
# открой http://localhost:8090
```

В браузере: клик — касание, печать с клавиатуры передаётся в приложение,
`Enter` — отправить. На Android клавиатуру даёт системный IME через
`game/java/com/cb4/GameActivity.java`.

## Сборка APK

Каждый push собирает APK в GitHub Actions (job `build-android`): CMake + NDK
(`arm64-v8a` и `armeabi-v7a`, API 29), `stage_assets.py` кладёт в APK шрифт,
звуки и Java-активность, артефакт — **Enjoer-Messenger-APK-Android10-14-arm64-arm32**.

Локально:

```sh
cmake -B build \
  -DCMAKE_TOOLCHAIN_FILE=$ANDROID_NDK_ROOT/build/cmake/android.toolchain.cmake \
  -DANDROID_ABI=arm64-v8a -DANDROID_PLATFORM=android-29 \
  -DANDROID_NDK=$ANDROID_NDK_ROOT
cmake --build build
python3 stage_assets.py game/assets staging/assets
```

## Шрифт

`ChillRoundGothic_Heavy` — открытый шрифт (SIL OFL, проект
Warren2060/ChillRoundGothic, «寒蝉圆黑体»); в репозитории лежит сабсет
(~76 КБ) с латиницей, кириллицей и знаками интерфейса (`…`, «», —, ✓ и т.д.).
Атлас запекается один раз при старте, как и в DimScript-версии движка.
