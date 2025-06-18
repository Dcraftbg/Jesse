#pragma once
#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>
typedef struct Atom Atom;
enum {
    JSVM_GET_GLOBAL,
    JSVM_GET_MEMBER,
    JSVM_PUSH_STR,
    JSVM_CALL,
    JSVM_INST_COUNT
};
typedef struct {
    uint8_t kind;
    union {
        Atom* atom;
        struct { const char* data; size_t len; } push_str;
        struct { size_t num_args; } call;
    } as;
} JsVmInstruction;
typedef struct JsVmString JsVmString;
typedef struct JsVmObject JsVmObject; 
typedef struct JsVmValue JsVmValue;
typedef struct JsVmStack JsVmStack;
// TODO: this is technically invalid.
// In javascript strings are UTF-32
// i.e. just array of codepoints.
// Fucking dumbasses....
struct JsVmString {
    char* items;
    size_t len, cap;
};
enum {
    JSVM_VALUE_STRING,
    JSVM_VALUE_OBJECT,
    JSVM_VALUE_FUNC,
    JSVM_VALUE_UNDEFINED,
    JSVM_VALUE_COUNT
};
struct JsVmValue {
    uint8_t kind;
    union {
        JsVmObject* object;
        JsVmString string;
        struct {
            void (*func)(JsVmValue* func, JsVmStack* stack, size_t num_args);
        } func;
    } as;
};
typedef struct JsVmObjectBucket JsVmObjectBucket;
struct JsVmObjectBucket {
    JsVmObjectBucket* next;
    Atom* key;
    JsVmValue value;
};
struct JsVmObject {
    struct {
        JsVmObjectBucket** items;
        size_t len;
    } buckets;
    size_t len;
};
bool jsvm_object_reserve(JsVmObject* map, size_t extra);
bool jsvm_object_insert(JsVmObject* map, Atom* name, JsVmValue value);

struct JsVmStack {
    JsVmValue* items;
    size_t len, cap;
};
void jsvm_interpret(JsVmObject* globals, JsVmStack* stack, JsVmInstruction* inst);

#include <stdio.h>
void jsvm_dump_value(FILE* sink, const JsVmValue* value);
