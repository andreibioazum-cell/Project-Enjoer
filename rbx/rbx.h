/* Enjoer — блочный мир от первого лица: публичное управление. */
#ifndef RBX_H
#define RBX_H

#include "runtime.h"

void rbx_key(const char *name, int down);
/* Сброс всех удержаний при потере фокуса/отмене жеста. */
void rbx_cancel_input(void);

#endif
