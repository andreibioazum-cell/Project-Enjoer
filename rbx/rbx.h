/* Enjoer — вход в 3D-игру в духе Roblox: сразу мир, не мессенджер. */
#ifndef RBX_H
#define RBX_H

#include "runtime.h"

void rbx_key(const char *name, int down);
/* Сброс всех удержаний при потере фокуса/отмене жеста. */
void rbx_cancel_input(void);

#endif
