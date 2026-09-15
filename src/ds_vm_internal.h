/* Internal types shared by the DimScript VM translation units.  Not public:
 * an embedder only needs ds_vm.h. */
#ifndef DS_VM_INTERNAL_H
#define DS_VM_INTERNAL_H

#include <stdint.h>
#include <stddef.h>

#include "dimscript_runtime.h"
#include "ds_vm.h" /* provides the opaque DsVM typedef */

#define DS_MAX_SCRIPTS 32
#define DS_MAX_STRUCTS 32
#define DS_MAX_FUNCTIONS 128
#define DS_MAX_GLOBALS 128
#define DS_MAX_LOCALS 48
#define DS_MAX_FIELDS 32
#define DS_MAX_NODES_PER_BODY 4096
#define DS_MAX_ERROR 512
#define DS_MAX_DEPTH 96

/* --- values --------------------------------------------------------------- */

typedef enum DsKind {
    DS_NIL = 0,
    DS_BOOL,
    DS_INT,
    DS_FLOAT,
    DS_STRING,
    DS_OBJECT,
    DS_LIST
} DsKind;

/* Arity sentinels used by the builtin tables. */
#define DS_BUILTIN_UNKNOWN (-1)
#define DS_BUILTIN_VALUE (-2)    /* a namespace constant such as math.pi */
#define DS_BUILTIN_VARIADIC (-3) /* any argument count */

typedef enum DsFieldType {
    DS_FT_ANY = 0,
    DS_FT_INT,
    DS_FT_FLOAT,
    DS_FT_STRING,
    DS_FT_BOOL,
    DS_FT_LIST,
    DS_FT_STRUCT
} DsFieldType;

/* Every heap value starts with this header so the collector can walk and free
 * it without knowing the concrete type.  `persistent` marks arena-allocated
 * strings that must never be swept. */
typedef struct DsGc {
    struct DsGc *next;
    uint32_t size;
    uint8_t kind;
    uint8_t mark;
    uint8_t persistent;
} DsGc;

typedef struct DsString {
    DsGc gc;
    int length;
    char data[1];
} DsString;

struct DsValue;

typedef struct DsList {
    DsGc gc;
    int count;
    int capacity;
    struct DsValue *values;
} DsList;

typedef struct DsObject {
    DsGc gc;
    int type;
    int field_count;
    struct DsValue *fields;
} DsObject;

typedef struct DsValue {
    DsKind kind;
    union {
        int boolean;
        long long integer;
        double number;
        DsString *string;
        DsObject *object;
        DsList *list;
    } as;
} DsValue;

typedef enum DsOperator {
    DS_OP_NONE = 0,
    DS_OP_ADD,
    DS_OP_SUB,
    DS_OP_MUL,
    DS_OP_DIV,
    DS_OP_MOD,
    DS_OP_CONCAT,
    DS_OP_EQ,
    DS_OP_NE,
    DS_OP_LT,
    DS_OP_LE,
    DS_OP_GT,
    DS_OP_GE,
    DS_OP_AND,
    DS_OP_OR,
    DS_OP_NEG,
    DS_OP_NOT
} DsOperator;

/* --- syntax tree ---------------------------------------------------------- */

typedef enum DsNodeKind {
    N_INT = 1,
    N_FLOAT,
    N_STRING,
    N_BOOL,
    N_NIL,
    N_NAME,
    N_MEMBER,
    N_INDEX,
    N_LISTLIT,
    N_NEW,
    N_UNARY,
    N_BINARY,
    N_CALL,
    S_ASSIGN,
    S_EXPR,
    S_IF,
    S_WHILE,
    S_FOR,
    S_BREAK,
    S_CONTINUE,
    S_RETURN,
    S_DELETE
} DsNodeKind;

/* How an N_NAME resolves. */
typedef enum DsNameKind {
    DS_NAME_LOCAL = 0,
    DS_NAME_GLOBAL,
    DS_NAME_NAMESPACE,
    DS_NAME_UNKNOWN
} DsNameKind;

typedef struct DsNode DsNode;

/* One syntax node.  The layout is intentionally flat: a game script is small,
 * and a single struct keeps the parser, resolver and interpreter free of
 * per-kind allocation bookkeeping. */
struct DsNode {
    DsNodeKind kind;
    DsOperator op;
    int line;
    int column;
    int flags;        /* S_ASSIGN: 1 when written as `local x = ...` */
    int name_kind;    /* N_NAME: one of DsNameKind */
    int slot;         /* N_NAME: local slot / global index; S_FOR: loop slot */
    int struct_index; /* N_NEW and struct-typed members */
    void *cache;      /* literal strings are materialised once */
    long long integer;
    double number;
    char *name;       /* identifiers, members, fields */
    char *text;       /* string literal bytes */
    int text_length;
    int boolean;
    struct DsNode *n0;
    struct DsNode *n1;
    struct DsNode *n2;
    struct DsNode **children; /* statements of a block or arguments of a call */
    int child_count;
    struct DsNode **tail;     /* else branch of an if */
    int tail_count;
};

typedef struct DsField {
    char *name;
    char *type_name;    /* as written in the source, resolved while linking */
    DsFieldType type;
    int struct_index;
} DsField;

typedef struct DsStruct {
    char *name;
    DsField fields[DS_MAX_FIELDS];
    int count;
} DsStruct;

typedef struct DsFunction {
    char *name;
    char *file;
    int line;
    int arity;
    char *params[DS_MAX_LOCALS];
    char *param_types[DS_MAX_LOCALS]; /* written type names, "" when untyped */
    DsFieldType param_kinds[DS_MAX_LOCALS];
    char *return_type;
    int local_count;
    char *locals[DS_MAX_LOCALS];
    DsNode **body;
    int body_count;
} DsFunction;

typedef struct DsGlobal {
    char *name;
    DsValue value;
    DsFieldType type;
    int struct_index;
} DsGlobal;

typedef struct DsScript {
    char *name;
    char *text;
    size_t length;
    int linked;
    DsNode **requires;
    int require_count;
    DsStruct **structs;
    int struct_count;
    DsNode **globals;
    int global_count;
    DsFunction **functions;
    int function_count;
} DsScript;

struct DsVM {
    /* Arena: everything structural (AST, names, struct layouts) is freed as a
     * whole when the VM is destroyed, which keeps the interpreter free of
     * bookkeeping for the parse phase. */
    void *arena_pages;
    size_t arena_bytes;

    DsScript scripts[DS_MAX_SCRIPTS];
    int script_count;
    DsStruct structs[DS_MAX_STRUCTS];
    int struct_count;
    DsFunction *functions[DS_MAX_FUNCTIONS];
    int function_count;
    DsGlobal globals[DS_MAX_GLOBALS];
    int global_count;

    DsGc *heap;
    int heap_objects;
    size_t heap_bytes;
    size_t heap_next_gc;

    int initialized;
    int started;
    int quit_requested;
    int depth;
    int raised;
    int control;    /* 0 none, 1 break, 2 continue, 3 return */
    long budget;    /* statements left in the current frame */
    DsValue returned;
    char error[DS_MAX_ERROR];
    char scratch[256];
};

/* --- heap/value helpers (ds_vm_heap.c) ----------------------------------- */
void *ds_vm_arena(DsVM *vm, size_t size);
char *ds_vm_arena_string(DsVM *vm, const char *text, size_t length);
void ds_vm_raise(DsVM *vm, int line, int column, const char *format, ...);
void ds_vm_set_error(DsVM *vm, int line, int column, const char *format, ...);

DsValue ds_nil_value(void);
DsValue ds_bool_value(int value);
DsValue ds_int_value(long long value);
DsValue ds_float_value(double value);
DsValue ds_string_value(DsString *string);
DsString *ds_new_string(DsVM *vm, const char *text, int length);
DsString *ds_copy_value_string(DsVM *vm, const DsValue *value);
DsString *ds_literal_string(DsVM *vm, const char *text, int length);
DsNode *ds_node_new(DsVM *vm, DsNodeKind kind, int line, int column);
DsObject *ds_new_object(DsVM *vm, int struct_index);
DsList *ds_new_list(DsVM *vm);
void ds_list_reserve(DsVM *vm, DsList *list, int count);
void ds_mark_value(DsVM *vm, DsValue *value);
void ds_vm_collect(DsVM *vm);

int ds_truthy(const DsValue *value);
int ds_to_number(const DsValue *value, double *out);
long long ds_to_integer(const DsValue *value);
const char *ds_value_text(DsVM *vm, const DsValue *value, char *buffer, size_t capacity);
int ds_values_equal(const DsValue *left, const DsValue *right);
const char *ds_kind_name(DsKind kind);

/* --- parser/VM helpers ---------------------------------------------------- */
DsStruct *ds_find_struct(DsVM *vm, const char *name);
int ds_builtin_member_arity(const char *namesp, const char *name);
int ds_builtin_function_arity(const char *name);
int ds_find_global(DsVM *vm, const char *name);
DsFunction *ds_find_function(DsVM *vm, const char *name);
DsValue ds_vm_default_for(DsVM *vm, DsFieldType type, int struct_index);
int ds_vm_call_named(DsVM *vm, const char *name, const DsValue *arguments, int count);
void ds_vm_coerce(DsVM *vm, DsValue *value, DsFieldType type, int struct_index, int line, int column);

/* ds_vm_lang.c: one translation unit owns the grammar. */
int ds_vm_parse_script(DsVM *vm, DsScript *script);
int ds_vm_link_program(DsVM *vm);

DsValue ds_vm_execute(DsVM *vm, DsFunction *function, const DsValue *arguments, int argument_count);
DsValue ds_vm_evaluate(DsVM *vm, DsNode *node, DsValue *locals);

#endif /* DS_VM_INTERNAL_H */
