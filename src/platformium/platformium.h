/* Platformium — Enjoer's 2D side-scrolling platformer playset.
 *
 * It is a small platformer *toolkit* on top of the shared 2D engine
 * (graphics, sound, assets, save storage): tile-based levels parsed from
 * plain-text maps, an AABB character controller with coyote time and jump
 * buffering, patrolling and flying enemies, moving platforms, springs,
 * one-way platforms, spikes and lava, coins, checkpoints, a goal flag,
 * particles, a smooth lookahead camera and touch/keyboard controls.
 *
 * Three levels are compiled in and any number of extra levels can ship as
 * text assets (text files in assets/levels, see the README there), which is
 * how more platformers get made without touching C code. */
#ifndef PLATFORMIUM_H
#define PLATFORMIUM_H

#include "engine.h"

/* Parse every level (built-in + optional asset levels) and load the tile
 * textures. Called once by the app router before any level can start. */
int platformium_init(AAssetManager *assets);
void platformium_shutdown(void);

/* Start a level by index (0-based) and enter play mode. */
void platformium_start_level(int index);
int platformium_level_count(void);
const char *platformium_level_name(int index);
int platformium_level_unlocked(int index);
int platformium_level_best(int index);        /* best score, 0 = unplayed */
int platformium_started(void);                /* a level is loaded */

/* Frame hooks, only valid after platformium_init(). */
void platformium_update(float d);
void platformium_draw(void);
void platformium_touch(float x, float y, int action, int pointer_id);
void platformium_key(const char *name, int down);
/* Release every held control on focus loss or gesture cancel. */
void platformium_cancel_input(void);
/* Persist unlocked levels and best scores (debounced internally). */
void platformium_progress_save(void);
/* The playset wants the app menu opened (e.g. after the final level).
 * The router consumes this once per frame. */
int platformium_take_menu_request(void);

/* ── introspection (tests, tooling, future scripting) ─────────────────── */
enum {
    PLATFORMIUM_TILE_EMPTY, PLATFORMIUM_TILE_SOLID, PLATFORMIUM_TILE_ONEWAY,
    PLATFORMIUM_TILE_SPIKE, PLATFORMIUM_TILE_LAVA, PLATFORMIUM_TILE_COIN,
    PLATFORMIUM_TILE_SPRING, PLATFORMIUM_TILE_GOAL, PLATFORMIUM_TILE_CHECK
};
enum {
    PLATFORMIUM_STATE_PLAY, PLATFORMIUM_STATE_DYING,
    PLATFORMIUM_STATE_CLEAR, PLATFORMIUM_STATE_GAMEOVER
};
void platformium_player_pos(float *x, float *y);
int platformium_player_grounded(void);
int platformium_state(void);
int platformium_score(void);
int platformium_coins(void);
int platformium_lives(void);
float platformium_timer(void);
int platformium_enemies_alive(void);
int platformium_enemy_count(void);
int platformium_enemy_info(int index, float *x, float *y, int *kind, int *alive);
int platformium_tile_at(int tx, int ty);    /* PLATFORMIUM_TILE_* code */
void platformium_level_size(int *w, int *h);
void platformium_goal_pos(int *tx, int *ty);

#endif
