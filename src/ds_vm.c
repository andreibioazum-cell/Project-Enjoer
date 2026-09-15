/* DimScript VM lifecycle: project assembly, linking, error text and the small
 * inspection surface the host, the tests and the preview use. */
#include "ds_vm.h"
#include "ds_vm_internal.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define DS_ARENA_PAGE (64u * 1024u)

typedef struct DsArenaPage {
    struct DsArenaPage *next;
    size_t used;
    size_t capacity;
} DsArenaPage;

DsVM *ds_vm_create(void) {
    DsVM *vm = (DsVM *)calloc(1, sizeof(DsVM));
    if (!vm) return NULL;
    vm->heap_next_gc = 256u * 1024u;
    return vm;
}

void ds_vm_destroy(DsVM *vm) {
    if (!vm) return;
    DsGc *block = vm->heap;
    while (block) {
        DsGc *next = block->next;
        if (block->kind == DS_LIST) free(((DsList *)block)->values);
        free(block);
        block = next;
    }
    for (int index = 0; index < vm->script_count; ++index) free(vm->scripts[index].text);
    DsArenaPage *page = (DsArenaPage *)vm->arena_pages;
    while (page) {
        DsArenaPage *next = page->next;
        free(page);
        page = next;
    }
    free(vm);
}

int ds_vm_add_source(DsVM *vm, const char *name, const char *text, size_t length) {
    if (!vm || !name || !text) return 0;
    if (length == (size_t)-1) length = strlen(text);
    for (int index = 0; index < vm->script_count; ++index) {
        if (!strcmp(vm->scripts[index].name, name)) {
            ds_vm_set_error(vm, 0, 0, "файл '%s' добавлен дважды", name);
            return 0;
        }
    }
    if (vm->script_count >= DS_MAX_SCRIPTS) {
        ds_vm_set_error(vm, 0, 0, "слишком много .ds файлов в игре (максимум %d)", DS_MAX_SCRIPTS);
        return 0;
    }
    char *copy = (char *)malloc(length + 1);
    if (!copy) return 0;
    memcpy(copy, text, length);
    copy[length] = '\0';
    DsScript *script = &vm->scripts[vm->script_count++];
    memset(script, 0, sizeof(*script));
    script->name = ds_vm_arena_string(vm, name, strlen(name));
    script->text = copy;
    script->length = length;
    return 1;
}

int ds_vm_link(DsVM *vm) {
    if (!vm) return 0;
    vm->error[0] = '\0';
    vm->raised = 0;
    if (!vm->script_count) {
        ds_vm_set_error(vm, 0, 0, "в игре нет ни одного .ds файла");
        return 0;
    }
    for (int index = 0; index < vm->script_count; ++index) {
        if (vm->scripts[index].linked) continue;
        if (!ds_vm_parse_script(vm, &vm->scripts[index])) return 0;
        vm->scripts[index].linked = 1;
    }
    return ds_vm_link_program(vm);
}

const char *ds_vm_error(const DsVM *vm) { return vm ? vm->error : ""; }
int ds_vm_has_error(const DsVM *vm) { return vm && vm->error[0] ? 1 : 0; }

void ds_vm_clear_error(DsVM *vm) {
    if (!vm) return;
    vm->error[0] = '\0';
    vm->raised = 0;
}

void ds_vm_set_budget(DsVM *vm, long statements) {
    if (vm) vm->budget = statements;
}

int ds_vm_quit_requested(const DsVM *vm) { return vm ? vm->quit_requested : 0; }

int ds_vm_script_count(const DsVM *vm) { return vm ? vm->script_count : 0; }

const char *ds_vm_script_name(const DsVM *vm, int index) {
    if (!vm || index < 0 || index >= vm->script_count) return "";
    return vm->scripts[index].name;
}

int ds_vm_function_count(const DsVM *vm) { return vm ? vm->function_count : 0; }

const char *ds_vm_function_name(const DsVM *vm, int index) {
    if (!vm || index < 0 || index >= vm->function_count) return "";
    return vm->functions[index]->name;
}

int ds_vm_has_function(const DsVM *vm, const char *name) {
    return vm && ds_find_function((DsVM *)vm, name) ? 1 : 0;
}

int ds_vm_global_count(const DsVM *vm) { return vm ? vm->global_count : 0; }

const char *ds_vm_global_name(const DsVM *vm, int index) {
    if (!vm || index < 0 || index >= vm->global_count) return "";
    return vm->globals[index].name;
}

const char *ds_vm_global_type(const DsVM *vm, int index) {
    if (!vm || index < 0 || index >= vm->global_count) return "";
    return ds_kind_name(vm->globals[index].value.kind);
}

double ds_vm_global_number(const DsVM *vm, const char *name) {
    if (!vm || !name) return 0.0;
    const int index = ds_find_global((DsVM *)vm, name);
    if (index < 0) return 0.0;
    double number = 0.0;
    ds_to_number(&vm->globals[index].value, &number);
    return number;
}

const char *ds_vm_global_string(const DsVM *vm, const char *name, int *length) {
    static char scratch[256];
    if (length) *length = 0;
    if (!vm || !name) return "";
    const int index = ds_find_global((DsVM *)vm, name);
    if (index < 0) return "";
    ds_value_text((DsVM *)vm, &((DsVM *)vm)->globals[index].value, scratch, sizeof(scratch));
    if (length) *length = (int)strlen(scratch);
    return scratch;
}

static const DsValue *global_field(const DsVM *vm, const char *global, const char *field) {
    if (!vm || !global || !field) return NULL;
    const int index = ds_find_global((DsVM *)vm, global);
    if (index < 0) return NULL;
    const DsValue *owner = &vm->globals[index].value;
    if (owner->kind != DS_OBJECT || !owner->as.object) return NULL;
    const DsStruct *type = &vm->structs[owner->as.object->type];
    for (int slot = 0; slot < type->count; ++slot)
        if (!strcmp(type->fields[slot].name, field)) return &owner->as.object->fields[slot];
    return NULL;
}

double ds_vm_global_field_number(const DsVM *vm, const char *global, const char *field) {
    const DsValue *value = global_field(vm, global, field);
    double number = 0.0;
    if (value) ds_to_number(value, &number);
    return number;
}

const char *ds_vm_global_field_string(const DsVM *vm, const char *global, const char *field,
                                      int *length) {
    static char scratch[256];
    if (length) *length = 0;
    const DsValue *value = global_field(vm, global, field);
    if (!value) return "";
    ds_value_text((DsVM *)vm, value, scratch, sizeof(scratch));
    if (length) *length = (int)strlen(scratch);
    return scratch;
}

int ds_vm_live_objects(const DsVM *vm) { return vm ? vm->heap_objects : 0; }
size_t ds_vm_heap_bytes(const DsVM *vm) { return vm ? vm->heap_bytes : 0; }
