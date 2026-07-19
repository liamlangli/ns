#include "ns_test.h"
#include "ns.h"
#include "ns_def.h"

#include <fcntl.h>
#include <stdlib.h>
#include <unistd.h>

static i32 raw_sum(i32 *data) {
    i32 sum = 0;
    for (szt i = 0; i < ns_buffer_len(data); ++i) sum += data[i];
    return sum;
}

static i32 raw_meta_ok(i32 *data) {
    return data &&
           ns_buffer_len(data) == 4 &&
           ns_buffer_size(data) == 4 &&
           ns_buffer_cap(data) == 4 &&
           ns_buffer_elem_size(data) == sizeof(i32) &&
           ns_type_is(ns_buffer_type(data), NS_TYPE_I32);
}

static void define_array_fn(ns_vm *vm, const char *name, void *fn) {
    ns_type i32_array = ns_type_i32;
    i32_array.array = true;
    i32_array.mut = true;
    i32_array.stack = true;

    ns_arg_def args[] = {
        {.name = ns_str_cstr("data"), .type = i32_array},
    };
    ns_fn_def def = {
        .name = ns_str_cstr(name),
        .lib = ns_str_cstr(""),
        .args = args,
        .arg_count = 1,
        .ret = ns_type_i32,
        .fn = fn,
    };
    ns_vm_def_fn(vm, &def);
}

int main() {
    setenv("NS_REPL_RECOVER", "1", 1);
    ns_expect(ns_str_equals_STR(ns_str_cstr("gpu_create_buffer"), "gpu_create_buffer") &&
                  !ns_str_equals_STR(ns_str_cstr("gpu_create_buffer_texture_binding"), "gpu_create_buffer"),
              "fixed-string equality rejects longer names that only share a prefix.");

    {
        ns_vm vm = {0};
        define_array_fn(&vm, "raw_sum", raw_sum);
        define_array_fn(&vm, "raw_meta_ok", raw_meta_ok);

        const char *script =
            "type AgeMap = [str: i32]\n"
            "type NameSet = set[str]\n"
            "struct Pair { a: i32, b: i32 }\n"
            "fn main() i32 {\n"
            "    let a = [i32](4)\n"
            "    assert a.len == 4\n"
            "    assert a.size == 4\n"
            "    assert a.cap == 4\n"
            "    a[0] = 5\n"
            "    a[1] = 7\n"
            "    assert a[0] == 5\n"
            "    assert raw_sum(a) == 12\n"
            "    assert raw_meta_ok(a) == 1\n"
            "    let p = [Pair](2)\n"
            "    p[1] = Pair { a: 3, b: 4 }\n"
            "    assert p.len == 2\n"
            "    assert p[1].b == 4\n"
            "    let s = \"abc\"\n"
            "    assert s.len == 3\n"
            "    assert s.size == 3\n"
            "    assert s.cap >= 3\n"
            "    let ages: AgeMap = [str: i32](4)\n"
            "    assert ages.len == 0\n"
            "    assert ages.size == 0\n"
            "    assert ages.cap == 4\n"
            "    ages[\"amy\"] = 32\n"
            "    assert ages.len == 1\n"
            "    let seen: NameSet = set[str](3)\n"
            "    assert seen.len == 0\n"
            "    assert seen.size == 0\n"
            "    assert seen.cap == 3\n"
            "    return a[1]\n"
            "}\n";
        ns_return_value ret = ns_eval(&vm, ns_str_cstr(script), ns_str_cstr("<buffer-test>"));
        ns_return_assert(ret);
        ns_expect(ns_eval_number_i32(&vm, ret.r) == 7, "header-backed arrays, strings, dicts, sets, and raw FFI.");
    }

    {
        ns_vm vm = {0};
        const char *script =
            "fn main() i32 {\n"
            "    let a = [i32](2)\n"
            "    let x = a[2]\n"
            "    return x\n"
            "}\n";
        fflush(stderr);
        int saved_stderr = dup(STDERR_FILENO);
        int null_fd = open("/dev/null", O_WRONLY);
        if (null_fd >= 0) dup2(null_fd, STDERR_FILENO);
        ns_return_value ret = ns_eval(&vm, ns_str_cstr(script), ns_str_cstr("<buffer-oob>"));
        fflush(stderr);
        if (saved_stderr >= 0) {
            dup2(saved_stderr, STDERR_FILENO);
            close(saved_stderr);
        }
        if (null_fd >= 0) close(null_fd);
        ns_expect(ns_return_is_error(ret), "array out-of-bounds read returns an eval error.");
    }

    return 0;
}
