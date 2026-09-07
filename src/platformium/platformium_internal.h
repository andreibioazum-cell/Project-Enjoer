/* Internal API of the Platformium 2D platformer playset.
 * World units are tiles (1.0 = one map cell); +y points down. */
#ifndef PLATFORMIUM_INTERNAL_H
#define PLATFORMIUM_INTERNAL_H
#include "platformium/platformium.h"
#include <math.h>

enum { PM_MAX_W = 256, PM_MAX_H = 64, PM_MAX_LEVELS = 16 };
enum { PM_MAX_ENEMIES = 64, PM_MAX_PLATFORMS = 24, PM_MAX_PARTICLES = 320 };

/* Tile codes stored in the level grid. */
enum {
    PM_EMPTY,     /* '.' or ' ' */
    PM_SOLID,     /* '#' full block */
    PM_ONEWAY,    /* '=' jump-through platform */
    PM_SPIKE,     /* '^' floor hazard */
    PM_LAVA,      /* '~' liquid hazard */
    PM_COIN,      /* 'o' collectible */
    PM_SPRING,    /* 'S' solid, bouncy top */
    PM_GOAL,      /* 'G' finish flag */
    PM_CHECK,     /* 'C' checkpoint pole */
    PM_TILE_COUNT
};
enum { PM_THEME_GRASS, PM_THEME_STONE, PM_THEME_SAND, PM_THEME_COUNT };
enum { PM_ENEMY_WALKER, PM_ENEMY_FLYER };
enum { PM_FX_DUST, PM_FX_SPARK, PM_FX_BURST, PM_FX_LAVA };
enum { PM_STATE_PLAY, PM_STATE_DYING, PM_STATE_CLEAR, PM_STATE_GAMEOVER };

/* Character controller tuning (tiles, seconds). */
#define PM_GRAVITY 46.0f
#define PM_MAX_FALL 24.0f
#define PM_RUN_SPEED 7.0f
#define PM_RUN_ACCEL 60.0f
#define PM_AIR_ACCEL 40.0f
#define PM_FRICTION 54.0f
#define PM_AIR_DRAG 4.0f
#define PM_JUMP_VEL 15.4f          /* ~2.6 tiles high */
#define PM_JUMP_CUT .45f           /* rising speed multiplier on release */
#define PM_COYOTE_TIME .09f        /* late jumps after leaving a ledge */
#define PM_JUMP_BUFFER .12f        /* early jumps before landing */
#define PM_SPRING_VEL 23.5f
#define PM_STOMP_BOUNCE 10.0f
#define PM_STOMP_BOUNCE_HELD 15.0f

typedef struct { char name[40]; int tx, ty, vertical; } PmSpawnNote;
typedef struct {
    char name[40];
    int w, h, theme, time_limit;
    unsigned char tiles[PM_MAX_W * PM_MAX_H];
    int spawn_tx, spawn_ty, goal_tx, goal_ty, coins_total;
    struct { int tx, ty, kind; } enemies[PM_MAX_ENEMIES]; int enemy_count;
    struct { int tx, ty, vertical; } lifts[PM_MAX_PLATFORMS]; int lift_count;
} PmLevelDef;

typedef struct {
    float x, y, vx, vy;          /* center position, tiles */
    int dir, grounded, alive;
    float coyote, buffer;        /* timers for late/early jumps */
    float squash, anim, invuln;  /* visuals and respawn protection */
} PmPlayer;

typedef struct {
    float x, y, px, py;          /* center, previous center */
    int kind, dir, alive;
    float t, squash;
} PmEnemy;

typedef struct {
    float x, y, w, h;            /* current box (center based) */
    float px, py;                /* previous position, for carrying */
    float ax, ay, bx, by;        /* patrol endpoints */
    float t, period;
} PmLift;

typedef struct {
    float x, y, vx, vy, life, max_life, size;
    uint32_t color; int gravity;
} PmParticle;

/* ── platformium_level.c ── */
int pm_levels_build(AAssetManager *assets);
int pm_level_parse(const char *text, size_t size, PmLevelDef *out);
const PmLevelDef *pm_level_def(int index);
int pm_level_def_count(void);
int pm_level_install(const PmLevelDef *def);
void pm_level_begin(int index);          /* copy def into the live grid */
PmLevelDef *pm_level_live(void);
int pm_tile(int tx, int ty);             /* PM_EMPTY outside the map */
int pm_tile_solid(int tx, int ty);       /* blocks movement: SOLID/SPRING */
int pm_tile_taken(int tx, int ty);
void pm_tile_take(int tx, int ty);
/* Axis-aligned box vs solid tiles; oneway only catches falling boxes whose
 * previous bottom was above the tile top. */
int pm_box_hits(float x, float y, float hw, float hh, float prev_bottom, int oneway);

/* ── platformium_input.c ── */
void pm_input_reset(void);
void pm_input_layout(void);
void pm_input_touch(float x, float y, int action, int id);
void pm_input_key(const char *name, int down);
/* control geometry for the HUD visuals and tests: 0 left, 1 right, 2 jump */
void pm_input_geom(int control, float *x, float *y, float *r);
float pm_input_move(void);               /* -1 .. 1 */
int pm_input_jump_held(void);
int pm_input_jump_take_press(void);      /* buffered press, consumed once */
void pm_input_draw(void);

/* ── platformium_player.c ── */
void pm_player_spawn(int tx, int ty);
void pm_player_revive(float x, float y);  /* checkpoint respawn, invulnerable */
void pm_player_update(float d);
void pm_player_kill(void);
void pm_player_draw(void);
void pm_player_pos(float *x, float *y);
void pm_player_shift(float dx, float dy); /* lift rides apply their delta */
int pm_player_rides(const PmLift *lift);  /* standing on this lift? */
const PmPlayer *pm_player_state(void);
PmPlayer *pm_player_mut(void);
void pm_spring_fx(float *x, float *y, float *amount);
#define PM_PLAYER_HW .34f                 /* half width */
#define PM_PLAYER_HH .46f                 /* half height */

/* ── platformium_actors.c ── */
void pm_actors_reset(void);              /* spawn enemies/lifts from grid */
int pm_actors_alive(void);
int pm_actors_count(void);
int pm_actors_info(int i, float *x, float *y, int *kind, int *alive);
void pm_actors_update(float d);
void pm_actors_draw(void);

/* ── platformium_camera.c ── */
void pm_camera_reset(float x, float y);
void pm_camera_update(float d);
void pm_camera_shake(float amount);
void pm_camera_center(float *x, float *y);
float pm_tile_px(void);                  /* pixels per tile this frame */
void pm_world_to_screen(float wx, float wy, float *sx, float *sy);

/* ── platformium_particles.c ── */
void pm_particles_clear(void);
void pm_particles_burst(float x, float y, int count, int kind);
void pm_particles_update(float d);
void pm_particles_draw(void);

/* ── platformium_render.c ── */
void pm_textures_load(AAssetManager *assets);
void pm_render_background(float time);
void pm_render_tiles(float time);
void pm_render_flag(float time);

/* ── platformium_hud.c ── */
void pm_hud_draw(void);
void pm_hud_overlay_draw(void);          /* clear / game-over panels */
/* Small centered-text helper shared by HUD and overlays. */
void pm_text_center(const char *s, float x, float y, uint32_t color, float scale);

/* ── platformium_game.c (module state shared with render/hud) ── */
extern int pm_state;
extern int pm_lives, pm_score, pm_coins, pm_level_coins;
extern float pm_timer, pm_state_time;
extern double pm_time;                   /* wall-clock accumulator for anims */
void pm_score_add(int points);
void pm_request_menu(void);              /* ask the app router for the menu */
int pm_current_level(void);
const PmLift *pm_lift_list(int *count);

#endif
