#ifndef __AYAB_TERM_H
#define __AYAB_TERM_H

#include "machine.h"
#include "shield.h"

void ayab_display(int argc, char *argv[], void *(*avr_run_thread)(void *), machine_t *machine, shield_t *shield);

#endif
