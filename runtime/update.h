/* Update check and one-button update (see update_win32.c). */
#pragma once
#include <stddef.h>
#include <stdint.h>

/* version: this build's version ("0.9.5"); game_dir: the folder of the running game;
 * rom: the embedded ROM the game is rebuilt from */
void update_init(const char *version, const char *game_dir, const uint8_t *rom, size_t rom_size);
void update_check(void);                    /* look for a newer release, in the background */
int  update_available(char *version, int size);   /* 1, once, when a newer release was found */
void update_begin(void);                    /* download and unpack the new builder, in the background */
int  update_progress(void);                 /* 0..99 working, 100 ready, -1 failed */
int  update_launch(void);                   /* start the new builder on this game; 1 = started (quit now) */
