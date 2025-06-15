#include <stdio.h>
#include <stdlib.h>
#include <ctype.h>
#include <assert.h>
#include "scratch.h"

#include "fileutils.h"
#include "atom.h"
#include "utf8.h"

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
static uint32_t jslexer_peak_char_n(JsLexer* lexer, size_t n) {
    const char* strm = lexer->cursor;
    uint32_t c;
    do {
        c = utf8_next(&strm, lexer->end);
    } while(n--);
    return c;
}
static uint32_t jslexer_peak_char(JsLexer* lexer) {
    return jslexer_peak_char_n(lexer, 0);
}
static uint32_t jslexer_next_char(JsLexer* lexer) {
    uint32_t res = utf8_next(&lexer->cursor, lexer->end);
    lexer->c++;
    if (res == '\n') {
        lexer->l++;
        lexer->c = 0;
    }
    return res;
}
static void jslexer_trim(JsLexer* lexer) {
    while (lexer->cursor < lexer->end && isspace(jslexer_peak_char(lexer))) jslexer_next_char(lexer);
}
static bool iswordc(uint32_t codepoint) {
    return codepoint == '_' || isalnum(codepoint);
}
static int jsparse_str(JsLexer* lexer, ScratchBuf* scratch) {
    int chr;
    bool escape = false;
    while(lexer->cursor < lexer->end) {
        chr = jslexer_next_char(lexer);
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
JsToken jstoken_next(JsLexer* lexer) {
    jslexer_trim(lexer);
    size_t l0 = lexer->l, c0 = lexer->c;
    if(lexer->cursor >= lexer->end) return MAKE_TOKEN(-JSERR_EOF);
    int chr;
    switch(chr=jslexer_peak_char(lexer)) {
    case '.':
    case '(':
    case ')':
        jslexer_next_char(lexer);
        return MAKE_TOKEN(chr);
    case '"': {
        ScratchBuf scratch;
        scratchbuf_init(&scratch);
        jslexer_next_char(lexer);
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
            while (lexer->cursor < lexer->end && iswordc(jslexer_peak_char(lexer))) jslexer_next_char(lexer);
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
void jslexer_new(JsLexer* lexer, const char* path, const char* src, const char* end, AtomTable* atom_table, char* str_buffer, size_t str_buffer_cap) {
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
void jstoken_dump(FILE* sink, JsToken* t) {
    switch(t->kind) {
    case JSTOKEN_ATOM:
        fprintf(sink, "%s", t->as.atom->data);
        break;
    case JSTOKEN_STR:
        fprintf(sink, "\"%.*s\"", (int)t->as.str.len, t->as.str.data);
        break;
    default:
        if(t->kind < 256) fprintf(sink, "%c", t->kind);
        else fprintf(sink, "Token(%d)", t->kind);
        break;
    }
}
void dump_all_tokens(JsLexer* lexer) {
    JsToken t;
    while((t=jstoken_next(lexer)).kind >= 0) {
        jstoken_dump(stdout, &t);
        printf("\n");
    }
}
int main(int argc, char** argv) {
    (void)argc;
    (void)argv;
    size_t size;
    const char* path = "./example.js";
    const char* content = read_entire_file(path, &size);
    if(!content) return 1;
    JsLexer lexer = { 0 };
    AtomTable atom_table = { 0 };
    static char str_buffer[4096*4]; 
    jslexer_new(&lexer, path, content, content + size, &atom_table, str_buffer, sizeof(str_buffer));
    dump_all_tokens(&lexer);
    return 0;
}
