// NS language tokenizer — backed by ns_token.wasm (falls back to JS if unavailable)

// token types (JS-side enum)
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
};

// Colors (r, g, b, a) for each token type — dark theme
export const TOKEN_COLOR = [
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

// ---------- WASM token-type → JS TT mapping -----------------------

// Enum values mirror ns_type.h ns_token_type
const C = {
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
};

// Build a lookup array: c_type_value → JS TT value
// (index is the C enum integer)
const _tt_map = new Int8Array(128).fill(TT.UNKNOWN);
const _kw = [
    C.AS, C.ANY, C.ASYNC, C.ASSERT, C.AWAIT, C.BREAK,
    C.CONTINUE, C.DO, C.LOOP, C.ELSE, C.FALSE,
    C.FOR, C.TO, C.IF, C.USE, C.IN, C.LET, C.NIL, C.MATCH,
    C.MODULE, C.RETURN, C.REF, C.STRUCT, C.TRUE, C.TYPE,
    C.VERTEX, C.FRAGMENT, C.KERNEL, C.OPS, C.FN,
];
const _ty = [
    C.TYPE_I8, C.TYPE_I16, C.TYPE_I32, C.TYPE_I64,
    C.TYPE_U8, C.TYPE_U16, C.TYPE_U32, C.TYPE_U64,
    C.TYPE_F32, C.TYPE_F64, C.TYPE_BOOL, C.TYPE_STR,
    C.TYPE_ANY, C.TYPE_VOID,
];
const _op = [
    C.LOGIC_OP, C.EQ_OP, C.REL_OP, C.SHIFT_OP,
    C.ADD_OP, C.MUL_OP, C.CMP_OP, C.BIT_INVERT_OP,
    C.BITWISE_OP, C.ASSIGN_OP, C.ASSIGN, C.RETURN_TYPE,
];
const _pu = [
    C.COMMA, C.DOT, C.COLON, C.QUESTION_MARK,
    C.OPEN_BRACE, C.CLOSE_BRACE, C.OPEN_PAREN, C.CLOSE_PAREN,
    C.OPEN_BRACKET, C.CLOSE_BRACKET,
];
for (const v of _kw) _tt_map[v] = TT.KEYWORD;
for (const v of _ty) _tt_map[v] = TT.TYPE;
for (const v of _op) _tt_map[v] = TT.OPERATOR;
for (const v of _pu) _tt_map[v] = TT.PUNCTUATION;
_tt_map[C.INT_LITERAL]  = TT.NUMBER;
_tt_map[C.FLT_LITERAL]  = TT.NUMBER;
_tt_map[C.STR_LITERAL]  = TT.STRING;
_tt_map[C.STR_FORMAT]   = TT.STRING;
_tt_map[C.COMMENT]      = TT.COMMENT;
_tt_map[C.IDENTIFIER]   = TT.IDENTIFIER;
_tt_map[C.SPACE]        = TT.WHITESPACE;

// ---------- WASM loader -------------------------------------------

let _wasm = null; // { memory, get_src_buf, get_tok_buf, tokenize }

async function _load_wasm() {
    try {
        const resp = await fetch(new URL('../public/ns_token.wasm', import.meta.url));
        if (!resp.ok) return;
        const buf   = await resp.arrayBuffer();
        const mod   = await WebAssembly.compile(buf);
        const inst  = await WebAssembly.instantiate(mod, {});
        const ex    = inst.exports;
        _wasm = {
            memory:      ex.memory,
            get_src_buf: ex.get_src_buf,
            get_tok_buf: ex.get_tok_buf,
            tokenize:    ex.tokenize,
        };
    } catch (e) {
        console.warn('ns_token.wasm unavailable, using JS fallback:', e);
    }
}

// Start loading immediately; callers will use JS fallback until ready.
_load_wasm();

// Text encoder for writing strings into WASM memory
const _enc = new TextEncoder();

// ---------- WASM tokenize_line ------------------------------------

function _wasm_tokenize_line(line) {
    const w = _wasm;
    const src_ptr = w.get_src_buf();
    const bytes   = _enc.encode(line);
    const src_len = bytes.length;
    if (src_len === 0) return [];

    new Uint8Array(w.memory.buffer, src_ptr, src_len).set(bytes);

    const count   = w.tokenize(src_len);
    const tok_ptr = w.get_tok_buf();
    const toks    = new Int32Array(w.memory.buffer, tok_ptr, count * 3);

    const result = [];
    let cursor = 0;
    for (let i = 0; i < count; i++) {
        const c_type = toks[i * 3];
        const offset = toks[i * 3 + 1];
        const len    = toks[i * 3 + 2];
        if (offset > cursor) {
            result.push({ type: TT.WHITESPACE, text: line.slice(cursor, offset) });
        }
        const tt     = (c_type >= 0 && c_type < _tt_map.length)
            ? _tt_map[c_type] : TT.UNKNOWN;
        result.push({ type: tt, text: line.slice(offset, offset + len) });
        cursor = offset + len;
    }
    if (cursor < line.length) {
        result.push({ type: TT.WHITESPACE, text: line.slice(cursor) });
    }
    return result;
}

// ---------- JS fallback tokenize_line (kept for when WASM not ready)

const NS_KEYWORDS = new Set([
    'fn', 'let', 'return', 'if', 'else', 'for', 'in', 'to',
    'type', 'use', 'break', 'continue', 'as', 'true', 'false',
    'struct', 'enum', 'import', 'pub', 'nil', 'loop', 'do',
    'match', 'mod', 'async', 'await', 'assert', 'ref', 'ops',
    'vertex', 'fragment', 'kernel',
]);

const NS_TYPES = new Set([
    'i8', 'u8', 'i16', 'u16', 'i32', 'u32', 'i64', 'u64',
    'f32', 'f64', 'bool', 'str', 'void', 'any',
]);

function _js_tokenize_line(line) {
    const tokens = [];
    let i = 0;
    const n = line.length;

    while (i < n) {
        const c = line[i];

        if (c === ' ' || c === '\t') {
            let j = i + 1;
            while (j < n && (line[j] === ' ' || line[j] === '\t')) j++;
            tokens.push({ type: TT.WHITESPACE, text: line.slice(i, j) });
            i = j; continue;
        }

        if (c === '/' && line[i + 1] === '/') {
            tokens.push({ type: TT.COMMENT, text: line.slice(i) });
            break;
        }

        if (c === '"' || c === '`' || c === "'") {
            const quote = c;
            let j = i + 1;
            while (j < n && line[j] !== quote) { if (line[j] === '\\') j++; j++; }
            if (j < n) j++;
            tokens.push({ type: TT.STRING, text: line.slice(i, j) });
            i = j; continue;
        }

        if (c >= '0' && c <= '9') {
            let j = i + 1;
            while (j < n && (
                (line[j] >= '0' && line[j] <= '9') ||
                line[j] === '.' || line[j] === '_' ||
                line[j] === 'x' || line[j] === 'X' ||
                (line[j] >= 'a' && line[j] <= 'f') ||
                (line[j] >= 'A' && line[j] <= 'F')
            )) j++;
            tokens.push({ type: TT.NUMBER, text: line.slice(i, j) });
            i = j; continue;
        }

        if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c === '_') {
            let j = i + 1;
            while (j < n && (
                (line[j] >= 'a' && line[j] <= 'z') ||
                (line[j] >= 'A' && line[j] <= 'Z') ||
                (line[j] >= '0' && line[j] <= '9') ||
                line[j] === '_'
            )) j++;
            const word = line.slice(i, j);
            let type = TT.IDENTIFIER;
            if (NS_KEYWORDS.has(word)) type = TT.KEYWORD;
            else if (NS_TYPES.has(word)) type = TT.TYPE;
            tokens.push({ type, text: word });
            i = j; continue;
        }

        const ops2 = ['==', '!=', '<=', '>=', '->', '=>', '&&', '||', '::', '<<', '>>'];
        let matched = false;
        for (const op of ops2) {
            if (line.startsWith(op, i)) {
                tokens.push({ type: TT.OPERATOR, text: op });
                i += op.length; matched = true; break;
            }
        }
        if (matched) continue;

        if ('+-*/%=<>!&|^~'.includes(c)) {
            tokens.push({ type: TT.OPERATOR, text: c }); i++; continue;
        }
        if ('(){}[].,;:?'.includes(c)) {
            tokens.push({ type: TT.PUNCTUATION, text: c }); i++; continue;
        }

        tokens.push({ type: TT.UNKNOWN, text: c }); i++;
    }
    return tokens;
}

// ---------- public API --------------------------------------------

/**
 * Tokenize a single line of NS source code.
 * Uses the WASM tokenizer when loaded, JS fallback otherwise.
 * Returns Array<{ type: TT, text: string }>.
 */
export function tokenize_line(line) {
    if (_wasm) return _wasm_tokenize_line(line);
    return _js_tokenize_line(line);
}
