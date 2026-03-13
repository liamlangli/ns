import { ast_node, parse_to_ast } from './interpreter.js';
import { ast_node_record, parser_run_result } from './gpu_parser.js';

export type normalized_node = {
    kind: string;
    payload?: Record<string, unknown>;
    children?: normalized_node[];
};

export type normalized_function_ast = {
    functionIndex: number;
    nodes: normalized_node[];
};

export type ast_mismatch = {
    functionIndex: number;
    path: string;
    reason: 'node-kind mismatch' | 'payload mismatch' | 'node-count mismatch';
    expected: unknown;
    actual: unknown;
};

function normalize_literal(value: unknown): Record<string, unknown> {
    if (value === null) return { literalType: 'null', value: 'null' };
    if (typeof value === 'number') return { literalType: Number.isInteger(value) ? 'int' : 'float', value: String(value) };
    if (typeof value === 'boolean') return { literalType: 'bool', value: value ? 'true' : 'false' };
    return { literalType: 'string', value: String(value) };
}

function normalize_ast_node(node: unknown): normalized_node {
    if (!node || typeof node !== 'object') return { kind: 'Unknown', payload: { value: node } };
    const ast = node as ast_node;
    const kind = String(ast.kind ?? 'Unknown');

    if (kind === 'Lit') return { kind: 'Literal', payload: normalize_literal(ast.value) };
    if (kind === 'Ident') return { kind: 'Identifier', payload: { name: String(ast.name ?? '') } };

    const payload: Record<string, unknown> = {};
    const children: normalized_node[] = [];

    for (const key of Object.keys(ast).sort()) {
        if (key === 'kind' || key === 'line') continue;
        const value = ast[key];
        if (Array.isArray(value)) {
            for (const child of value) {
                if (child && typeof child === 'object' && 'kind' in (child as object)) children.push(normalize_ast_node(child));
                else children.push({ kind: 'Value', payload: { key, value: child } });
            }
            continue;
        }
        if (value && typeof value === 'object' && 'kind' in (value as object)) {
            children.push(normalize_ast_node(value));
            continue;
        }
        if (key === 'name' || key === 'field' || key === 'mod' || key === 'op' || key === 'type' || key === 'ret_type' || key === 'var') {
            payload[key] = String(value ?? '');
        } else if (value !== undefined) {
            payload[key] = value as unknown;
        }
    }

    const normalized: normalized_node = { kind };
    if (Object.keys(payload).length) normalized.payload = payload;
    if (children.length) normalized.children = children;
    return normalized;
}

export function normalize_cpu_ast_by_function(source: string, function_spans: { start: number; end: number }[]): normalized_function_ast[] {
    return function_spans.map((span, functionIndex) => {
        const segment = source.slice(span.start, span.end);
        const parsed = parse_to_ast(segment);
        const root = normalize_ast_node(parsed);
        return {
            functionIndex,
            nodes: root.children ?? [root],
        };
    });
}

function gpu_kind_to_string(kind: number): string {
    if (kind === 1) return 'Identifier';
    if (kind === 2) return 'Literal';
    return 'Symbol';
}

function normalize_gpu_node(source: string, node: ast_node_record): normalized_node {
    const token_text = source.slice(node.tokenStart, node.tokenStart + node.token_count);
    if (node.kind === 1) return { kind: 'Identifier', payload: { name: token_text } };
    if (node.kind === 2) return { kind: 'Literal', payload: { literalType: 'int', value: token_text } };
    return { kind: gpu_kind_to_string(node.kind), payload: { token: token_text } };
}

export function normalize_gpu_ast_from_readback(source: string, result: parser_run_result): normalized_function_ast[] {
    return result.ast_by_function.map((nodes, functionIndex) => {
        const sorted = [...nodes].sort((a, b) => a.tokenStart - b.tokenStart || a.kind - b.kind || a.token_count - b.token_count);
        return {
            functionIndex,
            nodes: sorted.map((node) => normalize_gpu_node(source, node)),
        };
    });
}

function payload_equal(a?: Record<string, unknown>, b?: Record<string, unknown>): boolean {
    return JSON.stringify(a ?? {}) === JSON.stringify(b ?? {});
}

export function compare_normalized_asts(cpu: normalized_function_ast[], gpu: normalized_function_ast[]): { ok: boolean; mismatch?: ast_mismatch } {
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
                    mismatch: {
                        functionIndex: fn,
                        path: `${fn}/${i}`,
                        reason: 'node-count mismatch',
                        expected: c ?? null,
                        actual: g ?? null,
                    },
                };
            }
            if (c.kind !== g.kind) {
                return {
                    ok: false,
                    mismatch: {
                        functionIndex: fn,
                        path: `${fn}/${i}`,
                        reason: 'node-kind mismatch',
                        expected: c.kind,
                        actual: g.kind,
                    },
                };
            }
            if (!payload_equal(c.payload, g.payload)) {
                return {
                    ok: false,
                    mismatch: {
                        functionIndex: fn,
                        path: `${fn}/${i}`,
                        reason: 'payload mismatch',
                        expected: c.payload ?? {},
                        actual: g.payload ?? {},
                    },
                };
            }
        }
    }
    return { ok: true };
}
