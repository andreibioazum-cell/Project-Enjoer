/* sound/sound_internal.h — общий интерфейс звукового модуля.
 * Микшер и WAV-декодер — в sound.c, вывод (AudioTrack JNI) — в
 * sound_android.c. На ПК-превью модуль не собирается (стабы в
 * tools/preview/host_compat.c). */
#ifndef SOUND_INTERNAL_H
#define SOUND_INTERNAL_H

#include "runtime.h"

#define SND_RATE 44100

/* заполнить буфер смиксованным звуком (кадры стерео, 2*frames int16) */
void snd_frame(int16_t *out, int frames);

/* бэкенд вывода (sound_android.c) */
void snd_set_java_vm(void *vm);
int snd_backend_start(void);
void snd_backend_stop(void);
void snd_backend_pause(void);
void snd_backend_resume(void);

#endif
