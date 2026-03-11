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
} as const;

export type TokenType = typeof TT[keyof typeof TT];

export interface Token {
    type: TokenType;
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
];

// ---------- WASM token-type → JS TT mapping ---------------------------------

/** C enum values from ns_token_type (mirrors ns_type.h). */
const C_TOKEN = {
    UNKNOWN: -1, INVALID: 0,
    AS: 1, ANY: 2, ASYNC: 3, ASSERT: 4, AWAIT: 5, BREAK: 6,
    CONST: 7, CONTINUE: 8, COMMENT: 9, DO: 10, LOOP: 11,
    ELSE: 12, FALSE: 13, FOR: 14, TO: 15, IF: 16, USE: 17,
    IN: 18, LET: 19, NIL: 20, MATCH: 21, MODULE: 22,
    RETURN: 23, REF: 24, STRUCT: 25, TRUE: 26, TYPE: 27,
    VERTEX: 28, FRAGMENT: 29, COMPUTE: 30, KERNEL: 31, OPS: 32,
    TYPE_I8: 33, TYPE_I16: 34, TYPE_I32: 35, TYPE_I64: 36,
    TYPE_U8: 37, TYPE_U16: 38, TYPE_U32: 39, TYPE_U64: 40,
    TYPE_F32: 41, TYPE_F64: 42, TYPE_BOOL: 43, TYPE_STR: 44,
    TYPE_ANY: 45, TYPE_VOID: 46,
    INT_LITERAL: 47, FLT_LITERAL: 48,
    STR_LITERAL: 49, STR_FORMAT: 50,
    FN: 51, SPACE: 52, IDENTIFIER: 53,
    COMMA: 54, DOT: 55, ASSIGN: 56, COLON: 57, QUESTION_MARK: 58,
    LOGIC_OP: 59, EQ_OP: 60, REL_OP: 61,
    SHIFT_OP: 62, ADD_OP: 63, MUL_OP: 64,
    CMP_OP: 65, BIT_INVERT_OP: 66,
    BITWISE_OP: 67, ASSIGN_OP: 68,
    OPEN_BRACE: 69, CLOSE_BRACE: 70,
    OPEN_PAREN: 71, CLOSE_PAREN: 72,
    OPEN_BRACKET: 73, CLOSE_BRACKET: 74,
    RETURN_TYPE: 75,
    EOL: 76, EOF: 77,
} as const;

const _tt_map = new Int8Array(128).fill(TT.UNKNOWN);
const _kw: number[] = [
    C_TOKEN.AS, C_TOKEN.ANY, C_TOKEN.ASYNC, C_TOKEN.ASSERT, C_TOKEN.AWAIT, C_TOKEN.BREAK,
    C_TOKEN.CONST, C_TOKEN.CONTINUE, C_TOKEN.DO, C_TOKEN.LOOP, C_TOKEN.ELSE, C_TOKEN.FALSE,
    C_TOKEN.FOR, C_TOKEN.TO, C_TOKEN.IF, C_TOKEN.USE, C_TOKEN.IN, C_TOKEN.LET, C_TOKEN.NIL,
    C_TOKEN.MATCH, C_TOKEN.MODULE, C_TOKEN.RETURN, C_TOKEN.REF, C_TOKEN.STRUCT, C_TOKEN.TRUE,
    C_TOKEN.TYPE, C_TOKEN.VERTEX, C_TOKEN.FRAGMENT, C_TOKEN.COMPUTE, C_TOKEN.KERNEL, C_TOKEN.OPS,
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

interface WasmExports {
    memory:      WebAssembly.Memory;
    get_src_buf: () => number;
    get_tok_buf: () => number;
    tokenize:    (src_len: number) => number;
}

let _wasm: WasmExports | null = null;

async function _load_wasm(): Promise<void> {
    try {
        const resp = await fetch('/public/ns_token.wasm');
        if (!resp.ok) return;
        const buf  = await resp.arrayBuffer();
        const mod  = await WebAssembly.compile(buf);
        const inst = await WebAssembly.instantiate(mod, {});
        const ex   = inst.exports as unknown as WasmExports;
        _wasm = { memory: ex.memory, get_src_buf: ex.get_src_buf, get_tok_buf: ex.get_tok_buf, tokenize: ex.tokenize };
    } catch (e) {
        console.warn('ns_token.wasm unavailable, using JS fallback:', e);
    }
}
_load_wasm();

const _enc = new TextEncoder();

function _wasm_tokenize_line(line: string): Token[] {
    const w       = _wasm!;
    const src_ptr = w.get_src_buf();
    const bytes   = _enc.encode(line);
    const src_len = bytes.length;
    if (src_len === 0) return [];
    new Uint8Array(w.memory.buffer, src_ptr, src_len).set(bytes);
    const count   = w.tokenize(src_len);
    const tok_ptr = w.get_tok_buf();
    const toks    = new Int32Array(w.memory.buffer, tok_ptr, count * 3);
    const result: Token[] = [];
    for (let i = 0; i < count; i++) {
        const c_type = toks[i * 3]!;
        const offset = toks[i * 3 + 1]!;
        const len    = toks[i * 3 + 2]!;
        const tt     = (c_type >= 0 && c_type < _tt_map.length)
            ? (_tt_map[c_type]! as TokenType) : TT.UNKNOWN;
        result.push({ type: tt, text: line.slice(offset, offset + len) });
    }
    return result;
}

// ---------- JS fallback tokenizer -------------------------------------------

const NS_KEYWORDS = new Set([
    'fn','let','return','if','else','for','in','to','type','use','break',
    'continue','as','true','false','struct','nil','loop','do','match','mod',
    'async','await','assert','ref','ops','const','vertex','fragment','compute','kernel',
]);
const NS_TYPES = new Set([
    'i8','u8','i16','u16','i32','u32','i64','u64','f32','f64','bool','str','void','any',
]);

function _js_tokenize_line(line: string): Token[] {
    const tokens: Token[] = [];
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
            const type: TokenType = NS_KEYWORDS.has(word) ? TT.KEYWORD
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

// ---------- public API -------------------------------------------------------

/** Tokenize one line of NS source. Uses WASM when loaded, JS fallback otherwise. */
export function tokenize_line(line: string): Token[] {
    if (_wasm) return _wasm_tokenize_line(line);
    return _js_tokenize_line(line);
}
