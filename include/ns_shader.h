#pragma once

#include "ns_vm.h"
#include "ns_ast.h"

// ns_shader transcodes an ns function (plus its transitive struct and helper-fn
// dependencies) into shader source text for a target platform:
//   - NS_SHADER_MSL:         Metal Shading Language (Apple / Metal)
//   - NS_SHADER_GLSL_VULKAN: GLSL 450 with Vulkan layout qualifiers (SPIR-V ready)
//   - NS_SHADER_HLSL:        HLSL (DirectX 12)
//
// Shader entries are ordinary ns fns (see sample/ns/shader.ns): a vertex fn maps
// an input struct to a stage-io struct whose `position: float4` field is the clip
// position, and a fragment fn maps the stage-io struct to a float4 color. The
// supported language subset covers scalars, the simd vector structs
// (float2/3/4 and mat4), user structs, arithmetic/logic, if/for/loop and calls to
// other user fns; anything else fails with a source-located error.

typedef enum ns_shader_target {
    NS_SHADER_TARGET_UNKNOWN = 0,
    NS_SHADER_MSL,
    NS_SHADER_GLSL_VULKAN,
    NS_SHADER_HLSL,
} ns_shader_target;

typedef enum ns_shader_stage {
    NS_SHADER_STAGE_AUTO = 0, // infer from fn name/signature
    NS_SHADER_STAGE_VERTEX,
    NS_SHADER_STAGE_FRAGMENT,
} ns_shader_stage;

ns_return_define(str, ns_str);

// One shader entry: fn symbol index into vm->symbols plus its resolved stage.
typedef struct ns_shader_entry_desc {
    i32 fn_index;
    ns_shader_stage stage;
} ns_shader_entry_desc;

// Transpile one or more entry fns into a single self-contained shader source for
// `target`. The returned ns_str is heap-owned (dynamic); the caller frees it.
// GLSL requires one source per stage, so a GLSL program with more than one entry
// is an error: callers loop per entry instead.
ns_return_str ns_shader_transpile_program(ns_vm *vm, ns_ast_ctx *ctx, ns_shader_entry_desc *entries, i32 count, ns_shader_target target);

// Single-entry convenience; NS_SHADER_STAGE_AUTO infers the stage.
ns_return_str ns_shader_transpile(ns_vm *vm, ns_ast_ctx *ctx, i32 fn_index, ns_shader_target target, ns_shader_stage stage);

// "msl" | "glsl" | "hlsl" -> target enum (UNKNOWN on mismatch), and the reverse
// for messages and file suffixes.
ns_shader_target ns_shader_target_from_str(ns_str s);
ns_str ns_shader_target_name(ns_shader_target t);

// vertex: fn name starts with "vs", or returns a struct with a `position: float4`
// field; fragment: name starts with "fs"/"ps", or returns float4. AUTO on failure.
ns_shader_stage ns_shader_stage_infer(ns_vm *vm, ns_ast_ctx *ctx, i32 fn_index);

// Entry-point name to feed gpu_shader_stage_desc.entry: the fn name for MSL/HLSL,
// "main" for GLSL (the generated wrapper). Borrowed/static storage.
ns_str ns_shader_entry_name(ns_shader_target t, ns_str fn_name);

// Dispatch for `mod shader` ref fns (shader_transpile / shader_transpile_stage /
// shader_entry). Called from ns_eval_call_expr where the ast ctx is in scope;
// mirrors ns_vm_call_std.
ns_return_bool ns_shader_vm_call(ns_vm *vm, ns_ast_ctx *ctx);
