export interface FunctionSpan {
    byteOffset: number;
    byteLength: number;
}

export interface PreprocessLimits {
    maxSourceBytes: number;
    maxFunctionBytes: number;
    maxTokensPerFunction: number;
    maxAstNodes: number;
}

export const DEFAULT_PREPROCESS_LIMITS: PreprocessLimits = {
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

export interface TokenRecord {
    kind: number;
    flags: number;
    byteOffset: number;
    byteLength: number;
    nodeIndex: number;
}

export interface AstNodeRecord {
    kind: number;
    flags: number;
    tokenStart: number;
    tokenCount: number;
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

struct TokenRecord {
    kind: u32,
    flags: u32,
    byte_offset: u32,
    byte_length: u32,
    node_index: u32,
};

struct AstNodeRecord {
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

function isIdentStart(ch: number): boolean {
    return ch === CH.UNDERSCORE ||
        (ch >= CH.A && ch <= CH.Z) ||
        (ch >= CH.a && ch <= CH.z);
}

function isIdentContinue(ch: number): boolean {
    return isIdentStart(ch) || (ch >= CH.D0 && ch <= CH.D9);
}

function assertU32(value: number, field: string): void {
    if (!Number.isInteger(value) || value < 0 || value > 0xffff_ffff) {
        throw new Error(`${field} must be an unsigned 32-bit integer, got ${value}`);
    }
}

function matchFnKeyword(bytes: Uint8Array, i: number): boolean {
    if (i + 1 >= bytes.length) return false;
    if (bytes[i] !== CH.f || bytes[i + 1] !== CH.n) return false;

    const prev = i > 0 ? bytes[i - 1]! : 0;
    const next = i + 2 < bytes.length ? bytes[i + 2]! : 0;
    const prevOk = i === 0 || !isIdentContinue(prev);
    const nextOk = i + 2 >= bytes.length || !isIdentContinue(next);
    return prevOk && nextOk;
}

function findFunctionEnd(bytes: Uint8Array, fnOffset: number): number {
    let i = fnOffset + 2;
    let braceDepth = 0;
    let seenBodyBrace = false;

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
            seenBodyBrace = true;
            braceDepth++;
        } else if (ch === CH.RBRACE) {
            if (!seenBodyBrace) {
                throw new Error(`Invalid function starting at byte ${fnOffset}: unmatched '}' before body.`);
            }
            braceDepth--;
            if (braceDepth === 0) {
                return i + 1;
            }
        }

        i++;
    }

    throw new Error(`Unterminated function starting at byte ${fnOffset}.`);
}

function scanTopLevelFunctions(bytes: Uint8Array, limits: PreprocessLimits): FunctionSpan[] {
    const spans: FunctionSpan[] = [];
    let i = 0;
    let topLevelBraceDepth = 0;

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
            topLevelBraceDepth++;
            i++;
            continue;
        }
        if (ch === CH.RBRACE) {
            topLevelBraceDepth = Math.max(0, topLevelBraceDepth - 1);
            i++;
            continue;
        }

        if (topLevelBraceDepth === 0 && matchFnKeyword(bytes, i)) {
            const end = findFunctionEnd(bytes, i);
            const byteLength = end - i;
            if (byteLength > limits.maxFunctionBytes) {
                throw new Error(`Function at byte ${i} exceeds maxFunctionBytes (${byteLength} > ${limits.maxFunctionBytes}).`);
            }
            spans.push({ byteOffset: i, byteLength });
            i = end;
            continue;
        }

        i++;
    }

    return spans;
}

export interface PreprocessResult {
    sourceBytes: Uint8Array;
    functionSpans: FunctionSpan[];
    functionSpanTable: Uint32Array;
    limits: PreprocessLimits;
}

export function preprocessSourceForGpu(source: string, customLimits: Partial<PreprocessLimits> = {}): PreprocessResult {
    const limits = { ...DEFAULT_PREPROCESS_LIMITS, ...customLimits };
    const sourceBytes = UTF8.encode(source);

    if (sourceBytes.length > limits.maxSourceBytes) {
        throw new Error(`Source exceeds maxSourceBytes (${sourceBytes.length} > ${limits.maxSourceBytes}).`);
    }

    const functionSpans = scanTopLevelFunctions(sourceBytes, limits);
    const functionSpanTable = new Uint32Array(functionSpans.length * FUNCTION_SPAN_RECORD_WORDS);

    for (let i = 0; i < functionSpans.length; i++) {
        const span = functionSpans[i]!;
        const base = i * FUNCTION_SPAN_RECORD_WORDS;
        assertU32(span.byteOffset, 'function span byteOffset');
        assertU32(span.byteLength, 'function span byteLength');
        functionSpanTable[base] = span.byteOffset;
        functionSpanTable[base + 1] = span.byteLength;
        functionSpanTable[base + 2] = 0;
        functionSpanTable[base + 3] = 0;
    }

    return {
        sourceBytes,
        functionSpans,
        functionSpanTable,
        limits,
    };
}

export function ensureTokenCountWithinLimit(tokenCount: number, limits: PreprocessLimits = DEFAULT_PREPROCESS_LIMITS): void {
    if (tokenCount > limits.maxTokensPerFunction) {
        throw new Error(`Function token count exceeds maxTokensPerFunction (${tokenCount} > ${limits.maxTokensPerFunction}).`);
    }
}

export function ensureAstNodeCountWithinLimit(nodeCount: number, limits: PreprocessLimits = DEFAULT_PREPROCESS_LIMITS): void {
    if (nodeCount > limits.maxAstNodes) {
        throw new Error(`AST node count exceeds maxAstNodes (${nodeCount} > ${limits.maxAstNodes}).`);
    }
}

export function packTokenRecords(records: readonly TokenRecord[]): Uint32Array {
    const out = new Uint32Array(records.length * TOKEN_RECORD_WORDS);
    for (let i = 0; i < records.length; i++) {
        const r = records[i]!;
        const base = i * TOKEN_RECORD_WORDS;
        assertU32(r.kind, 'token.kind');
        assertU32(r.flags, 'token.flags');
        assertU32(r.byteOffset, 'token.byteOffset');
        assertU32(r.byteLength, 'token.byteLength');
        assertU32(r.nodeIndex, 'token.nodeIndex');
        out[base] = r.kind;
        out[base + 1] = r.flags;
        out[base + 2] = r.byteOffset;
        out[base + 3] = r.byteLength;
        out[base + 4] = r.nodeIndex;
    }
    return out;
}

export function packAstNodeRecords(records: readonly AstNodeRecord[]): Uint32Array {
    const out = new Uint32Array(records.length * AST_NODE_RECORD_WORDS);
    for (let i = 0; i < records.length; i++) {
        const r = records[i]!;
        const base = i * AST_NODE_RECORD_WORDS;
        assertU32(r.kind, 'node.kind');
        assertU32(r.flags, 'node.flags');
        assertU32(r.tokenStart, 'node.tokenStart');
        assertU32(r.tokenCount, 'node.tokenCount');
        assertU32(r.firstChild, 'node.firstChild');
        assertU32(r.childCount, 'node.childCount');
        out[base] = r.kind;
        out[base + 1] = r.flags;
        out[base + 2] = r.tokenStart;
        out[base + 3] = r.tokenCount;
        out[base + 4] = r.firstChild;
        out[base + 5] = r.childCount;
    }
    return out;
}
