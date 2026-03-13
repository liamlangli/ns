import { AstNode, parse_to_ast } from './interpreter.js';
import { AstNodeRecord, ParserRunResult } from './gpu_parser.js';

export type NormalizedNode = {
    kind: string;
    payload?: Record<string, unknown>;
    children?: NormalizedNode[];
};

export type NormalizedFunctionAst = {
    functionIndex: number;
    nodes: NormalizedNode[];
};

export type AstMismatch = {
    functionIndex: number;
    path: string;
    reason: 'node-kind mismatch' | 'payload mismatch' | 'node-count mismatch';
    expected: unknown;
    actual: unknown;
};

function normalizeLiteral(value: unknown): Record<string, unknown> {
    if (value === null) return { literalType: 'null', value: 'null' };
    if (typeof value === 'number') return { literalType: Number.isInteger(value) ? 'int' : 'float', value: String(value) };
    if (typeof value === 'boolean') return { literalType: 'bool', value: value ? 'true' : 'false' };
    return { literalType: 'string', value: String(value) };
}

function normalizeAstNode(node: unknown): NormalizedNode {
    if (!node || typeof node !== 'object') return { kind: 'Unknown', payload: { value: node } };
    const ast = node as AstNode;
    const kind = String(ast.kind ?? 'Unknown');

    if (kind === 'Lit') return { kind: 'Literal', payload: normalizeLiteral(ast.value) };
    if (kind === 'Ident') return { kind: 'Identifier', payload: { name: String(ast.name ?? '') } };

    const payload: Record<string, unknown> = {};
    const children: NormalizedNode[] = [];

    for (const key of Object.keys(ast).sort()) {
        if (key === 'kind' || key === 'line') continue;
        const value = ast[key];
        if (Array.isArray(value)) {
            for (const child of value) {
                if (child && typeof child === 'object' && 'kind' in (child as object)) children.push(normalizeAstNode(child));
                else children.push({ kind: 'Value', payload: { key, value: child } });
            }
            continue;
        }
        if (value && typeof value === 'object' && 'kind' in (value as object)) {
            children.push(normalizeAstNode(value));
            continue;
        }
        if (key === 'name' || key === 'field' || key === 'mod' || key === 'op' || key === 'type' || key === 'ret_type' || key === 'var') {
            payload[key] = String(value ?? '');
        } else if (value !== undefined) {
            payload[key] = value as unknown;
        }
    }

    const normalized: NormalizedNode = { kind };
    if (Object.keys(payload).length) normalized.payload = payload;
    if (children.length) normalized.children = children;
    return normalized;
}

export function normalize_cpu_ast_by_function(source: string, functionSpans: { start: number; end: number }[]): NormalizedFunctionAst[] {
    return functionSpans.map((span, functionIndex) => {
        const segment = source.slice(span.start, span.end);
        const parsed = parse_to_ast(segment);
        const root = normalizeAstNode(parsed);
        return {
            functionIndex,
            nodes: root.children ?? [root],
        };
    });
}

function gpuKindToString(kind: number): string {
    if (kind === 1) return 'Identifier';
    if (kind === 2) return 'Literal';
    return 'Symbol';
}

function normalizeGpuNode(source: string, node: AstNodeRecord): NormalizedNode {
    const tokenText = source.slice(node.tokenStart, node.tokenStart + node.tokenCount);
    if (node.kind === 1) return { kind: 'Identifier', payload: { name: tokenText } };
    if (node.kind === 2) return { kind: 'Literal', payload: { literalType: 'int', value: tokenText } };
    return { kind: gpuKindToString(node.kind), payload: { token: tokenText } };
}

export function normalize_gpu_ast_from_readback(source: string, result: ParserRunResult): NormalizedFunctionAst[] {
    return result.astByFunction.map((nodes, functionIndex) => {
        const sorted = [...nodes].sort((a, b) => a.tokenStart - b.tokenStart || a.kind - b.kind || a.tokenCount - b.tokenCount);
        return {
            functionIndex,
            nodes: sorted.map((node) => normalizeGpuNode(source, node)),
        };
    });
}

function payloadEqual(a?: Record<string, unknown>, b?: Record<string, unknown>): boolean {
    return JSON.stringify(a ?? {}) === JSON.stringify(b ?? {});
}

export function compare_normalized_asts(cpu: NormalizedFunctionAst[], gpu: NormalizedFunctionAst[]): { ok: boolean; mismatch?: AstMismatch } {
    const fnCount = Math.max(cpu.length, gpu.length);
    for (let fn = 0; fn < fnCount; fn++) {
        const cpuNodes = cpu[fn]?.nodes ?? [];
        const gpuNodes = gpu[fn]?.nodes ?? [];
        const nodeCount = Math.max(cpuNodes.length, gpuNodes.length);
        for (let i = 0; i < nodeCount; i++) {
            const c = cpuNodes[i];
            const g = gpuNodes[i];
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
            if (!payloadEqual(c.payload, g.payload)) {
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
