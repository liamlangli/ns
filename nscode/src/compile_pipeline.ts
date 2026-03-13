export interface function_span {
    byteOffset: number;
    byte_length: number;
}

export interface preprocess_limits {
    maxSourceBytes: number;
    maxFunctionBytes: number;
    maxTokensPerFunction: number;
    maxAstNodes: number;
}

export const DEFAULT_PREPROCESS_LIMITS: preprocess_limits = {
    maxSourceBytes: 8 * 1024 * 1024,
    maxFunctionBytes: 512 * 1024,
    maxTokensPerFunction: 16_384,
    maxAstNodes: 65_536,
};

export const FUNCTION_SPAN_RECORD_WORDS = 4;
export const FUNCTION_SPAN_RECORD_BYTES = FUNCTION_SPAN_RECORD_WORDS * 4;
export const TOKEN_RECORD_WORDS = 5;
export const TOKEN_RECORD_BYTES = TOKEN_RECORD_WORDS * 4;
export const AST_NODE_RECORD_WORDS = 6;
export const AST_NODE_RECORD_BYTES = AST_NODE_RECORD_WORDS * 4;

export interface token_record {
    kind: number;
    flags: number;
    byteOffset: number;
    byte_length: number;
    nodeIndex: number;
}

export interface ast_node_record {
    kind: number;
    flags: number;
    tokenStart: number;
    token_count: number;
    firstChild: number;
    childCount: number;
}

/**
 * WGSL struct definitions and field order shared with TypeScript packers below.
 */
export const COMPILE_RECORD_LAYOUT_WGSL = /* wgsl */`
struct FunctionSpanRecord {
    byte_offset: u32,
    byte_length: u32,
    token_start: u32,
    token_count: u32,
};

struct token_record {
    kind: u32,
    flags: u32,
    byte_offset: u32,
    byte_length: u32,
    node_index: u32,
};

struct ast_node_record {
    kind: u32,
    flags: u32,
    token_start: u32,
    token_count: u32,
    first_child: u32,
    child_count: u32,
};
`;

const UTF8 = new TextEncoder();

const CH = {
    LBRACE: 123,
    RBRACE: 125,
    LPAREN: 40,
    RPAREN: 41,
    SLASH: 47,
    STAR: 42,
    DQUOTE: 34,
    SQUOTE: 39,
    BACKTICK: 96,
    BACKSLASH: 92,
    UNDERSCORE: 95,
    D0: 48,
    D9: 57,
    A: 65,
    Z: 90,
    a: 97,
    z: 122,
    f: 102,
    n: 110,
} as const;

function is_ident_start(ch: number): boolean {
    return ch === CH.UNDERSCORE ||
        (ch >= CH.A && ch <= CH.Z) ||
        (ch >= CH.a && ch <= CH.z);
}

function is_ident_continue(ch: number): boolean {
    return is_ident_start(ch) || (ch >= CH.D0 && ch <= CH.D9);
}

function assert_u32(value: number, field: string): void {
    if (!Number.isInteger(value) || value < 0 || value > 0xffff_ffff) {
        throw new Error(`${field} must be an unsigned 32-bit integer, got ${value}`);
    }
}

function match_fn_keyword(bytes: Uint8Array, i: number): boolean {
    if (i + 1 >= bytes.length) return false;
    if (bytes[i] !== CH.f || bytes[i + 1] !== CH.n) return false;

    const prev = i > 0 ? bytes[i - 1]! : 0;
    const next = i + 2 < bytes.length ? bytes[i + 2]! : 0;
    const prev_ok = i === 0 || !is_ident_continue(prev);
    const next_ok = i + 2 >= bytes.length || !is_ident_continue(next);
    return prev_ok && next_ok;
}

function find_function_end(bytes: Uint8Array, fnOffset: number): number {
    let i = fnOffset + 2;
    let brace_depth = 0;
    let seen_body_brace = false;

    while (i < bytes.length) {
        const ch = bytes[i]!;
        const next = i + 1 < bytes.length ? bytes[i + 1]! : 0;

        if (ch === CH.SLASH && next === CH.SLASH) {
            i += 2;
            while (i < bytes.length && bytes[i] !== 10) i++;
            continue;
        }
        if (ch === CH.SLASH && next === CH.STAR) {
            i += 2;
            while (i + 1 < bytes.length && !(bytes[i] === CH.STAR && bytes[i + 1] === CH.SLASH)) i++;
            i += 2;
            continue;
        }
        if (ch === CH.DQUOTE || ch === CH.SQUOTE || ch === CH.BACKTICK) {
            const quote = ch;
            i++;
            while (i < bytes.length) {
                const s = bytes[i]!;
                if (s === CH.BACKSLASH) {
                    i += 2;
                    continue;
                }
                if (s === quote) {
                    i++;
                    break;
                }
                i++;
            }
            continue;
        }

        if (ch === CH.LBRACE) {
            seen_body_brace = true;
            brace_depth++;
        } else if (ch === CH.RBRACE) {
            if (!seen_body_brace) {
                throw new Error(`Invalid function starting at byte ${fnOffset}: unmatched '}' before body.`);
            }
            brace_depth--;
            if (brace_depth === 0) {
                return i + 1;
            }
        }

        i++;
    }

    throw new Error(`Unterminated function starting at byte ${fnOffset}.`);
}

function scan_top_level_functions(bytes: Uint8Array, limits: preprocess_limits): function_span[] {
    const spans: function_span[] = [];
    let i = 0;
    let top_level_brace_depth = 0;

    while (i < bytes.length) {
        const ch = bytes[i]!;
        const next = i + 1 < bytes.length ? bytes[i + 1]! : 0;

        if (ch === CH.SLASH && next === CH.SLASH) {
            i += 2;
            while (i < bytes.length && bytes[i] !== 10) i++;
            continue;
        }
        if (ch === CH.SLASH && next === CH.STAR) {
            i += 2;
            while (i + 1 < bytes.length && !(bytes[i] === CH.STAR && bytes[i + 1] === CH.SLASH)) i++;
            i += 2;
            continue;
        }
        if (ch === CH.DQUOTE || ch === CH.SQUOTE || ch === CH.BACKTICK) {
            const quote = ch;
            i++;
            while (i < bytes.length) {
                const s = bytes[i]!;
                if (s === CH.BACKSLASH) {
                    i += 2;
                    continue;
                }
                if (s === quote) {
                    i++;
                    break;
                }
                i++;
            }
            continue;
        }

        if (ch === CH.LBRACE) {
            top_level_brace_depth++;
            i++;
            continue;
        }
        if (ch === CH.RBRACE) {
            top_level_brace_depth = Math.max(0, top_level_brace_depth - 1);
            i++;
            continue;
        }

        if (top_level_brace_depth === 0 && match_fn_keyword(bytes, i)) {
            const end = find_function_end(bytes, i);
            const byte_length = end - i;
            if (byte_length > limits.maxFunctionBytes) {
                throw new Error(`Function at byte ${i} exceeds maxFunctionBytes (${byte_length} > ${limits.maxFunctionBytes}).`);
            }
            spans.push({ byteOffset: i, byte_length });
            i = end;
            continue;
        }

        i++;
    }

    return spans;
}

export interface preprocess_result {
    source_bytes: Uint8Array;
    function_spans: function_span[];
    function_span_table: Uint32Array;
    limits: preprocess_limits;
}

export function preprocess_source_for_gpu(source: string, customLimits: Partial<preprocess_limits> = {}): preprocess_result {
    const limits = { ...DEFAULT_PREPROCESS_LIMITS, ...customLimits };
    const source_bytes = UTF8.encode(source);

    if (source_bytes.length > limits.maxSourceBytes) {
        throw new Error(`Source exceeds maxSourceBytes (${source_bytes.length} > ${limits.maxSourceBytes}).`);
    }

    const function_spans = scan_top_level_functions(source_bytes, limits);
    const function_span_table = new Uint32Array(function_spans.length * FUNCTION_SPAN_RECORD_WORDS);

    for (let i = 0; i < function_spans.length; i++) {
        const span = function_spans[i]!;
        const base = i * FUNCTION_SPAN_RECORD_WORDS;
        assert_u32(span.byteOffset, 'function span byteOffset');
        assert_u32(span.byte_length, 'function span byte_length');
        function_span_table[base] = span.byteOffset;
        function_span_table[base + 1] = span.byte_length;
        function_span_table[base + 2] = 0;
        function_span_table[base + 3] = 0;
    }

    return {
        source_bytes,
        function_spans,
        function_span_table,
        limits,
    };
}

export function ensure_token_count_within_limit(token_count: number, limits: preprocess_limits = DEFAULT_PREPROCESS_LIMITS): void {
    if (token_count > limits.maxTokensPerFunction) {
        throw new Error(`Function token count exceeds maxTokensPerFunction (${token_count} > ${limits.maxTokensPerFunction}).`);
    }
}

export function ensure_ast_node_count_within_limit(node_count: number, limits: preprocess_limits = DEFAULT_PREPROCESS_LIMITS): void {
    if (node_count > limits.maxAstNodes) {
        throw new Error(`AST node count exceeds maxAstNodes (${node_count} > ${limits.maxAstNodes}).`);
    }
}

export function pack_token_records(records: readonly token_record[]): Uint32Array {
    const out = new Uint32Array(records.length * TOKEN_RECORD_WORDS);
    for (let i = 0; i < records.length; i++) {
        const r = records[i]!;
        const base = i * TOKEN_RECORD_WORDS;
        assert_u32(r.kind, 'token.kind');
        assert_u32(r.flags, 'token.flags');
        assert_u32(r.byteOffset, 'token.byteOffset');
        assert_u32(r.byte_length, 'token.byte_length');
        assert_u32(r.nodeIndex, 'token.nodeIndex');
        out[base] = r.kind;
        out[base + 1] = r.flags;
        out[base + 2] = r.byteOffset;
        out[base + 3] = r.byte_length;
        out[base + 4] = r.nodeIndex;
    }
    return out;
}

export function pack_ast_node_records(records: readonly ast_node_record[]): Uint32Array {
    const out = new Uint32Array(records.length * AST_NODE_RECORD_WORDS);
    for (let i = 0; i < records.length; i++) {
        const r = records[i]!;
        const base = i * AST_NODE_RECORD_WORDS;
        assert_u32(r.kind, 'node.kind');
        assert_u32(r.flags, 'node.flags');
        assert_u32(r.tokenStart, 'node.tokenStart');
        assert_u32(r.token_count, 'node.token_count');
        assert_u32(r.firstChild, 'node.firstChild');
        assert_u32(r.childCount, 'node.childCount');
        out[base] = r.kind;
        out[base + 1] = r.flags;
        out[base + 2] = r.tokenStart;
        out[base + 3] = r.token_count;
        out[base + 4] = r.firstChild;
        out[base + 5] = r.childCount;
    }
    return out;
}
