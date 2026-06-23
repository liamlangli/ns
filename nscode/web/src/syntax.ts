// NS language tokenizer — backed by ns_token.wasm (falls back to JS if unavailable)

export const TT = {
    KEYWORD:     0,
    TYPE:        1,
    NUMBER:      2,
    STRING:      3,
    COMMENT:     4,
    OPERATOR:    5,
    IDENTIFIER:  6,
    PUNCTUATION: 7,
    WHITESPACE:  8,
    UNKNOWN:     9,
    FUNC_DEF:    10,
    FUNC_CALL:   11,
} as const;

export type token_type = typeof TT[keyof typeof TT];

export interface token {
    type: token_type;
    text: string;
}

// Colors (r, g, b, a) for each token type — dark theme
export const TOKEN_COLOR: readonly [number, number, number, number][] = [
    [0.56, 0.74, 0.99, 1.0],  // KEYWORD    — blue
    [0.42, 0.88, 0.78, 1.0],  // TYPE       — teal
    [0.90, 0.75, 0.49, 1.0],  // NUMBER     — orange
    [0.72, 0.90, 0.60, 1.0],  // STRING     — green
    [0.50, 0.50, 0.50, 1.0],  // COMMENT    — gray
    [0.86, 0.60, 0.80, 1.0],  // OPERATOR   — pink
    [0.92, 0.92, 0.92, 1.0],  // IDENTIFIER — white
    [0.70, 0.70, 0.70, 1.0],  // PUNCTUATION— light gray
    [0.00, 0.00, 0.00, 0.0],  // WHITESPACE — transparent
    [0.92, 0.92, 0.92, 1.0],  // UNKNOWN    — white
    [0.86, 0.86, 0.67, 1.0],  // FUNC_DEF   — golden yellow
    [0.67, 0.86, 0.86, 1.0],  // FUNC_CALL  — light cyan
];

// ---------- WASM token-type → JS TT mapping ---------------------------------

/** C enum values from ns_token_type (mirrors ns_type.h). */
const C_TOKEN = {
    UNKNOWN: -1, INVALID: 0,
    AS: 1, ANY: 2, ASYNC: 3, ASSERT: 4, AWAIT: 5, BREAK: 6,
    CONTINUE: 7, COMMENT: 8, DO: 9, LOOP: 10,
    ELSE: 11, FALSE: 12, FOR: 13, TO: 14, IF: 15, USE: 16,
    IN: 17, LET: 18, NIL: 19, MATCH: 20, MODULE: 21,
    RETURN: 22, REF: 23, STRUCT: 24, TRUE: 25, TYPE: 26,
    OPS: 27,
    TYPE_I8: 28, TYPE_I16: 29, TYPE_I32: 30, TYPE_I64: 31,
    TYPE_U8: 32, TYPE_U16: 33, TYPE_U32: 34, TYPE_U64: 35,
    TYPE_F32: 36, TYPE_F64: 37, TYPE_BOOL: 38, TYPE_STR: 39,
    TYPE_ANY: 40, TYPE_VOID: 41,
    INT_LITERAL: 42, FLT_LITERAL: 43,
    STR_LITERAL: 44, STR_FORMAT: 45,
    FN: 46, SPACE: 47, IDENTIFIER: 48,
    COMMA: 49, DOT: 50, ASSIGN: 51, COLON: 52, QUESTION_MARK: 53,
    LOGIC_OP: 54, EQ_OP: 55, REL_OP: 56,
    SHIFT_OP: 57, ADD_OP: 58, MUL_OP: 59,
    CMP_OP: 60, BIT_INVERT_OP: 61,
    BITWISE_OP: 62, ASSIGN_OP: 63,
    OPEN_BRACE: 64, CLOSE_BRACE: 65,
    OPEN_PAREN: 66, CLOSE_PAREN: 67,
    OPEN_BRACKET: 68, CLOSE_BRACKET: 69,
    RETURN_TYPE: 70,
    EOL: 71, EOF: 72,
    KERNEL: 73,
} as const;

const _tt_map = new Int8Array(128).fill(TT.UNKNOWN);
const _kw: number[] = [
    C_TOKEN.AS, C_TOKEN.ANY, C_TOKEN.ASYNC, C_TOKEN.ASSERT, C_TOKEN.AWAIT, C_TOKEN.BREAK,
    C_TOKEN.CONTINUE, C_TOKEN.DO, C_TOKEN.LOOP, C_TOKEN.ELSE, C_TOKEN.FALSE,
    C_TOKEN.FOR, C_TOKEN.TO, C_TOKEN.IF, C_TOKEN.USE, C_TOKEN.IN, C_TOKEN.LET, C_TOKEN.NIL,
    C_TOKEN.MATCH, C_TOKEN.MODULE, C_TOKEN.RETURN, C_TOKEN.REF, C_TOKEN.STRUCT, C_TOKEN.TRUE,
    C_TOKEN.TYPE, C_TOKEN.KERNEL, C_TOKEN.OPS,
    C_TOKEN.FN,
];
const _ty: number[] = [
    C_TOKEN.TYPE_I8, C_TOKEN.TYPE_I16, C_TOKEN.TYPE_I32, C_TOKEN.TYPE_I64,
    C_TOKEN.TYPE_U8,  C_TOKEN.TYPE_U16, C_TOKEN.TYPE_U32, C_TOKEN.TYPE_U64,
    C_TOKEN.TYPE_F32, C_TOKEN.TYPE_F64, C_TOKEN.TYPE_BOOL, C_TOKEN.TYPE_STR,
    C_TOKEN.TYPE_ANY, C_TOKEN.TYPE_VOID,
];
const _op: number[] = [
    C_TOKEN.LOGIC_OP, C_TOKEN.EQ_OP, C_TOKEN.REL_OP, C_TOKEN.SHIFT_OP,
    C_TOKEN.ADD_OP,   C_TOKEN.MUL_OP, C_TOKEN.CMP_OP, C_TOKEN.BIT_INVERT_OP,
    C_TOKEN.BITWISE_OP, C_TOKEN.ASSIGN_OP, C_TOKEN.ASSIGN, C_TOKEN.RETURN_TYPE,
];
const _pu: number[] = [
    C_TOKEN.COMMA, C_TOKEN.DOT, C_TOKEN.COLON, C_TOKEN.QUESTION_MARK,
    C_TOKEN.OPEN_BRACE, C_TOKEN.CLOSE_BRACE, C_TOKEN.OPEN_PAREN, C_TOKEN.CLOSE_PAREN,
    C_TOKEN.OPEN_BRACKET, C_TOKEN.CLOSE_BRACKET,
];
for (const v of _kw) _tt_map[v] = TT.KEYWORD;
for (const v of _ty) _tt_map[v] = TT.TYPE;
for (const v of _op) _tt_map[v] = TT.OPERATOR;
for (const v of _pu) _tt_map[v] = TT.PUNCTUATION;
_tt_map[C_TOKEN.INT_LITERAL]  = TT.NUMBER;
_tt_map[C_TOKEN.FLT_LITERAL]  = TT.NUMBER;
_tt_map[C_TOKEN.STR_LITERAL]  = TT.STRING;
_tt_map[C_TOKEN.STR_FORMAT]   = TT.STRING;
_tt_map[C_TOKEN.COMMENT]      = TT.COMMENT;
_tt_map[C_TOKEN.IDENTIFIER]   = TT.IDENTIFIER;
_tt_map[C_TOKEN.SPACE]        = TT.WHITESPACE;

// ---------- WASM loader -----------------------------------------------------

interface wasm_exports {
    memory:      WebAssembly.Memory;
    get_src_buf: () => number;
    get_tok_buf: () => number;
    tokenize:    (src_len: number) => number;
}

let _wasm: wasm_exports | null = null;

function _wasm_contract_ok(w: wasm_exports): boolean {
    const probe = 'vertex fragment fn';
    const bytes = new TextEncoder().encode(probe);
    const src_ptr = w.get_src_buf();
    new Uint8Array(w.memory.buffer, src_ptr, bytes.length).set(bytes);
    const count = w.tokenize(bytes.length);
    const tok_ptr = w.get_tok_buf();
    const toks = new Int32Array(w.memory.buffer, tok_ptr, count * 3);
    const words: { type: number; text: string }[] = [];
    for (let i = 0; i < count; i++) {
        const type = toks[i * 3]!;
        const offset = toks[i * 3 + 1]!;
        const len = toks[i * 3 + 2]!;
        const text = probe.slice(offset, offset + len);
        if (text.trim().length > 0) words.push({ type, text });
    }
    return words.length >= 3
        && words[0]!.text === 'vertex' && words[0]!.type === C_TOKEN.IDENTIFIER
        && words[1]!.text === 'fragment' && words[1]!.type === C_TOKEN.IDENTIFIER
        && words[2]!.text === 'fn' && words[2]!.type === C_TOKEN.FN;
}

async function _load_wasm(): Promise<void> {
    try {
        const resp = await fetch(new URL('../public/ns_token.wasm', import.meta.url));
        if (!resp.ok) return;
        const buf  = await resp.arrayBuffer();
        const mod  = await WebAssembly.compile(buf);
        const inst = await WebAssembly.instantiate(mod, {});
        const ex   = inst.exports as unknown as wasm_exports;
        if (!_wasm_contract_ok(ex)) {
            throw new Error('ns_token.wasm token ids are stale');
        }
        _wasm = { memory: ex.memory, get_src_buf: ex.get_src_buf, get_tok_buf: ex.get_tok_buf, tokenize: ex.tokenize };
    } catch (e) {
        console.warn('ns_token.wasm unavailable, using JS fallback:', e);
    }
}
_load_wasm();

const _enc = new TextEncoder();

function _wasm_tokenize_line(line: string): token[] {
    const w       = _wasm!;
    const src_ptr = w.get_src_buf();
    const bytes   = _enc.encode(line);
    const src_len = bytes.length;
    if (src_len === 0) return [];
    new Uint8Array(w.memory.buffer, src_ptr, src_len).set(bytes);
    const count   = w.tokenize(src_len);
    const tok_ptr = w.get_tok_buf();
    const toks    = new Int32Array(w.memory.buffer, tok_ptr, count * 3);
    const result: token[] = [];
    let cursor = 0;
    for (let i = 0; i < count; i++) {
        const c_type = toks[i * 3]!;
        const offset = toks[i * 3 + 1]!;
        const len    = toks[i * 3 + 2]!;
        if (offset > cursor) {
            result.push({ type: TT.WHITESPACE, text: line.slice(cursor, offset) });
        }
        const tt     = (c_type >= 0 && c_type < _tt_map.length)
            ? (_tt_map[c_type]! as token_type) : TT.UNKNOWN;
        result.push({ type: tt, text: line.slice(offset, offset + len) });
        cursor = offset + len;
    }
    if (cursor < line.length) {
        result.push({ type: TT.WHITESPACE, text: line.slice(cursor) });
    }
    return result;
}

// ---------- JS fallback tokenizer -------------------------------------------

const NS_KEYWORDS = new Set([
    'fn','let','return','if','else','for','in','to','type','use','break',
    'continue','as','true','false','struct','nil','loop','do','match','mod',
    'async','await','assert','ref','ops','kernel',
]);
const NS_TYPES = new Set([
    'i8','u8','i16','u16','i32','u32','i64','u64','f32','f64','bool','str','void','any',
]);

function _js_tokenize_line(line: string): token[] {
    const tokens: token[] = [];
    let i = 0;
    const n = line.length;
    while (i < n) {
        const c = line[i]!;
        if (c === ' ' || c === '\t') {
            let j = i + 1;
            while (j < n && (line[j] === ' ' || line[j] === '\t')) j++;
            tokens.push({ type: TT.WHITESPACE, text: line.slice(i, j) });
            i = j; continue;
        }
        if (c === '/' && line[i + 1] === '/') {
            tokens.push({ type: TT.COMMENT, text: line.slice(i) }); break;
        }
        if (c === '"' || c === '`' || c === "'") {
            const quote = c; let j = i + 1;
            while (j < n && line[j] !== quote) { if (line[j] === '\\') j++; j++; }
            if (j < n) j++;
            tokens.push({ type: TT.STRING, text: line.slice(i, j) });
            i = j; continue;
        }
        if (c >= '0' && c <= '9') {
            let j = i + 1;
            while (j < n && (/[\da-fA-F._xX]/).test(line[j]!)) j++;
            tokens.push({ type: TT.NUMBER, text: line.slice(i, j) });
            i = j; continue;
        }
        if (/[a-zA-Z_]/.test(c)) {
            let j = i + 1;
            while (j < n && /[\w]/.test(line[j]!)) j++;
            const word = line.slice(i, j);
            const type: token_type = NS_KEYWORDS.has(word) ? TT.KEYWORD
                        : NS_TYPES.has(word)    ? TT.TYPE : TT.IDENTIFIER;
            tokens.push({ type, text: word });
            i = j; continue;
        }
        const ops2 = ['==','!=','<=','>=','->','=>','&&','||','::','<<','>>'];
        let matched = false;
        for (const op of ops2) {
            if (line.startsWith(op, i)) {
                tokens.push({ type: TT.OPERATOR, text: op });
                i += op.length; matched = true; break;
            }
        }
        if (matched) continue;
        if ('+-*/%=<>!&|^~'.includes(c)) { tokens.push({ type: TT.OPERATOR,    text: c }); i++; continue; }
        if ('(){}[].,;:?'.includes(c))   { tokens.push({ type: TT.PUNCTUATION, text: c }); i++; continue; }
        tokens.push({ type: TT.UNKNOWN, text: c }); i++;
    }
    return tokens;
}

// ---------- function-name classifier ----------------------------------------

/**
 * Post-processing pass: reclassifies IDENTIFIER tokens as FUNC_DEF or FUNC_CALL.
 * - FUNC_DEF: identifier immediately after the `fn` keyword
 * - FUNC_CALL: identifier followed by `(`
 */
function _classify_functions(tokens: token[]): token[] {
    const result = tokens.slice();
    const n = result.length;

    const prev_real = (i: number): number => {
        for (let j = i - 1; j >= 0; j--) {
            if (result[j]!.type !== TT.WHITESPACE) return j;
        }
        return -1;
    };

    const next_real = (i: number): number => {
        for (let j = i + 1; j < n; j++) {
            if (result[j]!.type !== TT.WHITESPACE) return j;
        }
        return -1;
    };

    for (let i = 0; i < n; i++) {
        const tok = result[i]!;
        if (tok.type !== TT.IDENTIFIER) continue;

        const pi = prev_real(i);
        const ni = next_real(i);
        const prev_tok = pi >= 0 ? result[pi] : null;
        const next_tok = ni >= 0 ? result[ni] : null;

        const after_fn     = prev_tok?.type === TT.KEYWORD && prev_tok.text === 'fn';
        const before_paren = next_tok?.type === TT.PUNCTUATION && next_tok.text === '(';

        if (after_fn) {
            result[i] = { type: TT.FUNC_DEF, text: tok.text };
        } else if (before_paren) {
            result[i] = { type: TT.FUNC_CALL, text: tok.text };
        }
    }

    return result;
}

// ---------- public API -------------------------------------------------------

/** Tokenize one line of NS source. Uses WASM when loaded, JS fallback otherwise. */
export function tokenize_line(line: string): token[] {
    const raw = _wasm ? _wasm_tokenize_line(line) : _js_tokenize_line(line);
    return _classify_functions(raw);
}
