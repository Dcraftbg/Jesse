#include <stdio.h>
#include <stdlib.h>
#include <ctype.h>
#include <assert.h>
#include "arena.h"
#include "scratch.h"
#include <darray.h>

#include "fileutils.h"
#include "atom.h"
#include "utf8.h"
#include "todo.h"
#include "jsvm.h"

typedef struct {
    const char *path;
    const char *cursor, *end;
    size_t l, c;
    AtomTable* atom_table;
    size_t str_buffer_head, str_buffer_cap;
    char* str_buffer;
} JsLexer;
enum {
    JSERR_INVALID_STRING=1,
    JSERR_INVALID_CHAR_IN_STRING,
    JSERR_EOF,
    JSERR_COUNT
};
enum {
    JSTOKEN_ATOM=256,
    JSTOKEN_STR,
    JSTOKEN_COUNT
};
typedef struct {
    const char* path;
    size_t l0, c0;
    size_t l1, c1;
    int kind;
    union {
        Atom* atom;
        struct {
            const char* data;
            size_t len;
        } str;
    } as;
} JsToken;
static uint32_t js_lexer_peak_char_n(JsLexer* lexer, size_t n) {
    const char* strm = lexer->cursor;
    uint32_t c;
    do {
        c = utf8_next(&strm, lexer->end);
    } while(n--);
    return c;
}
static uint32_t js_lexer_peak_char(JsLexer* lexer) {
    return js_lexer_peak_char_n(lexer, 0);
}
static uint32_t js_lexer_next_char(JsLexer* lexer) {
    uint32_t res = utf8_next(&lexer->cursor, lexer->end);
    lexer->c++;
    if (res == '\n') {
        lexer->l++;
        lexer->c = 0;
    }
    return res;
}
static void js_lexer_trim(JsLexer* lexer) {
    while (lexer->cursor < lexer->end && isspace(js_lexer_peak_char(lexer))) js_lexer_next_char(lexer);
}
static bool iswordc(uint32_t codepoint) {
    return codepoint == '_' || isalnum(codepoint);
}
static int jsparse_str(JsLexer* lexer, ScratchBuf* scratch) {
    int chr;
    bool escape = false;
    while(lexer->cursor < lexer->end) {
        chr = js_lexer_next_char(lexer);
        if(chr == '\n') return -JSERR_INVALID_STRING;
        if(chr == '"' && !escape) break;
        if(escape) {
            switch(chr) {
            case 't':
                scratchbuf_push(scratch, '\t');
                break;
            case 'n':
                scratchbuf_push(scratch, '\n');
                break;
            case 'r':
                scratchbuf_push(scratch, '\r');
                break;
            case '0':
                scratchbuf_push(scratch, '\0');
                break;
            default:
                if(chr >= 256) return -JSERR_INVALID_CHAR_IN_STRING;
                scratchbuf_push(scratch, chr);
                break;
            }
        } else {
            if(chr == '\\') escape = true;
            else if(chr >= 256) return -JSERR_INVALID_CHAR_IN_STRING;
            else scratchbuf_push(scratch, chr);
        }
    }
    if(lexer->cursor >= lexer->end) return -JSERR_INVALID_STRING;
    return 0;
}
#define MAKE_TOKEN(...) (JsToken) { lexer->path, l0, c0, lexer->l, lexer->c, .kind=__VA_ARGS__ }
static char* str_alloc(JsLexer* lexer, const char* data, size_t n) {
    if(lexer->str_buffer_head + n > lexer->str_buffer_cap) return NULL;
    char* buffer = lexer->str_buffer + lexer->str_buffer_head;
    lexer->str_buffer_head += n;
    memcpy(buffer, data, n);
    return buffer;
}
JsToken js_lexer_next(JsLexer* lexer) {
    js_lexer_trim(lexer);
    size_t l0 = lexer->l, c0 = lexer->c;
    if(lexer->cursor >= lexer->end) return MAKE_TOKEN(-JSERR_EOF);
    int chr;
    switch(chr=js_lexer_peak_char(lexer)) {
    case '.':
    case '(':
    case ')':
    case '+':
    case '-':
    case '*':
    case '/':
    case ',':
        js_lexer_next_char(lexer);
        return MAKE_TOKEN(chr);
    case '"': {
        ScratchBuf scratch;
        scratchbuf_init(&scratch);
        js_lexer_next_char(lexer);
        int e = jsparse_str(lexer, &scratch);
        if(e < 0) {
            scratchbuf_cleanup(&scratch);
            return MAKE_TOKEN(e);
        }
        char* str = str_alloc(lexer, scratch.data, scratch.len);
        size_t len = scratch.len;
        scratchbuf_cleanup(&scratch);
        return MAKE_TOKEN(JSTOKEN_STR, .as = { .str = { str, len }});
    } break;
    default:
        if(isalpha(chr) || chr == '_') {
            const char* start = lexer->cursor;
            while (lexer->cursor < lexer->end && iswordc(js_lexer_peak_char(lexer))) js_lexer_next_char(lexer);
            Atom* atom = atom_table_get(lexer->atom_table, start, lexer->cursor-start);
            if(!atom) {
                atom = atom_new(start, lexer->cursor-start);
                assert(atom && "Just buy more RAM");
                atom_table_insert(lexer->atom_table, atom);
            }
            return MAKE_TOKEN(JSTOKEN_ATOM, .as = { .atom = atom });
        }
        break;
    }
    fprintf(stderr, "TBD: parse `%c`\n", chr);
    abort();
}
typedef struct {
    size_t l, c;
    const char* cursor;
    size_t str_buffer_head;
} JsSnapshot;
JsSnapshot js_lexer_snap_take(JsLexer* lexer) {
    return (JsSnapshot) {
        lexer->l, lexer->c,
        lexer->cursor,
        lexer->str_buffer_head
    };
}
void js_lexer_snap_restore(JsLexer* lexer, const JsSnapshot* snapshot) {
    lexer->l = snapshot->l;
    lexer->c = snapshot->c;
    lexer->cursor = snapshot->cursor;
    lexer->str_buffer_head = snapshot->str_buffer_head;
}
JsToken js_lexer_peak(JsLexer* lexer, size_t ahead) {
    JsSnapshot snap = js_lexer_snap_take(lexer);
    JsToken t={0};
    do {
        t = js_lexer_next(lexer);
    } while(ahead--);
    js_lexer_snap_restore(lexer, &snap);
    return t;
}
static JsToken js_lexer_peak_next(JsLexer* lexer) {
    return js_lexer_peak(lexer, 0);
}
void js_lexer_new(JsLexer* lexer, const char* path, const char* src, const char* end, AtomTable* atom_table, char* str_buffer, size_t str_buffer_cap) {
    lexer->cursor = src;
    lexer->end = end;
    lexer->l = 1;
    lexer->c = 0;
    lexer->path = path;
    lexer->atom_table = atom_table;
    lexer->str_buffer = str_buffer;
    lexer->str_buffer_head = 0;
    lexer->str_buffer_cap = str_buffer_cap;
}
void js_token_dump(FILE* sink, JsToken* t) {
    switch(t->kind) {
    case JSTOKEN_ATOM:
        fprintf(sink, "%s", t->as.atom->data);
        break;
    case JSTOKEN_STR:
        fprintf(sink, "\"%.*s\"", (int)t->as.str.len, t->as.str.data);
        break;
    default:
        if(t->kind < 0) {
            // TODO: proper error logging with a 
            // strtab
            fprintf(sink, "ERROR(%d)", -t->kind);
        }
        else if(t->kind < 256) fprintf(sink, "%c", t->kind);
        else fprintf(sink, "Token(%d)", t->kind);
        break;
    }
}
void dump_all_tokens(JsLexer* lexer) {
    JsToken t;
    while((t=js_lexer_next(lexer)).kind >= 0) {
        js_token_dump(stdout, &t);
        printf("\n");
    }
}
enum {
    JSAST_STRING,
    JSAST_BINOP,
    JSAST_ATOM,
    JSAST_CALL,
    JSAST_COUNT
};
typedef struct JsAST JsAST;
typedef struct {
    JsAST** items;
    size_t len, cap;
} JsCallArgs;
static void js_call_args_dealloc(JsCallArgs* args) {
    free(args->items);
}
struct JsAST {
    // size_t l0, c0, l1, c1;
    int kind;
    union {
        Atom* atom;
        struct { int op; JsAST *lhs, *rhs; } binop;
        struct { const char* data; size_t len; } str;
        struct { JsAST* what; JsCallArgs args; } call;
    } as;
};
JsAST* js_ast_new_binop(Arena* arena, int op, JsAST* lhs, JsAST* rhs) {
    JsAST* ast = arena_alloc(arena, sizeof(*ast));
    if(!ast) return NULL;
    ast->kind = JSAST_BINOP;
    ast->as.binop.op = op;
    ast->as.binop.lhs = lhs;
    ast->as.binop.rhs = rhs;
    return ast;
}
JsAST* js_ast_new_str(Arena* arena, const char* data, size_t len) {
    JsAST* ast = arena_alloc(arena, sizeof(*ast));
    if(!ast) return NULL;
    ast->kind = JSAST_STRING;
    ast->as.str.data = data;
    ast->as.str.len = len;
    return ast;
}
JsAST* js_ast_new_atom(Arena* arena, Atom* atom) {
    JsAST* ast = arena_alloc(arena, sizeof(*ast));
    if(!ast) return NULL;
    ast->kind = JSAST_ATOM;
    ast->as.atom = atom;
    return ast;
}
JsAST* js_ast_new_call(Arena* arena, JsAST* what, JsCallArgs args) {
    JsAST* ast = arena_alloc(arena, sizeof(*ast));
    if(!ast) return NULL;
    ast->kind = JSAST_CALL;
    ast->as.call.what = what;
    ast->as.call.args = args;
    return ast;
}
JsAST* js_parse_basic(JsLexer* l, Arena* arena) {
    (void)arena;
    JsToken t = js_lexer_next(l);
    switch(t.kind) {
    case JSTOKEN_STR:
        return js_ast_new_str(arena, t.as.str.data, t.as.str.len);
    case JSTOKEN_ATOM:
        return js_ast_new_atom(arena, t.as.atom);
    }
    fprintf(stderr, "JS:ERROR Unexpected token: ");
    js_token_dump(stderr, &t);
    fprintf(stderr, "\n");
    return NULL;
}
void js_ast_dump(FILE* sink, JsAST* ast) {
    static_assert(JSAST_COUNT == 4, "Update js_ast_dump");
    switch(ast->kind) {
    case JSAST_ATOM:
        fprintf(sink, "%s", ast->as.atom->data);
        break;
    case JSAST_STRING:
        fprintf(sink, "\"%.*s\"", (int)ast->as.str.len, ast->as.str.data);
        break;
    case JSAST_CALL: {
        js_ast_dump(sink, ast->as.call.what);
        fprintf(sink, " (");
        JsCallArgs* args = &ast->as.call.args;
        for(size_t i = 0; i < args->len; ++i) {
            if(i > 0) fprintf(sink, ", ");    
            js_ast_dump(sink, args->items[i]);
        }
        fprintf(sink, ")");
    } break;
    case JSAST_BINOP:
        fprintf(sink, "(");
        js_ast_dump(sink, ast->as.binop.lhs);
        fprintf(sink, " ");
        if(ast->as.binop.op < 256) fprintf(sink, "%c", ast->as.binop.op);
        else fprintf(sink, "<OP:%04X>", ast->as.binop.op);
        fprintf(sink, " ");
        js_ast_dump(sink, ast->as.binop.rhs);
        fprintf(sink, ")");
        break;
    }
}
#define JS_INIT_PRECEDENCE 100
#define JS_BINOPS \
    X('.') \
    X('+') \
    X('-') \
    X('*') \
    X('/')

// TODO: Use the actual JS precedence from here
// https://en.cppreference.com/w/cpp/language/operator_precedence
int js_binop_prec(int op) {
    switch(op) {
    case '.':
        return 2;
    case '*':
    case '/':
        return 5;
    case '+':
    case '-':
        return 6;
    default:
        todof("op=%d",op);
        return -1;
    }
}

JsAST* js_parse_ast(JsLexer* l, Arena* arena, int expr_precedence);
JsAST* js_parse_astcall(JsLexer* l, Arena* arena, JsAST* what) {
    JsToken t;
    if((t=js_lexer_next(l)).kind != '(') {
        fprintf(stderr, "JS:ERROR Expected '(' in function call\n");
        return NULL;
    }
    JsCallArgs args = {0};
    for(;;) {
        t = js_lexer_peak_next(l);
        if(t.kind == ')') break;
        JsAST* value = js_parse_ast(l, arena, JS_INIT_PRECEDENCE);
        if(!value) {
            js_call_args_dealloc(&args);
            return NULL;
        }
        da_push(&args, value);
        t = js_lexer_peak_next(l);
        if(t.kind == ')') break;
        else if (t.kind == ',') js_lexer_next(l);
        else {
            // TODO: error reporting (path:line:chr)
            fprintf(stderr, "JS:ERROR Expected ')' or ',' in function call but found other\n");
            js_call_args_dealloc(&args);
            return NULL;
        }
    } 
    if((t=js_lexer_next(l)).kind != ')') {
        fprintf(stderr, "JS:ERROR Expected ')' in function call\n");
        js_call_args_dealloc(&args);
        return NULL;
    }
    return js_ast_new_call(arena, what, args);
}
JsAST* js_parse_ast(JsLexer* l, Arena* arena, int expr_precedence) {
    JsToken t;
    JsAST* v = js_parse_basic(l, arena);
    if(!v) return NULL;
    for(;;) {
        t = js_lexer_peak_next(l);
        switch(t.kind) {
        case '(': {
            if(2 > expr_precedence) return v;
            v = js_parse_astcall(l, arena, v);
        } break;
        #define X(op) case op:
        JS_BINOPS
        #undef X
        {
            int binop = t.kind;
            int bin_precedence = js_binop_prec(binop);
            if(bin_precedence > expr_precedence) return v;
            js_lexer_next(l);
            JsSnapshot snap = js_lexer_snap_take(l);
            JsAST* v2 = js_parse_basic(l, arena);
            if(!v2) return NULL;
            t = js_lexer_peak_next(l);
            int next_prec = -1;
            int next_op = 0;
            switch(t.kind) {
            #define X(op) case op:
            JS_BINOPS
            #undef X
                next_prec = js_binop_prec(next_op = t.kind);
                break;
            case '(':
                next_prec = 2;
                break;
            }
            if (bin_precedence > next_prec) {
                js_lexer_snap_restore(l, &snap);
                v2 = js_parse_ast(l, arena, bin_precedence);
            }
            v = js_ast_new_binop(arena, binop, v, v2);
        } break;
        default:
            return v;
        }
    }
    return v;
}
enum {
    JSSTATEMENT_EVAL,
};
typedef struct {
    int kind;
    union {
        JsAST* ast;
    } as;
} JsStatement;
JsStatement* js_statement_new_eval(Arena* arena, JsAST* ast) {
    JsStatement* stmt = arena_alloc(arena, sizeof(*stmt));
    if(!stmt) return NULL;
    stmt->kind = JSSTATEMENT_EVAL;
    stmt->as.ast = ast;
    return stmt;
}
JsStatement* js_parse_statement(JsLexer* l, Arena* arena) {
    JsAST* ast = js_parse_ast(l, arena, JS_INIT_PRECEDENCE);
    if(!ast) return NULL;
    return js_statement_new_eval(arena, ast);
}
typedef struct {
    JsStatement** items;
    size_t len, cap;
} JsStatements;
typedef struct {
    JsVmInstruction* items;
    size_t len, cap;
} JsVmInstructions;
void js_compile_ast(JsVmInstructions* insts, JsAST* ast) {
    (void)insts;
    static_assert(JSAST_COUNT == 4, "Update js_compile_ast");
    switch(ast->kind) {
    case JSAST_ATOM: {
        // TODO: locals :)
        JsVmInstruction inst = {
            .kind = JSVM_GET_GLOBAL,
            .as = {
                .atom = ast->as.atom
            }
        };
        da_push(insts, inst);
    } break;
    case JSAST_BINOP: {
        switch(ast->as.binop.op) {
        case '.': {
            assert(ast->as.binop.rhs->kind == JSAST_ATOM);
            js_compile_ast(insts, ast->as.binop.lhs);
            JsVmInstruction inst = {
                .kind = JSVM_GET_MEMBER,
                .as.atom = ast->as.binop.rhs->as.atom
            };
            da_push(insts, inst);
        } break;
        default:
            todof("js_compile_ast binop=%c", ast->as.binop.op);
        }
    } break;
    case JSAST_CALL: {
        for(size_t i = ast->as.call.args.len; i > 0; --i) {
            js_compile_ast(insts, ast->as.call.args.items[i-1]);
        }
        if(ast->as.call.what->kind == JSAST_BINOP && ast->as.call.what->as.binop.op == '.') {
            js_compile_ast(insts, ast->as.call.what->as.binop.lhs);
            da_push(insts, ((JsVmInstruction) {
                .kind = JSVM_DUP,
            }));
            da_push(insts, ((JsVmInstruction) {
                .kind = JSVM_GET_MEMBER,
                .as.atom = ast->as.call.what->as.binop.rhs->as.atom
            }));
            JsVmInstruction inst = {
                .kind = JSVM_CALL,
                .as.call.num_args = ast->as.call.args.len,
            };
            da_push(insts, inst);
            break;
        }
        da_push(insts, ((JsVmInstruction) {
            .kind = JSVM_THIS
        }));
        js_compile_ast(insts, ast->as.call.what);
        JsVmInstruction inst = {
            .kind = JSVM_CALL,
            .as.call.num_args = ast->as.call.args.len,
        };
        da_push(insts, inst);
    } break;
    case JSAST_STRING: {
        JsVmInstruction inst = {
            .kind = JSVM_PUSH_STR,
            .as.push_str = {
                .data = ast->as.str.data,
                .len = ast->as.str.len,
            }
        };
        da_push(insts, inst);
    } break;
    default:
        todof("js_compile_ast(%d)\n", ast->kind);
    }
}
// JS runtime
static void jsruntime_console_log(JsVmValue*, JsVmValue*, JsVmStack* stack, size_t num_args) {
    for(size_t i = 0; i < num_args; ++i) {
        if(i > 0) printf(" ");
        assert(stack->len > 0);
        JsVmValue arg = da_pop(stack);
        static_assert(JSVM_VALUE_COUNT == 4, "Update jsruntime_console_log");
        switch(arg.kind) {
        case JSVM_VALUE_UNDEFINED:
            printf("undefined");
            break;
        case JSVM_VALUE_FUNC:
            printf("<Function: #%08llx>", (unsigned long long)arg.as.func.func);
            break;
        case JSVM_VALUE_OBJECT:
            jsvm_dump_value(stdout, &arg);
            break;
        case JSVM_VALUE_STRING:
            for(size_t i = 0; i < arg.as.string.len; ++i) {
                char c = arg.as.string.items[i];
                if(isprint(c)) printf("%c", c);
                else printf("\\x%02X", c);
            }
            break;
        }
    }
    printf("\n");
}
static void jsruntime_console_toString(JsVmValue*, JsVmValue*, JsVmStack* stack, size_t) {
    JsVmValue value = {
        .kind = JSVM_VALUE_STRING,
        .as.string = { 0 }
    };
    da_reserve(&value.as.string, 256);
    value.as.string.len += snprintf(value.as.string.items, value.as.string.cap, "[object console]");
    da_push(
        stack,
        value
    );
}
const char* shift_args(int *argc, char ***argv) {
    if((*argc) <= 0) return NULL;
    return ((*argc)--, *((*argv)++));
}
void help(FILE* sink, const char* exe) {
    fprintf(sink, "%s ... (input path) ...\n", exe);
}
int main(int argc, char** argv) {
    (void)argc;
    (void)argv;
    size_t size;
    const char* path = NULL;
    const char* exe = shift_args(&argc, &argv);
    assert(exe);
    while(argc) {
        const char* arg = shift_args(&argc, &argv);
        if(!path) path = arg;
        else {
            fprintf(stderr, "Unexpected argument `%s`\n", arg);
            help(stderr, exe);
            return 1;
        }
    }
    if(!path) {
        fprintf(stderr, "Missing input path!\n");
        help(stderr, exe);
        return 1;
    }

    const char* content = read_entire_file(path, &size);
    if(!content) return 1;
    JsLexer lexer = { 0 };
    Arena arena = { 0 };
    AtomTable atom_table = { 0 };
    static char str_buffer[4096*4]; 
    js_lexer_new(&lexer, path, content, content + size, &atom_table, str_buffer, sizeof(str_buffer));
    JsToken t;
    size_t errors = 0;
    JsStatements statements = { 0 };
    while((t=js_lexer_peak_next(&lexer)).kind >= 0) {
        switch(t.kind) {
        case ';':
            js_lexer_next(&lexer);
            break;
        default: {
            JsStatement* stmt = js_parse_statement(&lexer, &arena);
            if(!stmt) {
                errors++;
                continue;
            }
            da_push(&statements, stmt);
        } break;
        }
    }
    if(t.kind != -JSERR_EOF) {
        fprintf(stderr, "JS:ERROR Lexing: ");
        js_token_dump(stderr, &t);
        fprintf(stderr, "\n");
        errors++;
    }
    if(errors) return 1;
    JsVmInstructions insts = { 0 };
    for(size_t i = 0; i < statements.len; ++i) {
        JsStatement* stmt = statements.items[i];
        switch(stmt->kind) {
        case JSSTATEMENT_EVAL:
            js_compile_ast(&insts, stmt->as.ast);
            break;
        }
    }
    JsVmStack stack = { 0 };
    JsVmObject globals = { 0 };
    {
        JsVmObject* console = malloc(sizeof(*console));
        assert(console && "Just buy more RAM");
        memset(console, 0, sizeof(*console));

        jsvm_object_insert(console,
            atom_table_get_or_insert_new_cstr(&atom_table, "log"),
            (JsVmValue) {
                .kind = JSVM_VALUE_FUNC,
                .as.func.func = jsruntime_console_log
            }
        );
        jsvm_object_insert(console,
            atom_table_get_or_insert_new_cstr(&atom_table, "toString"),
            (JsVmValue) {
                .kind = JSVM_VALUE_FUNC,
                .as.func.func = jsruntime_console_toString,
            }
        );
        jsvm_object_insert(&globals, 
            atom_table_get_or_insert_new_cstr(&atom_table, "console"),
            (JsVmValue) {
                .kind = JSVM_VALUE_OBJECT,
                .as.object = console
            }
        );
    }
    for(size_t i = 0; i < insts.len; ++i) {
        jsvm_interpret(&globals, &stack, &insts.items[i]);
    }
    return 0;
}
