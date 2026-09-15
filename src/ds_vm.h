/* DimScript VM — the interpreter that runs .ds game files inside the engine.
 *
 * This is the primary execution mode for Enjoer games: a game is a folder of
 * `.ds` files plus a `game.manifest`, and the engine loads it at runtime, so a
 * game can be changed without rebuilding the APK.  The ahead-of-time C compiler
 * in `dimscript/` produces exactly the same behaviour for shipping; both share
 * src/dimscript_runtime.c and therefore the same font-less render API.
 *
 * Memory model: the AST lives in an arena owned by the VM, script values live on
 * a small mark & sweep heap that is collected between frames (never while a
 * script is running), and a runtime error unwinds the current callback through
 * setjmp so a broken script cannot take the process down.
 */
#ifndef DS_VM_H
#define DS_VM_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct DsVM DsVM;

DsVM *ds_vm_create(void);
void ds_vm_destroy(DsVM *vm);

/* Add one .ds file to the project.  `name` is the module name used by
 * `require "name"`, without the .ds extension.  The source is copied. */
int ds_vm_add_source(DsVM *vm, const char *name, const char *text, size_t length);

/* Parse every file, follow `require`, then link structs, globals, callbacks and
 * local slots.  Returns 0 and sets the error text on the first problem. */
int ds_vm_link(DsVM *vm);

const char *ds_vm_error(const DsVM *vm);
void ds_vm_clear_error(DsVM *vm);
int ds_vm_has_error(const DsVM *vm);

/* --- inspection (used by tools, tests and the preview) -------------------- */
int ds_vm_script_count(const DsVM *vm);
const char *ds_vm_script_name(const DsVM *vm, int index);
int ds_vm_function_count(const DsVM *vm);
const char *ds_vm_function_name(const DsVM *vm, int index);
int ds_vm_has_function(const DsVM *vm, const char *name);
int ds_vm_global_count(const DsVM *vm);
const char *ds_vm_global_name(const DsVM *vm, int index);
const char *ds_vm_global_type(const DsVM *vm, int index);
double ds_vm_global_number(const DsVM *vm, const char *name);
/* Returns a pointer to the global's text; `length` may be NULL. */
const char *ds_vm_global_string(const DsVM *vm, const char *name, int *length);
/* Numeric field of a struct-typed global, e.g. `game.score`.  Lets the tools
 * peek at game state without adding reflection to the language. */
double ds_vm_global_field_number(const DsVM *vm, const char *global, const char *field);
/* Length of a string field of a struct-typed global. */
const char *ds_vm_global_field_string(const DsVM *vm, const char *global, const char *field,
                                      int *length);

/* --- lifecycle ------------------------------------------------------------ */
/* Evaluates the global declarations (`game = new ClickerGame`) and then runs the
 * script's `load` callback. */
int ds_vm_start(DsVM *vm);
/* Runs `quit` and forgets all script state, so a reload starts from scratch. */
int ds_vm_stop(DsVM *vm);
int ds_vm_quit_requested(const DsVM *vm);
/* Guards a frame against a runaway loop.  0 means "no limit". */
void ds_vm_set_budget(DsVM *vm, long statements);

int ds_vm_call_void(DsVM *vm, const char *name);
int ds_vm_call_number(DsVM *vm, const char *name, double value);
int ds_vm_call_touch(DsVM *vm, const char *name, int id, double x, double y);
int ds_vm_call_string(DsVM *vm, const char *name, const char *text);
int ds_vm_call_two_numbers(DsVM *vm, const char *name, double first, double second);

/* Frame-boundary collection.  Safe to call only between callbacks. */
void ds_vm_collect(DsVM *vm);

/* Heap statistics, handy for the regression test and the preview HUD. */
int ds_vm_live_objects(const DsVM *vm);
size_t ds_vm_heap_bytes(const DsVM *vm);

#ifdef __cplusplus
}
#endif
#endif /* DS_VM_H */
