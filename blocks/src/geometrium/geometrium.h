/* Enjoer first-person block world: public controls. */
#ifndef GEOMETRIUM_H
#define GEOMETRIUM_H

#include "engine.h"

void geometrium_key(const char *name, int down);
/* Release every held control on focus loss or gesture cancel. */
void geometrium_cancel_input(void);

#endif
