#!/usr/bin/env python3
"""Generate the typed native-call dispatcher used by embedded Apple apps."""

from __future__ import annotations

import argparse
import re
from dataclasses import dataclass
from pathlib import Path


SCALARS = {"i8", "i16", "i32", "i64", "u8", "u16", "u32", "u64", "f32", "f64", "bool"}


@dataclass
class Argument:
    name: str
    type: str
    native_pointer: bool = False


@dataclass
class Function:
    module: str
    name: str
    args: list[Argument]
    result: str
    native_result_pointer: bool = False


def split_args(text: str) -> list[str]:
    if not text.strip():
        return []
    parts: list[str] = []
    depth = 0
    start = 0
    for index, char in enumerate(text):
        if char in "[(":
            depth += 1
        elif char in "])":
            depth -= 1
        elif char == "," and depth == 0:
            parts.append(text[start:index].strip())
            start = index + 1
    parts.append(text[start:].strip())
    return parts


def parse_module(path: Path) -> tuple[dict[str, list[Argument]], dict[str, str], list[Function]]:
    source = path.read_text(encoding="utf-8")
    module_match = re.search(r"(?m)^mod\s+([A-Za-z_][A-Za-z0-9_]*)\s*$", source)
    if not module_match:
        raise ValueError(f"{path}: missing module declaration")
    module = module_match.group(1)
    structs: dict[str, list[Argument]] = {}
    for match in re.finditer(r"(?ms)^struct\s+([A-Za-z_][A-Za-z0-9_]*)\s*\{(.*?)\}", source):
        fields: list[Argument] = []
        for item in split_args(match.group(2)):
            item = re.sub(r"//.*", "", item).strip()
            if not item:
                continue
            name, separator, type_name = item.partition(":")
            if not separator:
                raise ValueError(f"{path}: invalid struct field {item!r}")
            fields.append(Argument(name.strip(), type_name.strip()))
        structs[match.group(1)] = fields
    aliases = {
        match.group(1): match.group(2).strip()
        for match in re.finditer(r"(?m)^type\s+([A-Za-z_][A-Za-z0-9_]*)\s*=\s*([^|\n]+)\s*$", source)
    }
    functions: list[Function] = []
    pattern = re.compile(
        r"(?m)^ref fn[ \t]+([A-Za-z_][A-Za-z0-9_]*)\(([^\n]*)\)"
        r"(?:[ \t]+([^\n/]+?))?[ \t]*(?://.*)?$"
    )
    for match in pattern.finditer(source):
        args: list[Argument] = []
        for item in split_args(match.group(2)):
            name, separator, type_name = item.partition(":")
            if not separator:
                raise ValueError(f"{path}: invalid argument {item!r}")
            args.append(Argument(name.strip(), type_name.strip()))
        functions.append(Function(module, match.group(1), args, (match.group(3) or "void").strip()))
    return structs, aliases, functions


def resolve(type_name: str, aliases: dict[str, str]) -> str:
    seen: set[str] = set()
    while type_name in aliases and type_name not in seen:
        seen.add(type_name)
        type_name = aliases[type_name]
    return type_name


def kind(type_name: str, structs: set[str], aliases: dict[str, str]) -> str:
    type_name = resolve(type_name, aliases)
    if type_name == "void" or type_name == "str" or type_name in SCALARS:
        return type_name
    if type_name.startswith("ref "):
        return "ref"
    if type_name.startswith("["):
        return "array"
    if type_name in structs:
        return "struct"
    raise ValueError(f"unsupported embedded FFI type: {type_name}")


def c_type(type_name: str, structs: set[str], aliases: dict[str, str], result: bool = False) -> str:
    type_kind = kind(type_name, structs, aliases)
    if type_kind == "void":
        return "void"
    if type_kind == "str":
        return "const char *"
    if type_kind == "bool":
        return "ns_bool"
    if type_kind in SCALARS:
        return type_kind
    if type_kind == "struct":
        return resolve(type_name, aliases)
    if type_kind in {"ref", "array"}:
        return "void *"
    raise AssertionError(type_kind)


def native_c_type(type_name: str, native_pointer: bool, structs: set[str], aliases: dict[str, str]) -> str:
    result = c_type(type_name, structs, aliases)
    if kind(type_name, structs, aliases) == "struct" and native_pointer:
        return f"{result} *"
    return result


def arg_expr(index: int, argument: Argument, structs: set[str], aliases: dict[str, str]) -> str:
    type_name = argument.type
    type_kind = kind(type_name, structs, aliases)
    if type_kind == "str":
        return f"ns_embedded_arg_str(vm, {index})"
    if type_kind == "struct":
        if argument.native_pointer:
            return f"({c_type(type_name, structs, aliases)} *)ns_embedded_arg_pointer(vm, {index})"
        native_type = c_type(type_name, structs, aliases)
        return f"*({native_type} *)ns_embedded_arg_pointer(vm, {index})"
    if type_kind in {"ref", "array"}:
        return f"ns_embedded_arg_pointer(vm, {index})"
    if type_kind == "bool":
        return f"ns_eval_bool(vm, ns_embedded_arg(vm, {index}))"
    if type_kind in SCALARS:
        return f"ns_eval_number_{type_kind}(vm, ns_embedded_arg(vm, {index}))"
    raise AssertionError(type_kind)


def implemented_functions(paths: list[Path], functions: list[Function]) -> set[str]:
    names = {function.name for function in functions}
    implemented: set[str] = set()
    for path in paths:
        source = path.read_text(encoding="utf-8")
        for name in names - implemented:
            function = next(candidate for candidate in functions if candidate.name == name)
            pattern = re.compile(rf"(?m)^[ \t]*(?!static\b)[^#\n()=;]*\b{re.escape(name)}[ \t]*\(")
            for match in pattern.finditer(source):
                depth = 1
                args_start = match.end()
                index = match.end()
                while index < len(source) and depth:
                    if source[index] == "(":
                        depth += 1
                    elif source[index] == ")":
                        depth -= 1
                    index += 1
                if depth:
                    continue
                while index < len(source) and source[index].isspace():
                    index += 1
                if index < len(source) and source[index] == "{":
                    declaration = source[match.start():args_start]
                    result_text = declaration[:declaration.rfind(name)]
                    function.native_result_pointer = "*" in result_text
                    native_args = split_args(source[args_start:index - 1])
                    if len(native_args) == len(function.args):
                        for argument, native_arg in zip(function.args, native_args):
                            argument.native_pointer = "*" in native_arg
                    implemented.add(name)
                    break
    return implemented


def invoke_body(function: Function, structs: set[str], aliases: dict[str, str]) -> str:
    """Body of a signature invoker: marshals args, calls `target`, stores the
    result. Identical signatures produce identical text, which emit() uses to
    deduplicate invokers."""
    result_kind = kind(function.result, structs, aliases)
    ret_c = native_c_type(function.result, function.native_result_pointer, structs, aliases)
    arg_cs = ", ".join(native_c_type(arg.type, arg.native_pointer, structs, aliases) for arg in function.args) or "void"
    args = ", ".join(arg_expr(i, arg, structs, aliases) for i, arg in enumerate(function.args))
    invocation = f"(({ret_c} (*)({arg_cs}))target)({args})"
    lines: list[str] = []
    if result_kind == "void":
        lines += [
            f"    {invocation};",
            "    ns_call *call = ns_array_last(vm->call_stack);",
            "    call->ret = ns_nil;",
        ]
    elif result_kind == "str":
        lines += [
            f"    const char *result = {invocation};",
            "    ns_call *call = ns_array_last(vm->call_stack);",
            "    call->ret.t = ns_type_str;",
            "    call->ret.o = ns_vm_push_string(vm, result ? ns_str_cstr((char *)result) : ns_str_null);",
        ]
    elif result_kind == "ref":
        lines += [f"    void *result = {invocation};", "    ns_embedded_return_pointer(vm, result);"]
    elif result_kind == "array":
        lines += [f"    void *result = {invocation};", "    ns_embedded_return_array(vm, result);"]
    elif result_kind == "struct":
        native_type = c_type(function.result, structs, aliases, result=True)
        if function.native_result_pointer:
            lines += [f"    {native_type} *result = {invocation};", "    ns_embedded_return_struct(vm, result);"]
        else:
            lines += [f"    {native_type} result = {invocation};", "    ns_embedded_return_struct(vm, &result);"]
    else:
        native_type = c_type(function.result, structs, aliases, result=True)
        lines += [f"    {native_type} result = {invocation};", "    ns_embedded_return_scalar(vm, &result, sizeof(result));"]
    lines.append("    return ns_return_ok(bool, true);")
    return "\n".join(lines)


def emit(functions: list[Function], structs: dict[str, list[Argument]], aliases: dict[str, str]) -> str:
    lines = [
        "/* Generated by tools/gen_embedded_ffi.py. Do not edit by hand. */",
        '#include "ns_vm.h"',
        "#include <string.h>",
        "",
        "#if defined(NS_XCLIB)",
        "",
    ]
    used_structs = {
        resolve(type_name, aliases)
        for function in functions
        for type_name in [function.result, *(argument.type for argument in function.args)]
        if kind(type_name, set(structs), aliases) == "struct"
    }
    for name in sorted(used_structs):
        lines.append("typedef struct {")
        for field in structs[name]:
            lines.append(f"    {c_type(field.type, set(structs), aliases)} {field.name};")
        lines.append(f"}} {name};")
    if used_structs:
        lines.append("")
    for function in functions:
        result = native_c_type(function.result, function.native_result_pointer, set(structs), aliases)
        args = ", ".join(native_c_type(arg.type, arg.native_pointer, set(structs), aliases) for arg in function.args) or "void"
        lines.append(f"extern {result} {function.name}({args});")
    lines += [
        "",
        "static ns_value ns_embedded_arg(ns_vm *vm, i32 index) {",
        "    ns_call *call = ns_array_last(vm->call_stack);",
        "    return vm->symbol_stack[call->arg_offset + index].val;",
        "}",
        "",
        "static const char *ns_embedded_arg_str(ns_vm *vm, i32 index) {",
        "    return ns_eval_str(vm, ns_embedded_arg(vm, index)).data;",
        "}",
        "",
        "static void *ns_embedded_arg_pointer(ns_vm *vm, i32 index) {",
        "    ns_value value = ns_embedded_arg(vm, index);",
        "    if (ns_type_is_array(value.t)) return ns_eval_array_raw(vm, value);",
        "    if (ns_type_is_ref(value.t)) return ns_type_in_stack(value.t) ? (void *)&vm->stack[value.o] : (void *)value.o;",
        "    return ns_type_in_stack(value.t) ? (void *)&vm->stack[value.o] : (void *)value.o;",
        "}",
        "",
        "static void ns_embedded_return_scalar(ns_vm *vm, const void *value, size_t size) {",
        "    ns_call *call = ns_array_last(vm->call_stack);",
        "    memcpy(&vm->stack[call->ret.o], value, size);",
        "}",
        "",
        "static void ns_embedded_return_pointer(ns_vm *vm, void *value) {",
        "    ns_call *call = ns_array_last(vm->call_stack);",
        "    call->ret.t = ns_type_set_stack(call->callee->fn.ret, false);",
        "    call->ret.o = (u64)value;",
        "}",
        "",
        "static void ns_embedded_return_struct(ns_vm *vm, const void *value) {",
        "    ns_call *call = ns_array_last(vm->call_stack);",
        "    if (value) memcpy(&vm->stack[call->ret.o], value, (size_t)ns_type_size(vm, call->callee->fn.ret));",
        "}",
        "",
        "static void ns_embedded_return_array(ns_vm *vm, void *value) {",
        "    ns_call *call = ns_array_last(vm->call_stack);",
        "    *(u64 *)&vm->stack[call->ret.o] = (u64)value;",
        "}",
        "",
    ]

    # ref fn names are C symbols, so they are globally unique: a name declared
    # by several modules must resolve to the same native function. Dispatch is
    # therefore keyed on the name alone.
    unique: dict[str, Function] = {}
    for function in functions:
        unique.setdefault(function.name, function)
    functions = sorted(unique.values(), key=lambda function: function.name)
    for function in functions:
        if not function.name.isascii():
            raise ValueError(f"non-ascii ref fn name: {function.name}")

    # One invoker per unique signature; the table row supplies the target.
    struct_names = set(structs)
    signature_ids: dict[str, int] = {}
    entry_rows: list[tuple[str, int]] = []
    for function in functions:
        body = invoke_body(function, struct_names, aliases)
        signature = signature_ids.setdefault(body, len(signature_ids))
        entry_rows.append((function.name, signature))

    lines.append("typedef ns_return_bool (*ns_embedded_invoke)(ns_vm *vm, void *target);")
    lines.append("")
    for body, signature in sorted(signature_ids.items(), key=lambda item: item[1]):
        lines += [f"static ns_return_bool ns_embedded_sig{signature}(ns_vm *vm, void *target) {{", body, "}", ""]

    lines += [
        "typedef struct ns_embedded_entry {",
        "    const char *name;",
        "    void *target;",
        "    ns_embedded_invoke invoke;",
        "} ns_embedded_entry;",
        "",
        "// Sorted by strcmp(name) for the binary search in ns_vm_call_embedded.",
        "static const ns_embedded_entry ns_embedded_entries[] = {",
    ]
    for name, signature in entry_rows:
        lines.append(f'    {{ "{name}", (void *){name}, ns_embedded_sig{signature} }},')
    lines += [
        "};",
        "",
        "// strcmp over an ns_str (data, len) and a NUL-terminated entry name.",
        "static i32 ns_embedded_name_cmp(ns_str name, const char *entry) {",
        "    const u8 *a = (const u8 *)name.data;",
        "    const u8 *b = (const u8 *)entry;",
        "    i32 i = 0;",
        "    while (i < name.len && b[i]) {",
        "        if (a[i] != b[i]) return (i32)a[i] - (i32)b[i];",
        "        ++i;",
        "    }",
        "    if (i < name.len) return (i32)a[i];",
        "    return -(i32)b[i];",
        "}",
        "",
        "// Binary search over ns_embedded_entries, memoized per fn symbol: the",
        "// dlsym/libffi path is compiled out under NS_XCLIB, so fn->fn.fn_ptr is",
        "// free to store the resolved entry index + 1.",
        "ns_return_bool ns_vm_call_embedded(ns_vm *vm) {",
        "    ns_call *call = ns_array_last(vm->call_stack);",
        "    ns_symbol *fn = call->callee;",
        "    size_t memo = (size_t)fn->fn.fn_ptr;",
        "    if (memo) {",
        "        const ns_embedded_entry *entry = &ns_embedded_entries[memo - 1];",
        "        return entry->invoke(vm, entry->target);",
        "    }",
        "    i32 lo = 0;",
        "    i32 hi = (i32)(sizeof(ns_embedded_entries) / sizeof(ns_embedded_entries[0])) - 1;",
        "    while (lo <= hi) {",
        "        i32 mid = lo + (hi - lo) / 2;",
        "        i32 cmp = ns_embedded_name_cmp(fn->name, ns_embedded_entries[mid].name);",
        "        if (cmp == 0) {",
        "            fn->fn.fn_ptr = (void *)(size_t)(mid + 1);",
        "            return ns_embedded_entries[mid].invoke(vm, ns_embedded_entries[mid].target);",
        "        }",
        "        if (cmp < 0) hi = mid - 1; else lo = mid + 1;",
        "    }",
        "    static char message[512];",
        "    snprintf(message, sizeof(message), \"embedded native function is not forwarded: %.*s.%.*s\",",
        "             fn->lib.len, fn->lib.data, fn->name.len, fn->name.data);",
        "    return ns_return_error(bool, ns_code_loc_nil, NS_ERR_EVAL, message);",
        "}",
        "#endif",
        "",
    ]
    return "\n".join(lines)


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("--output", required=True, type=Path)
    parser.add_argument("--native-source", action="append", default=[], type=Path)
    parser.add_argument("modules", nargs="+", type=Path)
    args = parser.parse_args()

    structs: dict[str, list[Argument]] = {}
    aliases: dict[str, str] = {}
    functions: list[Function] = []
    for module in args.modules:
        module_structs, module_aliases, module_functions = parse_module(module)
        structs.update(module_structs)
        aliases.update(module_aliases)
        functions.extend(module_functions)
    if args.native_source:
        implemented = implemented_functions(args.native_source, functions)
        functions = [function for function in functions if function.name in implemented]
    args.output.write_text(emit(functions, structs, aliases), encoding="utf-8")


if __name__ == "__main__":
    main()
