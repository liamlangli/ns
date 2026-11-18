#include "ns_native_rt.h"
#include <assert.h>
#include <string.h>

static void check(const uint8_t *bytes, int count, int length, const char *expected) {
    const char *kind = NULL;
    assert(ns_debug_x86_instruction(bytes, count, &kind) == length);
    assert(!strcmp(kind, expected));
    // A truncated instruction must never be accepted as the full one.
    if (length > 1) assert(ns_debug_x86_instruction(bytes, length - 1, &kind) == 0);
}
int main(void) {
    const uint8_t move[] = {0x48,0x89,0xd8};
    const uint8_t constant[] = {0x49,0xb8,1,2,3,4,5,6,7,8};
    const uint8_t stack_load[] = {0x48,0x8b,0x84,0x24,0x80,0,0,0};
    const uint8_t indexed_load[] = {0x48,0x8b,0x04,0x25,0x80,0,0,0};
    const uint8_t add[] = {0x48,0x83,0xc0,1};
    const uint8_t multiply[] = {0x48,0x0f,0xaf,0xc1};
    const uint8_t divide[] = {0x48,0xf7,0xf9};
    const uint8_t fp_move[] = {0x66,0x48,0x0f,0x6e,0xc0};
    const uint8_t fp_divide[] = {0xf2,0x0f,0x5e,0xc1};
    const uint8_t call[] = {0xe8,0,0,0,0};
    const uint8_t indirect_call[] = {0xff,0xd0};
    const uint8_t memory_call[] = {0xff,0x10};
    const uint8_t memory_multiply[] = {0x48,0x0f,0xaf,0x01};
    const uint8_t branch[] = {0x0f,0x85,0,0,0,0};
    const uint8_t ret[] = {0xc3};
    const uint8_t avx[] = {0xc5,0xf8,0x77};
    const uint8_t prefixes[] = {0x48,0x48};
    check(move, sizeof(move), 3, "alu");
    check(constant, sizeof(constant), 10, "alu");
    check(stack_load, sizeof(stack_load), 8, "memory");
    check(indexed_load, sizeof(indexed_load), 8, "memory");
    check(add, sizeof(add), 4, "alu");
    check(multiply, sizeof(multiply), 4, "multiply");
    check(divide, sizeof(divide), 3, "divide");
    check(fp_move, sizeof(fp_move), 5, "alu");
    check(fp_divide, sizeof(fp_divide), 4, "fp_divide");
    check(call, sizeof(call), 5, "call");
    check(indirect_call, sizeof(indirect_call), 2, "call");
    check(memory_call, sizeof(memory_call), 2, "call_memory");
    check(memory_multiply, sizeof(memory_multiply), 4, "memory");
    check(branch, sizeof(branch), 6, "branch");
    check(ret, sizeof(ret), 1, "branch");
    check(avx, sizeof(avx), 0, "unknown");
    check(prefixes, sizeof(prefixes), 0, "unknown");
    check(NULL, 0, 0, "unknown");
    puts("debug instruction boundaries: passed");
    return 0;
}
