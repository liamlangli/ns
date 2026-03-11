// NS language tokenizer for syntax highlighting

const NS_KEYWORDS = new Set([
    'fn', 'let', 'return', 'if', 'else', 'for', 'in', 'to',
    'type', 'use', 'break', 'continue', 'as', 'true', 'false',
    'struct', 'enum', 'import', 'pub',
]);

const NS_TYPES = new Set([
    'i8', 'u8', 'i16', 'u16', 'i32', 'u32', 'i64', 'u64',
    'f32', 'f64', 'bool', 'str', 'void', 'unknown',
]);

// Token types
export const TT = {
    KEYWORD:    0,
    TYPE:       1,
    NUMBER:     2,
    STRING:     3,
    COMMENT:    4,
    OPERATOR:   5,
    IDENTIFIER: 6,
    PUNCTUATION:7,
    WHITESPACE: 8,
    UNKNOWN:    9,
};

// Colors (r, g, b, a) for each token type — dark theme
export const TOKEN_COLOR = [
    [0.56, 0.74, 0.99, 1.0],  // KEYWORD   — blue
    [0.42, 0.88, 0.78, 1.0],  // TYPE      — teal
    [0.90, 0.75, 0.49, 1.0],  // NUMBER    — orange
    [0.72, 0.90, 0.60, 1.0],  // STRING    — green
    [0.50, 0.50, 0.50, 1.0],  // COMMENT   — gray
    [0.86, 0.60, 0.80, 1.0],  // OPERATOR  — pink
    [0.92, 0.92, 0.92, 1.0],  // IDENT     — white
    [0.70, 0.70, 0.70, 1.0],  // PUNCT     — light gray
    [0.00, 0.00, 0.00, 0.0],  // WHITESPACE— transparent
    [0.92, 0.92, 0.92, 1.0],  // UNKNOWN   — white
];

/**
 * Tokenize a single line of NS code.
 * Returns array of {type, text} tokens.
 */
export function tokenize_line(line) {
    const tokens = [];
    let i = 0;
    const n = line.length;

    while (i < n) {
        const c = line[i];

        // Whitespace
        if (c === ' ' || c === '\t') {
            let j = i + 1;
            while (j < n && (line[j] === ' ' || line[j] === '\t')) j++;
            tokens.push({ type: TT.WHITESPACE, text: line.slice(i, j) });
            i = j;
            continue;
        }

        // Line comment
        if (c === '/' && line[i + 1] === '/') {
            tokens.push({ type: TT.COMMENT, text: line.slice(i) });
            break;
        }

        // String literal
        if (c === '"' || c === '`' || c === "'") {
            const quote = c;
            let j = i + 1;
            while (j < n && line[j] !== quote) {
                if (line[j] === '\\') j++;
                j++;
            }
            if (j < n) j++; // closing quote
            tokens.push({ type: TT.STRING, text: line.slice(i, j) });
            i = j;
            continue;
        }

        // Number
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
            i = j;
            continue;
        }

        // Identifier or keyword
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
            i = j;
            continue;
        }

        // Operators
        const ops2 = ['==', '!=', '<=', '>=', '->', '=>', '&&', '||', '::', '<<', '>>'];
        let matched = false;
        for (const op of ops2) {
            if (line.startsWith(op, i)) {
                tokens.push({ type: TT.OPERATOR, text: op });
                i += op.length;
                matched = true;
                break;
            }
        }
        if (matched) continue;

        if ('+-*/%=<>!&|^~'.includes(c)) {
            tokens.push({ type: TT.OPERATOR, text: c });
            i++;
            continue;
        }

        // Punctuation
        if ('(){}[].,;:'.includes(c)) {
            tokens.push({ type: TT.PUNCTUATION, text: c });
            i++;
            continue;
        }

        // Unknown
        tokens.push({ type: TT.UNKNOWN, text: c });
        i++;
    }

    return tokens;
}
