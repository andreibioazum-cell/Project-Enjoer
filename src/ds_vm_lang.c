/* Lexer, parser and linker for DimScript source text.
 *
 * The grammar is the one the README documents:
 *
 *   game = new ClickerGame
 *
 *   load() {
 *       game.score = 0
 *   }
 *
 *   update(dt: float) {
 *       if game.text_scale > 1.0 then
 *           game.text_scale = game.text_scale - (4.0 * dt)
 *       }
 *   }
 *
 * Blocks open with `{` (or implicitly after `then`/`do`) and are always closed
 * by `}`; `end` is accepted as an alias.  Newlines separate statements, so no
 * semicolons are needed, and `require "module"` pulls another .ds file of the
 * same game folder into the program. */
#include "ds_vm_internal.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum { TK_EOF = 0, TK_NEWLINE, TK_IDENT, TK_KEYWORD, TK_NUMBER, TK_STRING, TK_OP };

enum {
    KW_NONE = 0,
    KW_STRUCT,
    KW_NEW,
    KW_DELETE,
    KW_IF,
    KW_THEN,
    KW_DO,
    KW_ELSE,
    KW_END,
    KW_RETURN,
    KW_TRUE,
    KW_FALSE,
    KW_NIL,
    KW_AND,
    KW_OR,
    KW_NOT,
    KW_WHILE,
    KW_FOR,
    KW_BREAK,
    KW_CONTINUE,
    KW_REQUIRE,
    KW_LOCAL
};

static const char *const KEYWORDS[] = {
    "struct", "new", "delete", "if", "then", "do", "else", "end", "return", "true",
    "false", "nil", "and", "or", "not", "while", "for", "break", "continue",
    "require", "local"
};

typedef struct {
    int kind;
    int keyword;
    const char *text;
    int length;
    char *string;
    int string_length;
    double number;
    long long integer;
    int is_float;
    int line;
    int column;
} DsToken;

/* A growable pointer list used for both statements and declarations; the
 * contents are copied into the VM arena once a parse phase is complete. */
typedef struct {
    void **items;
    int count;
    int capacity;
} PtrList;

typedef struct {
    DsVM *vm;
    DsScript *script;
    DsToken *tokens;
    int count;
    int index;
} Parser;

DsNode *ds_node_new(DsVM *vm, DsNodeKind kind, int line, int column) {
    DsNode *node = (DsNode *)ds_vm_arena(vm, sizeof(DsNode));
    memset(node, 0, sizeof(DsNode));
    node->kind = kind;
    node->line = line;
    node->column = column;
    node->struct_index = -1;
    node->slot = -1;
    /* Every name starts unresolved; the linker rewrites this, and 0 would look
     * like "local" because the arena gives zeroed memory. */
    node->name_kind = DS_NAME_UNKNOWN;
    return node;
}

static void list_push(Parser *parser, PtrList *list, void *item) {
    (void)parser;
    if (list->count >= list->capacity) {
        const int capacity = list->capacity ? list->capacity * 2 : 8;
        void **grown = (void **)realloc(list->items, (size_t)capacity * sizeof(void *));
        if (!grown) {
            fputs("DimScript: out of memory while parsing\n", stderr);
            abort();
        }
        list->items = grown;
        list->capacity = capacity;
    }
    list->items[list->count++] = item;
}

static void **list_finish(Parser *parser, PtrList *list, int *count) {
    *count = list->count;
    if (!list->count) {
        free(list->items);
        list->items = NULL;
        return NULL;
    }
    void **items = (void **)ds_vm_arena(parser->vm, (size_t)list->count * sizeof(void *));
    memcpy(items, list->items, (size_t)list->count * sizeof(void *));
    free(list->items);
    list->items = NULL;
    list->count = 0;
    list->capacity = 0;
    return items;
}

/* --- lexer ---------------------------------------------------------------- */

typedef struct {
    DsToken *items;
    int count;
    int capacity;
} TokenList;

static void token_push(Parser *parser, TokenList *list, DsToken token) {
    (void)parser;
    if (list->count >= list->capacity) {
        const int capacity = list->capacity ? list->capacity * 2 : 64;
        DsToken *grown = (DsToken *)realloc(list->items, (size_t)capacity * sizeof(DsToken));
        if (!grown) {
            fputs("DimScript: out of memory while lexing\n", stderr);
            abort();
        }
        list->items = grown;
        list->capacity = capacity;
    }
    list->items[list->count++] = token;
}

static int lookup_keyword(const char *text, int length) {
    for (int index = 0; index < (int)(sizeof(KEYWORDS) / sizeof(KEYWORDS[0])); ++index) {
        const char *keyword = KEYWORDS[index];
        if ((int)strlen(keyword) == length && !strncmp(text, keyword, (size_t)length))
            return index + 1;
    }
    return KW_NONE;
}

static int ident_byte(unsigned char value) {
    return isalnum(value) || value == '_' || value >= 0x80;
}

static char *decode_string_literal(DsVM *vm, const char *raw, int length, int line, int column,
                                   int *out_length) {
    char *buffer = (char *)ds_vm_arena(vm, (size_t)length + 1);
    int used = 0;
    for (int index = 0; index < length; ++index) {
        const char value = raw[index];
        if (value != '\\') {
            buffer[used++] = value;
            continue;
        }
        if (index + 1 >= length) {
            ds_vm_set_error(vm, line, column, "незакрытая escape-последовательность в строке");
            return NULL;
        }
        const char escaped = raw[++index];
        switch (escaped) {
        case 'n': buffer[used++] = '\n'; break;
        case 'r': buffer[used++] = '\r'; break;
        case 't': buffer[used++] = '\t'; break;
        case '0': buffer[used++] = '\0'; break;
        case '\\': buffer[used++] = '\\'; break;
        case '"': buffer[used++] = '"'; break;
        case '\'': buffer[used++] = '\''; break;
        default: buffer[used++] = escaped; break; /* unknown escapes stay literal */
        }
    }
    buffer[used] = '\0';
    *out_length = used;
    return buffer;
}

static int lex_script(Parser *parser, TokenList *out) {
    DsVM *vm = parser->vm;
    const char *source = parser->script->text;
    const int length = (int)parser->script->length;
    int index = 0;
    int line = 1;
    int column = 1;

    while (index < length) {
        const char value = source[index];
        DsToken token;
        memset(&token, 0, sizeof(token));
        token.line = line;
        token.column = column;

        if (value == '\n') {
            token.kind = TK_NEWLINE;
            token.text = source + index;
            token.length = 1;
            token_push(parser, out, token);
            ++index;
            ++line;
            column = 0;
            continue;
        }
        if (value == ' ' || value == '\t' || value == '\r' || value == '\f' || value == '\v') {
            ++index;
            ++column;
            continue;
        }
        if ((value == '-' && index + 1 < length && source[index + 1] == '-') ||
            (value == '/' && index + 1 < length && source[index + 1] == '/')) {
            while (index < length && source[index] != '\n') {
                ++index;
                ++column;
            }
            continue;
        }

        if (value == '"' || value == '\'') {
            const char quote = value;
            const int start = index + 1;
            int cursor = start;
            int closed = 0;
            while (cursor < length) {
                if (source[cursor] == '\\' && cursor + 1 < length) {
                    cursor += 2;
                    continue;
                }
                if (source[cursor] == '\n') break;
                if (source[cursor] == quote) {
                    closed = 1;
                    break;
                }
                ++cursor;
            }
            if (!closed) {
                ds_vm_set_error(vm, line, column, "незакрытая строка");
                return 0;
            }
            int decoded_length = 0;
            token.kind = TK_STRING;
            token.string = decode_string_literal(vm, source + start, cursor - start, line, column,
                                                 &decoded_length);
            if (!token.string) return 0;
            token.string_length = decoded_length;
            token.text = source + start;
            token.length = cursor - start;
            column += (cursor + 1) - index;
            index = cursor + 1;
            token_push(parser, out, token);
            continue;
        }

        if (isdigit((unsigned char)value) ||
            (value == '.' && index + 1 < length && isdigit((unsigned char)source[index + 1]))) {
            const int start = index;
            int saw_dot = 0;
            while (index < length && isdigit((unsigned char)source[index])) ++index;
            if (index < length && source[index] == '.' &&
                !(index + 1 < length && source[index + 1] == '.')) {
                saw_dot = 1;
                ++index;
                while (index < length && isdigit((unsigned char)source[index])) ++index;
            }
            if (index < length && (source[index] == 'e' || source[index] == 'E')) {
                int probe = index + 1;
                if (probe < length && (source[probe] == '+' || source[probe] == '-')) ++probe;
                if (probe < length && isdigit((unsigned char)source[probe])) {
                    saw_dot = 1;
                    index = probe;
                    while (index < length && isdigit((unsigned char)source[index])) ++index;
                }
            }
            char buffer[64];
            const int token_length = index - start;
            if (token_length >= (int)sizeof(buffer)) {
                ds_vm_set_error(vm, line, column, "числовой литерал слишком длинный");
                return 0;
            }
            memcpy(buffer, source + start, (size_t)token_length);
            buffer[token_length] = '\0';
            token.kind = TK_NUMBER;
            token.text = source + start;
            token.length = token_length;
            token.is_float = saw_dot;
            if (saw_dot) token.number = strtod(buffer, NULL);
            else token.integer = strtoll(buffer, NULL, 10);
            column += token_length;
            token_push(parser, out, token);
            continue;
        }

        if (isalpha((unsigned char)value) || value == '_' || (unsigned char)value >= 0x80) {
            const int start = index;
            while (index < length && ident_byte((unsigned char)source[index])) ++index;
            token.kind = TK_IDENT;
            token.text = source + start;
            token.length = index - start;
            token.keyword = lookup_keyword(source + start, index - start);
            if (token.keyword) token.kind = TK_KEYWORD;
            column += index - start;
            token_push(parser, out, token);
            continue;
        }

        /* Longest operators first: `..` is concatenation, not two dots. */
        static const char *const LONG_OPERATORS[] = {">=", "<=", "==", "!=", "~=", "..", "&&", "||"};
        int matched = 0;
        for (int candidate = 0; candidate < (int)(sizeof(LONG_OPERATORS) / sizeof(LONG_OPERATORS[0]));
             ++candidate) {
            const char *oper = LONG_OPERATORS[candidate];
            if (index + 2 <= length && !strncmp(source + index, oper, 2)) {
                token.kind = TK_OP;
                token.text = source + index;
                token.length = 2;
                index += 2;
                column += 2;
                token_push(parser, out, token);
                matched = 1;
                break;
            }
        }
        if (matched) continue;

        if (strchr("{}()[]:,.=+-*/%<>!;", value)) {
            token.kind = TK_OP;
            token.text = source + index;
            token.length = 1;
            ++index;
            ++column;
            token_push(parser, out, token);
            continue;
        }

        ds_vm_set_error(vm, line, column, "неизвестный символ '%c'", value);
        return 0;
    }

    {
        DsToken terminator;
        memset(&terminator, 0, sizeof(terminator));
        terminator.kind = TK_EOF;
        terminator.line = line;
        terminator.column = column;
        terminator.text = source + length;
        token_push(parser, out, terminator);
    }
    return 1;
}

/* --- parser --------------------------------------------------------------- */

static DsToken *current_token(Parser *parser) { return &parser->tokens[parser->index]; }

static DsToken *peek_token(Parser *parser, int offset) {
    int position = parser->index + offset;
    if (position >= parser->count) position = parser->count - 1;
    return &parser->tokens[position];
}

static void advance_token(Parser *parser) {
    if (parser->index < parser->count - 1) ++parser->index;
}

static int token_is_op(const DsToken *token, const char *value) {
    if (token->kind != TK_OP) return 0;
    const int size = (int)strlen(value);
    return token->length == size && !strncmp(token->text, value, (size_t)size);
}

static int at_op(Parser *parser, const char *value) { return token_is_op(current_token(parser), value); }

static int at_keyword(Parser *parser, int keyword) {
    DsToken *token = current_token(parser);
    return token->kind == TK_KEYWORD && token->keyword == keyword;
}

static int accept_op(Parser *parser, const char *value) {
    if (at_op(parser, value)) {
        advance_token(parser);
        return 1;
    }
    return 0;
}

static int accept_keyword(Parser *parser, int keyword) {
    if (at_keyword(parser, keyword)) {
        advance_token(parser);
        return 1;
    }
    return 0;
}

static int identifier_like(const DsToken *token) {
    if (token->kind == TK_IDENT) return 1;
    /* A few keywords are soft: they may still be used as names so that
     * `count`, `do` or `end` do not break a game by being spelled out. */
    if (token->kind == TK_KEYWORD)
        return token->keyword == KW_THEN || token->keyword == KW_DO || token->keyword == KW_END ||
               token->keyword == KW_LOCAL || token->keyword == KW_REQUIRE ||
               token->keyword == KW_FOR || token->keyword == KW_STRUCT;
    return 0;
}

static char *expect_identifier(Parser *parser, const char *what) {
    DsToken *token = current_token(parser);
    if (!identifier_like(token)) {
        ds_vm_set_error(parser->vm, token->line, token->column, "ожидалось %s", what);
        return NULL;
    }
    advance_token(parser);
    return ds_vm_arena_string(parser->vm, token->text, (size_t)token->length);
}

static void skip_newlines(Parser *parser) {
    while (current_token(parser)->kind == TK_NEWLINE || at_op(parser, ";")) advance_token(parser);
}

static int binary_operator(const DsToken *token, DsOperator *out) {
    if (token->kind == TK_KEYWORD) {
        if (token->keyword == KW_OR) { *out = DS_OP_OR; return 1; }
        if (token->keyword == KW_AND) { *out = DS_OP_AND; return 1; }
        return 0;
    }
    if (token->kind != TK_OP) return 0;
    if (token_is_op(token, "||")) { *out = DS_OP_OR; return 1; }
    if (token_is_op(token, "&&")) { *out = DS_OP_AND; return 1; }
    if (token_is_op(token, "==")) { *out = DS_OP_EQ; return 1; }
    if (token_is_op(token, "!=") || token_is_op(token, "~=")) { *out = DS_OP_NE; return 1; }
    if (token_is_op(token, ">")) { *out = DS_OP_GT; return 1; }
    if (token_is_op(token, "<")) { *out = DS_OP_LT; return 1; }
    if (token_is_op(token, ">=")) { *out = DS_OP_GE; return 1; }
    if (token_is_op(token, "<=")) { *out = DS_OP_LE; return 1; }
    if (token_is_op(token, "..")) { *out = DS_OP_CONCAT; return 1; }
    if (token_is_op(token, "+")) { *out = DS_OP_ADD; return 1; }
    if (token_is_op(token, "-")) { *out = DS_OP_SUB; return 1; }
    if (token_is_op(token, "*")) { *out = DS_OP_MUL; return 1; }
    if (token_is_op(token, "/")) { *out = DS_OP_DIV; return 1; }
    if (token_is_op(token, "%")) { *out = DS_OP_MOD; return 1; }
    return 0;
}

static int precedence_of(DsOperator oper) {
    switch (oper) {
    case DS_OP_OR: return 1;
    case DS_OP_AND: return 2;
    case DS_OP_EQ:
    case DS_OP_NE: return 3;
    case DS_OP_LT:
    case DS_OP_LE:
    case DS_OP_GT:
    case DS_OP_GE: return 4;
    case DS_OP_CONCAT: return 5;
    case DS_OP_ADD:
    case DS_OP_SUB: return 6;
    case DS_OP_MUL:
    case DS_OP_DIV:
    case DS_OP_MOD: return 7;
    default: return 0;
    }
}

static DsNode *parse_expression(Parser *parser);
static DsNode *parse_expression_prec(Parser *parser, int minimum);
static int parse_statements(Parser *parser, PtrList *out);
static int parse_statement(Parser *parser, PtrList *out);

/* Shared by `[...]` and `(...)`: a comma separated list closed by `closing`.
 * Newlines are allowed inside brackets, which is what makes a long call
 * readable in a game script. */
static int parse_argument_list(Parser *parser, PtrList *out, const char *closing) {
    skip_newlines(parser);
    if (!at_op(parser, closing)) {
        for (;;) {
            DsNode *item = parse_expression(parser);
            if (!item) return 0;
            list_push(parser, out, item);
            skip_newlines(parser);
            if (!accept_op(parser, ",")) break;
            skip_newlines(parser);
        }
    }
    if (!accept_op(parser, closing)) {
        ds_vm_set_error(parser->vm, current_token(parser)->line, current_token(parser)->column,
                        "ожидалось '%s'", closing);
        return 0;
    }
    return 1;
}

static DsNode *unary_node(Parser *parser, DsOperator oper, DsToken *token) {
    DsNode *operand = parse_expression_prec(parser, 8);
    if (!operand) return NULL;
    DsNode *node = ds_node_new(parser->vm, N_UNARY, token->line, token->column);
    node->op = oper;
    node->n0 = operand;
    return node;
}

static DsNode *parse_prefix(Parser *parser) {
    DsToken *token = current_token(parser);
    DsVM *vm = parser->vm;

    if (token_is_op(token, "-") || token_is_op(token, "+")) {
        advance_token(parser);
        return unary_node(parser, token_is_op(token, "-") ? DS_OP_NEG : DS_OP_ADD, token);
    }
    if (token_is_op(token, "!") || at_keyword(parser, KW_NOT)) {
        advance_token(parser);
        return unary_node(parser, DS_OP_NOT, token);
    }
    if (token->kind == TK_NUMBER) {
        advance_token(parser);
        DsNode *node = ds_node_new(vm, token->is_float ? N_FLOAT : N_INT, token->line, token->column);
        if (token->is_float) node->number = token->number;
        else node->integer = token->integer;
        return node;
    }
    if (token->kind == TK_STRING) {
        advance_token(parser);
        DsNode *node = ds_node_new(vm, N_STRING, token->line, token->column);
        node->text = token->string;
        node->text_length = token->string_length;
        return node;
    }
    if (token->kind == TK_KEYWORD && (token->keyword == KW_TRUE || token->keyword == KW_FALSE)) {
        advance_token(parser);
        DsNode *node = ds_node_new(vm, N_BOOL, token->line, token->column);
        node->boolean = token->keyword == KW_TRUE;
        return node;
    }
    if (token->kind == TK_KEYWORD && token->keyword == KW_NIL) {
        advance_token(parser);
        return ds_node_new(vm, N_NIL, token->line, token->column);
    }
    if (token->kind == TK_KEYWORD && token->keyword == KW_NEW) {
        advance_token(parser);
        char *name = expect_identifier(parser, "имя типа после new");
        if (!name) return NULL;
        DsNode *node = ds_node_new(vm, N_NEW, token->line, token->column);
        node->name = name;
        return node;
    }
    if (token_is_op(token, "(")) {
        advance_token(parser);
        DsNode *inner = parse_expression(parser);
        if (!inner) return NULL;
        if (!accept_op(parser, ")")) {
            ds_vm_set_error(vm, current_token(parser)->line, current_token(parser)->column,
                            "ожидалось ')'");
            return NULL;
        }
        return inner;
    }
    if (token_is_op(token, "[")) {
        advance_token(parser);
        PtrList items;
        memset(&items, 0, sizeof(items));
        if (!parse_argument_list(parser, &items, "]")) return NULL;
        DsNode *node = ds_node_new(vm, N_LISTLIT, token->line, token->column);
        node->children = (DsNode **)list_finish(parser, &items, &node->child_count);
        return node;
    }
    if (identifier_like(token)) {
        advance_token(parser);
        DsNode *node = ds_node_new(vm, N_NAME, token->line, token->column);
        node->name = ds_vm_arena_string(vm, token->text, (size_t)token->length);
        return node;
    }

    ds_vm_set_error(vm, token->line, token->column, "неожиданный символ в выражении");
    return NULL;
}

static DsNode *parse_expression_prec(Parser *parser, int minimum) {
    DsNode *expression = parse_prefix(parser);
    if (!expression) return NULL;

    for (;;) {
        /* Member access, calls and indexing bind tighter than any operator. */
        if (at_op(parser, ".")) {
            advance_token(parser);
            DsToken *token = current_token(parser);
            char *name = expect_identifier(parser, "имя поля или метода");
            if (!name) return NULL;
            DsNode *node = ds_node_new(parser->vm, N_MEMBER, token->line, token->column);
            node->n0 = expression;
            node->name = name;
            expression = node;
            continue;
        }
        if (at_op(parser, "(")) {
            DsToken *token = current_token(parser);
            advance_token(parser);
            PtrList args;
            memset(&args, 0, sizeof(args));
            if (!parse_argument_list(parser, &args, ")")) return NULL;
            DsNode *node = ds_node_new(parser->vm, N_CALL, token->line, token->column);
            node->n0 = expression;
            node->children = (DsNode **)list_finish(parser, &args, &node->child_count);
            expression = node;
            continue;
        }
        if (at_op(parser, "[")) {
            DsToken *token = current_token(parser);
            advance_token(parser);
            DsNode *index = parse_expression(parser);
            if (!index) return NULL;
            if (!accept_op(parser, "]")) {
                ds_vm_set_error(parser->vm, current_token(parser)->line, current_token(parser)->column,
                                "ожидалось ']'");
                return NULL;
            }
            DsNode *node = ds_node_new(parser->vm, N_INDEX, token->line, token->column);
            node->n0 = expression;
            node->n1 = index;
            expression = node;
            continue;
        }

        DsOperator oper = DS_OP_NONE;
        if (!binary_operator(current_token(parser), &oper)) break;
        const int precedence = precedence_of(oper);
        if (precedence < minimum) break;
        DsToken *token = current_token(parser);
        advance_token(parser);
        /* A line may end after an operator, never before one: that is what
         * makes newline a statement separator without breaking long formulas. */
        skip_newlines(parser);
        DsNode *right = parse_expression_prec(parser, precedence + 1);
        if (!right) return NULL;
        DsNode *node = ds_node_new(parser->vm, N_BINARY, token->line, token->column);
        node->op = oper;
        node->n0 = expression;
        node->n1 = right;
        expression = node;
    }
    return expression;
}

static DsNode *parse_expression(Parser *parser) { return parse_expression_prec(parser, 1); }

static void consume_block_open(Parser *parser) {
    accept_keyword(parser, KW_THEN);
    accept_keyword(parser, KW_DO);
    skip_newlines(parser);
    accept_op(parser, "{");
}

/* Parses statements up to the closing `}` (or `end`). */
static int parse_statements(Parser *parser, PtrList *out) {
    skip_newlines(parser);
    while (!at_op(parser, "}")) {
        DsToken *token = current_token(parser);
        if (token->kind == TK_EOF) {
            ds_vm_set_error(parser->vm, token->line, token->column, "незакрытый блок, ожидалась '}'");
            return 0;
        }
        if (at_keyword(parser, KW_END)) {
            advance_token(parser);
            return 1;
        }
        if (!parse_statement(parser, out)) return 0;
        skip_newlines(parser);
    }
    accept_op(parser, "}");
    return 1;
}

static int parse_if(Parser *parser, PtrList *out) {
    DsVM *vm = parser->vm;
    DsToken *token = current_token(parser);
    advance_token(parser); /* if */
    DsNode *node = ds_node_new(vm, S_IF, token->line, token->column);
    node->n0 = parse_expression(parser);
    if (!node->n0) return 0;
    consume_block_open(parser);
    PtrList body;
    memset(&body, 0, sizeof(body));
    if (!parse_statements(parser, &body)) return 0;
    node->children = (DsNode **)list_finish(parser, &body, &node->child_count);

    const int checkpoint = parser->index;
    skip_newlines(parser);
    if (at_keyword(parser, KW_ELSE)) {
        advance_token(parser);
        PtrList other;
        memset(&other, 0, sizeof(other));
        skip_newlines(parser);
        if (at_keyword(parser, KW_IF)) {
            if (!parse_statement(parser, &other)) return 0;
        } else {
            consume_block_open(parser);
            if (!parse_statements(parser, &other)) return 0;
        }
        node->tail = (DsNode **)list_finish(parser, &other, &node->tail_count);
    } else {
        parser->index = checkpoint;
    }
    list_push(parser, out, node);
    return 1;
}

static int parse_while(Parser *parser, PtrList *out) {
    DsVM *vm = parser->vm;
    DsToken *token = current_token(parser);
    advance_token(parser); /* while */
    DsNode *node = ds_node_new(vm, S_WHILE, token->line, token->column);
    node->n0 = parse_expression(parser);
    if (!node->n0) return 0;
    consume_block_open(parser);
    PtrList body;
    memset(&body, 0, sizeof(body));
    if (!parse_statements(parser, &body)) return 0;
    node->children = (DsNode **)list_finish(parser, &body, &node->child_count);
    list_push(parser, out, node);
    return 1;
}

static int parse_for(Parser *parser, PtrList *out) {
    DsVM *vm = parser->vm;
    DsToken *token = current_token(parser);
    advance_token(parser); /* for */
    char *name = expect_identifier(parser, "имя переменной цикла");
    if (!name) return 0;
    if (!accept_op(parser, "=")) {
        ds_vm_set_error(vm, current_token(parser)->line, current_token(parser)->column,
                        "в for ожидалось '=': for i = 0, 10 do ... }");
        return 0;
    }
    DsNode *start = parse_expression(parser);
    if (!start) return 0;
    if (!accept_op(parser, ",")) {
        ds_vm_set_error(vm, current_token(parser)->line, current_token(parser)->column,
                        "в for ожидалась ',' (границы): for i = 0, 10 do ... }");
        return 0;
    }
    DsNode *stop = parse_expression(parser);
    if (!stop) return 0;
    DsNode *step = NULL;
    if (accept_op(parser, ",")) {
        step = parse_expression(parser);
        if (!step) return 0;
    }
    DsNode *node = ds_node_new(vm, S_FOR, token->line, token->column);
    node->name = name;
    node->n0 = start;
    node->n1 = stop;
    node->n2 = step;
    consume_block_open(parser);
    PtrList body;
    memset(&body, 0, sizeof(body));
    if (!parse_statements(parser, &body)) return 0;
    node->children = (DsNode **)list_finish(parser, &body, &node->child_count);
    list_push(parser, out, node);
    return 1;
}

static int parse_statement(Parser *parser, PtrList *out) {
    DsVM *vm = parser->vm;
    DsToken *token = current_token(parser);

    if (at_keyword(parser, KW_IF)) return parse_if(parser, out);
    if (at_keyword(parser, KW_WHILE)) return parse_while(parser, out);
    if (at_keyword(parser, KW_FOR)) return parse_for(parser, out);

    if (at_keyword(parser, KW_BREAK)) {
        advance_token(parser);
        list_push(parser, out, ds_node_new(vm, S_BREAK, token->line, token->column));
        return 1;
    }
    if (at_keyword(parser, KW_CONTINUE)) {
        advance_token(parser);
        list_push(parser, out, ds_node_new(vm, S_CONTINUE, token->line, token->column));
        return 1;
    }
    if (at_keyword(parser, KW_RETURN)) {
        advance_token(parser);
        DsNode *node = ds_node_new(vm, S_RETURN, token->line, token->column);
        if (current_token(parser)->kind != TK_NEWLINE && !at_op(parser, "}") &&
            current_token(parser)->kind != TK_EOF && !at_keyword(parser, KW_END)) {
            node->n0 = parse_expression(parser);
            if (!node->n0) return 0;
        }
        list_push(parser, out, node);
        return 1;
    }
    if (at_keyword(parser, KW_DELETE)) {
        advance_token(parser);
        DsNode *node = ds_node_new(vm, S_DELETE, token->line, token->column);
        node->n0 = parse_expression(parser);
        if (!node->n0) return 0;
        list_push(parser, out, node);
        return 1;
    }

    const int explicit_local = accept_keyword(parser, KW_LOCAL);
    DsNode *expression = parse_expression(parser);
    if (!expression) return 0;
    if (at_op(parser, "=")) {
        advance_token(parser);
        skip_newlines(parser);
        DsNode *value = parse_expression(parser);
        if (!value) return 0;
        if (expression->kind != N_NAME && expression->kind != N_MEMBER &&
            expression->kind != N_INDEX) {
            ds_vm_set_error(vm, token->line, token->column,
                            "слева от '=' должно быть имя, поле или элемент списка");
            return 0;
        }
        DsNode *node = ds_node_new(vm, S_ASSIGN, token->line, token->column);
        node->n0 = expression;
        node->n1 = value;
        node->flags = explicit_local ? 1 : 0;
        list_push(parser, out, node);
        return 1;
    }
    if (explicit_local) {
        ds_vm_set_error(vm, token->line, token->column, "после local ожидалось 'имя = значение'");
        return 0;
    }
    DsNode *node = ds_node_new(vm, S_EXPR, token->line, token->column);
    node->n0 = expression;
    list_push(parser, out, node);
    return 1;
}

static int parse_struct_declaration(Parser *parser, DsStruct *type) {
    advance_token(parser); /* struct */
    char *name = expect_identifier(parser, "имя структуры");
    if (!name) return 0;
    type->name = name;
    if (!accept_op(parser, "{")) {
        ds_vm_set_error(parser->vm, current_token(parser)->line, current_token(parser)->column,
                        "после имени структуры ожидалось '{'");
        return 0;
    }
    skip_newlines(parser);
    while (!at_op(parser, "}")) {
        if (current_token(parser)->kind == TK_EOF) {
            ds_vm_set_error(parser->vm, current_token(parser)->line, current_token(parser)->column,
                            "незакрытая структура, ожидалась '}'");
            return 0;
        }
        if (at_keyword(parser, KW_END)) {
            advance_token(parser);
            return 1;
        }
        if (type->count >= DS_MAX_FIELDS) {
            ds_vm_set_error(parser->vm, current_token(parser)->line, current_token(parser)->column,
                            "в структуре больше %d полей", DS_MAX_FIELDS);
            return 0;
        }
        DsToken *field_token = current_token(parser);
        char *field_name = expect_identifier(parser, "имя поля");
        if (!field_name) return 0;
        if (!accept_op(parser, ":")) {
            ds_vm_set_error(parser->vm, current_token(parser)->line, current_token(parser)->column,
                            "после имени поля ожидалось ':' и тип (int, float, string, bool или list)");
            return 0;
        }
        char *type_name = expect_identifier(parser, "тип поля");
        if (!type_name) return 0;
        for (int index = 0; index < type->count; ++index) {
            if (!strcmp(type->fields[index].name, field_name)) {
                ds_vm_set_error(parser->vm, field_token->line, field_token->column,
                                "поле '%s' объявлено дважды", field_name);
                return 0;
            }
        }
        type->fields[type->count].name = field_name;
        type->fields[type->count].type_name = type_name;
        type->fields[type->count].type = DS_FT_ANY;
        type->fields[type->count].struct_index = -1;
        ++type->count;
        accept_op(parser, ",");
        accept_op(parser, ";");
        skip_newlines(parser);
    }
    accept_op(parser, "}");
    return 1;
}

static int parse_function_declaration(Parser *parser, DsFunction *function) {
    DsToken *name_token = current_token(parser);
    function->name = expect_identifier(parser, "имя функции");
    if (!function->name) return 0;
    function->line = name_token->line;
    function->file = parser->script->name;
    if (!accept_op(parser, "(")) {
        ds_vm_set_error(parser->vm, current_token(parser)->line, current_token(parser)->column,
                        "после имени функции ожидалось '('");
        return 0;
    }
    skip_newlines(parser);
    if (!at_op(parser, ")")) {
        for (;;) {
            char *parameter = expect_identifier(parser, "имя параметра");
            if (!parameter) return 0;
            char *type_name = (char *)"";
            if (accept_op(parser, ":")) {
                type_name = expect_identifier(parser, "тип параметра");
                if (!type_name) return 0;
            }
            if (function->arity >= DS_MAX_LOCALS) {
                ds_vm_set_error(parser->vm, name_token->line, name_token->column,
                                "слишком много параметров (максимум %d)", DS_MAX_LOCALS);
                return 0;
            }
            function->params[function->arity] = parameter;
            function->param_types[function->arity] = type_name;
            function->param_kinds[function->arity] = DS_FT_ANY;
            ++function->arity;
            skip_newlines(parser);
            if (!accept_op(parser, ",")) break;
            skip_newlines(parser);
        }
    }
    if (!accept_op(parser, ")")) {
        ds_vm_set_error(parser->vm, current_token(parser)->line, current_token(parser)->column,
                        "ожидалось ')'");
        return 0;
    }
    function->return_type = (char *)"";
    if (accept_op(parser, ":")) {
        function->return_type = expect_identifier(parser, "тип результата");
        if (!function->return_type) return 0;
    }
    skip_newlines(parser);
    if (!accept_op(parser, "{")) {
        ds_vm_set_error(parser->vm, current_token(parser)->line, current_token(parser)->column,
                        "после объявления функции ожидалось '{'");
        return 0;
    }
    PtrList body;
    memset(&body, 0, sizeof(body));
    if (!parse_statements(parser, &body)) return 0;
    function->body = (DsNode **)list_finish(parser, &body, &function->body_count);
    return 1;
}

static int parse_script(Parser *parser) {
    PtrList globals;
    PtrList requires;
    PtrList structs;
    PtrList functions;
    memset(&globals, 0, sizeof(globals));
    memset(&requires, 0, sizeof(requires));
    memset(&structs, 0, sizeof(structs));
    memset(&functions, 0, sizeof(functions));
    DsVM *vm = parser->vm;

    skip_newlines(parser);
    while (current_token(parser)->kind != TK_EOF) {
        DsToken *token = current_token(parser);
        if (at_keyword(parser, KW_REQUIRE)) {
            advance_token(parser);
            DsToken *text_token = current_token(parser);
            if (text_token->kind != TK_STRING) {
                ds_vm_set_error(vm, text_token->line, text_token->column,
                                "require ожидает строку: require \"ui\"");
                return 0;
            }
            advance_token(parser);
            DsNode *node = ds_node_new(vm, N_STRING, text_token->line, text_token->column);
            node->text = text_token->string;
            node->text_length = text_token->string_length;
            list_push(parser, &requires, node);
        } else if (at_keyword(parser, KW_STRUCT)) {
            DsStruct *type = (DsStruct *)ds_vm_arena(vm, sizeof(DsStruct));
            memset(type, 0, sizeof(DsStruct));
            if (!parse_struct_declaration(parser, type)) return 0;
            list_push(parser, &structs, type);
        } else if (identifier_like(token) && peek_token(parser, 1)->kind == TK_OP &&
                   token_is_op(peek_token(parser, 1), "(")) {
            DsFunction *function = (DsFunction *)ds_vm_arena(vm, sizeof(DsFunction));
            memset(function, 0, sizeof(DsFunction));
            if (!parse_function_declaration(parser, function)) return 0;
            list_push(parser, &functions, function);
        } else if (identifier_like(token)) {
            DsNode *target = parse_expression(parser);
            if (!target) return 0;
            if (target->kind != N_NAME) {
                ds_vm_set_error(vm, token->line, token->column,
                                "на верхнем уровне допустимо только 'имя = значение'");
                return 0;
            }
            if (!accept_op(parser, "=")) {
                ds_vm_set_error(vm, current_token(parser)->line, current_token(parser)->column,
                                "ожидалось '=' после имени глобальной переменной");
                return 0;
            }
            skip_newlines(parser);
            DsNode *value = parse_expression(parser);
            if (!value) return 0;
            DsNode *node = ds_node_new(vm, S_ASSIGN, token->line, token->column);
            node->n0 = target;
            node->n1 = value;
            list_push(parser, &globals, node);
        } else {
            ds_vm_set_error(vm, token->line, token->column,
                            "ожидался struct, функция, глобальное присваивание или require");
            return 0;
        }
        skip_newlines(parser);
    }

    DsScript *script = parser->script;
    script->globals = (DsNode **)list_finish(parser, &globals, &script->global_count);
    script->requires = (DsNode **)list_finish(parser, &requires, &script->require_count);
    script->structs = (DsStruct **)list_finish(parser, &structs, &script->struct_count);
    script->functions = (DsFunction **)list_finish(parser, &functions, &script->function_count);
    return 1;
}

int ds_vm_parse_script(DsVM *vm, DsScript *script) {
    Parser parser;
    TokenList tokens;
    memset(&tokens, 0, sizeof(tokens));
    parser.vm = vm;
    parser.script = script;
    parser.tokens = NULL;
    parser.count = 0;
    parser.index = 0;

    if (!lex_script(&parser, &tokens)) {
        free(tokens.items);
        return 0;
    }
    parser.tokens = tokens.items;
    parser.count = tokens.count;
    const int ok = parse_script(&parser);
    free(tokens.items);
    parser.tokens = NULL;
    return ok;
}

/* --- linking -------------------------------------------------------------- */

static const struct {
    const char *name;
    int arity;
} CALLBACKS[] = {
    {"load", 0},
    {"update", 1},
    {"draw", 0},
    {"touchpressed", 3},
    {"touchmoved", 3},
    {"touchreleased", 3},
    {"keypressed", 1},
    {"keyreleased", 1},
    {"resized", 2},
    {"quit", 0},
};

static int callback_arity(const char *name) {
    for (int index = 0; index < (int)(sizeof(CALLBACKS) / sizeof(CALLBACKS[0])); ++index)
        if (!strcmp(CALLBACKS[index].name, name)) return CALLBACKS[index].arity;
    return -1;
}

static int add_local(DsFunction *function, const char *name) {
    for (int index = 0; index < function->local_count; ++index)
        if (!strcmp(function->locals[index], name)) return index;
    if (function->local_count >= DS_MAX_LOCALS) return -1;
    function->locals[function->local_count] = (char *)name;
    return function->local_count++;
}

static int resolve_name(DsVM *vm, DsFunction *function, DsNode *node) {
    static const char *const NAMESPACES[] = {"render", "math", "engine", "input"};
    if (node->name_kind != DS_NAME_UNKNOWN) return 1;
    if (function) {
        for (int index = 0; index < function->local_count; ++index)
            if (!strcmp(function->locals[index], node->name)) {
                node->name_kind = DS_NAME_LOCAL;
                node->slot = index;
                return 1;
            }
    }
    const int global = ds_find_global(vm, node->name);
    if (global >= 0) {
        node->name_kind = DS_NAME_GLOBAL;
        node->slot = global;
        return 1;
    }
    for (int index = 0; index < (int)(sizeof(NAMESPACES) / sizeof(NAMESPACES[0])); ++index)
        if (!strcmp(node->name, NAMESPACES[index])) {
            node->name_kind = DS_NAME_NAMESPACE;
            node->slot = -1;
            return 1;
        }
    if (ds_find_function(vm, node->name)) {
        ds_vm_set_error(vm, node->line, node->column,
                        "'%s' — функция, используйте вызов %s(...)", node->name, node->name);
        return 0;
    }
    ds_vm_set_error(vm, node->line, node->column,
                    "неизвестное имя '%s' (нет такой глобальной переменной или неймспейса)",
                    node->name);
    return 0;
}

static int register_global(DsVM *vm, const char *name, DsNode *initializer) {
    for (int index = 0; index < vm->global_count; ++index) {
        if (!strcmp(vm->globals[index].name, name)) {
            ds_vm_set_error(vm, initializer->line, initializer->column,
                            "глобальная переменная '%s' объявлена дважды", name);
            return -1;
        }
    }
    if (vm->global_count >= DS_MAX_GLOBALS) {
        ds_vm_set_error(vm, initializer->line, initializer->column,
                        "слишком много глобальных переменных (максимум %d)", DS_MAX_GLOBALS);
        return -1;
    }
    DsGlobal *global = &vm->globals[vm->global_count];
    memset(global, 0, sizeof(*global));
    global->name = (char *)name;
    global->value = ds_nil_value();
    global->type = DS_FT_ANY;
    global->struct_index = -1;
    switch (initializer->kind) {
    case N_NEW:
        global->type = DS_FT_STRUCT;
        global->struct_index = initializer->struct_index;
        break;
    case N_LISTLIT: global->type = DS_FT_LIST; break;
    case N_INT: global->type = DS_FT_INT; break;
    case N_FLOAT: global->type = DS_FT_FLOAT; break;
    case N_STRING: global->type = DS_FT_STRING; break;
    case N_BOOL: global->type = DS_FT_BOOL; break;
    default: break;
    }
    return vm->global_count++;
}

static int resolve_expression(DsVM *vm, DsFunction *function, DsNode *node);
static int resolve_block(DsVM *vm, DsFunction *function, DsNode **statements, int count);

static int resolve_call(DsVM *vm, DsFunction *function, DsNode *node) {
    DsNode *callee = node->n0;
    if (callee->kind == N_NAME) {
        DsFunction *target = ds_find_function(vm, callee->name);
        if (target) {
            if (target->arity != node->child_count) {
                ds_vm_set_error(vm, node->line, node->column,
                                "'%s' ожидает %d аргументов, получено %d", target->name,
                                target->arity, node->child_count);
                return 0;
            }
        } else if (ds_builtin_function_arity(callee->name) == DS_BUILTIN_UNKNOWN) {
            ds_vm_set_error(vm, node->line, node->column,
                            "неизвестная функция '%s' (есть print, str, len и list-методы)",
                            callee->name);
            return 0;
        }
    } else if (callee->kind == N_MEMBER) {
        if (callee->n0 && callee->n0->kind == N_NAME && !resolve_name(vm, function, callee->n0))
            return 0;
        if (callee->n0 && callee->n0->kind == N_NAME &&
            callee->n0->name_kind == DS_NAME_NAMESPACE) {
            const int expected = ds_builtin_member_arity(callee->n0->name, callee->name);
            if (expected == DS_BUILTIN_UNKNOWN) {
                ds_vm_set_error(vm, callee->line, callee->column, "нет такой функции %s.%s",
                                callee->n0->name, callee->name);
                return 0;
            }
            if (expected == DS_BUILTIN_VALUE) {
                ds_vm_set_error(vm, callee->line, callee->column, "%s.%s — значение, его нельзя вызвать",
                                callee->n0->name, callee->name);
                return 0;
            }
            if (expected >= 0 && expected != node->child_count) {
                ds_vm_set_error(vm, node->line, node->column, "%s.%s ожидает %d аргументов, получено %d",
                                callee->n0->name, callee->name, expected, node->child_count);
                return 0;
            }
        } else if (!resolve_expression(vm, function, callee->n0)) {
            return 0;
        }
    } else if (!resolve_expression(vm, function, callee)) {
        return 0;
    }
    for (int index = 0; index < node->child_count; ++index)
        if (!resolve_expression(vm, function, node->children[index])) return 0;
    return 1;
}

static int resolve_expression(DsVM *vm, DsFunction *function, DsNode *node) {
    if (!node) return 1;
    switch (node->kind) {
    case N_NAME:
        return resolve_name(vm, function, node);
    case N_MEMBER:
        if (!resolve_expression(vm, function, node->n0)) return 0;
        if (node->n0 && node->n0->kind == N_NEW) {
            ds_vm_set_error(vm, node->line, node->column,
                            "у new нельзя взять поле: сохраните объект в переменную");
            return 0;
        }
        return 1;
    case N_INDEX:
        if (!resolve_expression(vm, function, node->n0)) return 0;
        return resolve_expression(vm, function, node->n1);
    case N_LISTLIT:
        for (int index = 0; index < node->child_count; ++index)
            if (!resolve_expression(vm, function, node->children[index])) return 0;
        return 1;
    case N_NEW: {
        DsStruct *type = ds_find_struct(vm, node->name);
        if (!type) {
            ds_vm_set_error(vm, node->line, node->column, "неизвестная структура '%s'", node->name);
            return 0;
        }
        node->struct_index = (int)(type - vm->structs);
        return 1;
    }
    case N_UNARY:
        return resolve_expression(vm, function, node->n0);
    case N_BINARY:
        if (!resolve_expression(vm, function, node->n0)) return 0;
        return resolve_expression(vm, function, node->n1);
    case N_CALL:
        return resolve_call(vm, function, node);
    case S_ASSIGN: {
        DsNode *target = node->n0;
        if (target->kind == N_NAME) {
            if (function) {
                const int global = ds_find_global(vm, target->name);
                if (node->flags && global >= 0) {
                    ds_vm_set_error(vm, node->line, node->column,
                                    "'%s' уже глобальная переменная, local не нужен", target->name);
                    return 0;
                }
                if (global >= 0 && !node->flags) {
                    target->name_kind = DS_NAME_GLOBAL;
                    target->slot = global;
                } else {
                    const int slot = add_local(function, target->name);
                    if (slot < 0) {
                        ds_vm_set_error(vm, node->line, node->column,
                                        "слишком много локальных переменных (максимум %d)",
                                        DS_MAX_LOCALS);
                        return 0;
                    }
                    target->name_kind = DS_NAME_LOCAL;
                    target->slot = slot;
                }
            } else {
                /* Top level: the declaration was registered before resolution. */
                target->name_kind = DS_NAME_GLOBAL;
            }
        } else if (!resolve_expression(vm, function, target)) {
            return 0;
        }
        return resolve_expression(vm, function, node->n1);
    }
    case S_EXPR:
        return resolve_expression(vm, function, node->n0);
    default:
        return 1;
    }
}

static int resolve_block(DsVM *vm, DsFunction *function, DsNode **statements, int count) {
    for (int index = 0; index < count; ++index) {
        DsNode *node = statements[index];
        switch (node->kind) {
        case S_ASSIGN:
        case S_EXPR:
            if (!resolve_expression(vm, function, node)) return 0;
            break;
        case S_DELETE:
        case S_RETURN:
            if (!resolve_expression(vm, function, node->n0)) return 0;
            break;
        case S_IF:
            if (!resolve_expression(vm, function, node->n0)) return 0;
            if (!resolve_block(vm, function, node->children, node->child_count)) return 0;
            if (!resolve_block(vm, function, node->tail, node->tail_count)) return 0;
            break;
        case S_WHILE:
            if (!resolve_expression(vm, function, node->n0)) return 0;
            if (!resolve_block(vm, function, node->children, node->child_count)) return 0;
            break;
        case S_FOR: {
            if (function) {
                int slot = -1;
                for (int parameter = 0; parameter < function->arity; ++parameter)
                    if (!strcmp(function->params[parameter], node->name)) slot = parameter;
                if (slot < 0) slot = add_local(function, node->name);
                if (slot < 0) {
                    ds_vm_set_error(vm, node->line, node->column,
                                    "слишком много локальных переменных (максимум %d)", DS_MAX_LOCALS);
                    return 0;
                }
                node->slot = slot;
            }
            if (!resolve_expression(vm, function, node->n0)) return 0;
            if (!resolve_expression(vm, function, node->n1)) return 0;
            if (!resolve_expression(vm, function, node->n2)) return 0;
            if (!resolve_block(vm, function, node->children, node->child_count)) return 0;
            break;
        }
        default:
            break;
        }
    }
    return 1;
}

static int resolve_type_name(DsVM *vm, const char *name, DsFieldType *type, int *struct_index,
                             int line, int column) {
    *struct_index = -1;
    *type = DS_FT_ANY;
    if (!name || !name[0]) return 1;
    if (!strcmp(name, "int")) { *type = DS_FT_INT; return 1; }
    if (!strcmp(name, "float")) { *type = DS_FT_FLOAT; return 1; }
    if (!strcmp(name, "string") || !strcmp(name, "str")) { *type = DS_FT_STRING; return 1; }
    if (!strcmp(name, "bool") || !strcmp(name, "boolean")) { *type = DS_FT_BOOL; return 1; }
    if (!strcmp(name, "list")) { *type = DS_FT_LIST; return 1; }
    if (!strcmp(name, "void")) { *type = DS_FT_ANY; return 1; }
    for (int index = 0; index < vm->struct_count; ++index) {
        if (!strcmp(vm->structs[index].name, name)) {
            *type = DS_FT_STRUCT;
            *struct_index = index;
            return 1;
        }
    }
    ds_vm_set_error(vm, line, column, "неизвестный тип '%s'", name);
    return 0;
}

int ds_vm_link_program(DsVM *vm) {
    int visited[DS_MAX_SCRIPTS];
    int order[DS_MAX_SCRIPTS];
    int order_count = 0;
    memset(visited, 0, sizeof(visited));

    /* `main` first, then everything else; requires are resolved depth first so a
     * module never sees a struct that has not been registered yet. */
    int starts[DS_MAX_SCRIPTS];
    int start_count = 0;
    for (int index = 0; index < vm->script_count; ++index)
        if (!strcmp(vm->scripts[index].name, "main")) starts[start_count++] = index;
    for (int index = 0; index < vm->script_count; ++index) {
        int duplicate = 0;
        for (int existing = 0; existing < start_count; ++existing)
            if (starts[existing] == index) duplicate = 1;
        if (!duplicate) starts[start_count++] = index;
    }

    for (int cursor = 0; cursor < start_count; ++cursor) {
        const int root = starts[cursor];
        if (visited[root]) continue;
        int stack[DS_MAX_SCRIPTS];
        int depth = 0;
        stack[depth++] = root;
        while (depth > 0) {
            const int script_index = stack[depth - 1];
            DsScript *script = &vm->scripts[script_index];
            int pending = -1;
            for (int index = 0; index < script->require_count && pending < 0; ++index) {
                const DsNode *require = script->requires[index];
                int found = -1;
                for (int candidate = 0; candidate < vm->script_count; ++candidate)
                    if (!strcmp(vm->scripts[candidate].name, require->text)) found = candidate;
                if (found < 0) {
                    ds_vm_set_error(vm, require->line, require->column,
                                    "require \"%s\": нет файла %s.ds в папке игры", require->text,
                                    require->text);
                    return 0;
                }
                if (!visited[found]) pending = found;
            }
            if (pending >= 0) {
                if (depth >= DS_MAX_SCRIPTS) {
                    ds_vm_set_error(vm, 0, 0, "слишком длинная цепочка require");
                    return 0;
                }
                stack[depth++] = pending;
                continue;
            }
            --depth;
            if (!visited[script_index]) {
                visited[script_index] = 1;
                order[order_count++] = script_index;
            }
        }
    }

    /* Register declarations in dependency order. */
    for (int position = 0; position < order_count; ++position) {
        DsScript *script = &vm->scripts[order[position]];
        for (int index = 0; index < script->struct_count; ++index) {
            DsStruct *type = script->structs[index];
            if (ds_find_struct(vm, type->name)) {
                ds_vm_set_error(vm, 0, 0, "struct '%s' объявлена дважды", type->name);
                return 0;
            }
            if (vm->struct_count >= DS_MAX_STRUCTS) {
                ds_vm_set_error(vm, 0, 0, "слишком много struct (максимум %d)", DS_MAX_STRUCTS);
                return 0;
            }
            vm->structs[vm->struct_count++] = *type;
        }
        for (int index = 0; index < script->function_count; ++index) {
            DsFunction *function = script->functions[index];
            if (ds_find_function(vm, function->name)) {
                ds_vm_set_error(vm, function->line, 1, "функция '%s' объявлена дважды", function->name);
                return 0;
            }
            if (vm->function_count >= DS_MAX_FUNCTIONS) {
                ds_vm_set_error(vm, function->line, 1, "слишком много функций (максимум %d)",
                                DS_MAX_FUNCTIONS);
                return 0;
            }
            const int arity = callback_arity(function->name);
            if (arity >= 0 && function->arity != arity) {
                ds_vm_set_error(vm, function->line, 1, "callback '%s' ожидает %d параметров, получено %d",
                                function->name, arity, function->arity);
                return 0;
            }
            vm->functions[vm->function_count++] = function;
        }
    }

    /* Globals are registered after every struct and function name exists. */
    for (int position = 0; position < order_count; ++position) {
        DsScript *script = &vm->scripts[order[position]];
        for (int index = 0; index < script->global_count; ++index) {
            DsNode *node = script->globals[index];
            if (node->n1->kind == N_NEW) {
                DsStruct *type = ds_find_struct(vm, node->n1->name);
                if (!type) {
                    ds_vm_set_error(vm, node->line, node->column, "неизвестная структура '%s'",
                                    node->n1->name);
                    return 0;
                }
                node->n1->struct_index = (int)(type - vm->structs);
            }
            if (!resolve_expression(vm, NULL, node->n1)) return 0;
            const int slot = register_global(vm, node->n0->name, node->n1);
            if (slot < 0) return 0;
            node->slot = slot;
            node->n0->name_kind = DS_NAME_GLOBAL;
            node->n0->slot = slot;
        }
    }

    /* Resolve struct field types and parameter types once all names exist. */
    for (int index = 0; index < vm->struct_count; ++index) {
        DsStruct *type = &vm->structs[index];
        for (int field = 0; field < type->count; ++field) {
            DsFieldType kind;
            int struct_index;
            if (!resolve_type_name(vm, type->fields[field].type_name, &kind, &struct_index, 0, 0))
                return 0;
            if (kind == DS_FT_ANY) {
                ds_vm_set_error(vm, 0, 0, "поле '%s' в struct '%s' должно иметь тип int/float/string/bool/list",
                                type->fields[field].name, type->name);
                return 0;
            }
            type->fields[field].type = kind;
            type->fields[field].struct_index = struct_index;
        }
    }
    for (int index = 0; index < vm->function_count; ++index) {
        DsFunction *function = vm->functions[index];
        function->local_count = function->arity;
        for (int parameter = 0; parameter < function->arity; ++parameter) {
            DsFieldType kind;
            int struct_index;
            if (!resolve_type_name(vm, function->param_types[parameter], &kind, &struct_index,
                                   function->line, 1))
                return 0;
            function->param_kinds[parameter] = kind;
            function->locals[parameter] = function->params[parameter];
        }
        /* A written return type must name a known type.  The interpreter keeps
         * values dynamically typed; the ahead-of-time compiler is where the
         * annotation becomes a real C return type. */
        {
            DsFieldType unused_kind = DS_FT_ANY;
            int unused_struct = -1;
            if (!resolve_type_name(vm, function->return_type, &unused_kind, &unused_struct,
                                   function->line, 1))
                return 0;
        }
        if (!resolve_block(vm, function, function->body, function->body_count)) return 0;
    }
    return 1;
}
