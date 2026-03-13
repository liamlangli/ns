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

type tk_value = typeof TK[keyof typeof TK];

const KEYWORDS = new Set([
    'fn','let','return','if','else','for','in','to','use',
    'break','continue','as','type','struct','true','false',
]);

class token {
    type: string;
    value: number | string | boolean | null;
    line: number;
    constructor(type: string, value: number | string | boolean | null, line: number) {
        this.type = type; this.value = value; this.line = line;
    }
}

function lex(src: string): token[] {
    const tokens: token[] = [];
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
            tokens.push(new token(is_float ? TK.FLOAT : TK.INT,
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
            tokens.push(new token(TK.STRING, s, line));
            i = j; continue;
        }
        if (src[i] === '`') {
            tokens.push(new token(TK.BACKTICK, '`', line));
            i++;
            let s = '';
            while (i < n && src[i] !== '`') {
                if (src[i] === '{') {
                    if (s) tokens.push(new token(TK.STRING, s, line));
                    s = '';
                    tokens.push(new token(TK.LBRACE, '{', line));
                    i++;
                    let depth = 1;
                    let inner = '';
                    while (i < n && depth > 0) {
                        if (src[i] === '{') depth++;
                        else if (src[i] === '}') { depth--; if (depth === 0) { i++; break; } }
                        inner += src[i++];
                    }
                    tokens.push(...lex(inner));
                    tokens.push(new token(TK.RBRACE, '}', line));
                } else {
                    if (src[i] === '\n') line++;
                    s += src[i++];
                }
            }
            if (s) tokens.push(new token(TK.STRING, s, line));
            tokens.push(new token(TK.BACKTICK, '`', line));
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
            tokens.push(new token(TK.STRING, s, line));
            i = j; continue;
        }
        if (is_alpha(src[i]!)) {
            let j = i;
            while (j < n && is_alpha_num(src[j]!)) j++;
            const word = src.slice(i, j);
            if (word === 'true' || word === 'false')
                tokens.push(new token(TK.BOOL, word === 'true', line));
            else if (KEYWORDS.has(word))
                tokens.push(new token(word, word, line));
            else
                tokens.push(new token(TK.IDENT, word, line));
            i = j; continue;
        }
        const two = src.slice(i, i+2);
        const two_map: Record<string, string> = {
            '==': TK.EQ, '!=': TK.NEQ, '<=': TK.LE, '>=': TK.GE,
            '&&': TK.AND, '||': TK.OR, '->': TK.ARROW,
            '+=': TK.PLUS_ASSIGN, '-=': TK.MINUS_ASSIGN,
            '*=': TK.STAR_ASSIGN, '/=': TK.SLASH_ASSIGN,
        };
        if (two_map[two]) { tokens.push(new token(two_map[two]!, two, line)); i += 2; continue; }
        const one_map: Record<string, string> = {
            '(': TK.LPAREN, ')': TK.RPAREN, '{': TK.LBRACE, '}': TK.RBRACE,
            '[': TK.LBRACKET, ']': TK.RBRACKET,
            ',': TK.COMMA, ':': TK.COLON, ';': TK.SEMI, '.': TK.DOT,
            '+': TK.PLUS, '-': TK.MINUS, '*': TK.STAR,
            '/': TK.SLASH, '%': TK.PERCENT,
            '<': TK.LT, '>': TK.GT, '=': TK.ASSIGN,
            '!': TK.BANG, '&': TK.AMPERSAND, '|': TK.PIPE,
        };
        if (one_map[src[i]!]) { tokens.push(new token(one_map[src[i]!]!, src[i]!, line)); i++; continue; }
        i++;
    }
    tokens.push(new token(TK.EOF, null, line));
    return tokens;
}

function is_alpha(c: string): boolean { return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c === '_'; }
function is_alpha_num(c: string): boolean { return is_alpha(c) || (c >= '0' && c <= '9'); }
function escape_char(c: string): string {
    return ({ n: '\n', t: '\t', r: '\r', '\\': '\\', '"': '"', "'": "'" } as Record<string, string>)[c] ?? c;
}

class parse_error extends Error {
    line: number;
    constructor(msg: string, line: number) { super(`Line ${line}: ${msg}`); this.line = line; }
}

// eslint-disable-next-line @typescript-eslint/no-explicit-any
export type ast_node = Record<string, any>;

export interface ns_fn {
    __fn: true;
    call?: (args: ns_value[]) => ns_value;
    def?: ast_node;
    lambda?: ast_node;
    closure? : env_scope;
}

export type ns_value = number | string | boolean | null | ns_fn | ns_value[];

class ns_parser {
    private tokens: token[];
    private pos: number;
    constructor(tokens: token[]) { this.tokens = tokens; this.pos = 0; }
    peek(): token { return this.tokens[this.pos]!; }
    at(type: string): boolean { return this.peek().type === type; }
    advance(): token { return this.tokens[this.pos++]!; }
    expect(type: string): token {
        const t = this.peek();
        if (t.type !== type) throw new parse_error(`expected '${type}', got '${t.type}' ('${t.value}')`, t.line);
        return this.advance();
    }
    eat(type: string): boolean { if (this.at(type)) { this.advance(); return true; } return false; }
    parse_program(): ast_node {
        const stmts: ast_node[] = [];
        while (!this.at(TK.EOF)) stmts.push(this.parse_top_level());
        return { kind: 'Program', stmts };
    }
    parse_top_level(): ast_node {
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
    parse_fn(): ast_node {
        const line = this.peek().line;
        this.expect(TK.FN);
        const name = this.expect(TK.IDENT).value as string;
        this.expect(TK.LPAREN);
        const params: ast_node[] = [];
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
    parse_let(): ast_node {
        const line = this.peek().line;
        this.expect(TK.LET);
        const name = this.expect(TK.IDENT).value as string;
        if (this.eat(TK.COLON)) this.parse_type();
        let init: ast_node | null = null;
        if (this.eat(TK.ASSIGN)) init = this.parse_expr();
        this.eat(TK.SEMI);
        return { kind: 'LetStmt', name, init, line };
    }
    parse_use(): ast_node {
        this.expect(TK.USE);
        const mod = this.expect(TK.IDENT).value as string;
        this.eat(TK.SEMI);
        return { kind: 'UseStmt', mod };
    }
    parse_type_alias(): ast_node {
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
    parse_struct(): ast_node {
        this.expect(TK.STRUCT);
        this.expect(TK.IDENT);
        this.parse_block();
        return { kind: 'StructDef' };
    }
    parse_block(): ast_node {
        this.expect(TK.LBRACE);
        const stmts: ast_node[] = [];
        while (!this.at(TK.RBRACE) && !this.at(TK.EOF)) stmts.push(this.parse_stmt());
        this.expect(TK.RBRACE);
        return { kind: 'Block', stmts };
    }
    parse_stmt(): ast_node {
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
    parse_return(): ast_node {
        const line = this.peek().line;
        this.expect(TK.RETURN);
        let value: ast_node | null = null;
        if (!this.at(TK.RBRACE) && !this.at(TK.EOF) && !this.at(TK.SEMI)) value = this.parse_expr();
        this.eat(TK.SEMI);
        return { kind: 'ReturnStmt', value, line };
    }
    parse_if(): ast_node {
        const line = this.peek().line;
        this.expect(TK.IF);
        const cond = this.parse_expr();
        const then = this.parse_block();
        let alt: ast_node | null = null;
        if (this.eat(TK.ELSE)) alt = this.at(TK.IF) ? this.parse_if() : this.parse_block();
        return { kind: 'IfStmt', cond, then, alt, line };
    }
    parse_for(): ast_node {
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
    parse_expr_stmt(): ast_node {
        const expr = this.parse_expr();
        this.eat(TK.SEMI);
        return { kind: 'ExprStmt', expr };
    }
    parse_expr(): ast_node { return this.parse_assign(); }
    parse_assign(): ast_node {
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
    parse_or(): ast_node {
        let left = this.parse_and();
        while (this.at(TK.OR)) {
            const line = this.peek().line; this.advance();
            left = { kind: 'Binary', op: '||', left, right: this.parse_and(), line };
        }
        return left;
    }
    parse_and(): ast_node {
        let left = this.parse_cmp();
        while (this.at(TK.AND)) {
            const line = this.peek().line; this.advance();
            left = { kind: 'Binary', op: '&&', left, right: this.parse_cmp(), line };
        }
        return left;
    }
    parse_cmp(): ast_node {
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
    parse_add(): ast_node {
        let left = this.parse_mul();
        while (this.at(TK.PLUS) || this.at(TK.MINUS)) {
            const op = this.peek().type; const line = this.peek().line; this.advance();
            left = { kind: 'Binary', op, left, right: this.parse_mul(), line };
        }
        return left;
    }
    parse_mul(): ast_node {
        let left = this.parse_unary();
        while (this.at(TK.STAR) || this.at(TK.SLASH) || this.at(TK.PERCENT)) {
            const op = this.peek().type; const line = this.peek().line; this.advance();
            left = { kind: 'Binary', op, left, right: this.parse_unary(), line };
        }
        return left;
    }
    parse_unary(): ast_node {
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
    parse_postfix(): ast_node {
        let base = this.parse_primary();
        for (;;) {
            if (this.at(TK.LPAREN)) {
                const line = this.peek().line; this.advance();
                const args: ast_node[] = [];
                while (!this.at(TK.RPAREN) && !this.at(TK.EOF)) { args.push(this.parse_expr()); this.eat(TK.COMMA); }
                this.expect(TK.RPAREN);
                let trailing_block: ast_node | null = null;
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
    parse_primary(): ast_node {
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
        throw new parse_error(`unexpected token '${t.type}' ('${t.value}')`, t.line);
    }
    parse_template_lit(): ast_node {
        const line = this.peek().line;
        this.expect(TK.BACKTICK);
        const parts: ast_node[] = [];
        while (!this.at(TK.BACKTICK) && !this.at(TK.EOF)) {
            if (this.at(TK.STRING)) parts.push({ kind: 'Lit', value: this.advance().value });
            else if (this.at(TK.LBRACE)) { this.advance(); parts.push(this.parse_expr()); this.expect(TK.RBRACE); }
            else break;
        }
        this.eat(TK.BACKTICK);
        return { kind: 'Template', parts, line };
    }
    parse_lambda(): ast_node {
        const saved = this.pos;
        try {
            this.expect(TK.LBRACE);
            const params: string[] = [];
            while (this.at(TK.IDENT)) { params.push(this.advance().value as string); this.eat(TK.COMMA); }
            if (this.at(TK.IN) && params.length > 0) {
                this.advance();
                const body: ast_node[] = [];
                while (!this.at(TK.RBRACE) && !this.at(TK.EOF)) body.push(this.parse_stmt());
                this.expect(TK.RBRACE);
                return { kind: 'Lambda', params, body };
            }
            this.pos = saved;
        } catch (_) { this.pos = saved; }
        const block = this.parse_block();
        return { kind: 'BlockExpr', block };
    }
    parse_fn_expr(): ast_node {
        this.expect(TK.FN);
        this.expect(TK.LPAREN);
        const params: ast_node[] = [];
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

export function parse_to_ast(source: string): ast_node {
    const tokens = lex(source);
    const parser_instance = new ns_parser(tokens);
    return parser_instance.parse_program();
}

class return_signal { value: ns_value; constructor(value: ns_value) { this.value = value; } }
class break_signal  {}
class continue_signal {}
class ns_error extends Error {
    constructor(msg: string, line?: number) { super(line ? `Line ${line}: ${msg}` : msg); }
}

export class env_scope {
    private vars: Map<string, ns_value>;
    parent : env_scope | null;
    constructor(parent : env_scope | null = null) { this.vars = new Map(); this.parent = parent; }
    get(name: string): ns_value {
        if (this.vars.has(name)) return this.vars.get(name)!;
        if (this.parent) return this.parent.get(name);
        throw new ns_error(`undefined variable '${name}'`);
    }
    set(name: string, value: ns_value): void {
        if (this.vars.has(name)) { this.vars.set(name, value); return; }
        if (this.parent && this.parent.has(name)) { this.parent.set(name, value); return; }
        this.vars.set(name, value);
    }
    has(name: string): boolean { return this.vars.has(name) || (this.parent ? this.parent.has(name) : false); }
    def(name: string, value: ns_value): void { this.vars.set(name, value); }
}

const MAX_CALLS  = 100000;
const MAX_ITERS  = 1000000;

export interface ns_interpreter_options {
    print?: (v: string) => void;
    error?: (v: string) => void;
}

export class ns_interpreter {
    private print: (v: string) => void;
    private error: (v: string) => void;
    private call_depth: number;
    private iter_count: number;
    private globals : env_scope;
    // scrollbar drag state (used in v_scrollbar-like internal tracking)
    private _scroll_drag_y: number = 0;
    private _scroll_orig: number = 0;

    constructor({ print = console.log, error = console.error }: ns_interpreter_options = {}) {
        this.print      = print;
        this.error      = error;
        this.call_depth = 0;
        this.iter_count = 0;
        this.globals    = new env_scope();
        this._seed_globals();
    }
    private _seed_globals(): void {
        const g = this.globals, self = this;
        g.def('print',   { __fn: true, call: (args: ns_value[]) => { self.print(args.map(ns_str).join('')); return null; } });
        g.def('println', { __fn: true, call: (args: ns_value[]) => { self.print(args.map(ns_str).join('')); return null; } });
        g.def('assert',  { __fn: true, call: ([cond, msg]: ns_value[]) => {
            if (!cond) throw new ns_error(`assertion failed${msg ? ': ' + String(msg) : ''}`);
            return null;
        }});
        g.def('sqrt',  { __fn: true, call: ([x]: ns_value[]) => Math.sqrt(x as number) });
        g.def('abs',   { __fn: true, call: ([x]: ns_value[]) => Math.abs(x as number) });
        g.def('floor', { __fn: true, call: ([x]: ns_value[]) => Math.floor(x as number) });
        g.def('ceil',  { __fn: true, call: ([x]: ns_value[]) => Math.ceil(x as number) });
        g.def('round', { __fn: true, call: ([x]: ns_value[]) => Math.round(x as number) });
        g.def('min',   { __fn: true, call: ([a, b]: ns_value[]) => Math.min(a as number, b as number) });
        g.def('max',   { __fn: true, call: ([a, b]: ns_value[]) => Math.max(a as number, b as number) });
        g.def('pow',   { __fn: true, call: ([a, b]: ns_value[]) => Math.pow(a as number, b as number) });
        g.def('sin',   { __fn: true, call: ([x]: ns_value[]) => Math.sin(x as number) });
        g.def('cos',   { __fn: true, call: ([x]: ns_value[]) => Math.cos(x as number) });
        g.def('log',   { __fn: true, call: ([x]: ns_value[]) => Math.log(x as number) });
    }
    run(source: string): void {
        this.call_depth = 0; this.iter_count = 0;
        let ast: ast_node;
        try { ast = parse_to_ast(source); }
        catch (e) { this.error('Parse error: ' + (e as Error).message); return; }
        try { this.eval_program(ast, this.globals); }
        catch (e) { if (e instanceof return_signal) return; this.error('Runtime error: ' + (e as Error).message); }
    }
    private eval_program(ast: ast_node, env : env_scope): void {
        for (const stmt of ast.stmts as ast_node[]) if (stmt.kind === 'FnDef') env.def(stmt.name as string, { __fn: true, def: stmt, closure : env_scope });
        for (const stmt of ast.stmts as ast_node[]) if (stmt.kind !== 'FnDef') this.eval_stmt(stmt, env_scope);
    }
    private eval_block(block: ast_node, env : env_scope): ns_value | return_signal | break_signal | continue_signal {
        const local = new env_scope(env);
        for (const stmt of block.stmts as ast_node[]) {
            const sig = this.eval_stmt(stmt, local);
            if (sig instanceof return_signal || sig instanceof break_signal || sig instanceof continue_signal) return sig;
        }
        return null;
    }
    private eval_stmt(stmt: ast_node, env : env_scope): ns_value | return_signal | break_signal | continue_signal {
        switch (stmt.kind as string) {
        case 'FnDef' : env_scope.def(stmt.name as string, { __fn: true, def: stmt, closure : env_scope }); break;
        case 'LetStmt': { const val = stmt.init ? this.eval_expr(stmt.init as ast_node, env_scope) : null; env.def(stmt.name as string, val); break; }
        case 'ReturnStmt': return new return_signal(stmt.value ? this.eval_expr(stmt.value as ast_node, env_scope) : null);
        case 'Break':    return new break_signal();
        case 'Continue': return new continue_signal();
        case 'IfStmt':   return this.eval_if(stmt, env_scope);
        case 'ForStmt':  return this.eval_for(stmt, env_scope);
        case 'ExprStmt': this.eval_expr(stmt.expr as ast_node, env_scope); break;
        case 'Block':    return this.eval_block(stmt, env_scope);
        case 'UseStmt': case 'TypeAlias': case 'StructDef': break;
        }
        return null;
    }
    private eval_if(stmt: ast_node, env : env_scope): ns_value | return_signal | break_signal | continue_signal {
        const cond = this.eval_expr(stmt.cond as ast_node, env_scope);
        if (cond) return this.eval_block(stmt.then as ast_node, env_scope);
        if (stmt.alt) {
            if ((stmt.alt as ast_node).kind === 'IfStmt') return this.eval_if(stmt.alt as ast_node, env_scope);
            return this.eval_block(stmt.alt as ast_node, env_scope);
        }
        return null;
    }
    private eval_for(stmt: ast_node, env : env_scope): ns_value | return_signal | break_signal | continue_signal {
        const from = this.eval_expr(stmt.from as ast_node, env_scope) as number;
        const to   = this.eval_expr(stmt.to as ast_node, env_scope) as number;
        const local = new env_scope(env);
        local.def(stmt.var as string, from);
        for (let i = from; i < to; i++) {
            if (++this.iter_count > MAX_ITERS) throw new ns_error('iteration limit exceeded');
            local.set(stmt.var as string, i);
            const sig = this.eval_block(stmt.body as ast_node, local);
            if (sig instanceof return_signal) return sig;
            if (sig instanceof break_signal)  break;
        }
        return null;
    }
    private eval_expr(node: ast_node, env : env_scope): ns_value {
        switch (node.kind as string) {
        case 'Lit':    return node.value as ns_value;
        case 'Ident':  return env.get(node.name as string);
        case 'Assign': {
            let val = this.eval_expr(node.value as ast_node, env_scope);
            if (node.op === '+=') val = (env.get((node.target as ast_node).name as string) as number) + (val as number);
            if (node.op === '-=') val = (env.get((node.target as ast_node).name as string) as number) - (val as number);
            if (node.op === '*=') val = (env.get((node.target as ast_node).name as string) as number) * (val as number);
            if (node.op === '/=') val = (env.get((node.target as ast_node).name as string) as number) / (val as number);
            if ((node.target as ast_node).kind === 'Field') {
                const obj = this.eval_expr((node.target as ast_node).obj as ast_node, env_scope);
                // eslint-disable-next-line @typescript-eslint/no-explicit-any
                if (obj && typeof obj === 'object') (obj as any)[(node.target as ast_node).field as string] = val;
            } else if ((node.target as ast_node).kind === 'Ident') {
                env.set((node.target as ast_node).name as string, val);
            }
            return val;
        }
        case 'Binary': {
            if (node.op === '&&') return this.eval_expr(node.left as ast_node, env_scope) && this.eval_expr(node.right as ast_node, env_scope);
            if (node.op === '||') return this.eval_expr(node.left as ast_node, env_scope) || this.eval_expr(node.right as ast_node, env_scope);
            const l = this.eval_expr(node.left as ast_node, env_scope);
            const r = this.eval_expr(node.right as ast_node, env_scope);
            switch (node.op as string) {
            case '+':  return typeof l === 'string' || typeof r === 'string' ? String(l) + String(r) : (l as number) + (r as number);
            case '-':  return (l as number) - (r as number);
            case '*':  return (l as number) * (r as number);
            case '/':  if (r === 0) throw new ns_error('division by zero', node.line as number); return (l as number) / (r as number);
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
            const v = this.eval_expr(node.expr as ast_node, env_scope);
            if (node.op === '-') return -(v as number);
            if (node.op === '!') return !v;
            break;
        }
        case 'Call': return this.eval_call(node, env_scope);
        case 'Field': {
            const obj = this.eval_expr(node.obj as ast_node, env_scope);
            if (obj == null) throw new ns_error(`null field access '.${node.field as string}'`, node.line as number);
            // eslint-disable-next-line @typescript-eslint/no-explicit-any
            return ((obj as any)[node.field as string] as ns_value) ?? null;
        }
        case 'Template': {
            let s = '';
            for (const p of node.parts as ast_node[]) s += ns_str(this.eval_expr(p, env_scope));
            return s;
        }
        case 'Lambda':    return { __fn: true, lambda: node, closure : env_scope };
        case 'FnExpr':    return { __fn: true, def: node, closure : env_scope };
        case 'BlockExpr': return this.eval_block(node.block as ast_node, env_scope) as ns_value;
        }
        return null;
    }
    private eval_call(node: ast_node, env : env_scope): ns_value {
        if (++this.call_depth > MAX_CALLS) throw new ns_error('call stack overflow');
        try {
            let fn: ns_value;
            if ((node.callee as ast_node).kind === 'Ident') {
                try { fn = env.get((node.callee as ast_node).name as string); }
                catch (_) { throw new ns_error(`undefined function '${(node.callee as ast_node).name as string}'`, node.line as number); }
            } else if ((node.callee as ast_node).kind === 'Field') {
                const obj = this.eval_expr((node.callee as ast_node).obj as ast_node, env_scope);
                const method = (node.callee as ast_node).field as string;
                const args   = (node.args as ast_node[]).map(a => this.eval_expr(a, env_scope));
                if (Array.isArray(obj)) {
                    if (method === 'push') { (obj as ns_value[]).push(...args); return null; }
                    if (method === 'len')  return (obj as ns_value[]).length;
                    if (method === 'pop')  return (obj as ns_value[]).pop() ?? null;
                }
                if (typeof obj === 'string') {
                    if (method === 'len') return obj.length;
                }
                return null;
            } else {
                fn = this.eval_expr(node.callee as ast_node, env_scope);
            }
            if (!fn || typeof fn !== 'object' || !(fn as ns_fn).__fn) throw new ns_error(`'${String((node.callee as ast_node).name ?? '?')}' is not a function`, node.line as number);
            const nsfn = fn as ns_fn;
            if (nsfn.call) {
                const args = (node.args as ast_node[]).map(a => this.eval_expr(a, env_scope));
                return nsfn.call(args) ?? null;
            }
            const def    = nsfn.def ?? nsfn.lambda;
            const params: ast_node[] = (def as ast_node).params ?? [];
            const args   = (node.args as ast_node[]).map(a => this.eval_expr(a, env_scope));
            if (node.trailing_block) {
                args.push({ __fn: true,
                    lambda: { kind: 'Lambda', params: params.length > args.length ?
                        [(params[params.length-1] as ast_node).name ?? 'value'] : [],
                        body: (node.trailing_block as ast_node).stmts },
                    closure : env_scope });
            }
            const local = new env_scope(nsfn.closure ?? this.globals);
            params.forEach((p: ast_node, i: number) => {
                const p_name = typeof p === 'string' ? p : (p.name as string);
                local.def(p_name, args[i] ?? null);
            });
            const sig = this.eval_block((def as ast_node).body as ast_node, local);
            if (sig instanceof return_signal) return sig.value ?? null;
            return null;
        } finally { this.call_depth--; }
    }
}

function ns_str(v: ns_value): string {
    if (v === null || v === undefined) return 'null';
    if (typeof v === 'boolean') return v ? 'true' : 'false';
    if (typeof v === 'number') return Number.isInteger(v) ? String(v) : String(v);
    if (typeof v === 'object' && (v as ns_fn).__fn) return '<fn>';
    if (typeof v === 'object') return JSON.stringify(v);
    return String(v);
}

// Suppress unused variable warning for tk_value (used only as type annotation context)
const _TK_UNUSED: tk_value | undefined = undefined;
void _TK_UNUSED;
