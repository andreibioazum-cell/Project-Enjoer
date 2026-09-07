/* App router: the engine *is* the app.
 *
 * game_* is the lifecycle every platform entry point calls (Android native
 * activity, PC preview, host tests). What runs behind it is the engine: a
 * project directory with a project.eng manifest, a .escn scene tree, physics
 * and C scripts. The launcher menu lists the projects it found and runs the
 * one you pick; the round button in the top-left corner, Escape or M reopens
 * it at any time.
 */
#include <stdio.h>
#include "engine.h"
#include "engine/eng_api.h"
#include "engine/eng_internal.h"
#include "engine/render/rend_internal.h"

/* Where projects live: the repository folder on the PC, the staged asset
 * folder of the same name inside the APK. The preview can override it. */
#ifndef ENG_PROJECT_ROOT
#define ENG_PROJECT_ROOT "projects"
#endif

static char project_root[512] = ENG_PROJECT_ROOT;
static int ready;
static int menu_open = 1;
static int pointer_down;

/* ── geometry shared with the tests ───────────────────────────────────── */

static float ui(void) {
    float u = fminf(screen_w / 960.0f, screen_h / 540.0f);
    return u > 0 ? u : 0;
}

void game_menu_button_geom(float *x, float *y, float *r) {
    float u = ui();
    if (x) *x = 26 * u;
    if (y) *y = 26 * u;
    if (r) *r = 17 * u;
}

int game_project_count(void) { return eng_project_count(); }

int game_current_project(void) { return eng_project_index(); }

const char *game_project_title(int index) { return eng_project_title(index); }

void game_menu_card_geom(int index, float *x, float *y, float *w, float *h) {
    float u = ui();
    int count = game_project_count();
    float cw, ch, gap, total, top;
    if (count < 1) count = 1;
    cw = fminf(620 * u, screen_w * .88f);
    ch = 96 * u;
    gap = 16 * u;
    total = ch * count + gap * (count - 1);
    top = screen_h * .5f - total * .5f + 30 * u;
    if (x) *x = screen_w * .5f - cw * .5f;
    if (y) *y = top + index * (ch + gap);
    if (w) *w = cw;
    if (h) *h = ch;
}

int game_menu_open(void) { return menu_open; }

/* ── launcher visuals ─────────────────────────────────────────────────── */

static void text_center_shadow(const char *s, float x, float y, uint32_t c, float scale) {
    float w = text_width(s) * scale;
    text_scaled(s, x - w * .5f + scale * 4, y + scale * 4, 0x88000000u, scale);
    text_scaled(s, x - w * .5f, y, c, scale);
}

static void draw_menu_button(void) {
    float x, y, r;
    game_menu_button_geom(&x, &y, &r);
    if (r <= 0) return;
    circle(x, y, r, 0xb3ffffffu);
    for (int i = -1; i <= 1; i++)
        rect(x - r * .45f, y + i * r * .36f - r * .09f, r * .9f, r * .18f, 0xff232a35u);
}

static void draw_menu(void) {
    float u = ui();
    int count = game_project_count();
    if (u <= 0) return;
    rect(0, 0, (float)screen_w, (float)screen_h, 0xff10141bu);
    text_center_shadow("ENJOER ENGINE", screen_w * .5f, screen_h * .09f, 0xffffffffu, 1.05f * u);
    text_center_shadow("projects · scenes · nodes · physics · C scripts",
                       screen_w * .5f, screen_h * .09f + 52 * u, 0xaa9fb4c8u, .42f * u);

    if (!count) {
        text_center_shadow("no projects found", screen_w * .5f, screen_h * .5f - 12 * u,
                           0xffffb0a0u, .55f * u);
        char hint[160];
        snprintf(hint, sizeof(hint), "expected a project.eng under %s/", eng_project_root());
        text_center_shadow(hint, screen_w * .5f, screen_h * .5f + 34 * u, 0x889fb4c8u, .38f * u);
        return;
    }

    for (int card = 0; card < count; card++) {
        float x, y, w, h;
        const char *title = game_project_title(card);
        const char *dir = eng_project_dir(card);
        int running = card == eng_project_index();
        game_menu_card_geom(card, &x, &y, &w, &h);
        roundrect(x, y, w, h, 12 * u, running ? 0xf22e3a49u : 0xf2232a35u);
        roundrect(x + 6 * u, y + 6 * u, 10 * u, h - 12 * u, 5 * u,
                  running ? 0xff2ee6a8u : 0xff58c8f0u);
        text_center_shadow(title && title[0] ? title : "project",
                           x + w * .5f, y + 16 * u, 0xffffffffu, .58f * u);
        if (dir)
            text_center_shadow(dir, x + w * .5f, y + 54 * u, 0xaa9fb4c8u, .34f * u);
        if (running)
            text_center_shadow("running", x + w - 56 * u, y + 8 * u, 0xff2ee6a8u, .30f * u);
    }
    text_center_shadow("tap a project to run it — round button, Esc or M brings this list back",
                       screen_w * .5f, screen_h - 40 * u, 0x889fb4c8u, .34f * u);
}

static void draw_status(void) {
    float u = ui();
    char line[192];
    if (u <= 0) return;
    snprintf(line, sizeof(line), "%s  ·  %d nodes  ·  %.0f fps",
             eng_project_name(), eng_scene_node_count(), rend_fps());
    text_scaled(line, 58 * u, screen_h - 30 * u, 0xaaffffffu, .34f * u);
}

/* ── lifecycle ────────────────────────────────────────────────────────── */

static void select_project(int index) {
    if (!eng_project_open(index)) return;
    pointer_down = 0;
    menu_open = 0;
}

void game_set_project_root(const char *root) {
    snprintf(project_root, sizeof(project_root), "%s", root && root[0] ? root : ENG_PROJECT_ROOT);
}

/* Run one project directory immediately (the preview's --project). It does not
 * have to be inside the scanned root. */
int game_open_project(const char *directory) {
    if (!ready || !eng_project_load(directory)) return 0;
    pointer_down = 0;
    menu_open = 0;
    return 1;
}

void game_init(AAssetManager *assets) {
    ready = eng_init(assets);
    if (!ready) return;
    eng_project_scan(project_root);   /* 0 projects is a state, not a failure */
    pointer_down = 0;
    menu_open = 1;
}

void game_update(void) {
    if (!ready) return;
    rend_perf_frame(dt);
    if (menu_open || !eng_project_active()) return;   /* frozen behind the menu */
    /* Engine time only advances while a project runs, so time-driven scripts
     * resume where they stopped instead of jumping. */
    eng_time_internal(app_now(), dt);
    eng_update((float)dt);
}

void game_draw(Buffer *buffer) {
    double start = app_now();
    if (!buffer) return;
    if (!menu_open && eng_project_active()) {
        if (!eng_draw(buffer)) {
            rect(0, 0, (float)screen_w, (float)screen_h, 0xff10141bu);
            text_center_shadow("this scene has no Camera3D", screen_w * .5f, screen_h * .5f,
                               0xffffb0a0u, .5f * ui());
        } else {
            draw_status();
        }
    } else {
        rect(0, 0, (float)screen_w, (float)screen_h, 0xff10141bu);
    }
    rend_render_time(app_now() - start);
    if (menu_open) draw_menu();
    else draw_menu_button();
}

void game_touch(float x, float y, int action, int id) {
    float u = ui();
    (void)id;
    if (u <= 0 || !ready) return;
    if (menu_open) {
        if (action != 0) return;                       /* react on release */
        for (int i = 0; i < game_project_count(); i++) {
            float cx, cy, cw, ch;
            game_menu_card_geom(i, &cx, &cy, &cw, &ch);
            if (x >= cx && x <= cx + cw && y >= cy && y <= cy + ch) {
                select_project(i);
                return;
            }
        }
        return;
    }
    /* the round button toggles the launcher */
    float bx, by, br;
    game_menu_button_geom(&bx, &by, &br);
    if (action == 0 && br > 0) {
        float dx = x - bx, dy = y - by;
        if (dx * dx + dy * dy <= br * br * 1.69f) {
            menu_open = 1;
            eng_input_reset();
            pointer_down = 0;
            return;
        }
    }
    if (action == 0) pointer_down = 1;
    else if (action != 2) pointer_down = 0;            /* up or cancel */
    eng_input_feed_pointer(x, y, pointer_down);
}

/* Browser/Android key names to the engine's polled key codes. */
static int key_code(const char *name) {
    if (!strcmp(name, "a") || !strcmp(name, "ArrowLeft")) return ENG_KEY_LEFT;
    if (!strcmp(name, "d") || !strcmp(name, "ArrowRight")) return ENG_KEY_RIGHT;
    if (!strcmp(name, "w") || !strcmp(name, "ArrowUp")) return ENG_KEY_UP;
    if (!strcmp(name, "s") || !strcmp(name, "ArrowDown")) return ENG_KEY_DOWN;
    if (!strcmp(name, "space")) return ENG_KEY_SPACE;
    if (!strcmp(name, "Shift")) return ENG_KEY_SHIFT;
    if (!strcmp(name, "A")) return ENG_KEY_A;
    if (!strcmp(name, "D")) return ENG_KEY_D;
    if (!strcmp(name, "W")) return ENG_KEY_W;
    if (!strcmp(name, "S")) return ENG_KEY_S;
    return -1;
}

void game_key(const char *name, int down) {
    int code;
    if (!name || !ready) return;
    if (down && (!strcmp(name, "Escape") || !strcmp(name, "m"))) {
        menu_open = !menu_open;
        eng_input_reset();
        pointer_down = 0;
        return;
    }
    if (menu_open) return;
    code = key_code(name);
    if (code >= 0) eng_input_feed_key(code, down);
}

void game_cancel_input(void) {
    pointer_down = 0;
    eng_input_reset();
}

void game_reset(void) {
    int index = eng_project_index();
    if (index >= 0) eng_project_open(index);
}

/* Nothing to persist: scenes are read-only assets and a project starts from
 * its scene file every time. The hook stays for the platform lifecycle. */
void game_save(void) {}
