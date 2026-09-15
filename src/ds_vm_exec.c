/* The DimScript evaluator: statements, expressions, builtins and the public
 * interpreter API.
 *
 * Everything here is deliberately plain C99 with no allocation surprises: the
 * AST is read-only, values are on the C stack, and only `new`, list growth and
 * string concatenation touch the GC heap.  That is what lets a broken script
 * fail as an error message instead of taking the game down. */
#include "ds_vm_internal.h"

#include <ctype.h>
#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* --- lookups -------------------------------------------------------------- */

DsStruct *ds_find_struct(DsVM *vm, const char *name) {
    if (!vm || !name) return NULL;
    for (int index = 0; index < vm->struct_count; ++index)
        if (!strcmp(vm->structs[index].name, name)) return &vm->structs[index];
    return NULL;
}

DsFunction *ds_find_function(DsVM *vm, const char *name) {
    if (!vm || !name) return NULL;
    for (int index = 0; index < vm->function_count; ++index)
        if (!strcmp(vm->functions[index]->name, name)) return vm->functions[index];
    return NULL;
}

int ds_find_global(DsVM *vm, const char *name) {
    if (!vm || !name) return -1;
    for (int index = 0; index < vm->global_count; ++index)
        if (!strcmp(vm->globals[index].name, name)) return index;
    return -1;
}

static int field_index(DsVM *vm, DsObject *object, const char *name) {
    const DsStruct *type = &vm->structs[object->type];
    for (int index = 0; index < type->count; ++index)
        if (!strcmp(type->fields[index].name, name)) return index;
    return -1;
}

DsValue ds_vm_default_for(DsVM *vm, DsFieldType type, int struct_index) {
    (void)vm;
    (void)struct_index;
    switch (type) {
    case DS_FT_INT: return ds_int_value(0);
    case DS_FT_FLOAT: return ds_float_value(0.0);
    case DS_FT_BOOL: return ds_bool_value(0);
    case DS_FT_STRING: return ds_string_value(NULL);
    case DS_FT_LIST: return ds_nil_value();
    case DS_FT_STRUCT: return ds_nil_value();
    default: return ds_nil_value();
    }
}

/* --- builtins ------------------------------------------------------------- */

typedef struct {
    const char *namesp;
    const char *name;
    int arity; /* -1 accepts any count */
} DsBuiltin;

static const DsBuiltin BUILTINS[] = {
    {"render", "clear", 3},
    {"render", "color", DS_BUILTIN_VARIADIC},
    {"render", "rect", 4},
    {"render", "frame", 5},
    {"render", "circle", 3},
    {"render", "ring", 4},
    {"render", "line", 5},
    {"render", "tri", 6},
    {"render", "text", 4},
    {"math", "floor", 1},
    {"math", "ceil", 1},
    {"math", "round", 1},
    {"math", "abs", 1},
    {"math", "sign", 1},
    {"math", "sqrt", 1},
    {"math", "sin", 1},
    {"math", "cos", 1},
    {"math", "tan", 1},
    {"math", "min", 2},
    {"math", "max", 2},
    {"math", "mod", 2},
    {"math", "pow", 2},
    {"math", "lerp", 3},
    {"math", "random", DS_BUILTIN_VARIADIC},
    {"engine", "width", 0},
    {"engine", "height", 0},
    {"engine", "time", 0},
    {"engine", "delta", 0},
    {"engine", "fps", 0},
    {"engine", "frame", 0},
    {"engine", "quit", 0},
    {"input", "key", 1},
    {"input", "touch_x", 1},
    {"input", "touch_y", 1},
    {"input", "touch_down", 1},
    {"input", "touches", 0},
};

static const DsBuiltin *find_builtin(const char *namesp, const char *name) {
    for (int index = 0; index < (int)(sizeof(BUILTINS) / sizeof(BUILTINS[0])); ++index)
        if (!strcmp(BUILTINS[index].namesp, namesp) && !strcmp(BUILTINS[index].name, name))
            return &BUILTINS[index];
    return NULL;
}

int ds_builtin_member_arity(const char *namesp, const char *name) {
    const DsBuiltin *builtin = find_builtin(namesp, name);
    if (builtin) return builtin->arity;
    /* Namespace constants are properties, not calls. */
    if (!strcmp(namesp, "math") && (!strcmp(name, "pi") || !strcmp(name, "e") ||
                                    !strcmp(name, "tau") || !strcmp(name, "inf")))
        return DS_BUILTIN_VALUE;
    return DS_BUILTIN_UNKNOWN;
}

int ds_builtin_function_arity(const char *name) {
    if (!strcmp(name, "print") || !strcmp(name, "log")) return DS_BUILTIN_VARIADIC;
    if (!strcmp(name, "str") || !strcmp(name, "len") || !strcmp(name, "num")) return 1;
    return DS_BUILTIN_UNKNOWN;
}

/* --- value helpers -------------------------------------------------------- */

static double number_of(const DsValue *value) {
    double number = 0.0;
    ds_to_number(value, &number);
    return number;
}

static int is_number_like(const DsValue *value) {
    return value && (value->kind == DS_INT || value->kind == DS_FLOAT || value->kind == DS_BOOL);
}

static DsValue make_number(DsVM *vm, double value, int prefer_int) {
    (void)vm;
    if (prefer_int) {
        const double truncated = floor(value);
        if (truncated == value && value >= -9.2e18 && value <= 9.2e18) return ds_int_value((long long)value);
    }
    return ds_float_value(value);
}

static DsValue concat_values(DsVM *vm, const DsValue *left, const DsValue *right) {
    DsString *head = ds_copy_value_string(vm, left);
    DsString *tail = ds_copy_value_string(vm, right);
    const int total = head->length + tail->length;
    DsString *result = ds_new_string(vm, NULL, total);
    memcpy(result->data, head->data, (size_t)head->length);
    memcpy(result->data + head->length, tail->data, (size_t)tail->length);
    result->data[total] = '\0';
    return ds_string_value(result);
}

static int string_compare(const DsValue *left, const DsValue *right, int *result) {
    char left_buffer[256], right_buffer[256];
    ds_value_text(NULL, left, left_buffer, sizeof(left_buffer));
    ds_value_text(NULL, right, right_buffer, sizeof(right_buffer));
    *result = strcmp(left_buffer, right_buffer);
    return 1;
}

/* Coercion keeps the two execution modes honest: a `score: int` field stores an
 * integer, exactly like the C that the ahead-of-time compiler emits. */
void ds_vm_coerce(DsVM *vm, DsValue *value, DsFieldType type, int struct_index, int line,
                  int column) {
    if (type == DS_FT_ANY) return;
    switch (type) {
    case DS_FT_INT:
        if (is_number_like(value)) {
            *value = ds_int_value((long long)floor(number_of(value)));
        } else if (value->kind == DS_NIL) {
            *value = ds_int_value(0);
        } else {
            ds_vm_set_error(vm, line, column, "поле int ожидает число, получено %s",
                            ds_kind_name(value->kind));
        }
        break;
    case DS_FT_FLOAT:
        if (is_number_like(value)) *value = ds_float_value(number_of(value));
        else if (value->kind == DS_NIL) *value = ds_float_value(0.0);
        else ds_vm_set_error(vm, line, column, "поле float ожидает число, получено %s",
                             ds_kind_name(value->kind));
        break;
    case DS_FT_BOOL:
        *value = ds_bool_value(ds_truthy(value));
        break;
    case DS_FT_STRING:
        if (value->kind != DS_STRING) *value = ds_string_value(ds_copy_value_string(vm, value));
        break;
    case DS_FT_LIST:
        if (value->kind != DS_LIST && value->kind != DS_NIL)
            ds_vm_set_error(vm, line, column, "поле list ожидает список, получено %s",
                            ds_kind_name(value->kind));
        break;
    case DS_FT_STRUCT:
        if (value->kind == DS_NIL) break;
        if (value->kind != DS_OBJECT)
            ds_vm_set_error(vm, line, column, "ожидался объект struct, получено %s",
                            ds_kind_name(value->kind));
        else if (struct_index >= 0 && value->as.object->type != struct_index)
            ds_vm_set_error(vm, line, column, "ожидался объект '%s', получено '%s'",
                            vm->structs[struct_index].name, vm->structs[value->as.object->type].name);
        break;
    default:
        break;
    }
}

/* --- statements ----------------------------------------------------------- */

static int exec_statement(DsVM *vm, DsNode *node, DsValue *locals);
static DsValue eval_node(DsVM *vm, DsNode *node, DsValue *locals);
static int exec_block(DsVM *vm, DsNode **statements, int count, DsValue *locals) {
    for (int index = 0; index < count; ++index) {
        if (vm->raised || vm->control) return !vm->raised;
        if (vm->budget > 0 && --vm->budget == 0) {
            ds_vm_set_error(vm, statements[index]->line, statements[index]->column,
                            "script исчерпал лимит инструкций за кадр (вечный цикл?)");
            return 0;
        }
        if (!exec_statement(vm, statements[index], locals)) return 0;
    }
    return 1;
}

static int normalize_index(DsVM *vm, DsList *list, double raw, int line, int column, int *out) {
    /* Negative indexes count from the end, like in Python: -1 is the last. */
    if (raw < 0.0) raw = (double)list->count + raw;
    const int index = (int)floor(raw);
    if (index < 0 || index >= list->count) {
        ds_vm_set_error(vm, line, column, "индекс %.0f вне списка (длина %d)", raw, list->count);
        return 0;
    }
    *out = index;
    return 1;
}

static int call_list_method(DsVM *vm, DsNode *callee, DsNode *node, DsValue *locals,
                            DsValue *result) {
    DsValue owner = eval_node(vm, callee->n0, locals);
    if (vm->raised) return 0;
    if (owner.kind != DS_LIST || !owner.as.list) {
        ds_vm_set_error(vm, node->line, node->column, "метод '%s' есть только у list", callee->name);
        return 0;
    }
    DsList *list = owner.as.list;
    if (!strcmp(callee->name, "push")) {
        if (node->child_count != 1) {
            ds_vm_set_error(vm, node->line, node->column, "push ожидает 1 аргумент");
            return 0;
        }
        DsValue value = eval_node(vm, node->children[0], locals);
        if (vm->raised) return 0;
        ds_list_reserve(vm, list, list->count + 1);
        list->values[list->count++] = value;
        *result = ds_int_value(list->count);
        return 1;
    }
    if (!strcmp(callee->name, "insert")) {
        if (node->child_count != 2) {
            ds_vm_set_error(vm, node->line, node->column, "insert ожидает 2 аргумента");
            return 0;
        }
        DsValue position = eval_node(vm, node->children[0], locals);
        DsValue value = eval_node(vm, node->children[1], locals);
        if (vm->raised) return 0;
        int index = (int)floor(number_of(&position));
        if (index < 0) index = 0;
        if (index > list->count) index = list->count;
        ds_list_reserve(vm, list, list->count + 1);
        for (int cursor = list->count; cursor > index; --cursor) list->values[cursor] = list->values[cursor - 1];
        list->values[index] = value;
        ++list->count;
        *result = ds_int_value(list->count);
        return 1;
    }
    if (!strcmp(callee->name, "remove") || !strcmp(callee->name, "remove_at")) {
        if (node->child_count != 1) {
            ds_vm_set_error(vm, node->line, node->column, "%s ожидает 1 аргумент", callee->name);
            return 0;
        }
        DsValue raw = eval_node(vm, node->children[0], locals);
        int index = 0;
        if (vm->raised) return 0;
        if (!normalize_index(vm, list, number_of(&raw), node->line, node->column, &index)) return 0;
        DsValue removed = list->values[index];
        for (int cursor = index; cursor + 1 < list->count; ++cursor)
            list->values[cursor] = list->values[cursor + 1];
        list->values[list->count - 1] = ds_nil_value();
        --list->count;
        *result = removed;
        return 1;
    }
    if (!strcmp(callee->name, "clear")) {
        for (int cursor = 0; cursor < list->count; ++cursor) list->values[cursor] = ds_nil_value();
        list->count = 0;
        *result = ds_nil_value();
        return 1;
    }
    if (!strcmp(callee->name, "new")) {
        *result = ds_string_value(ds_literal_string(vm, "list.new() не нужен: пишите []", -1));
        return 1;
    }
    ds_vm_set_error(vm, node->line, node->column,
                    "у list есть push, insert, remove, clear и count, нет '%s'", callee->name);
    return 0;
}

/* Builtins receive already evaluated arguments: an expression must never run
 * twice just because a builtin wanted the number behind it. */
static DsValue call_builtin(DsVM *vm, const char *namesp, const char *name,
                            const DsValue *values, int count) {
    double numbers[8];
    for (int index = 0; index < count && index < 8; ++index) numbers[index] = number_of(&values[index]);

    if (!strcmp(namesp, "render")) {
        if (!strcmp(name, "clear")) ds_render_clear((float)numbers[0], (float)numbers[1], (float)numbers[2]);
        else if (!strcmp(name, "color")) {
            if (count >= 4)
                ds_render_color_alpha((float)numbers[0], (float)numbers[1], (float)numbers[2],
                                      (float)numbers[3]);
            else
                ds_render_color((float)numbers[0], (float)numbers[1], (float)numbers[2]);
        } else if (!strcmp(name, "rect"))
            ds_render_rect((float)numbers[0], (float)numbers[1], (float)numbers[2], (float)numbers[3]);
        else if (!strcmp(name, "frame"))
            ds_render_frame((float)numbers[0], (float)numbers[1], (float)numbers[2], (float)numbers[3],
                            (float)numbers[4]);
        else if (!strcmp(name, "circle"))
            ds_render_circle((float)numbers[0], (float)numbers[1], (float)numbers[2]);
        else if (!strcmp(name, "ring"))
            ds_render_ring((float)numbers[0], (float)numbers[1], (float)numbers[2], (float)numbers[3]);
        else if (!strcmp(name, "line"))
            ds_render_line((float)numbers[0], (float)numbers[1], (float)numbers[2], (float)numbers[3],
                           (float)numbers[4]);
        else if (!strcmp(name, "tri"))
            ds_render_triangle((float)numbers[0], (float)numbers[1], (float)numbers[2], (float)numbers[3],
                               (float)numbers[4], (float)numbers[5]);
        else if (!strcmp(name, "text")) {
            char buffer[ENJOER_DRAW_TEXT_LENGTH];
            ds_value_text(vm, &values[0], buffer, sizeof(buffer));
            ds_render_text(buffer, (float)numbers[1], (float)numbers[2], (float)numbers[3]);
        }
        return ds_nil_value();
    }

    if (!strcmp(namesp, "math")) {
        if (!strcmp(name, "floor")) return make_number(vm, floor(numbers[0]), 1);
        if (!strcmp(name, "ceil")) return make_number(vm, ceil(numbers[0]), 1);
        if (!strcmp(name, "round")) return make_number(vm, floor(numbers[0] + 0.5), 1);
        if (!strcmp(name, "abs")) return make_number(vm, fabs(numbers[0]), values[0].kind == DS_INT);
        if (!strcmp(name, "sign")) return ds_int_value(numbers[0] > 0.0 ? 1 : numbers[0] < 0.0 ? -1 : 0);
        if (!strcmp(name, "sqrt")) {
            const double value = numbers[0] < 0.0 ? 0.0 : sqrt(numbers[0]);
            return make_number(vm, value, value == floor(value));
        }
        if (!strcmp(name, "sin")) return ds_float_value(sin(numbers[0]));
        if (!strcmp(name, "cos")) return ds_float_value(cos(numbers[0]));
        if (!strcmp(name, "tan")) return ds_float_value(tan(numbers[0]));
        if (!strcmp(name, "min")) return numbers[0] <= numbers[1] ? values[0] : values[1];
        if (!strcmp(name, "max")) return numbers[0] >= numbers[1] ? values[0] : values[1];
        if (!strcmp(name, "mod")) {
            if (numbers[1] == 0.0) {
                ds_vm_set_error(vm, 0, 0, "math.mod: деление на ноль");
                return ds_nil_value();
            }
            return make_number(vm, numbers[0] - floor(numbers[0] / numbers[1]) * numbers[1], 1);
        }
        if (!strcmp(name, "pow")) return ds_float_value(pow(numbers[0], numbers[1]));
        if (!strcmp(name, "lerp")) return ds_float_value(numbers[0] + (numbers[1] - numbers[0]) * numbers[2]);
        if (!strcmp(name, "random")) {
            const double value = ds_engine_random();
            if (count == 0) return ds_float_value(value);
            if (count == 1) return make_number(vm, floor(value * (numbers[0] + 1.0)), 1);
            return make_number(vm, floor(numbers[0] + value * (numbers[1] - numbers[0] + 1.0)), 1);
        }
        return ds_nil_value();
    }

    if (!strcmp(namesp, "engine")) {
        const DsEngineState *state = ds_engine_state();
        if (!strcmp(name, "width")) return ds_float_value((double)state->width);
        if (!strcmp(name, "height")) return ds_float_value((double)state->height);
        if (!strcmp(name, "time")) return ds_float_value(state->time);
        if (!strcmp(name, "delta")) return ds_float_value(state->delta_time);
        if (!strcmp(name, "fps")) return ds_float_value(state->fps);
        if (!strcmp(name, "frame")) return ds_int_value((long long)state->frame);
        if (!strcmp(name, "quit")) {
            vm->quit_requested = 1;
            return ds_nil_value();
        }
        return ds_nil_value();
    }

    if (!strcmp(namesp, "input")) {
        const DsEngineState *state = ds_engine_state();
        if (!strcmp(name, "key")) {
            int down = 0;
            char wanted[32];
            if (count >= 1 && values[0].kind == DS_STRING && values[0].as.string) {
                snprintf(wanted, sizeof(wanted), "%.*s", values[0].as.string->length,
                         values[0].as.string->data);
                for (int index = 0; index < state->key_count; ++index)
                    if (!strncmp(state->key_names[index], wanted, sizeof(wanted))) down = state->key_down[index];
            }
            return ds_bool_value(down);
        }
        const int pointer = (int)numbers[0];
        const DsTouchState *touch = NULL;
        for (int index = 0; index < DS_MAX_TOUCHES; ++index)
            if (state->touches[index].used && state->touches[index].id == pointer)
                touch = &state->touches[index];
        if (!strcmp(name, "touch_x")) return ds_float_value(touch ? (double)touch->x : 0.0);
        if (!strcmp(name, "touch_y")) return ds_float_value(touch ? (double)touch->y : 0.0);
        if (!strcmp(name, "touch_down")) return ds_bool_value(touch ? touch->down : 0);
        if (!strcmp(name, "touches")) return ds_int_value(state->touch_count);
        return ds_nil_value();
    }

    ds_vm_set_error(vm, 0, 0, "нет такой функции %s.%s", namesp, name);
    return ds_nil_value();
}

static DsValue eval_node(DsVM *vm, DsNode *node, DsValue *locals) {
    if (!node) return ds_nil_value();
    switch (node->kind) {
    case N_INT:
        return ds_int_value(node->integer);
    case N_FLOAT:
        return ds_float_value(node->number);
    case N_STRING:
        if (!node->cache) node->cache = ds_literal_string(vm, node->text, node->text_length);
        return ds_string_value((DsString *)node->cache);
    case N_BOOL:
        return ds_bool_value(node->boolean);
    case N_NIL:
        return ds_nil_value();
    case N_NAME: {
        if (node->name_kind == DS_NAME_LOCAL && locals) return locals[node->slot];
        if (node->name_kind == DS_NAME_LOCAL) return ds_nil_value();
        if (node->name_kind == DS_NAME_GLOBAL) return vm->globals[node->slot].value;
        if (node->name_kind == DS_NAME_NAMESPACE)
            return ds_string_value(ds_literal_string(vm, node->name, -1));
        ds_vm_set_error(vm, node->line, node->column, "неизвестное имя '%s'", node->name);
        return ds_nil_value();
    }
    case N_MEMBER: {
        if (node->n0->kind == N_NAME && node->n0->name_kind == DS_NAME_NAMESPACE) {
            const char *namesp = node->n0->name;
            const int arity = ds_builtin_member_arity(namesp, node->name);
            if (arity == DS_BUILTIN_VALUE) {
                if (!strcmp(node->name, "pi")) return ds_float_value(3.1415926535897931);
                if (!strcmp(node->name, "tau")) return ds_float_value(6.2831853071795862);
                if (!strcmp(node->name, "e")) return ds_float_value(2.7182818284590451);
                return ds_float_value(1.0 / 0.0);
            }
            if (arity == 0) return call_builtin(vm, namesp, node->name, NULL, 0);
            ds_vm_set_error(vm, node->line, node->column, "%s.%s нужно вызывать: %s.%s(...)",
                            namesp, node->name, namesp, node->name);
            return ds_nil_value();
        }
        DsValue owner = eval_node(vm, node->n0, locals);
        if (vm->raised) return ds_nil_value();
        if (owner.kind == DS_OBJECT && owner.as.object) {
            const int index = field_index(vm, owner.as.object, node->name);
            if (index < 0) {
                ds_vm_set_error(vm, node->line, node->column, "у '%s' нет поля '%s'",
                                vm->structs[owner.as.object->type].name, node->name);
                return ds_nil_value();
            }
            return owner.as.object->fields[index];
        }
        if (owner.kind == DS_LIST && owner.as.list) {
            if (!strcmp(node->name, "count")) return ds_int_value(owner.as.list->count);
            if (!strcmp(node->name, "length")) return ds_int_value(owner.as.list->count);
            ds_vm_set_error(vm, node->line, node->column,
                            "у list есть только свойство count; методы вызываются как list.%s(...)",
                            node->name);
            return ds_nil_value();
        }
        if (owner.kind == DS_STRING && owner.as.string) {
            if (!strcmp(node->name, "length") || !strcmp(node->name, "count"))
                return ds_int_value(owner.as.string->length);
        }
        if (owner.kind == DS_NIL) {
            ds_vm_set_error(vm, node->line, node->column,
                            "у nil нет поля '%s' (нужно new или [] прежде чем читать поля)", node->name);
            return ds_nil_value();
        }
        ds_vm_set_error(vm, node->line, node->column, "тип %s не имеет поля '%s'",
                        ds_kind_name(owner.kind), node->name);
        return ds_nil_value();
    }
    case N_INDEX: {
        DsValue owner = eval_node(vm, node->n0, locals);
        if (vm->raised) return ds_nil_value();
        if (owner.kind != DS_LIST || !owner.as.list) {
            ds_vm_set_error(vm, node->line, node->column, "индекс [] применим только к list");
            return ds_nil_value();
        }
        DsValue raw = eval_node(vm, node->n1, locals);
        int index = 0;
        if (vm->raised) return ds_nil_value();
        if (!normalize_index(vm, owner.as.list, number_of(&raw), node->line, node->column, &index))
            return ds_nil_value();
        return owner.as.list->values[index];
    }
    case N_LISTLIT: {
        DsList *list = ds_new_list(vm);
        for (int index = 0; index < node->child_count; ++index) {
            DsValue value = eval_node(vm, node->children[index], locals);
            if (vm->raised) return ds_nil_value();
            ds_list_reserve(vm, list, list->count + 1);
            list->values[list->count++] = value;
        }
        DsValue result = ds_nil_value();
        result.kind = DS_LIST;
        result.as.list = list;
        return result;
    }
    case N_NEW: {
        if (node->struct_index < 0) {
            DsStruct *type = ds_find_struct(vm, node->name);
            if (!type) {
                ds_vm_set_error(vm, node->line, node->column, "неизвестная структура '%s'", node->name);
                return ds_nil_value();
            }
            node->struct_index = (int)(type - vm->structs);
        }
        DsValue result = ds_nil_value();
        result.kind = DS_OBJECT;
        result.as.object = ds_new_object(vm, node->struct_index);
        return result;
    }
    case N_UNARY: {
        DsValue value = eval_node(vm, node->n0, locals);
        if (vm->raised) return ds_nil_value();
        if (node->op == DS_OP_NOT) return ds_bool_value(!ds_truthy(&value));
        if (!is_number_like(&value)) {
            ds_vm_set_error(vm, node->line, node->column, "унарный '-' работает только с числами");
            return ds_nil_value();
        }
        if (node->op == DS_OP_ADD) return value;
        if (value.kind == DS_INT) return ds_int_value(-value.as.integer);
        return ds_float_value(-number_of(&value));
    }
    case N_BINARY: {
        if (node->op == DS_OP_AND || node->op == DS_OP_OR) {
            DsValue left = eval_node(vm, node->n0, locals);
            if (vm->raised) return ds_nil_value();
            const int truth = ds_truthy(&left);
            if ((node->op == DS_OP_AND && !truth) || (node->op == DS_OP_OR && truth))
                return ds_bool_value(truth);
            DsValue right = eval_node(vm, node->n1, locals);
            if (vm->raised) return ds_nil_value();
            return ds_bool_value(ds_truthy(&right));
        }
        DsValue left = eval_node(vm, node->n0, locals);
        if (vm->raised) return ds_nil_value();
        DsValue right = eval_node(vm, node->n1, locals);
        if (vm->raised) return ds_nil_value();

        if (node->op == DS_OP_CONCAT) return concat_values(vm, &left, &right);

        if (node->op == DS_OP_EQ || node->op == DS_OP_NE) {
            const int equal = ds_values_equal(&left, &right);
            return ds_bool_value(node->op == DS_OP_EQ ? equal : !equal);
        }

        if (left.kind == DS_STRING || right.kind == DS_STRING) {
            int order = 0;
            if (node->op != DS_OP_ADD && node->op != DS_OP_SUB && node->op != DS_OP_MUL &&
                node->op != DS_OP_DIV && node->op != DS_OP_MOD) {
                string_compare(&left, &right, &order);
                switch (node->op) {
                case DS_OP_LT: return ds_bool_value(order < 0);
                case DS_OP_LE: return ds_bool_value(order <= 0);
                case DS_OP_GT: return ds_bool_value(order > 0);
                case DS_OP_GE: return ds_bool_value(order >= 0);
                default: break;
                }
            }
            ds_vm_set_error(vm, node->line, node->column,
                            "строку можно только склеить оператором '..'");
            return ds_nil_value();
        }

        if (!is_number_like(&left) || !is_number_like(&right)) {
            ds_vm_set_error(vm, node->line, node->column,
                            "арифметика ждёт числа, получено %s и %s", ds_kind_name(left.kind),
                            ds_kind_name(right.kind));
            return ds_nil_value();
        }

        const int both_int = left.kind == DS_INT && right.kind == DS_INT;
        switch (node->op) {
        case DS_OP_ADD:
            return both_int ? ds_int_value(left.as.integer + right.as.integer)
                            : ds_float_value(number_of(&left) + number_of(&right));
        case DS_OP_SUB:
            return both_int ? ds_int_value(left.as.integer - right.as.integer)
                            : ds_float_value(number_of(&left) - number_of(&right));
        case DS_OP_MUL:
            return both_int ? ds_int_value(left.as.integer * right.as.integer)
                            : ds_float_value(number_of(&left) * number_of(&right));
        case DS_OP_DIV: {
            const double divisor = number_of(&right);
            if (divisor == 0.0) {
                ds_vm_set_error(vm, node->line, node->column, "деление на ноль");
                return ds_nil_value();
            }
            return ds_float_value(number_of(&left) / divisor);
        }
        case DS_OP_MOD: {
            const double divisor = number_of(&right);
            if (divisor == 0.0) {
                ds_vm_set_error(vm, node->line, node->column, "остаток от нуля");
                return ds_nil_value();
            }
            if (both_int) {
                long long value = left.as.integer % right.as.integer;
                if (value != 0 && ((value < 0) != (right.as.integer < 0))) value += right.as.integer;
                return ds_int_value(value);
            }
            const double a = number_of(&left), b = number_of(&right);
            return ds_float_value(a - floor(a / b) * b);
        }
        case DS_OP_LT:
            return both_int ? ds_bool_value(left.as.integer < right.as.integer)
                            : ds_bool_value(number_of(&left) < number_of(&right));
        case DS_OP_LE:
            return both_int ? ds_bool_value(left.as.integer <= right.as.integer)
                            : ds_bool_value(number_of(&left) <= number_of(&right));
        case DS_OP_GT:
            return both_int ? ds_bool_value(left.as.integer > right.as.integer)
                            : ds_bool_value(number_of(&left) > number_of(&right));
        case DS_OP_GE:
            return both_int ? ds_bool_value(left.as.integer >= right.as.integer)
                            : ds_bool_value(number_of(&left) >= number_of(&right));
        default:
            ds_vm_set_error(vm, node->line, node->column, "неизвестный оператор");
            return ds_nil_value();
        }
    }
    case N_CALL: {
        DsNode *callee = node->n0;
        if (callee->kind == N_NAME) {
            DsFunction *function = ds_find_function(vm, callee->name);
            if (function) {
                DsValue arguments[DS_MAX_LOCALS];
                for (int index = 0; index < node->child_count; ++index) {
                    arguments[index] = eval_node(vm, node->children[index], locals);
                    if (vm->raised) return ds_nil_value();
                }
                return ds_vm_execute(vm, function, arguments, node->child_count);
            }
            if (!strcmp(callee->name, "print") || !strcmp(callee->name, "log")) {
                char line[512];
                size_t used = 0;
                line[0] = '\0';
                for (int index = 0; index < node->child_count; ++index) {
                    DsValue value = eval_node(vm, node->children[index], locals);
                    if (vm->raised) return ds_nil_value();
                    char buffer[256];
                    const char *text = ds_value_text(vm, &value, buffer, sizeof(buffer));
                    if (index && used + 1 < sizeof(line)) line[used++] = ' ';
                    snprintf(line + used, sizeof(line) - used, "%s", text);
                    used += strlen(line + used);
                }
                ds_log(line);
                return ds_nil_value();
            }
            if (!strcmp(callee->name, "str")) {
                if (node->child_count < 1) {
                    ds_vm_set_error(vm, node->line, node->column, "str ожидает 1 аргумент");
                    return ds_nil_value();
                }
                DsValue value = eval_node(vm, node->children[0], locals);
                if (vm->raised) return ds_nil_value();
                return ds_string_value(ds_copy_value_string(vm, &value));
            }
            if (!strcmp(callee->name, "len")) {
                if (node->child_count != 1) {
                    ds_vm_set_error(vm, node->line, node->column, "len ожидает 1 аргумент");
                    return ds_nil_value();
                }
                DsValue value = eval_node(vm, node->children[0], locals);
                if (vm->raised) return ds_nil_value();
                if (value.kind == DS_LIST && value.as.list) return ds_int_value(value.as.list->count);
                if (value.kind == DS_STRING && value.as.string) return ds_int_value(value.as.string->length);
                if (value.kind == DS_NIL) return ds_int_value(0);
                ds_vm_set_error(vm, node->line, node->column, "len работает со list и string");
                return ds_nil_value();
            }
            if (!strcmp(callee->name, "num")) {
                if (node->child_count != 1) {
                    ds_vm_set_error(vm, node->line, node->column, "num ожидает 1 аргумент");
                    return ds_nil_value();
                }
                DsValue value = eval_node(vm, node->children[0], locals);
                if (vm->raised) return ds_nil_value();
                double number = 0.0;
                if (ds_to_number(&value, &number)) return is_number_like(&value) && value.kind == DS_INT
                                                            ? value
                                                            : ds_float_value(number);
                if (value.kind == DS_STRING && value.as.string) {
                    char *end = NULL;
                    const double parsed = strtod(value.as.string->data, &end);
                    if (end == value.as.string->data) return ds_nil_value();
                    const char *text = value.as.string->data;
                    int is_integer = 1;
                    for (const char *cursor = text; cursor < end; ++cursor)
                        if (*cursor == '.' || *cursor == 'e' || *cursor == 'E') is_integer = 0;
                    return is_integer ? ds_int_value((long long)parsed) : ds_float_value(parsed);
                }
                return ds_nil_value();
            }
            ds_vm_set_error(vm, node->line, node->column, "неизвестная функция '%s'", callee->name);
            return ds_nil_value();
        }
        if (callee->kind == N_MEMBER) {
            if (callee->n0->kind == N_NAME && callee->n0->name_kind == DS_NAME_NAMESPACE) {
                const DsBuiltin *builtin = find_builtin(callee->n0->name, callee->name);
                if (!builtin) {
                    ds_vm_set_error(vm, node->line, node->column, "нет такой функции %s.%s",
                                    callee->n0->name, callee->name);
                    return ds_nil_value();
                }
                if (builtin->arity >= 0 && builtin->arity != node->child_count) {
                    ds_vm_set_error(vm, node->line, node->column, "%s.%s ожидает %d аргументов, получено %d",
                                    builtin->namesp, builtin->name, builtin->arity, node->child_count);
                    return ds_nil_value();
                }
                DsValue arguments[8];
                const int supplied = node->child_count > 8 ? 8 : node->child_count;
                for (int index = 0; index < supplied; ++index) {
                    arguments[index] = eval_node(vm, node->children[index], locals);
                    if (vm->raised) return ds_nil_value();
                }
                return call_builtin(vm, builtin->namesp, builtin->name, arguments, supplied);
            }
            DsValue result = ds_nil_value();
            if (call_list_method(vm, callee, node, locals, &result)) return result;
            return ds_nil_value();
        }
        ds_vm_set_error(vm, node->line, node->column, "вызвать можно только функцию или метод list");
        return ds_nil_value();
    }
    default:
        ds_vm_set_error(vm, node->line, node->column, "это выражение нельзя вычислить");
        return ds_nil_value();
    }
}

static int exec_statement(DsVM *vm, DsNode *node, DsValue *locals) {
    switch (node->kind) {
    case S_ASSIGN: {
        DsValue value = eval_node(vm, node->n1, locals);
        if (vm->raised) return 0;
        DsNode *target = node->n0;
        if (target->kind == N_NAME) {
            if (target->name_kind == DS_NAME_LOCAL) {
                locals[target->slot] = value;
            } else {
                DsGlobal *global = &vm->globals[target->slot];
                ds_vm_coerce(vm, &value, global->type, global->struct_index, node->line, node->column);
                if (vm->raised) return 0;
                global->value = value;
            }
            return 1;
        }
        if (target->kind == N_MEMBER) {
            DsValue owner = eval_node(vm, target->n0, locals);
            if (vm->raised) return 0;
            if (owner.kind != DS_OBJECT || !owner.as.object) {
                ds_vm_set_error(vm, node->line, node->column,
                                "нельзя записать поле '%s' в %s (нужен объект new ...)", target->name,
                                ds_kind_name(owner.kind));
                return 0;
            }
            const int index = field_index(vm, owner.as.object, target->name);
            if (index < 0) {
                ds_vm_set_error(vm, node->line, node->column, "у '%s' нет поля '%s'",
                                vm->structs[owner.as.object->type].name, target->name);
                return 0;
            }
            const DsField *field = &vm->structs[owner.as.object->type].fields[index];
            ds_vm_coerce(vm, &value, field->type, field->struct_index, node->line, node->column);
            if (vm->raised) return 0;
            owner.as.object->fields[index] = value;
            return 1;
        }
        if (target->kind == N_INDEX) {
            DsValue owner = eval_node(vm, target->n0, locals);
            if (vm->raised) return 0;
            if (owner.kind != DS_LIST || !owner.as.list) {
                ds_vm_set_error(vm, node->line, node->column, "запись по индексу работает только с list");
                return 0;
            }
            DsValue raw = eval_node(vm, target->n1, locals);
            int index = 0;
            if (vm->raised) return 0;
            if (!normalize_index(vm, owner.as.list, number_of(&raw), node->line, node->column, &index))
                return 0;
            owner.as.list->values[index] = value;
            return 1;
        }
        ds_vm_set_error(vm, node->line, node->column, "некорректная цель присваивания");
        return 0;
    }
    case S_EXPR:
        eval_node(vm, node->n0, locals);
        return !vm->raised;
    case S_DELETE:
        if (node->n0->kind == N_NAME && node->n0->name_kind == DS_NAME_GLOBAL) {
            vm->globals[node->n0->slot].value = ds_nil_value();
            return 1;
        }
        if (node->n0->kind == N_NAME && node->n0->name_kind == DS_NAME_LOCAL) {
            locals[node->n0->slot] = ds_nil_value();
            return 1;
        }
        if (node->n0->kind == N_MEMBER) {
            DsValue owner = eval_node(vm, node->n0->n0, locals);
            if (vm->raised) return 0;
            if (owner.kind == DS_OBJECT && owner.as.object) {
                const int index = field_index(vm, owner.as.object, node->n0->name);
                if (index >= 0) owner.as.object->fields[index] = ds_nil_value();
                return 1;
            }
        }
        ds_vm_set_error(vm, node->line, node->column, "delete ожидает имя объекта или поле");
        return 0;
    case S_IF: {
        DsValue condition = eval_node(vm, node->n0, locals);
        if (vm->raised) return 0;
        if (ds_truthy(&condition)) return exec_block(vm, node->children, node->child_count, locals);
        return exec_block(vm, node->tail, node->tail_count, locals);
    }
    case S_WHILE: {
        for (;;) {
            if (vm->raised) return 0;
            DsValue condition = eval_node(vm, node->n0, locals);
            if (vm->raised) return 0;
            if (!ds_truthy(&condition)) return 1;
            if (!exec_block(vm, node->children, node->child_count, locals)) return 0;
            if (vm->control == 1) {
                vm->control = 0;
                return 1;
            }
            if (vm->control == 3) return 1;
            vm->control = 0;
        }
    }
    case S_FOR: {
        DsValue start = eval_node(vm, node->n0, locals);
        if (vm->raised) return 0;
        DsValue stop = eval_node(vm, node->n1, locals);
        if (vm->raised) return 0;
        double step = 1.0;
        if (node->n2) {
            DsValue value = eval_node(vm, node->n2, locals);
            if (vm->raised) return 0;
            step = number_of(&value);
            if (step == 0.0) {
                ds_vm_set_error(vm, node->line, node->column, "шаг for не может быть нулём");
                return 0;
            }
        }
        int integer_loop = start.kind == DS_INT && stop.kind == DS_INT && step == floor(step);
        long long integer = integer_loop ? start.as.integer : 0;
        double number = integer_loop ? (double)integer : number_of(&start);
        const double limit = number_of(&stop);
        for (;;) {
            if (vm->raised) return 0;
            const int in_range = step > 0.0
                                     ? (integer_loop ? integer <= (long long)limit : number <= limit)
                                     : (integer_loop ? integer >= (long long)limit : number >= limit);
            if (!in_range) return 1;
            locals[node->slot] = integer_loop ? ds_int_value(integer) : ds_float_value(number);
            if (!exec_block(vm, node->children, node->child_count, locals)) return 0;
            if (vm->control == 1) {
                vm->control = 0;
                return 1;
            }
            if (vm->control == 3) return 1;
            vm->control = 0;
            if (integer_loop) integer += (long long)step;
            else number += step;
        }
    }
    case S_BREAK:
        vm->control = 1;
        return 1;
    case S_CONTINUE:
        vm->control = 2;
        return 1;
    case S_RETURN:
        vm->returned = node->n0 ? eval_node(vm, node->n0, locals) : ds_nil_value();
        vm->control = 3;
        return !vm->raised;
    default:
        ds_vm_set_error(vm, node->line, node->column, "неизвестная конструкция");
        return 0;
    }
}

DsValue ds_vm_execute(DsVM *vm, DsFunction *function, const DsValue *arguments, int count) {
    DsValue locals[DS_MAX_LOCALS];
    if (!function) return ds_nil_value();
    if (vm->depth >= DS_MAX_DEPTH) {
        ds_vm_set_error(vm, function->line, 1, "слишком глубокая рекурсия (максимум %d кадров)",
                        DS_MAX_DEPTH);
        return ds_nil_value();
    }
    for (int index = 0; index < function->local_count; ++index)
        locals[index] = index < function->arity
                            ? ds_vm_default_for(vm, function->param_kinds[index], -1)
                            : ds_nil_value();
    for (int index = 0; index < count && index < function->local_count; ++index) {
        locals[index] = arguments[index];
        ds_vm_coerce(vm, &locals[index], function->param_kinds[index], -1, function->line, 1);
        if (vm->raised) return ds_nil_value();
    }
    ++vm->depth;
    const int ok = exec_block(vm, function->body, function->body_count, locals);
    --vm->depth;
    DsValue result = vm->returned;
    if (vm->control == 1 || vm->control == 2)
        ds_vm_set_error(vm, function->line, 1, "%s вне цикла", vm->control == 1 ? "break" : "continue");
    vm->control = 0;
    vm->returned = ds_nil_value();
    if (!ok) return ds_nil_value();
    return result;
}

/* --- public interpreter API ----------------------------------------------- */

/* A missing callback is not an error (a game may implement only some of them);
 * a raised error is sticky, so a broken script stops being called until the host
 * reloads it. */
int ds_vm_call_named(DsVM *vm, const char *name, const DsValue *arguments, int count) {
    if (!vm || vm->raised) return 0;
    DsFunction *function = ds_find_function(vm, name);
    if (!function) return 1;
    ds_vm_execute(vm, function, arguments, count);
    return vm->raised ? 0 : 1;
}

static int invoke(DsVM *vm, const char *name, const DsValue *arguments, int count) {
    if (!vm || vm->raised) return 0;
    return ds_vm_call_named(vm, name, arguments, count);
}

int ds_vm_call_void(DsVM *vm, const char *name) { return invoke(vm, name, NULL, 0); }

int ds_vm_call_number(DsVM *vm, const char *name, double value) {
    DsValue argument = ds_float_value(value);
    return invoke(vm, name, &argument, 1);
}

int ds_vm_call_two_numbers(DsVM *vm, const char *name, double first, double second) {
    DsValue arguments[2] = {ds_float_value(first), ds_float_value(second)};
    return invoke(vm, name, arguments, 2);
}

int ds_vm_call_touch(DsVM *vm, const char *name, int id, double x, double y) {
    DsValue arguments[3] = {ds_int_value(id), ds_float_value(x), ds_float_value(y)};
    return invoke(vm, name, arguments, 3);
}

int ds_vm_call_string(DsVM *vm, const char *name, const char *text) {
    DsString *string = ds_new_string(vm, text, -1);
    DsValue argument = ds_string_value(string);
    const int ok = invoke(vm, name, &argument, 1);
    /* The argument only lives for this call; the collector removes it next
     * frame because nothing in the program reaches it. */
    return ok;
}

int ds_vm_start(DsVM *vm) {
    if (!vm || vm->started) return !vm->raised;
    vm->started = 1;
    for (int position = 0; position < vm->script_count; ++position) {
        DsScript *script = &vm->scripts[position];
        for (int index = 0; index < script->global_count; ++index) {
            DsNode *node = script->globals[index];
            DsValue value = eval_node(vm, node->n1, NULL);
            if (vm->raised) return 0;
            DsGlobal *global = &vm->globals[node->slot];
            ds_vm_coerce(vm, &value, global->type, global->struct_index, node->line, node->column);
            if (vm->raised) return 0;
            global->value = value;
        }
    }
    return invoke(vm, "load", NULL, 0);
}

int ds_vm_stop(DsVM *vm) {
    if (!vm) return 0;
    const int ok = invoke(vm, "quit", NULL, 0);
    for (int index = 0; index < vm->global_count; ++index) vm->globals[index].value = ds_nil_value();
    ds_vm_collect(vm);
    vm->started = 0;
    return ok;
}
