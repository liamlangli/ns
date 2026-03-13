import { parse_to_ast } from './interpreter.js';

function normalize_literal(value) {
    if (value === null) return { literalType: 'null', value: 'null' };
    if (typeof value === 'number') return { literalType: Number.isInteger(value) ? 'int' : 'float', value: String(value) };
    if (typeof value === 'boolean') return { literalType: 'bool', value: value ? 'true' : 'false' };
    return { literalType: 'string', value: String(value) };
}

function normalize_ast_node(node) {
    if (!node || typeof node !== 'object') return { kind: 'Unknown', payload: { value: node } };
    const kind = String(node.kind ?? 'Unknown');

    if (kind === 'Lit') return { kind: 'Literal', payload: normalize_literal(node.value) };
    if (kind === 'Ident') return { kind: 'Identifier', payload: { name: String(node.name ?? '') } };

    const payload = {};
    const children = [];

    for (const key of Object.keys(node).sort()) {
        if (key === 'kind' || key === 'line') continue;
        const value = node[key];
        if (Array.isArray(value)) {
            for (const child of value) {
                if (child && typeof child === 'object' && 'kind' in child) children.push(normalize_ast_node(child));
                else children.push({ kind: 'Value', payload: { key, value: child } });
            }
            continue;
        }
        if (value && typeof value === 'object' && 'kind' in value) {
            children.push(normalize_ast_node(value));
            continue;
        }
        if (key === 'name' || key === 'field' || key === 'mod' || key === 'op' || key === 'type' || key === 'ret_type' || key === 'var') {
            payload[key] = String(value ?? '');
        } else if (value !== undefined) {
            payload[key] = value;
        }
    }

    return {
        kind,
        payload: Object.keys(payload).length ? payload : undefined,
        children: children.length ? children : undefined,
    };
}

export function normalize_cpu_ast_by_function(source, function_spans) {
    return function_spans.map((span, functionIndex) => {
        const segment = source.slice(span.start, span.end);
        const parsed = parse_to_ast(segment);
        const root = normalize_ast_node(parsed);
        return { functionIndex, nodes: root.children ?? [root] };
    });
}

function gpu_kind_to_string(kind) {
    if (kind === 1) return 'Identifier';
    if (kind === 2) return 'Literal';
    return 'Symbol';
}

function normalize_gpu_node(source, node) {
    const token_text = source.slice(node.tokenStart, node.tokenStart + node.token_count);
    if (node.kind === 1) return { kind: 'Identifier', payload: { name: token_text } };
    if (node.kind === 2) return { kind: 'Literal', payload: { literalType: 'int', value: token_text } };
    return { kind: gpu_kind_to_string(node.kind), payload: { token: token_text } };
}

export function normalize_gpu_ast_from_readback(source, result) {
    return result.ast_by_function.map((nodes, functionIndex) => {
        const sorted = [...nodes].sort((a, b) => a.tokenStart - b.tokenStart || a.kind - b.kind || a.token_count - b.token_count);
        return { functionIndex, nodes: sorted.map((node) => normalize_gpu_node(source, node)) };
    });
}

function payload_equal(a, b) {
    return JSON.stringify(a ?? {}) === JSON.stringify(b ?? {});
}

export function compare_normalized_asts(cpu, gpu) {
    const fn_count = Math.max(cpu.length, gpu.length);
    for (let fn = 0; fn < fn_count; fn++) {
        const cpu_nodes = cpu[fn]?.nodes ?? [];
        const gpu_nodes = gpu[fn]?.nodes ?? [];
        const node_count = Math.max(cpu_nodes.length, gpu_nodes.length);
        for (let i = 0; i < node_count; i++) {
            const c = cpu_nodes[i];
            const g = gpu_nodes[i];
            if (!c || !g) {
                return {
                    ok: false,
                    mismatch: { functionIndex: fn, path: `${fn}/${i}`, reason: 'node-count mismatch', expected: c ?? null, actual: g ?? null },
                };
            }
            if (c.kind !== g.kind) {
                return {
                    ok: false,
                    mismatch: { functionIndex: fn, path: `${fn}/${i}`, reason: 'node-kind mismatch', expected: c.kind, actual: g.kind },
                };
            }
            if (!payload_equal(c.payload, g.payload)) {
                return {
                    ok: false,
                    mismatch: { functionIndex: fn, path: `${fn}/${i}`, reason: 'payload mismatch', expected: c.payload ?? {}, actual: g.payload ?? {} },
                };
            }
        }
    }
    return { ok: true };
}
