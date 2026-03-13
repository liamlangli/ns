// NanoScript tree-walk interpreter
// Supports: fn, let, if/else, for-in-to, return, arithmetic, comparisons,
// logical ops, function calls, recursion, use std (print/println)

const TK = {
    INT: 'INT', FLOAT: 'FLOAT', STRING: 'STRING', BOOL: 'BOOL',
    FN: 'fn', LET: 'let', RETURN: 'return',
    IF: 'if', ELSE: 'else', FOR: 'for', IN: 'in', TO: 'to',
    USE: 'use', BREAK: 'break', CONTINUE: 'continue', AS: 'as',
    TYPE: 'type', STRUCT: 'struct',
    IDENT: 'IDENT',
    LPAREN: '(', RPAREN: ')', LBRACE: '{', RBRACE: '}',
    LBRACKET: '[', RBRACKET: ']',
    COMMA: ',', COLON: ':', SEMI: ';', DOT: '.', ARROW: '->',
    PLUS: '+', MINUS: '-', STAR: '*', SLASH: '/', PERCENT: '%',
    EQ: '==', NEQ: '!=', LT: '<', LE: '<=', GT: '>', GE: '>=',
    AND: '&&', OR: '||', BANG: '!', AMPERSAND: '&', PIPE: '|',
    ASSIGN: '=', PLUS_ASSIGN: '+=', MINUS_ASSIGN: '-=',
    STAR_ASSIGN: '*=', SLASH_ASSIGN: '/=',
    BACKTICK: '`', EOF: 'EOF',
} as const;

type TKValue = typeof TK[keyof typeof TK];

const KEYWORDS = new Set([
    'fn','let','return','if','else','for','in','to','use',
    'break','continue','as','type','struct','true','false',
]);

class Token {
    type: string;
    value: number | string | boolean | null;
    line: number;
    constructor(type: string, value: number | string | boolean | null, line: number) {
        this.type = type; this.value = value; this.line = line;
    }
}

function lex(src: string): Token[] {
    const tokens: Token[] = [];
    let i = 0, line = 1;
    const n = src.length;
    while (i < n) {
        if (src[i] === '\n') { line++; i++; continue; }
        if (src[i] === ' ' || src[i] === '\t' || src[i] === '\r') { i++; continue; }
        if (src[i] === '/' && src[i+1] === '/') {
            while (i < n && src[i] !== '\n') i++;
            continue;
        }
        if (src[i]! >= '0' && src[i]! <= '9') {
            let j = i;
            let is_float = false;
            while (j < n && (src[j]! >= '0' && src[j]! <= '9' || src[j] === '_')) j++;
            if (j < n && src[j] === '.' && src[j+1]! >= '0' && src[j+1]! <= '9') {
                is_float = true; j++;
                while (j < n && (src[j]! >= '0' && src[j]! <= '9')) j++;
            }
            if (j < n && (src[j] === 'e' || src[j] === 'E')) {
                is_float = true; j++;
                if (j < n && (src[j] === '+' || src[j] === '-')) j++;
                while (j < n && src[j]! >= '0' && src[j]! <= '9') j++;
            }
            const raw = src.slice(i, j).replace(/_/g,'');
            tokens.push(new Token(is_float ? TK.FLOAT : TK.INT,
                is_float ? parseFloat(raw) : parseInt(raw, 10), line));
            i = j; continue;
        }
        if (src[i] === '"') {
            let j = i + 1, s = '';
            while (j < n && src[j] !== '"') {
                if (src[j] === '\\') { j++; s += escape_char(src[j]!); }
                else s += src[j];
                j++;
            }
            j++;
            tokens.push(new Token(TK.STRING, s, line));
            i = j; continue;
        }
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
            if (i < n) i++;
            continue;
        }
        if (src[i] === "'") {
            let j = i + 1, s = '';
            while (j < n && src[j] !== "'") {
                if (src[j] === '\\') { j++; s += escape_char(src[j]!); }
                else s += src[j];
                j++;
            }
            j++;
            tokens.push(new Token(TK.STRING, s, line));
            i = j; continue;
        }
        if (is_alpha(src[i]!)) {
            let j = i;
            while (j < n && is_alpha_num(src[j]!)) j++;
            const word = src.slice(i, j);
            if (word === 'true' || word === 'false')
                tokens.push(new Token(TK.BOOL, word === 'true', line));
            else if (KEYWORDS.has(word))
                tokens.push(new Token(word, word, line));
            else
                tokens.push(new Token(TK.IDENT, word, line));
            i = j; continue;
        }
        const two = src.slice(i, i+2);
        const two_map: Record<string, string> = {
            '==': TK.EQ, '!=': TK.NEQ, '<=': TK.LE, '>=': TK.GE,
            '&&': TK.AND, '||': TK.OR, '->': TK.ARROW,
            '+=': TK.PLUS_ASSIGN, '-=': TK.MINUS_ASSIGN,
            '*=': TK.STAR_ASSIGN, '/=': TK.SLASH_ASSIGN,
        };
        if (two_map[two]) { tokens.push(new Token(two_map[two]!, two, line)); i += 2; continue; }
        const one_map: Record<string, string> = {
            '(': TK.LPAREN, ')': TK.RPAREN, '{': TK.LBRACE, '}': TK.RBRACE,
            '[': TK.LBRACKET, ']': TK.RBRACKET,
            ',': TK.COMMA, ':': TK.COLON, ';': TK.SEMI, '.': TK.DOT,
            '+': TK.PLUS, '-': TK.MINUS, '*': TK.STAR,
            '/': TK.SLASH, '%': TK.PERCENT,
            '<': TK.LT, '>': TK.GT, '=': TK.ASSIGN,
            '!': TK.BANG, '&': TK.AMPERSAND, '|': TK.PIPE,
        };
        if (one_map[src[i]!]) { tokens.push(new Token(one_map[src[i]!]!, src[i]!, line)); i++; continue; }
        i++;
    }
    tokens.push(new Token(TK.EOF, null, line));
    return tokens;
}

function is_alpha(c: string): boolean { return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c === '_'; }
function is_alpha_num(c: string): boolean { return is_alpha(c) || (c >= '0' && c <= '9'); }
function escape_char(c: string): string {
    return ({ n: '\n', t: '\t', r: '\r', '\\': '\\', '"': '"', "'": "'" } as Record<string, string>)[c] ?? c;
}

class ParseError extends Error {
    line: number;
    constructor(msg: string, line: number) { super(`Line ${line}: ${msg}`); this.line = line; }
}

// eslint-disable-next-line @typescript-eslint/no-explicit-any
export type AstNode = Record<string, any>;

export interface NSFn {
    __fn: true;
    call?: (args: NSValue[]) => NSValue;
    def?: AstNode;
    lambda?: AstNode;
    closure?: Env;
}

export type NSValue = number | string | boolean | null | NSFn | NSValue[];

class Parser {
    private tokens: Token[];
    private pos: number;
    constructor(tokens: Token[]) { this.tokens = tokens; this.pos = 0; }
    peek(): Token { return this.tokens[this.pos]!; }
    at(type: string): boolean { return this.peek().type === type; }
    advance(): Token { return this.tokens[this.pos++]!; }
    expect(type: string): Token {
        const t = this.peek();
        if (t.type !== type) throw new ParseError(`expected '${type}', got '${t.type}' ('${t.value}')`, t.line);
        return this.advance();
    }
    eat(type: string): boolean { if (this.at(type)) { this.advance(); return true; } return false; }
    parse_program(): AstNode {
        const stmts: AstNode[] = [];
        while (!this.at(TK.EOF)) stmts.push(this.parse_top_level());
        return { kind: 'Program', stmts };
    }
    parse_top_level(): AstNode {
        const t = this.peek();
        if (t.type === TK.FN)     return this.parse_fn();
        if (t.type === TK.LET)    return this.parse_let();
        if (t.type === TK.USE)    return this.parse_use();
        if (t.type === TK.TYPE)   return this.parse_type_alias();
        if (t.type === TK.STRUCT) return this.parse_struct();
        if (t.type === TK.IF)     return this.parse_if();
        if (t.type === TK.FOR)    return this.parse_for();
        if (t.type === TK.LBRACE) return this.parse_block();
        return this.parse_expr_stmt();
    }
    parse_fn(): AstNode {
        const line = this.peek().line;
        this.expect(TK.FN);
        const name = this.expect(TK.IDENT).value as string;
        this.expect(TK.LPAREN);
        const params: AstNode[] = [];
        while (!this.at(TK.RPAREN) && !this.at(TK.EOF)) {
            const p_name = this.expect(TK.IDENT).value as string;
            let p_type = 'i32';
            if (this.eat(TK.COLON)) p_type = this.parse_type();
            params.push({ name: p_name, type: p_type });
            this.eat(TK.COMMA);
        }
        this.expect(TK.RPAREN);
        let ret_type = 'void';
        if (!this.at(TK.LBRACE)) ret_type = this.parse_type();
        const body = this.parse_block();
        return { kind: 'FnDef', name, params, ret_type, body, line };
    }
    parse_type(): string {
        const t = this.advance().value as string;
        while (this.at(TK.LT) || this.at(TK.LBRACKET)) {
            if (this.at(TK.LT)) { this.advance(); this.parse_type(); this.expect(TK.GT); }
            if (this.at(TK.LBRACKET)) { this.advance(); this.expect(TK.RBRACKET); }
        }
        if (this.at(TK.ARROW)) { this.advance(); this.parse_type(); }
        return t;
    }
    parse_let(): AstNode {
        const line = this.peek().line;
        this.expect(TK.LET);
        const name = this.expect(TK.IDENT).value as string;
        if (this.eat(TK.COLON)) this.parse_type();
        let init: AstNode | null = null;
        if (this.eat(TK.ASSIGN)) init = this.parse_expr();
        this.eat(TK.SEMI);
        return { kind: 'LetStmt', name, init, line };
    }
    parse_use(): AstNode {
        this.expect(TK.USE);
        const mod = this.expect(TK.IDENT).value as string;
        this.eat(TK.SEMI);
        return { kind: 'UseStmt', mod };
    }
    parse_type_alias(): AstNode {
        this.expect(TK.TYPE);
        this.expect(TK.IDENT);
        this.expect(TK.ASSIGN);
        while (!this.at(TK.EOF) && !this.at(TK.FN) && !this.at(TK.LET) &&
               !this.at(TK.TYPE) && !this.at(TK.USE)) {
            if (this.at(TK.LBRACE)) { this.parse_block(); break; }
            this.advance();
        }
        return { kind: 'TypeAlias' };
    }
    parse_struct(): AstNode {
        this.expect(TK.STRUCT);
        this.expect(TK.IDENT);
        this.parse_block();
        return { kind: 'StructDef' };
    }
    parse_block(): AstNode {
        this.expect(TK.LBRACE);
        const stmts: AstNode[] = [];
        while (!this.at(TK.RBRACE) && !this.at(TK.EOF)) stmts.push(this.parse_stmt());
        this.expect(TK.RBRACE);
        return { kind: 'Block', stmts };
    }
    parse_stmt(): AstNode {
        const t = this.peek();
        if (t.type === TK.LET)      return this.parse_let();
        if (t.type === TK.RETURN)   return this.parse_return();
        if (t.type === TK.IF)       return this.parse_if();
        if (t.type === TK.FOR)      return this.parse_for();
        if (t.type === TK.BREAK)    { this.advance(); this.eat(TK.SEMI); return { kind: 'Break' }; }
        if (t.type === TK.CONTINUE) { this.advance(); this.eat(TK.SEMI); return { kind: 'Continue' }; }
        if (t.type === TK.LBRACE)   return this.parse_block();
        return this.parse_expr_stmt();
    }
    parse_return(): AstNode {
        const line = this.peek().line;
        this.expect(TK.RETURN);
        let value: AstNode | null = null;
        if (!this.at(TK.RBRACE) && !this.at(TK.EOF) && !this.at(TK.SEMI)) value = this.parse_expr();
        this.eat(TK.SEMI);
        return { kind: 'ReturnStmt', value, line };
    }
    parse_if(): AstNode {
        const line = this.peek().line;
        this.expect(TK.IF);
        const cond = this.parse_expr();
        const then = this.parse_block();
        let alt: AstNode | null = null;
        if (this.eat(TK.ELSE)) alt = this.at(TK.IF) ? this.parse_if() : this.parse_block();
        return { kind: 'IfStmt', cond, then, alt, line };
    }
    parse_for(): AstNode {
        const line = this.peek().line;
        this.expect(TK.FOR);
        const var_name = this.expect(TK.IDENT).value as string;
        this.expect(TK.IN);
        const from = this.parse_expr();
        this.expect(TK.TO);
        const to = this.parse_expr();
        const body = this.parse_block();
        return { kind: 'ForStmt', var: var_name, from, to, body, line };
    }
    parse_expr_stmt(): AstNode {
        const expr = this.parse_expr();
        this.eat(TK.SEMI);
        return { kind: 'ExprStmt', expr };
    }
    parse_expr(): AstNode { return this.parse_assign(); }
    parse_assign(): AstNode {
        const left = this.parse_or();
        const assign_ops: Record<string, string> = {
            [TK.ASSIGN]: '=', [TK.PLUS_ASSIGN]: '+=',
            [TK.MINUS_ASSIGN]: '-=', [TK.STAR_ASSIGN]: '*=', [TK.SLASH_ASSIGN]: '/=',
        };
        const op = assign_ops[this.peek().type];
        if (op) {
            const line = this.peek().line; this.advance();
            const right = this.parse_expr();
            return { kind: 'Assign', op, target: left, value: right, line };
        }
        return left;
    }
    parse_or(): AstNode {
        let left = this.parse_and();
        while (this.at(TK.OR)) {
            const line = this.peek().line; this.advance();
            left = { kind: 'Binary', op: '||', left, right: this.parse_and(), line };
        }
        return left;
    }
    parse_and(): AstNode {
        let left = this.parse_cmp();
        while (this.at(TK.AND)) {
            const line = this.peek().line; this.advance();
            left = { kind: 'Binary', op: '&&', left, right: this.parse_cmp(), line };
        }
        return left;
    }
    parse_cmp(): AstNode {
        const left = this.parse_add();
        const cmp_ops: Record<string, string> = {
            [TK.EQ]:'==', [TK.NEQ]:'!=', [TK.LT]:'<', [TK.LE]:'<=', [TK.GT]:'>', [TK.GE]:'>=',
        };
        const op = cmp_ops[this.peek().type];
        if (op) {
            const line = this.peek().line; this.advance();
            return { kind: 'Binary', op, left, right: this.parse_add(), line };
        }
        return left;
    }
    parse_add(): AstNode {
        let left = this.parse_mul();
        while (this.at(TK.PLUS) || this.at(TK.MINUS)) {
            const op = this.peek().type; const line = this.peek().line; this.advance();
            left = { kind: 'Binary', op, left, right: this.parse_mul(), line };
        }
        return left;
    }
    parse_mul(): AstNode {
        let left = this.parse_unary();
        while (this.at(TK.STAR) || this.at(TK.SLASH) || this.at(TK.PERCENT)) {
            const op = this.peek().type; const line = this.peek().line; this.advance();
            left = { kind: 'Binary', op, left, right: this.parse_unary(), line };
        }
        return left;
    }
    parse_unary(): AstNode {
        if (this.at(TK.MINUS)) {
            const line = this.peek().line; this.advance();
            return { kind: 'Unary', op: '-', expr: this.parse_unary(), line };
        }
        if (this.at(TK.BANG)) {
            const line = this.peek().line; this.advance();
            return { kind: 'Unary', op: '!', expr: this.parse_unary(), line };
        }
        return this.parse_postfix();
    }
    parse_postfix(): AstNode {
        let base = this.parse_primary();
        for (;;) {
            if (this.at(TK.LPAREN)) {
                const line = this.peek().line; this.advance();
                const args: AstNode[] = [];
                while (!this.at(TK.RPAREN) && !this.at(TK.EOF)) { args.push(this.parse_expr()); this.eat(TK.COMMA); }
                this.expect(TK.RPAREN);
                let trailing_block: AstNode | null = null;
                if (this.at(TK.LBRACE)) trailing_block = this.parse_block();
                base = { kind: 'Call', callee: base, args, trailing_block, line };
            } else if (this.at(TK.DOT)) {
                const line = this.peek().line; this.advance();
                const field = this.expect(TK.IDENT).value as string;
                base = { kind: 'Field', obj: base, field, line };
            } else if (this.at(TK.AS)) {
                this.advance(); this.parse_type();
            } else { break; }
        }
        return base;
    }
    parse_primary(): AstNode {
        const t = this.peek();
        if (t.type === TK.INT)    { this.advance(); return { kind: 'Lit', value: t.value }; }
        if (t.type === TK.FLOAT)  { this.advance(); return { kind: 'Lit', value: t.value }; }
        if (t.type === TK.STRING) { this.advance(); return { kind: 'Lit', value: t.value }; }
        if (t.type === TK.BOOL)   { this.advance(); return { kind: 'Lit', value: t.value }; }
        if (t.type === TK.IDENT)  { this.advance(); return { kind: 'Ident', name: t.value, line: t.line }; }
        if (t.type === TK.LPAREN) { this.advance(); const e = this.parse_expr(); this.expect(TK.RPAREN); return e; }
        if (t.type === TK.BACKTICK) return this.parse_template_lit();
        if (t.type === TK.LBRACE)   return this.parse_lambda();
        if (t.type === TK.FN)       return this.parse_fn_expr();
        throw new ParseError(`unexpected token '${t.type}' ('${t.value}')`, t.line);
    }
    parse_template_lit(): AstNode {
        const line = this.peek().line;
        this.expect(TK.BACKTICK);
        const parts: AstNode[] = [];
        while (!this.at(TK.BACKTICK) && !this.at(TK.EOF)) {
            if (this.at(TK.STRING)) parts.push({ kind: 'Lit', value: this.advance().value });
            else if (this.at(TK.LBRACE)) { this.advance(); parts.push(this.parse_expr()); this.expect(TK.RBRACE); }
            else break;
        }
        this.eat(TK.BACKTICK);
        return { kind: 'Template', parts, line };
    }
    parse_lambda(): AstNode {
        const saved = this.pos;
        try {
            this.expect(TK.LBRACE);
            const params: string[] = [];
            while (this.at(TK.IDENT)) { params.push(this.advance().value as string); this.eat(TK.COMMA); }
            if (this.at(TK.IN) && params.length > 0) {
                this.advance();
                const body: AstNode[] = [];
                while (!this.at(TK.RBRACE) && !this.at(TK.EOF)) body.push(this.parse_stmt());
                this.expect(TK.RBRACE);
                return { kind: 'Lambda', params, body };
            }
            this.pos = saved;
        } catch (_) { this.pos = saved; }
        const block = this.parse_block();
        return { kind: 'BlockExpr', block };
    }
    parse_fn_expr(): AstNode {
        this.expect(TK.FN);
        this.expect(TK.LPAREN);
        const params: AstNode[] = [];
        while (!this.at(TK.RPAREN) && !this.at(TK.EOF)) {
            const n = this.expect(TK.IDENT).value as string;
            let type = 'i32';
            if (this.eat(TK.COLON)) type = this.parse_type();
            params.push({ name: n, type });
            this.eat(TK.COMMA);
        }
        this.expect(TK.RPAREN);
        let ret_type = 'void';
        if (!this.at(TK.LBRACE)) ret_type = this.parse_type();
        const body = this.parse_block();
        return { kind: 'FnExpr', params, ret_type, body };
    }
}

export function parse_to_ast(source: string): AstNode {
    const tokens = lex(source);
    const parser = new Parser(tokens);
    return parser.parse_program();
}

class ReturnSignal { value: NSValue; constructor(value: NSValue) { this.value = value; } }
class BreakSignal  {}
class ContinueSignal {}
class NSError extends Error {
    constructor(msg: string, line?: number) { super(line ? `Line ${line}: ${msg}` : msg); }
}

export class Env {
    private vars: Map<string, NSValue>;
    parent: Env | null;
    constructor(parent: Env | null = null) { this.vars = new Map(); this.parent = parent; }
    get(name: string): NSValue {
        if (this.vars.has(name)) return this.vars.get(name)!;
        if (this.parent) return this.parent.get(name);
        throw new NSError(`undefined variable '${name}'`);
    }
    set(name: string, value: NSValue): void {
        if (this.vars.has(name)) { this.vars.set(name, value); return; }
        if (this.parent && this.parent.has(name)) { this.parent.set(name, value); return; }
        this.vars.set(name, value);
    }
    has(name: string): boolean { return this.vars.has(name) || (this.parent ? this.parent.has(name) : false); }
    def(name: string, value: NSValue): void { this.vars.set(name, value); }
}

const MAX_CALLS  = 100000;
const MAX_ITERS  = 1000000;

export interface NSInterpreterOptions {
    print?: (v: string) => void;
    error?: (v: string) => void;
}

export class NSInterpreter {
    private print: (v: string) => void;
    private error: (v: string) => void;
    private call_depth: number;
    private iter_count: number;
    private globals: Env;
    // scrollbar drag state (used in v_scrollbar-like internal tracking)
    private _scroll_drag_y: number = 0;
    private _scroll_orig: number = 0;

    constructor({ print = console.log, error = console.error }: NSInterpreterOptions = {}) {
        this.print      = print;
        this.error      = error;
        this.call_depth = 0;
        this.iter_count = 0;
        this.globals    = new Env();
        this._seed_globals();
    }
    private _seed_globals(): void {
        const g = this.globals, self = this;
        g.def('print',   { __fn: true, call: (args: NSValue[]) => { self.print(args.map(ns_str).join('')); return null; } });
        g.def('println', { __fn: true, call: (args: NSValue[]) => { self.print(args.map(ns_str).join('')); return null; } });
        g.def('assert',  { __fn: true, call: ([cond, msg]: NSValue[]) => {
            if (!cond) throw new NSError(`assertion failed${msg ? ': ' + String(msg) : ''}`);
            return null;
        }});
        g.def('sqrt',  { __fn: true, call: ([x]: NSValue[]) => Math.sqrt(x as number) });
        g.def('abs',   { __fn: true, call: ([x]: NSValue[]) => Math.abs(x as number) });
        g.def('floor', { __fn: true, call: ([x]: NSValue[]) => Math.floor(x as number) });
        g.def('ceil',  { __fn: true, call: ([x]: NSValue[]) => Math.ceil(x as number) });
        g.def('round', { __fn: true, call: ([x]: NSValue[]) => Math.round(x as number) });
        g.def('min',   { __fn: true, call: ([a, b]: NSValue[]) => Math.min(a as number, b as number) });
        g.def('max',   { __fn: true, call: ([a, b]: NSValue[]) => Math.max(a as number, b as number) });
        g.def('pow',   { __fn: true, call: ([a, b]: NSValue[]) => Math.pow(a as number, b as number) });
        g.def('sin',   { __fn: true, call: ([x]: NSValue[]) => Math.sin(x as number) });
        g.def('cos',   { __fn: true, call: ([x]: NSValue[]) => Math.cos(x as number) });
        g.def('log',   { __fn: true, call: ([x]: NSValue[]) => Math.log(x as number) });
    }
    run(source: string): void {
        this.call_depth = 0; this.iter_count = 0;
        let ast: AstNode;
        try { ast = parse_to_ast(source); }
        catch (e) { this.error('Parse error: ' + (e as Error).message); return; }
        try { this.eval_program(ast, this.globals); }
        catch (e) { if (e instanceof ReturnSignal) return; this.error('Runtime error: ' + (e as Error).message); }
    }
    private eval_program(ast: AstNode, env: Env): void {
        for (const stmt of ast.stmts as AstNode[]) if (stmt.kind === 'FnDef') env.def(stmt.name as string, { __fn: true, def: stmt, closure: env });
        for (const stmt of ast.stmts as AstNode[]) if (stmt.kind !== 'FnDef') this.eval_stmt(stmt, env);
    }
    private eval_block(block: AstNode, env: Env): NSValue | ReturnSignal | BreakSignal | ContinueSignal {
        const local = new Env(env);
        for (const stmt of block.stmts as AstNode[]) {
            const sig = this.eval_stmt(stmt, local);
            if (sig instanceof ReturnSignal || sig instanceof BreakSignal || sig instanceof ContinueSignal) return sig;
        }
        return null;
    }
    private eval_stmt(stmt: AstNode, env: Env): NSValue | ReturnSignal | BreakSignal | ContinueSignal {
        switch (stmt.kind as string) {
        case 'FnDef': env.def(stmt.name as string, { __fn: true, def: stmt, closure: env }); break;
        case 'LetStmt': { const val = stmt.init ? this.eval_expr(stmt.init as AstNode, env) : null; env.def(stmt.name as string, val); break; }
        case 'ReturnStmt': return new ReturnSignal(stmt.value ? this.eval_expr(stmt.value as AstNode, env) : null);
        case 'Break':    return new BreakSignal();
        case 'Continue': return new ContinueSignal();
        case 'IfStmt':   return this.eval_if(stmt, env);
        case 'ForStmt':  return this.eval_for(stmt, env);
        case 'ExprStmt': this.eval_expr(stmt.expr as AstNode, env); break;
        case 'Block':    return this.eval_block(stmt, env);
        case 'UseStmt': case 'TypeAlias': case 'StructDef': break;
        }
        return null;
    }
    private eval_if(stmt: AstNode, env: Env): NSValue | ReturnSignal | BreakSignal | ContinueSignal {
        const cond = this.eval_expr(stmt.cond as AstNode, env);
        if (cond) return this.eval_block(stmt.then as AstNode, env);
        if (stmt.alt) {
            if ((stmt.alt as AstNode).kind === 'IfStmt') return this.eval_if(stmt.alt as AstNode, env);
            return this.eval_block(stmt.alt as AstNode, env);
        }
        return null;
    }
    private eval_for(stmt: AstNode, env: Env): NSValue | ReturnSignal | BreakSignal | ContinueSignal {
        const from = this.eval_expr(stmt.from as AstNode, env) as number;
        const to   = this.eval_expr(stmt.to as AstNode, env) as number;
        const local = new Env(env);
        local.def(stmt.var as string, from);
        for (let i = from; i < to; i++) {
            if (++this.iter_count > MAX_ITERS) throw new NSError('iteration limit exceeded');
            local.set(stmt.var as string, i);
            const sig = this.eval_block(stmt.body as AstNode, local);
            if (sig instanceof ReturnSignal) return sig;
            if (sig instanceof BreakSignal)  break;
        }
        return null;
    }
    private eval_expr(node: AstNode, env: Env): NSValue {
        switch (node.kind as string) {
        case 'Lit':    return node.value as NSValue;
        case 'Ident':  return env.get(node.name as string);
        case 'Assign': {
            let val = this.eval_expr(node.value as AstNode, env);
            if (node.op === '+=') val = (env.get((node.target as AstNode).name as string) as number) + (val as number);
            if (node.op === '-=') val = (env.get((node.target as AstNode).name as string) as number) - (val as number);
            if (node.op === '*=') val = (env.get((node.target as AstNode).name as string) as number) * (val as number);
            if (node.op === '/=') val = (env.get((node.target as AstNode).name as string) as number) / (val as number);
            if ((node.target as AstNode).kind === 'Field') {
                const obj = this.eval_expr((node.target as AstNode).obj as AstNode, env);
                // eslint-disable-next-line @typescript-eslint/no-explicit-any
                if (obj && typeof obj === 'object') (obj as any)[(node.target as AstNode).field as string] = val;
            } else if ((node.target as AstNode).kind === 'Ident') {
                env.set((node.target as AstNode).name as string, val);
            }
            return val;
        }
        case 'Binary': {
            if (node.op === '&&') return this.eval_expr(node.left as AstNode, env) && this.eval_expr(node.right as AstNode, env);
            if (node.op === '||') return this.eval_expr(node.left as AstNode, env) || this.eval_expr(node.right as AstNode, env);
            const l = this.eval_expr(node.left as AstNode, env);
            const r = this.eval_expr(node.right as AstNode, env);
            switch (node.op as string) {
            case '+':  return typeof l === 'string' || typeof r === 'string' ? String(l) + String(r) : (l as number) + (r as number);
            case '-':  return (l as number) - (r as number);
            case '*':  return (l as number) * (r as number);
            case '/':  if (r === 0) throw new NSError('division by zero', node.line as number); return (l as number) / (r as number);
            case '%':  return (l as number) % (r as number);
            case '==': return l === r;
            case '!=': return l !== r;
            case '<':  return (l as number) < (r as number);
            case '<=': return (l as number) <= (r as number);
            case '>':  return (l as number) > (r as number);
            case '>=': return (l as number) >= (r as number);
            }
            break;
        }
        case 'Unary': {
            const v = this.eval_expr(node.expr as AstNode, env);
            if (node.op === '-') return -(v as number);
            if (node.op === '!') return !v;
            break;
        }
        case 'Call': return this.eval_call(node, env);
        case 'Field': {
            const obj = this.eval_expr(node.obj as AstNode, env);
            if (obj == null) throw new NSError(`null field access '.${node.field as string}'`, node.line as number);
            // eslint-disable-next-line @typescript-eslint/no-explicit-any
            return ((obj as any)[node.field as string] as NSValue) ?? null;
        }
        case 'Template': {
            let s = '';
            for (const p of node.parts as AstNode[]) s += ns_str(this.eval_expr(p, env));
            return s;
        }
        case 'Lambda':    return { __fn: true, lambda: node, closure: env };
        case 'FnExpr':    return { __fn: true, def: node, closure: env };
        case 'BlockExpr': return this.eval_block(node.block as AstNode, env) as NSValue;
        }
        return null;
    }
    private eval_call(node: AstNode, env: Env): NSValue {
        if (++this.call_depth > MAX_CALLS) throw new NSError('call stack overflow');
        try {
            let fn: NSValue;
            if ((node.callee as AstNode).kind === 'Ident') {
                try { fn = env.get((node.callee as AstNode).name as string); }
                catch (_) { throw new NSError(`undefined function '${(node.callee as AstNode).name as string}'`, node.line as number); }
            } else if ((node.callee as AstNode).kind === 'Field') {
                const obj = this.eval_expr((node.callee as AstNode).obj as AstNode, env);
                const method = (node.callee as AstNode).field as string;
                const args   = (node.args as AstNode[]).map(a => this.eval_expr(a, env));
                if (Array.isArray(obj)) {
                    if (method === 'push') { (obj as NSValue[]).push(...args); return null; }
                    if (method === 'len')  return (obj as NSValue[]).length;
                    if (method === 'pop')  return (obj as NSValue[]).pop() ?? null;
                }
                if (typeof obj === 'string') {
                    if (method === 'len') return obj.length;
                }
                return null;
            } else {
                fn = this.eval_expr(node.callee as AstNode, env);
            }
            if (!fn || typeof fn !== 'object' || !(fn as NSFn).__fn) throw new NSError(`'${String((node.callee as AstNode).name ?? '?')}' is not a function`, node.line as number);
            const nsfn = fn as NSFn;
            if (nsfn.call) {
                const args = (node.args as AstNode[]).map(a => this.eval_expr(a, env));
                return nsfn.call(args) ?? null;
            }
            const def    = nsfn.def ?? nsfn.lambda;
            const params: AstNode[] = (def as AstNode).params ?? [];
            const args   = (node.args as AstNode[]).map(a => this.eval_expr(a, env));
            if (node.trailing_block) {
                args.push({ __fn: true,
                    lambda: { kind: 'Lambda', params: params.length > args.length ?
                        [(params[params.length-1] as AstNode).name ?? 'value'] : [],
                        body: (node.trailing_block as AstNode).stmts },
                    closure: env });
            }
            const local = new Env(nsfn.closure ?? this.globals);
            params.forEach((p: AstNode, i: number) => {
                const p_name = typeof p === 'string' ? p : (p.name as string);
                local.def(p_name, args[i] ?? null);
            });
            const sig = this.eval_block((def as AstNode).body as AstNode, local);
            if (sig instanceof ReturnSignal) return sig.value ?? null;
            return null;
        } finally { this.call_depth--; }
    }
}

function ns_str(v: NSValue): string {
    if (v === null || v === undefined) return 'null';
    if (typeof v === 'boolean') return v ? 'true' : 'false';
    if (typeof v === 'number') return Number.isInteger(v) ? String(v) : String(v);
    if (typeof v === 'object' && (v as NSFn).__fn) return '<fn>';
    if (typeof v === 'object') return JSON.stringify(v);
    return String(v);
}

// Suppress unused variable warning for TKValue (used only as type annotation context)
const _TK_UNUSED: TKValue | undefined = undefined;
void _TK_UNUSED;
