/* Platformium lifecycle: level state machine (play, dying, clear, game
 * over), scoring, lives and the persistent progress file. */
#define _POSIX_C_SOURCE 200809L
#include "platformium_internal.h"
#include <stdio.h>

int pm_state = PM_STATE_PLAY;
int pm_lives = 3, pm_score, pm_coins, pm_level_coins;
float pm_timer, pm_state_time;
double pm_time;

static int ready;
static int current_level;
static int level_start_score;
static int unlocked_count = 1;
static int best_score[PM_MAX_LEVELS];
static int progress_dirty;
static int menu_request;
static float time_bonus;

void pm_score_add(int points) { pm_score += points; }
int pm_current_level(void) { return current_level; }

void pm_request_menu(void) { menu_request = 1; }
int platformium_take_menu_request(void) {
    int r = menu_request;
    menu_request = 0;
    return r;
}

/* ── progress persistence ────────────────────────────────────────────── */

static void progress_load(void) {
    char path[512];
    if (!app_save_path(path, sizeof(path), "platformium.save")) return;
    FILE *f = fopen(path, "r");
    if (!f) return;
    char line[96];
    while (fgets(line, sizeof(line), f)) {
        int a, b;
        if (sscanf(line, "unlocked %d", &a) == 1) unlocked_count = a;
        else if (sscanf(line, "best %d %d", &a, &b) == 2 && a >= 0 && a < PM_MAX_LEVELS)
            best_score[a] = b > 0 ? b : 0;
    }
    fclose(f);
    if (unlocked_count < 1) unlocked_count = 1;
    if (unlocked_count > pm_level_def_count()) unlocked_count = pm_level_def_count();
}

void platformium_progress_save(void) {
    if (!progress_dirty) return;
    char path[512], temp[512];
    if (!app_save_path(path, sizeof(path), "platformium.save") ||
        !app_save_path(temp, sizeof(temp), "platformium.save.tmp")) return;
    FILE *f = fopen(temp, "w");
    if (!f) return;
    fprintf(f, "v 1\nunlocked %d\n", unlocked_count);
    for (int i = 0; i < pm_level_def_count(); i++)
        fprintf(f, "best %d %d\n", i, best_score[i]);
    int ok = ferror(f) == 0;
    ok = ok && fclose(f) == 0;
    if (ok) {
        remove(path);
        if (rename(temp, path) == 0) progress_dirty = 0;
    } else {
        remove(temp);
    }
}

/* ── lifecycle ───────────────────────────────────────────────────────── */

int platformium_init(AAssetManager *assets) {
    if (!pm_levels_build(assets)) return 0;
    pm_textures_load(assets);
    snd_load("jump.wav");
    snd_load("place.wav");
    snd_load("break.wav");
    unlocked_count = 1;
    progress_load();
    ready = 1;
    app_log("Enjoer: Platformium with %d levels", pm_level_def_count());
    return 1;
}

void platformium_shutdown(void) {
    ready = 0;
}

int platformium_started(void) { return ready && pm_level_live() != NULL; }
int platformium_level_count(void) { return pm_level_def_count(); }

const char *platformium_level_name(int index) {
    const PmLevelDef *def = pm_level_def(index);
    return def ? def->name : "";
}

int platformium_level_unlocked(int index) {
    return index >= 0 && index < unlocked_count;
}

int platformium_level_best(int index) {
    if (index < 0 || index >= PM_MAX_LEVELS) return 0;
    return best_score[index];
}

void platformium_start_level(int index) {
    if (!ready) return;
    if (index < 0 || index >= pm_level_def_count()) return;
    pm_level_begin(index);
    PmLevelDef *level = pm_level_live();
    if (!level) return;
    current_level = index;
    level_start_score = pm_score;
    pm_state = PM_STATE_PLAY;
    pm_state_time = 0;
    pm_lives = 3;
    pm_coins = 0;
    pm_timer = (float)level->time_limit;
    pm_player_spawn(level->spawn_tx, level->spawn_ty);
    pm_actors_reset();
    pm_particles_clear();
    pm_camera_reset(level->spawn_tx + .5f, level->spawn_ty + .5f);
    pm_input_reset();
}

/* ── state transitions ───────────────────────────────────────────────── */

void pm_player_died(void) {
    if (pm_state != PM_STATE_PLAY) return;
    pm_lives--;
    pm_state = PM_STATE_DYING;
    pm_state_time = 0;
}

static void respawn(void) {
    extern float pm_respawn_x, pm_respawn_y;
    pm_player_revive(pm_respawn_x, pm_respawn_y);
    pm_actors_reset();
    pm_state = PM_STATE_PLAY;
    pm_state_time = 0;
}

void pm_level_clear(void) {
    if (pm_state != PM_STATE_PLAY) return;
    time_bonus = pm_timer > 0 ? (float)(int)pm_timer * 5.0f : 0;
    pm_score += (int)time_bonus;
    int level_score = pm_score - level_start_score;
    if (level_score > best_score[current_level]) best_score[current_level] = level_score;
    if (current_level + 1 < pm_level_def_count() && unlocked_count < current_level + 2) {
        unlocked_count = current_level + 2;
        app_log("platformium: unlocked level %d", current_level + 1);
    }
    progress_dirty = 1;
    platformium_progress_save();
    pm_state = PM_STATE_CLEAR;
    pm_state_time = 0;
    snd_play("place.wav");
    pm_camera_shake(.2f);
}

static void advance_or_menu(void) {
    int next = current_level + 1;
    if (next < pm_level_def_count()) platformium_start_level(next);
    else pm_request_menu();
}

static void retry_level(void) {
    pm_score = level_start_score;
    platformium_start_level(current_level);
}

/* ── frame hooks ─────────────────────────────────────────────────────── */

void platformium_update(float d) {
    if (!ready || !pm_level_live()) return;
    if (!isfinite(d) || d < 0) d = 0;
    if (d > .05f) d = .05f;
    pm_time += d;
    pm_state_time += d;
    pm_input_layout();
    switch (pm_state) {
        case PM_STATE_PLAY:
            pm_actors_update(d);
            pm_player_update(d);
            pm_particles_update(d);
            pm_camera_update(d);
            pm_timer -= d;
            if (pm_timer <= 0) {
                pm_timer = 0;
                pm_player_kill();
            }
            break;
        case PM_STATE_DYING:
            pm_particles_update(d);
            pm_camera_update(d);
            if (pm_state_time > 1.1f) {
                if (pm_lives > 0) respawn();
                else {
                    pm_state = PM_STATE_GAMEOVER;
                    pm_state_time = 0;
                }
            }
            break;
        case PM_STATE_CLEAR:
            pm_actors_update(d);
            pm_particles_update(d);
            pm_camera_update(d);
            if (pm_state_time < 2.0f) pm_particles_burst(
                pm_player_state()->x, pm_player_state()->y - 2.0f, 1, PM_FX_SPARK);
            break;
        default:
            pm_particles_update(d);
            break;
    }
}

void platformium_draw(void) {
    if (!ready || !pm_level_live()) return;
    pm_render_background((float)pm_time);
    pm_render_tiles((float)pm_time);
    pm_render_flag((float)pm_time);
    pm_actors_draw();
    pm_player_draw();
    pm_particles_draw();
    pm_hud_draw();
    pm_hud_overlay_draw();
}

void platformium_touch(float x, float y, int action, int id) {
    if (!ready || !pm_level_live()) return;
    if (action == 3) { pm_input_reset(); return; }
    if (pm_state == PM_STATE_CLEAR) {
        if (action == 0 && pm_state_time > .6f) advance_or_menu();
        return;
    }
    if (pm_state == PM_STATE_GAMEOVER) {
        if (action == 0 && pm_state_time > .6f) retry_level();
        return;
    }
    if (pm_state == PM_STATE_DYING) return;
    pm_input_touch(x, y, action, id);
}

void platformium_key(const char *name, int down) {
    if (!ready || !pm_level_live() || !name) return;
    if (down && !strcmp(name, "r")) { retry_level(); return; }
    if (pm_state == PM_STATE_CLEAR) {
        if (down && (!strcmp(name, "space") || !strcmp(name, "w") || !strcmp(name, "ArrowUp")) &&
            pm_state_time > .6f) advance_or_menu();
        return;
    }
    if (pm_state == PM_STATE_GAMEOVER) {
        if (down && (!strcmp(name, "space") || !strcmp(name, "w") || !strcmp(name, "ArrowUp")) &&
            pm_state_time > .6f) retry_level();
        return;
    }
    if (pm_state == PM_STATE_DYING) return;
    pm_input_key(name, down);
}

void platformium_cancel_input(void) { pm_input_reset(); }

/* ── introspection ───────────────────────────────────────────────────── */

void platformium_player_pos(float *x, float *y) { pm_player_pos(x, y); }
int platformium_player_grounded(void) { return pm_player_state()->grounded; }
int platformium_state(void) { return pm_state; }
int platformium_score(void) { return pm_score; }
int platformium_coins(void) { return pm_coins; }
int platformium_lives(void) { return pm_lives; }
float platformium_timer(void) { return pm_timer; }
int platformium_tile_at(int tx, int ty) { return pm_tile(tx, ty); }
int platformium_enemies_alive(void) { return pm_actors_alive(); }
int platformium_enemy_count(void) { return pm_actors_count(); }
int platformium_enemy_info(int i, float *x, float *y, int *kind, int *alive) {
    return pm_actors_info(i, x, y, kind, alive);
}

void platformium_level_size(int *w, int *h) {
    PmLevelDef *level = pm_level_live();
    if (w) *w = level ? level->w : 0;
    if (h) *h = level ? level->h : 0;
}

void platformium_goal_pos(int *tx, int *ty) {
    PmLevelDef *level = pm_level_live();
    if (tx) *tx = level ? level->goal_tx : -1;
    if (ty) *ty = level ? level->goal_ty : -1;
}
