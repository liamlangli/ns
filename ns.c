#include <stdio.h>
#include <stdlib.h>

typedef unsigned long u64;
typedef unsigned int u32;
typedef unsigned short u16;
typedef unsigned char u8;
typedef signed long i64;
typedef signed int i32;
typedef signed short i16;
typedef signed char i8;
typedef float f32;
typedef double f64;

typedef struct ns_context_t
{
    int call_stack_depth;
} ns_context_t;

typedef struct ns_value
{
    unsigned long id;
} ns_value;

#define ns_const_value ns_value

ns_value ns_eval(ns_context_t *ctx, const char *source, size_t content_length, const char *filename, int flag)
{
    int i;
    return (ns_value){0};
}

ns_context_t *ns_make_context()
{
    ns_context_t *ctx = (ns_context_t *)malloc(sizeof(ns_context_t));
    ctx->call_stack_depth = 0;
    return ctx;
}

int main(int argc, char **argv)
{
    ns_context_t *ctx = ns_make_context();
    printf("hello world\n");
    free(ctx);
    return 0;
}
