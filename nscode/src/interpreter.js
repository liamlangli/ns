// NanoScript tree-walk interpreter
// Supports: fn, let, if/else, for-in-to, return, arithmetic, comparisons,
// logical ops, function calls, recursion, use std (print/println)

// ─────────────────────────────────────────────────────────────────────────────
// Lexer
// ─────────────────────────────────────────────────────────────────────────────

const TK = {
    // literals
    INT: 'INT', FLOAT: 'FLOAT', STRING: 'STRING', BOOL: 'BOOL',
    // keywords
    FN: 'fn', LET: 'let', RETURN: 'return',
    IF: 'if', ELSE: 'else', FOR: 'for', IN: 'in', TO: 'to',
    USE: 'use', BREAK: 'break', CONTINUE: 'continue', AS: 'as',
    TYPE: 'type', STRUCT: 'struct',
    // identifier
    IDENT: 'IDENT',
    // punctuation
    LPAREN: '(', RPAREN: ')', LBRACE: '{', RBRACE: '}',
    LBRACKET: '[', RBRACKET: ']',
    COMMA: ',', COLON: ':', SEMI: ';', DOT: '.', ARROW: '->',
    // operators
    PLUS: '+', MINUS: '-', STAR: '*', SLASH: '/', PERCENT: '%',
    EQ: '==', NEQ: '!=', LT: '<', LE: '<=', GT: '>', GE: '>=',
    AND: '&&', OR: '||', BANG: '!', AMPERSAND: '&', PIPE: '|',
    ASSIGN: '=', PLUS_ASSIGN: '+=', MINUS_ASSIGN: '-=',
    STAR_ASSIGN: '*=', SLASH_ASSIGN: '/=',
    // special
    BACKTICK: '`', EOF: 'EOF',
};

const KEYWORDS = new Set([
    'fn','let','return','if','else','for','in','to','use',
    'break','continue','as','type','struct','true','false',
]);

class Token {
    constructor(type, value, line) { this.type = type; this.value = value; this.line = line; }
}

function lex(src) {
    const tokens = [];
    let i = 0, line = 1;
    const n = src.length;

    while (i < n) {
        // Skip whitespace
        if (src[i] === '\n') { line++; i++; continue; }
        if (src[i] === ' ' || src[i] === '\t' || src[i] === '\r') { i++; continue; }

        // Line comment
        if (src[i] === '/' && src[i+1] === '/') {
            while (i < n && src[i] !== '\n') i++;
            continue;
        }

        // Numbers
        if (src[i] >= '0' && src[i] <= '9') {
            let j = i;
            let isFloat = false;
            while (j < n && (src[j] >= '0' && src[j] <= '9' || src[j] === '_')) j++;
            if (j < n && src[j] === '.' && src[j+1] >= '0' && src[j+1] <= '9') {
                isFloat = true;
                j++;
                while (j < n && (src[j] >= '0' && src[j] <= '9')) j++;
            }
            if (j < n && (src[j] === 'e' || src[j] === 'E')) {
                isFloat = true; j++;
                if (j < n && (src[j] === '+' || src[j] === '-')) j++;
                while (j < n && src[j] >= '0' && src[j] <= '9') j++;
            }
            const raw = src.slice(i, j).replace(/_/g,'');
            tokens.push(new Token(isFloat ? TK.FLOAT : TK.INT,
                isFloat ? parseFloat(raw) : parseInt(raw, 10), line));
            i = j; continue;
        }

        // String double-quote
        if (src[i] === '"') {
            let j = i + 1, s = '';
            while (j < n && src[j] !== '"') {
                if (src[j] === '\\') { j++; s += escapeChar(src[j]); }
                else s += src[j];
                j++;
            }
            j++; // closing "
            tokens.push(new Token(TK.STRING, s, line));
            i = j; continue;
        }

        // Template string backtick `…{expr}…`
        if (src[i] === '`') {
            tokens.push(new Token(TK.BACKTICK, '`', line));
            i++;
            let s = '';
            while (i < n && src[i] !== '`') {
                if (src[i] === '{') {
                    if (s) tokens.push(new Token(TK.STRING, s, line));
                    s = '';
                    tokens.push(new Token(TK.LBRACE, '{', line));
                    i++;
                    let depth = 1;
                    let inner = '';
                    while (i < n && depth > 0) {
                        if (src[i] === '{') depth++;
                        else if (src[i] === '}') { depth--; if (depth === 0) { i++; break; } }
                        inner += src[i++];
                    }
                    tokens.push(...lex(inner));
                    tokens.push(new Token(TK.RBRACE, '}', line));
                } else {
                    if (src[i] === '\n') line++;
                    s += src[i++];
                }
            }
            if (s) tokens.push(new Token(TK.STRING, s, line));
            tokens.push(new Token(TK.BACKTICK, '`', line));
            if (i < n) i++; // closing `
            continue;
        }

        // Single-quote string
        if (src[i] === "'") {
            let j = i + 1, s = '';
            while (j < n && src[j] !== "'") {
                if (src[j] === '\\') { j++; s += escapeChar(src[j]); }
                else s += src[j];
                j++;
            }
            j++;
            tokens.push(new Token(TK.STRING, s, line));
            i = j; continue;
        }

        // Identifiers / keywords
        if (isAlpha(src[i])) {
            let j = i;
            while (j < n && isAlphaNum(src[j])) j++;
            const word = src.slice(i, j);
            if (word === 'true' || word === 'false')
                tokens.push(new Token(TK.BOOL, word === 'true', line));
            else if (KEYWORDS.has(word))
                tokens.push(new Token(word, word, line));
            else
                tokens.push(new Token(TK.IDENT, word, line));
            i = j; continue;
        }

        // Two-char operators
        const two = src.slice(i, i+2);
        const twoMap = {
            '==': TK.EQ, '!=': TK.NEQ, '<=': TK.LE, '>=': TK.GE,
            '&&': TK.AND, '||': TK.OR, '->': TK.ARROW,
            '+=': TK.PLUS_ASSIGN, '-=': TK.MINUS_ASSIGN,
            '*=': TK.STAR_ASSIGN, '/=': TK.SLASH_ASSIGN,
        };
        if (twoMap[two]) { tokens.push(new Token(twoMap[two], two, line)); i += 2; continue; }

        // Single-char operators
        const oneMap = {
            '(': TK.LPAREN, ')': TK.RPAREN, '{': TK.LBRACE, '}': TK.RBRACE,
            '[': TK.LBRACKET, ']': TK.RBRACKET,
            ',': TK.COMMA, ':': TK.COLON, ';': TK.SEMI, '.': TK.DOT,
            '+': TK.PLUS, '-': TK.MINUS, '*': TK.STAR,
            '/': TK.SLASH, '%': TK.PERCENT,
            '<': TK.LT, '>': TK.GT, '=': TK.ASSIGN,
            '!': TK.BANG, '&': TK.AMPERSAND, '|': TK.PIPE,
        };
        if (oneMap[src[i]]) { tokens.push(new Token(oneMap[src[i]], src[i], line)); i++; continue; }

        i++; // skip unknown
    }

    tokens.push(new Token(TK.EOF, null, line));
    return tokens;
}

function isAlpha(c) { return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c === '_'; }
function isAlphaNum(c) { return isAlpha(c) || (c >= '0' && c <= '9'); }
function escapeChar(c) {
    return { n: '\n', t: '\t', r: '\r', '\\': '\\', '"': '"', "'": "'" }[c] ?? c;
}

// ─────────────────────────────────────────────────────────────────────────────
// Parser → AST
// ─────────────────────────────────────────────────────────────────────────────

class ParseError extends Error {
    constructor(msg, line) { super(`Line ${line}: ${msg}`); this.line = line; }
}

class Parser {
    constructor(tokens) {
        this.tokens = tokens;
        this.pos = 0;
    }

    peek() { return this.tokens[this.pos]; }
    at(type) { return this.peek().type === type; }
    advance() { return this.tokens[this.pos++]; }
    expect(type) {
        const t = this.peek();
        if (t.type !== type) throw new ParseError(`expected '${type}', got '${t.type}' ('${t.value}')`, t.line);
        return this.advance();
    }
    eat(type) { if (this.at(type)) { this.advance(); return true; } return false; }

    parseProgram() {
        const stmts = [];
        while (!this.at(TK.EOF)) {
            stmts.push(this.parseTopLevel());
        }
        return { kind: 'Program', stmts };
    }

    parseTopLevel() {
        const t = this.peek();
        if (t.type === TK.FN)     return this.parseFn();
        if (t.type === TK.LET)    return this.parseLet();
        if (t.type === TK.USE)    return this.parseUse();
        if (t.type === TK.TYPE)   return this.parseTypeAlias();
        if (t.type === TK.STRUCT) return this.parseStruct();
        return this.parseExprStmt();
    }

    parseFn() {
        const line = this.peek().line;
        this.expect(TK.FN);
        const name = this.expect(TK.IDENT).value;
        this.expect(TK.LPAREN);
        const params = [];
        while (!this.at(TK.RPAREN) && !this.at(TK.EOF)) {
            const pname = this.expect(TK.IDENT).value;
            let ptype = 'i32';
            if (this.eat(TK.COLON)) ptype = this.parseType();
            params.push({ name: pname, type: ptype });
            this.eat(TK.COMMA);
        }
        this.expect(TK.RPAREN);
        let retType = 'void';
        if (!this.at(TK.LBRACE)) retType = this.parseType();
        const body = this.parseBlock();
        return { kind: 'FnDef', name, params, retType, body, line };
    }

    parseType() {
        // Skip type annotation (we don't enforce types in the interpreter)
        let t = this.advance().value;
        // handle pointer / generic suffixes
        while (this.at(TK.LT) || this.at(TK.LBRACKET)) {
            if (this.at(TK.LT)) { this.advance(); this.parseType(); this.expect(TK.GT); }
            if (this.at(TK.LBRACKET)) { this.advance(); this.expect(TK.RBRACKET); }
        }
        // function type  (a, b) -> c
        if (this.at(TK.ARROW)) { this.advance(); this.parseType(); }
        return t;
    }

    parseLet() {
        const line = this.peek().line;
        this.expect(TK.LET);
        const name = this.expect(TK.IDENT).value;
        if (this.eat(TK.COLON)) this.parseType(); // skip type annotation
        let init = null;
        if (this.eat(TK.ASSIGN)) init = this.parseExpr();
        this.eat(TK.SEMI);
        return { kind: 'LetStmt', name, init, line };
    }

    parseUse() {
        this.expect(TK.USE);
        const mod = this.expect(TK.IDENT).value;
        this.eat(TK.SEMI);
        return { kind: 'UseStmt', mod };
    }

    parseTypeAlias() {
        this.expect(TK.TYPE);
        this.expect(TK.IDENT);
        this.expect(TK.ASSIGN);
        // consume until end of line / next fn or let
        while (!this.at(TK.EOF) && !this.at(TK.FN) && !this.at(TK.LET) &&
               !this.at(TK.TYPE) && !this.at(TK.USE)) {
            if (this.at(TK.LBRACE)) { this.parseBlock(); break; }
            this.advance();
        }
        return { kind: 'TypeAlias' };
    }

    parseStruct() {
        this.expect(TK.STRUCT);
        this.expect(TK.IDENT);
        this.parseBlock();
        return { kind: 'StructDef' };
    }

    parseBlock() {
        this.expect(TK.LBRACE);
        const stmts = [];
        while (!this.at(TK.RBRACE) && !this.at(TK.EOF)) {
            stmts.push(this.parseStmt());
        }
        this.expect(TK.RBRACE);
        return { kind: 'Block', stmts };
    }

    parseStmt() {
        const t = this.peek();
        if (t.type === TK.LET)      return this.parseLet();
        if (t.type === TK.RETURN)   return this.parseReturn();
        if (t.type === TK.IF)       return this.parseIf();
        if (t.type === TK.FOR)      return this.parseFor();
        if (t.type === TK.BREAK)    { this.advance(); this.eat(TK.SEMI); return { kind: 'Break' }; }
        if (t.type === TK.CONTINUE) { this.advance(); this.eat(TK.SEMI); return { kind: 'Continue' }; }
        if (t.type === TK.LBRACE)   return this.parseBlock();
        return this.parseExprStmt();
    }

    parseReturn() {
        const line = this.peek().line;
        this.expect(TK.RETURN);
        let value = null;
        if (!this.at(TK.RBRACE) && !this.at(TK.EOF) && !this.at(TK.SEMI)) {
            value = this.parseExpr();
        }
        this.eat(TK.SEMI);
        return { kind: 'ReturnStmt', value, line };
    }

    parseIf() {
        const line = this.peek().line;
        this.expect(TK.IF);
        const cond = this.parseExpr();
        const then = this.parseBlock();
        let alt = null;
        if (this.eat(TK.ELSE)) {
            alt = this.at(TK.IF) ? this.parseIf() : this.parseBlock();
        }
        return { kind: 'IfStmt', cond, then, alt, line };
    }

    parseFor() {
        const line = this.peek().line;
        this.expect(TK.FOR);
        const varName = this.expect(TK.IDENT).value;
        this.expect(TK.IN);
        const from = this.parseExpr();
        this.expect(TK.TO);
        const to = this.parseExpr();
        const body = this.parseBlock();
        return { kind: 'ForStmt', var: varName, from, to, body, line };
    }

    parseExprStmt() {
        const expr = this.parseExpr();
        this.eat(TK.SEMI);
        return { kind: 'ExprStmt', expr };
    }

    parseExpr() { return this.parseAssign(); }

    parseAssign() {
        const left = this.parseOr();
        const assignOps = {
            [TK.ASSIGN]: '=', [TK.PLUS_ASSIGN]: '+=',
            [TK.MINUS_ASSIGN]: '-=', [TK.STAR_ASSIGN]: '*=',
            [TK.SLASH_ASSIGN]: '/=',
        };
        const op = assignOps[this.peek().type];
        if (op) {
            const line = this.peek().line;
            this.advance();
            const right = this.parseExpr();
            return { kind: 'Assign', op, target: left, value: right, line };
        }
        return left;
    }

    parseOr() {
        let left = this.parseAnd();
        while (this.at(TK.OR)) {
            const line = this.peek().line;
            this.advance();
            left = { kind: 'Binary', op: '||', left, right: this.parseAnd(), line };
        }
        return left;
    }

    parseAnd() {
        let left = this.parseCmp();
        while (this.at(TK.AND)) {
            const line = this.peek().line;
            this.advance();
            left = { kind: 'Binary', op: '&&', left, right: this.parseCmp(), line };
        }
        return left;
    }

    parseCmp() {
        let left = this.parseAdd();
        const cmpOps = {
            [TK.EQ]:'==', [TK.NEQ]:'!=', [TK.LT]:'<',
            [TK.LE]:'<=', [TK.GT]:'>', [TK.GE]:'>=',
        };
        const op = cmpOps[this.peek().type];
        if (op) {
            const line = this.peek().line;
            this.advance();
            left = { kind: 'Binary', op, left, right: this.parseAdd(), line };
        }
        return left;
    }

    parseAdd() {
        let left = this.parseMul();
        while (this.at(TK.PLUS) || this.at(TK.MINUS)) {
            const op = this.peek().type; const line = this.peek().line;
            this.advance();
            left = { kind: 'Binary', op, left, right: this.parseMul(), line };
        }
        return left;
    }

    parseMul() {
        let left = this.parseUnary();
        while (this.at(TK.STAR) || this.at(TK.SLASH) || this.at(TK.PERCENT)) {
            const op = this.peek().type; const line = this.peek().line;
            this.advance();
            left = { kind: 'Binary', op, left, right: this.parseUnary(), line };
        }
        return left;
    }

    parseUnary() {
        if (this.at(TK.MINUS)) {
            const line = this.peek().line; this.advance();
            return { kind: 'Unary', op: '-', expr: this.parseUnary(), line };
        }
        if (this.at(TK.BANG)) {
            const line = this.peek().line; this.advance();
            return { kind: 'Unary', op: '!', expr: this.parseUnary(), line };
        }
        return this.parsePostfix();
    }

    parsePostfix() {
        let base = this.parsePrimary();
        for (;;) {
            if (this.at(TK.LPAREN)) {
                const line = this.peek().line;
                this.advance();
                const args = [];
                while (!this.at(TK.RPAREN) && !this.at(TK.EOF)) {
                    args.push(this.parseExpr());
                    this.eat(TK.COMMA);
                }
                this.expect(TK.RPAREN);
                // Optional trailing block: fn(args) { ... }
                let trailingBlock = null;
                if (this.at(TK.LBRACE)) trailingBlock = this.parseBlock();
                base = { kind: 'Call', callee: base, args, trailingBlock, line };
            } else if (this.at(TK.DOT)) {
                const line = this.peek().line; this.advance();
                const field = this.expect(TK.IDENT).value;
                base = { kind: 'Field', obj: base, field, line };
            } else if (this.at(TK.AS)) {
                this.advance(); this.parseType(); // skip cast type
            } else {
                break;
            }
        }
        return base;
    }

    parsePrimary() {
        const t = this.peek();

        if (t.type === TK.INT)   { this.advance(); return { kind: 'Lit', value: t.value }; }
        if (t.type === TK.FLOAT) { this.advance(); return { kind: 'Lit', value: t.value }; }
        if (t.type === TK.STRING){ this.advance(); return { kind: 'Lit', value: t.value }; }
        if (t.type === TK.BOOL)  { this.advance(); return { kind: 'Lit', value: t.value }; }

        if (t.type === TK.IDENT) {
            this.advance();
            return { kind: 'Ident', name: t.value, line: t.line };
        }

        if (t.type === TK.LPAREN) {
            this.advance();
            const e = this.parseExpr();
            this.expect(TK.RPAREN);
            return e;
        }

        // Template string
        if (t.type === TK.BACKTICK) {
            return this.parseTemplateLit();
        }

        // Block lambda: { a, b in ... } or { ... }
        if (t.type === TK.LBRACE) {
            return this.parseLambda();
        }

        // simd3 / struct literals — just skip for now
        if (t.type === TK.FN) {
            return this.parseFnExpr();
        }

        throw new ParseError(`unexpected token '${t.type}' ('${t.value}')`, t.line);
    }

    parseTemplateLit() {
        const line = this.peek().line;
        this.expect(TK.BACKTICK);
        const parts = [];
        while (!this.at(TK.BACKTICK) && !this.at(TK.EOF)) {
            if (this.at(TK.STRING)) {
                parts.push({ kind: 'Lit', value: this.advance().value });
            } else if (this.at(TK.LBRACE)) {
                this.advance();
                parts.push(this.parseExpr());
                this.expect(TK.RBRACE);
            } else break;
        }
        this.eat(TK.BACKTICK);
        return { kind: 'Template', parts, line };
    }

    parseLambda() {
        // Peek ahead: is this { name, name in ... } or { name in ... } ?
        const saved = this.pos;
        try {
            this.expect(TK.LBRACE);
            const params = [];
            while (this.at(TK.IDENT)) {
                params.push(this.advance().value);
                this.eat(TK.COMMA);
            }
            if (this.at(TK.IN) && params.length > 0) {
                this.advance(); // consume 'in'
                const body = [];
                while (!this.at(TK.RBRACE) && !this.at(TK.EOF)) body.push(this.parseStmt());
                this.expect(TK.RBRACE);
                return { kind: 'Lambda', params, body };
            }
            // Not a lambda — rewind and parse as block-expr
            this.pos = saved;
        } catch (_) { this.pos = saved; }
        // Plain block used as expression (not common, but handle)
        const block = this.parseBlock();
        return { kind: 'BlockExpr', block };
    }

    parseFnExpr() {
        // anonymous fn expression
        this.expect(TK.FN);
        this.expect(TK.LPAREN);
        const params = [];
        while (!this.at(TK.RPAREN) && !this.at(TK.EOF)) {
            const n = this.expect(TK.IDENT).value;
            let type = 'i32';
            if (this.eat(TK.COLON)) type = this.parseType();
            params.push({ name: n, type });
            this.eat(TK.COMMA);
        }
        this.expect(TK.RPAREN);
        let retType = 'void';
        if (!this.at(TK.LBRACE)) retType = this.parseType();
        const body = this.parseBlock();
        return { kind: 'FnExpr', params, retType, body };
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// Runtime signals
// ─────────────────────────────────────────────────────────────────────────────

class ReturnSignal { constructor(value) { this.value = value; } }
class BreakSignal  {}
class ContinueSignal {}
class NSError extends Error {
    constructor(msg, line) { super(line ? `Line ${line}: ${msg}` : msg); }
}

// ─────────────────────────────────────────────────────────────────────────────
// Environment (lexical scope)
// ─────────────────────────────────────────────────────────────────────────────

class Env {
    constructor(parent = null) { this.vars = new Map(); this.parent = parent; }

    get(name) {
        if (this.vars.has(name)) return this.vars.get(name);
        if (this.parent) return this.parent.get(name);
        throw new NSError(`undefined variable '${name}'`);
    }

    set(name, value) {
        if (this.vars.has(name)) { this.vars.set(name, value); return; }
        if (this.parent && this.parent.has(name)) { this.parent.set(name, value); return; }
        this.vars.set(name, value); // create in current scope
    }

    has(name) { return this.vars.has(name) || (this.parent ? this.parent.has(name) : false); }

    def(name, value) { this.vars.set(name, value); }
}

// ─────────────────────────────────────────────────────────────────────────────
// Interpreter
// ─────────────────────────────────────────────────────────────────────────────

const MAX_CALLS  = 100000;
const MAX_ITERS  = 1000000;

export class NSInterpreter {
    constructor({ print = console.log, error = console.error } = {}) {
        this.print = print;
        this.error = error;
        this.callDepth = 0;
        this.iterCount = 0;
        this.globals = new Env();
        this._seedGlobals();
    }

    _seedGlobals() {
        const g = this.globals;
        const self = this;

        // std library
        g.def('print',   { __fn: true, call: (args) => { self.print(args.map(nsStr).join('')); return null; } });
        g.def('println', { __fn: true, call: (args) => { self.print(args.map(nsStr).join('') + '\n'); return null; } });
        g.def('assert',  { __fn: true, call: ([cond, msg]) => {
            if (!cond) throw new NSError(`assertion failed${msg ? ': ' + msg : ''}`);
            return null;
        }});

        // math
        g.def('sqrt',  { __fn: true, call: ([x]) => Math.sqrt(x) });
        g.def('abs',   { __fn: true, call: ([x]) => Math.abs(x) });
        g.def('floor', { __fn: true, call: ([x]) => Math.floor(x) });
        g.def('ceil',  { __fn: true, call: ([x]) => Math.ceil(x) });
        g.def('round', { __fn: true, call: ([x]) => Math.round(x) });
        g.def('min',   { __fn: true, call: ([a, b]) => Math.min(a, b) });
        g.def('max',   { __fn: true, call: ([a, b]) => Math.max(a, b) });
        g.def('pow',   { __fn: true, call: ([a, b]) => Math.pow(a, b) });
        g.def('sin',   { __fn: true, call: ([x]) => Math.sin(x) });
        g.def('cos',   { __fn: true, call: ([x]) => Math.cos(x) });
        g.def('log',   { __fn: true, call: ([x]) => Math.log(x) });
    }

    run(source) {
        this.callDepth = 0;
        this.iterCount = 0;
        let tokens, ast;
        try { tokens = lex(source); }
        catch (e) { this.error('Lex error: ' + e.message); return; }

        try {
            const parser = new Parser(tokens);
            ast = parser.parseProgram();
        } catch (e) { this.error('Parse error: ' + e.message); return; }

        try {
            this.evalProgram(ast, this.globals);
        } catch (e) {
            if (e instanceof ReturnSignal) return; // top-level return
            this.error('Runtime error: ' + e.message);
        }
    }

    evalProgram(ast, env) {
        // First pass: register all fn definitions
        for (const stmt of ast.stmts) {
            if (stmt.kind === 'FnDef') {
                env.def(stmt.name, { __fn: true, def: stmt, closure: env });
            }
        }
        // Second pass: execute non-fn statements
        for (const stmt of ast.stmts) {
            if (stmt.kind !== 'FnDef') this.evalStmt(stmt, env);
        }
    }

    evalBlock(block, env) {
        const local = new Env(env);
        for (const stmt of block.stmts) {
            const sig = this.evalStmt(stmt, local);
            if (sig instanceof ReturnSignal || sig instanceof BreakSignal || sig instanceof ContinueSignal)
                return sig;
        }
        return null;
    }

    evalStmt(stmt, env) {
        switch (stmt.kind) {
        case 'FnDef':
            env.def(stmt.name, { __fn: true, def: stmt, closure: env });
            break;
        case 'LetStmt': {
            const val = stmt.init ? this.evalExpr(stmt.init, env) : null;
            env.def(stmt.name, val);
            break;
        }
        case 'ReturnStmt':
            return new ReturnSignal(stmt.value ? this.evalExpr(stmt.value, env) : null);
        case 'Break':    return new BreakSignal();
        case 'Continue': return new ContinueSignal();
        case 'IfStmt':   return this.evalIf(stmt, env);
        case 'ForStmt':  return this.evalFor(stmt, env);
        case 'ExprStmt': this.evalExpr(stmt.expr, env); break;
        case 'Block':    return this.evalBlock(stmt, env);
        case 'UseStmt':  break; // handled by globals
        case 'TypeAlias': case 'StructDef': break;
        default:
            // ignore unknown statement types
        }
        return null;
    }

    evalIf(stmt, env) {
        const cond = this.evalExpr(stmt.cond, env);
        if (cond) return this.evalBlock(stmt.then, env);
        if (stmt.alt) {
            if (stmt.alt.kind === 'IfStmt') return this.evalIf(stmt.alt, env);
            return this.evalBlock(stmt.alt, env);
        }
        return null;
    }

    evalFor(stmt, env) {
        const from = this.evalExpr(stmt.from, env);
        const to   = this.evalExpr(stmt.to,   env);
        const local = new Env(env);
        local.def(stmt.var, from);
        for (let i = from; i < to; i++) {
            if (++this.iterCount > MAX_ITERS)
                throw new NSError('iteration limit exceeded (infinite loop?)');
            local.set(stmt.var, i);
            const sig = this.evalBlock(stmt.body, local);
            if (sig instanceof ReturnSignal) return sig;
            if (sig instanceof BreakSignal)  break;
            // ContinueSignal: just continue
        }
        return null;
    }

    evalExpr(node, env) {
        switch (node.kind) {
        case 'Lit':    return node.value;
        case 'Ident':  return env.get(node.name);

        case 'Assign': {
            let val = this.evalExpr(node.value, env);
            if (node.op === '+=') val = env.get(node.target.name) + val;
            if (node.op === '-=') val = env.get(node.target.name) - val;
            if (node.op === '*=') val = env.get(node.target.name) * val;
            if (node.op === '/=') val = env.get(node.target.name) / val;
            if (node.target.kind === 'Field') {
                const obj = this.evalExpr(node.target.obj, env);
                if (obj && typeof obj === 'object') obj[node.target.field] = val;
            } else if (node.target.kind === 'Ident') {
                env.set(node.target.name, val);
            }
            return val;
        }

        case 'Binary': {
            // Short-circuit logical ops
            if (node.op === '&&') return this.evalExpr(node.left, env) && this.evalExpr(node.right, env);
            if (node.op === '||') return this.evalExpr(node.left, env) || this.evalExpr(node.right, env);
            const l = this.evalExpr(node.left, env);
            const r = this.evalExpr(node.right, env);
            switch (node.op) {
            case '+':  return typeof l === 'string' || typeof r === 'string' ? String(l) + String(r) : l + r;
            case '-':  return l - r;
            case '*':  return l * r;
            case '/':  if (r === 0) throw new NSError('division by zero', node.line); return l / r;
            case '%':  return l % r;
            case '==': return l === r;
            case '!=': return l !== r;
            case '<':  return l < r;
            case '<=': return l <= r;
            case '>':  return l > r;
            case '>=': return l >= r;
            }
            break;
        }

        case 'Unary': {
            const v = this.evalExpr(node.expr, env);
            if (node.op === '-') return -v;
            if (node.op === '!') return !v;
            break;
        }

        case 'Call': return this.evalCall(node, env);

        case 'Field': {
            const obj = this.evalExpr(node.obj, env);
            if (obj == null) throw new NSError(`null field access '.${node.field}'`, node.line);
            return obj[node.field] ?? null;
        }

        case 'Template': {
            let s = '';
            for (const p of node.parts) s += nsStr(this.evalExpr(p, env));
            return s;
        }

        case 'Lambda':
            return { __fn: true, lambda: node, closure: env };

        case 'FnExpr':
            return { __fn: true, def: node, closure: env };

        case 'BlockExpr':
            return this.evalBlock(node.block, env);

        default:
            return null;
        }
        return null;
    }

    evalCall(node, env) {
        if (++this.callDepth > MAX_CALLS)
            throw new NSError('call stack overflow (infinite recursion?)');

        try {
            // Resolve callee
            let fn;
            if (node.callee.kind === 'Ident') {
                try { fn = env.get(node.callee.name); }
                catch (_) { throw new NSError(`undefined function '${node.callee.name}'`, node.line); }
            } else if (node.callee.kind === 'Field') {
                // method call: obj.method(args) — find method in builtins
                const obj = this.evalExpr(node.callee.obj, env);
                const method = node.callee.field;
                const args   = node.args.map(a => this.evalExpr(a, env));
                // Array / string methods
                if (Array.isArray(obj)) {
                    if (method === 'push')  { obj.push(...args); return null; }
                    if (method === 'len')   return obj.length;
                    if (method === 'pop')   return obj.pop() ?? null;
                }
                if (typeof obj === 'string') {
                    if (method === 'len') return obj.length;
                }
                return null;
            } else {
                fn = this.evalExpr(node.callee, env);
            }

            if (!fn || !fn.__fn)
                throw new NSError(`'${node.callee.name ?? '?'}' is not a function`, node.line);

            // Native JS function
            if (fn.call) {
                const args = node.args.map(a => this.evalExpr(a, env));
                return fn.call(args) ?? null;
            }

            // NS-defined function or lambda
            const def = fn.def ?? fn.lambda;
            const params = def.params ?? def.params ?? [];
            const args   = node.args.map(a => this.evalExpr(a, env));

            // Add trailing block as last param (if any)
            if (node.trailingBlock) {
                // synthesise a lambda
                args.push({ __fn: true,
                    lambda: { kind: 'Lambda', params: params.length > args.length ?
                        [params[params.length-1].name ?? 'value'] : [],
                        body: node.trailingBlock.stmts },
                    closure: env });
            }

            const local = new Env(fn.closure ?? this.globals);
            (params).forEach((p, i) => {
                const pname = typeof p === 'string' ? p : p.name;
                local.def(pname, args[i] ?? null);
            });

            const sig = this.evalBlock(def.body, local);
            if (sig instanceof ReturnSignal) return sig.value ?? null;
            return null;
        } finally {
            this.callDepth--;
        }
    }
}

function nsStr(v) {
    if (v === null || v === undefined) return 'null';
    if (typeof v === 'boolean') return v ? 'true' : 'false';
    if (typeof v === 'number') {
        // Remove trailing .0 for integer floats
        return Number.isInteger(v) ? String(v) : String(v);
    }
    if (typeof v === 'object' && v.__fn) return '<fn>';
    if (typeof v === 'object') return JSON.stringify(v);
    return String(v);
}
