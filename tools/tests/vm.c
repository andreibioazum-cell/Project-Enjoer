/* Regression test for the DimScript interpreter that runs inside the engine.
 *
 * It is intentionally dependency free: the same .ds sources the games use are
 * fed to the VM, and the assertions check state, the recorded frame and the
 * error paths. */
#include "ds_vm.h"
#include "dimscript_runtime.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CHECK(value)                                                                            \
    do {                                                                                        \
        if (!(value)) {                                                                         \
            fprintf(stderr, "FAIL %s:%d: %s (%s)\n", __FILE__, __LINE__, #value,                \
                    ds_vm_error(vm));                                                           \
            return 1;                                                                          \
        }                                                                                       \
    } while (0)

#define CHECK_TEXT(needle, haystack)                                                            \
    do {                                                                                        \
        if (!(haystack) || !strstr((haystack), (needle))) {                                     \
            fprintf(stderr, "FAIL %s:%d: expected '%s' in '%s'\n", __FILE__, __LINE__, (needle),\
                    (haystack) ? (haystack) : "");                                              \
            return 1;                                                                           \
        }                                                                                       \
    } while (0)

static char *read_file(const char *path, size_t *length) {
    FILE *file = fopen(path, "rb");
    if (!file) return NULL;
    if (fseek(file, 0, SEEK_END) != 0) { fclose(file); return NULL; }
    const long size = ftell(file);
    rewind(file);
    if (size <= 0 || size > 1 << 20) { fclose(file); return NULL; }
    char *buffer = (char *)malloc((size_t)size + 1);
    if (!buffer) { fclose(file); return NULL; }
    if (fread(buffer, 1, (size_t)size, file) != (size_t)size) {
        free(buffer);
        fclose(file);
        return NULL;
    }
    buffer[size] = '\0';
    fclose(file);
    if (length) *length = (size_t)size;
    return buffer;
}

int main(void) {
    DsVM *vm = NULL;
    ds_runtime_init();
    ds_engine_reset(960, 540);

    /* --- the README example, interpreted straight from examples/ ---------- */
    size_t length = 0;
    char *source = read_file("examples/clicker.ds", &length);
    CHECK(source);
    vm = ds_vm_create();
    CHECK(vm);
    CHECK(ds_vm_add_source(vm, "main", source, length));
    CHECK(ds_vm_link(vm));
    CHECK(ds_vm_has_function(vm, "touchpressed"));
    CHECK(ds_vm_global_count(vm) == 1);
    CHECK(!strcmp(ds_vm_global_name(vm, 0), "game"));
    enjoer_frame_begin(960, 540);

    CHECK(ds_vm_start(vm));
    CHECK(!strcmp(ds_vm_global_type(vm, 0), "struct")); /* initialized by `new` */
    CHECK(ds_vm_global_field_number(vm, "game", "score") == 0.0);
    CHECK(ds_vm_global_field_number(vm, "game", "text_scale") == 1.0);

    enjoer_frame_begin(960, 540);
    CHECK(ds_vm_call_touch(vm, "touchpressed", 0, 120.0, 240.0));
    CHECK(ds_vm_call_touch(vm, "touchpressed", 0, 120.0, 240.0));
    CHECK(ds_vm_global_field_number(vm, "game", "score") == 2.0);
    CHECK(ds_vm_global_field_number(vm, "game", "text_scale") == 1.6);

    ds_engine_new_frame(0.0, 1.0 / 60.0, 60.0);
    CHECK(ds_vm_call_number(vm, "update", 1.0 / 60.0));
    CHECK(ds_vm_global_field_number(vm, "game", "text_scale") < 1.6);
    CHECK(ds_vm_call_void(vm, "draw"));
    const EnjoerFrame *frame = enjoer_frame();
    CHECK(frame->text_count == 3);
    CHECK(frame->vertex_count == 0); /* the clicker draws text only */
    CHECK(strstr(frame->texts[1].text, "Счет: 2") != NULL);
    CHECK(frame->texts[2].scale > 0.7f && frame->texts[2].scale <= 1.0f);

    for (int index = 0; index < 240; ++index) {
        ds_engine_new_frame(index / 60.0, 1.0 / 60.0, 60.0);
        CHECK(ds_vm_call_number(vm, "update", 1.0 / 60.0));
        enjoer_frame_begin(960, 540);
        CHECK(ds_vm_call_void(vm, "draw"));
        ds_vm_collect(vm);
    }
    CHECK(ds_vm_global_field_number(vm, "game", "text_scale") == 1.0);
    CHECK(ds_vm_heap_bytes(vm) < 64u * 1024u); /* strings must not leak */
    CHECK(ds_vm_call_void(vm, "quit"));
    ds_vm_destroy(vm);
    free(source);

    /* --- multi-file game: main.ds requires two modules --------------------- */
    vm = ds_vm_create();
    size_t main_length = 0, blocks_length = 0, hud_length = 0;
    char *main_source = read_file("games/brick/main.ds", &main_length);
    char *blocks_source = read_file("games/brick/blocks.ds", &blocks_length);
    char *hud_source = read_file("games/brick/hud.ds", &hud_length);
    CHECK(main_source && blocks_source && hud_source);
    CHECK(ds_vm_add_source(vm, "main", main_source, main_length));
    CHECK(ds_vm_add_source(vm, "blocks", blocks_source, blocks_length));
    CHECK(ds_vm_add_source(vm, "hud", hud_source, hud_length));
    CHECK(ds_vm_link(vm));
    CHECK(ds_vm_script_count(vm) == 3);
    CHECK(ds_vm_has_function(vm, "update") && ds_vm_has_function(vm, "draw"));
    CHECK(ds_vm_has_function(vm, "launch_ball")); /* declared in blocks.ds */
    CHECK(ds_vm_start(vm));
    CHECK(!strcmp(ds_vm_global_type(vm, 0), "struct")); /* initialized by `new` */
    CHECK(ds_vm_global_field_number(vm, "game", "score") == 0.0);
    for (int index = 0; index < 20; ++index) {
        ds_engine_new_frame(index / 60.0, 1.0 / 60.0, 60.0);
        enjoer_frame_begin(960, 540);
        CHECK(ds_vm_call_void(vm, "draw"));
        CHECK(enjoer_frame()->vertex_count > 100); /* bricks are triangles */
        CHECK(ds_vm_call_number(vm, "update", 1.0 / 60.0));
        ds_vm_collect(vm);
    }
    CHECK(ds_vm_global_field_number(vm, "game", "lives") >= 0.0);
    ds_vm_destroy(vm);
    free(main_source);
    free(blocks_source);
    free(hud_source);

    /* --- language surface: lists, loops, methods, builtins, errors -------- */
    static const char *LANGUAGE_TEST =
        "struct Counters {\n"
        "    total: int\n"
        "    ratio: float\n"
        "    label: string\n"
        "    flags: bool\n"
        "    items: list\n"
        "}\n"
        "counters = new Counters\n"
        "tick = 0\n"
        "\n"
        "sum_to(n: int): int {\n"
        "    total = 0\n"
        "    for i = 1, n do\n"
        "        total = total + i\n"
        "    }\n"
        "    return total\n"
        "}\n"
        "\n"
        "load() {\n"
        "    counters.total = sum_to(10)\n"
        "    counters.items = [10, 20, 30]\n"
        "    counters.items.push(40)\n"
        "    counters.items[0] = counters.items[-1] + 5\n"
        "    counters.items.remove(1)\n"
        "    counters.ratio = counters.total / 4.0\n"
        "    counters.label = \"sum=\" .. counters.total .. \" n=\" .. counters.items.count\n"
        "    counters.flags = counters.total > 40 and counters.items.count == 3\n"
        "    while tick < 3 do\n"
        "        tick = tick + 1\n"
        "        if tick == 2 then\n"
        "            continue\n"
        "        }\n"
        "        counters.total = counters.total + 1000\n"
        "    }\n"
        "    render.clear(0.05, 0.08, 0.16)\n"
        "    render.color(1.0, 0.4, 0.2)\n"
        "    render.rect(10.0, 20.0, 30.0, 40.0)\n"
        "    render.circle(200.0, 100.0, 25.0)\n"
        "    render.line(0.0, 0.0, 100.0, 100.0, 2.0)\n"
        "    render.text(\"label \" .. counters.label, 12.0, 34.0, 1.0)\n"
        "    print(\"counters \" .. counters.label)\n"
        "    if engine.width() > 100.0 then\n"
        "        counters.ratio = counters.ratio + math.floor(1.7)\n"
        "    } else {\n"
        "        counters.ratio = 0.0\n"
        "    }\n"
        "}\n";
    vm = ds_vm_create();
    CHECK(ds_vm_add_source(vm, "main", LANGUAGE_TEST, (size_t)-1));
    CHECK(ds_vm_link(vm));
    enjoer_frame_begin(960, 540);
    CHECK(ds_vm_start(vm));
    /* sum_to(10) is 55, the while loop adds 1000 twice (one `continue` step is
     * skipped), and the ratio gains math.floor(1.7). */
    CHECK(ds_vm_global_field_number(vm, "counters", "total") == 2055.0);
    CHECK(ds_vm_global_field_number(vm, "counters", "ratio") == 55.0 / 4.0 + 1.0);
    CHECK(!strcmp(ds_vm_global_field_string(vm, "counters", "label", NULL), "sum=55 n=3"));
    CHECK(!strcmp(ds_vm_global_type(vm, 1), "int"));
    CHECK(ds_vm_global_number(vm, "tick") == 3.0);
    CHECK(enjoer_frame()->text_count == 1);
    /* rect = 6 verts, circle = 24 triangles, line = one quad */
    CHECK(enjoer_frame()->vertex_count == 6 + 72 + 6);
    CHECK(enjoer_frame()->has_clear);
    ds_vm_destroy(vm);

    /* --- link errors are reported instead of crashing --------------------- */
    static const char *BAD_NAME = "load() {\n    unknown_thing = 1\n    x = missing.name\n}\n";
    vm = ds_vm_create();
    CHECK(ds_vm_add_source(vm, "main", BAD_NAME, (size_t)-1));
    CHECK(!ds_vm_link(vm));
    CHECK_TEXT("неизвестное имя", ds_vm_error(vm));
    ds_vm_destroy(vm);

    static const char *BAD_CALLBACK = "load(x: float) {\n    y = x\n}\n";
    vm = ds_vm_create();
    CHECK(ds_vm_add_source(vm, "main", BAD_CALLBACK, (size_t)-1));
    CHECK(!ds_vm_link(vm));
    CHECK_TEXT("callback", ds_vm_error(vm));
    ds_vm_destroy(vm);

    static const char *BAD_REQUIRE = "require \"nope\"\nload() {\n}\n";
    vm = ds_vm_create();
    CHECK(ds_vm_add_source(vm, "main", BAD_REQUIRE, (size_t)-1));
    CHECK(!ds_vm_link(vm));
    CHECK_TEXT("nope.ds", ds_vm_error(vm));
    ds_vm_destroy(vm);

    /* --- a runtime error stops the callback, not the process -------------- */
    static const char *RUNTIME_ERROR =
        "value = 0\n"
        "load() {\n"
        "    value = 1.0 / 0.0\n"
        "}\n";
    vm = ds_vm_create();
    CHECK(ds_vm_add_source(vm, "main", RUNTIME_ERROR, (size_t)-1));
    CHECK(ds_vm_link(vm));
    CHECK(!ds_vm_start(vm));
    CHECK_TEXT("деление на ноль", ds_vm_error(vm));
    ds_vm_destroy(vm);

    /* --- an endless loop is cut off by the frame budget ------------------- */
    static const char *MISSING = "update(dt: float) {\n    y = missing.value\n}\n";
    vm = ds_vm_create();
    CHECK(ds_vm_add_source(vm, "main", MISSING, (size_t)-1));
    CHECK(!ds_vm_link(vm));
    CHECK_TEXT("неизвестное имя", ds_vm_error(vm));
    ds_vm_destroy(vm);

    static const char *INFINITE_BUDGET = "count = 0\n"
                                         "update(dt: float) {\n"
                                         "    while true do\n"
                                         "        count = count + 1\n"
                                         "    }\n"
                                         "}\n";
    vm = ds_vm_create();
    CHECK(ds_vm_add_source(vm, "main", INFINITE_BUDGET, (size_t)-1));
    CHECK(ds_vm_link(vm));
    CHECK(ds_vm_start(vm));
    ds_vm_set_budget(vm, 5000);
    CHECK(!ds_vm_call_number(vm, "update", 1.0 / 60.0));
    CHECK_TEXT("лимит инструкций", ds_vm_error(vm));
    ds_vm_destroy(vm);

    ds_runtime_shutdown();
    puts("PASS DimScript VM: scripts, multi-file games, lists, loops, render batch, errors");
    return 0;
}
