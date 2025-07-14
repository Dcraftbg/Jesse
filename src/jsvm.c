#include "jsvm.h"
#include <stdio.h>
#include <assert.h>
#include <todo.h>
#include <string.h>
#include <darray.h>
#include <stdlib.h>
#include <ctype.h>
#include <atom.h>

#define JSVM_OBJECT_ALLOC malloc
#define JSVM_OBJECT_DEALLOC(ptr, n) free(ptr)
#define JSVM_OBJECT_BUCKET_ALLOC malloc

bool jsvm_object_reserve(JsVmObject* map, size_t extra) {
    if(map->len + extra > map->buckets.len) {
        size_t ncap = map->buckets.len*2 + extra;
        JsVmObjectBucket** newbuckets = JSVM_OBJECT_ALLOC(sizeof(*newbuckets)*ncap);
        if(!newbuckets) return false;
        memset(newbuckets, 0, sizeof(*newbuckets) * ncap);
        for(size_t i = 0; i < map->buckets.len; ++i) {
            JsVmObjectBucket* oldbucket = map->buckets.items[i];
            while(oldbucket) {
                JsVmObjectBucket* next = oldbucket->next;
                size_t hash = ((size_t)oldbucket->key) % ncap;
                JsVmObjectBucket* newbucket = newbuckets[hash];
                oldbucket->next = newbucket;
                newbuckets[hash] = oldbucket;
                oldbucket = next;
            }
        }
        JSVM_OBJECT_DEALLOC(map->buckets.items, map->buckets.cap * sizeof(*map->buckets.items));
        map->buckets.items = newbuckets;
        map->buckets.len = ncap;
    }
    return true;
}
bool jsvm_object_insert(JsVmObject* map, Atom* name, JsVmValue value) {
    if(!jsvm_object_reserve(map, 1)) return false;
    size_t hash = ((size_t)name) % map->buckets.len;
    JsVmObjectBucket* into = map->buckets.items[hash];
    JsVmObjectBucket* bucket = JSVM_OBJECT_BUCKET_ALLOC(sizeof(JsVmObjectBucket));
    if(!bucket) return false;
    bucket->next = into;
    bucket->key = name;
    bucket->value = value;
    map->buckets.items[hash] = bucket;
    map->len++;
    return true;
}
static JsVmObjectBucket* jsvm_object_get(JsVmObject* map, Atom* name) {
    if(map->len == 0) return NULL;
    assert(map->buckets.len > 0);
    size_t hash = ((size_t)name) % map->buckets.len;
    JsVmObjectBucket* bucket = map->buckets.items[hash];
    while(bucket) {
        if(bucket->key == name) return bucket;
        bucket = bucket->next;
    }
    return NULL;
}
static inline JsVmValue jsvm_undefined(void) {
    return (JsVmValue) {
        .kind = JSVM_VALUE_UNDEFINED
    };
}
void jsvm_dump_value(FILE* sink, const JsVmValue* value) {
    static_assert(JSVM_VALUE_COUNT == 4, "Update jsvm_dump_value");
    switch(value->kind) {
    case JSVM_VALUE_UNDEFINED:
        fprintf(sink, "undefined");
        break;
    case JSVM_VALUE_FUNC:
        fprintf(sink, "<Function: #%08llx>", (unsigned long long)value->as.func.func);
        break;
    case JSVM_VALUE_OBJECT: {
        JsVmObject* object = value->as.object;
        size_t n = 0;
        fprintf(sink, "{");
        for(size_t i = 0; i < object->buckets.len; ++i) {
            JsVmObjectBucket* bucket = object->buckets.items[i];
            while(bucket) {
                if(n > 0) fprintf(sink, ", ");
                fprintf(sink, "%s: ", bucket->key->data);
                jsvm_dump_value(sink, &bucket->value);
                n++;
                bucket = bucket->next;
            }
        }
        fprintf(sink, "}");
    } break;
    case JSVM_VALUE_STRING:
        fprintf(sink, "\"");
        for(size_t i = 0; i < value->as.string.len; ++i) {
            char c = value->as.string.items[i];
            if(isgraph(c)) fprintf(sink, "%c", c);
            else fprintf(sink, "\\x%02X", c);
        }
        fprintf(sink, "\"");
        break;
    }
}
void jsvm_interpret(JsVmObject* globals, JsVmStack* stack, JsVmInstruction* inst) {
    (void)stack;
    (void)inst;
    static_assert(JSVM_INST_COUNT == 6, "Update jsvm_interpret");
    switch(inst->kind) {
    case JSVM_PUSH_STR: {
        JsVmValue value = {
            .kind = JSVM_VALUE_STRING,
            .as.string = { 0 }
        };
        da_reserve(&value.as.string, inst->as.push_str.len);
        memcpy(value.as.string.items, inst->as.push_str.data, inst->as.push_str.len);
        value.as.string.len += inst->as.push_str.len;
        da_push(stack, value);
    } break;
    case JSVM_GET_GLOBAL: {
        JsVmObjectBucket* bucket = jsvm_object_get(globals, inst->as.atom);
        // TODO: technically incorrect. We'd need jsvm_value_clone
        da_push(stack, bucket ? bucket->value : jsvm_undefined());
    } break;
    case JSVM_GET_MEMBER: {
        assert(stack->len > 0);
        JsVmValue value = da_pop(stack);
        switch(value.kind) {
        case JSVM_VALUE_OBJECT: {
            JsVmObjectBucket* bucket = jsvm_object_get(value.as.object, inst->as.atom);
            // TODO: technically incorrect. We'd need jsvm_value_clone
            da_push(stack, bucket ? bucket->value : jsvm_undefined());
            if(!bucket) {
                fprintf(stderr, "ERROR Failed to get member: %s of ", inst->as.atom->data);
                jsvm_dump_value(stderr, &value);
                fprintf(stderr, "\n");
            }
        } break;
        default:
            fprintf(stderr, "TODO "__FILE__":"STRINGIFY1(__LINE__)": throw runtime error on getting field of non object: ");
            jsvm_dump_value(stderr, &value);
            fprintf(stderr, "\n");
            abort();
        }
    } break;
    case JSVM_CALL: {
        assert(stack->len > 0);
        JsVmValue value = da_pop(stack);
        JsVmValue this = da_pop(stack);
        switch(value.kind) {
        case JSVM_VALUE_FUNC: {
            value.as.func.func(&this, &value, stack, inst->as.call.num_args);
        } break;
        default:
            fprintf(stderr, "TODO "__FILE__":"STRINGIFY1(__LINE__)": throw runtime error on calling non function: ");
            jsvm_dump_value(stderr, &value);
            fprintf(stderr, "\n");
            abort();
        }
    } break;
    case JSVM_DUP: {
        assert(stack->len > 0);
        da_reserve(stack, 1);
        JsVmValue value = stack->items[stack->len-1];
        // TODO: technically incorrect. We'd need jsvm_value_clone
        da_push(stack, value);
    } break;
    case JSVM_THIS: {
        // TODO: this
        da_push(stack, jsvm_undefined());
    } break;
    default:
        todof("jsvm_interpret(%d)\n", inst->kind);
    }
}
