#include "ns.h"
#include "ns_type.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct ns_runtime_t {
    int ref_count;
} ns_runtime_t;

typedef struct ns_context_t {
    ns_value this;
} ns_context_t;

typedef struct ns_token_t {
    ns_token type;
    char val[256];
} ns_token_t;

ns_runtime_t *ns_make_runtime() {
    ns_runtime_t *rt = (ns_runtime_t *)malloc(sizeof(ns_runtime_t));
    rt->ref_count = 0;
    return rt;
}

ns_context_t *ns_make_context(ns_runtime_t *rt) {
    ns_context_t *ctx = (ns_context_t *)malloc(sizeof(ns_context_t));
    ctx->this = NS_NIL;
    return ctx;
}

int ns_is_null(ns_value value) { return value.type == NS_TYPE_NIL; }

int tokenize(ns_token_t *token, char *src, int from) {
    int i = from;
    int to = from + 1;
    int len;
    char lead = src[i]; // TODO parse utf8 characters
    switch (lead) {
    case '0' ... '9': {
        // parse interger or float
        int is_float = 0;
        while (src[i] >= '0' && src[i] <= '9') {
            i++;
        }
        if (src[i] == '.') {
            is_float = 1;
            i++;
            while (src[i] >= '0' && src[i] <= '9') {
                i++;
            }
        }
        if (is_float) {
            token->type = NS_TOKEN_FLOAT_LITERIAL;
        } else {
            token->type = NS_TOKEN_INT_LITERIAL;
        }
        len = i - from;
        memcpy(token->val, src + from, len);
        token->val[len] = '\0';
        to = i;
    } break;
    case ':': {
        token->type = NS_TOKEN_COLON;
        strcpy(token->val, ":");
        to = i + 1;
    } break;
    case ';': {
        token->type = NS_TOKEN_SEMICOLON;
        strcpy(token->val, ";");
        to = i + 1;
    } break;
    case '+':
    case '-':
    case '*':
    case '%': {
        token->type = NS_TOKEN_OPEATOR;
        token->val[0] = lead;
        token->val[1] = '\0';
        to = i + 1;
    } break;
    case '/': {
        if (src[i + 1] == '/') {
            while (src[i] != '\n' && src[i] != '\0') {
                i++;
            }
            token->type = NS_TOKEN_SPACE;
            len = i - from;
            memcpy(token->val, src + from, len);
            token->val[len] = '\0';
            to = i;
        } else {
            token->type = NS_TOKEN_OPEATOR;
            token->val[0] = lead;
            token->val[1] = '\0';
            to = i + 1;
        }
    } break;
    case '^':
    case '&':
    case '|':
    case '~':
    case '!':
    case '?': {
        token->type = NS_TOKEN_BITWISE_OPEATOR;
        token->val[0] = lead;
        token->val[1] = '\0';
        to = i + 1;
    } break;
    case '<':
    case '>': {
        if (src[i + 1] == '=') {
            token->type = NS_TOKEN_BOOL_OPEATOR;
            token->val[0] = lead;
            token->val[1] = '=';
            token->val[2] = '\0';
            to = i + 2;
        } else {
            token->type = NS_TOKEN_BOOL_OPEATOR;
            token->val[0] = lead;
            token->val[1] = '\0';
            to = i + 1;
        }
    } break;
    case '(': {
        token->type = NS_TOKEN_OPEN_BRACE;
        strcpy(token->val, "(");
        to = i + 1;
    } break;
    case ')': {
        token->type = NS_TOKEN_CLOSE_BRACE;
        strcpy(token->val, ")");
        to = i + 1;
    } break;
    case '{': {
        token->type = NS_TOKEN_OPEN_PAREN;
        strcpy(token->val, "{");
        to = i + 1;
    } break;
    case '}': {
        token->type = NS_TOKEN_CLOSE_PAREN;
        strcpy(token->val, "}");
        to = i + 1;
    } break;
    case '[': {
        token->type = NS_TOKEN_OPEN_BRACKET;
        strcpy(token->val, "[");
        to = i + 1;
    } break;
    case ']': {
        token->type = NS_TOKEN_CLOSE_BRACKET;
        strcpy(token->val, "]");
        to = i + 1;
    } break;
    case '=': {
        if (src[i + 1] == '=') {
            token->type = NS_TOKEN_BOOL_OPEATOR;
            token->val[0] = '=';
            token->val[1] = '=';
            token->val[2] = '\0';
            to = i + 2;
        } else {
            token->type = NS_TOKEN_ASSIGN;
            token->val[0] = '=';
            token->val[1] = '\0';
            to = i + 1;
        }
    } break;
    case 39: // '
    case 34: // "
    case 96: // `
    {
        // parse string literial
        char quote = lead;
        i++;
        while (src[i] != quote && src[i] != '\0') {
            i++;
        }
        token->type = NS_TOKEN_STRING_LITERIAL;
        len = i - from;
        memcpy(token->val, src + from, len + 1);
        token->val[len + 1] = '\0';
        to = i + 1;
    } break;
    // try parse key words
    case 'a': {
        if (src[i + 1] == 's' && src[i + 2] == 'y' && src[i + 3] == 'n' && src[i + 4] == 'c') {
            token->type = NS_TOKEN_ASYNC;
            strcpy(token->val, "async");
            to = i + 5;
        } else if (src[i + 1] == 'w' && src[i + 2] == 'a' && src[i + 3] == 'i' && src[i + 4] == 't') {
            token->type = NS_TOKEN_AWAIT;
            strcpy(token->val, "await");
            to = i + 5;
        } else {
            goto identifer;
        }
    } break;
    case 'c': {
        if (src[i + 1] == 'o' && src[i + 2] == 'n' && src[i + 3] == 's' && src[i + 4] == 't') {
            token->type = NS_TOKEN_CONST;
            strcpy(token->val, "const");
            to = i + 5;
        } else {
            goto identifer;
        }
    } break;
    case 'f': {
        if (src[i + 1] == 'n') {
            token->type = NS_TOKEN_FN;
            strcpy(token->val, "fn");
            to = i + 2;
        } else if (src[i + 1] == '3' && src[i + 2] == '2') {
            token->type = NS_TOKEN_TYPE;
            strcpy(token->val, "f32");
            to = i + 3;
        } else if (src[i + 1] == '6' && src[i + 2] == '4') {
            token->type = NS_TOKEN_TYPE;
            strcpy(token->val, "f64");
            to = i + 3;
        } else {
            goto identifer;
        }
    } break;
    case 'i':
        if (src[i + 1] == 'n') {
            token->type = NS_TOKEN_IN;
            strcpy(token->val, "in");
            to = i + 2;
        } else if (src[i + 1] == '3' && src[i + 2] == '2') {
            token->type = NS_TOKEN_TYPE;
            strcpy(token->val, "i32");
            to = i + 3;
        } else if (src[i + 1] == '6' && src[i + 2] == '4') {
            token->type = NS_TOKEN_TYPE;
            strcpy(token->val, "i64");
            to = i + 3;
        } else {
            goto identifer;
        }
        break;
    case 'l': {
        if (src[i + 1] == 'e' && src[i + 2] == 't') {
            token->type = NS_TOKEN_LET;
            strcpy(token->val, "let");
            to = i + 3;
        } else {
            goto identifer;
        }
    } break;
    case 'u': {
        if (src[i + 1] == '3' && src[i + 2] == '2') {
            token->type = NS_TOKEN_TYPE;
            strcpy(token->val, "u32");
            to = i + 3;
        } else if (src[i + 1] == '6' && src[i + 2] == '4') {
            token->type = NS_TOKEN_TYPE;
            strcpy(token->val, "u64");
            to = i + 3;
        } else {
            goto identifer;
        }
    } break;
    case 's':
        if (src[i + 1] == 't' && src[i + 2] == 'r' && src[i + 3] == 'u' && src[i + 4] == 'c' && src[i + 5] == 't') {
            token->type = NS_TOKEN_STRUCT;
            strcpy(token->val, "struct");
            to = i + 6;
        } else {
            goto identifer;
        }
        break;
    case ' ':
    case '\t':
    case '\n':
    case '\r': {
        token->type = NS_TOKEN_SPACE;
        while (src[i] == ' ' || src[i] == '\t' || src[i] == '\n' || src[i] == '\r') {
            i++;
        }
        len = i - from;
        memcpy(token->val, src + from, len);
        token->val[len] = '\0';
        to = i;
    } break;
    case '\0': {
        token->type = NS_TOKEN_EOF;
        token->val[0] = '\0';
    } break;
    identifer:
    default: {
        token->type = NS_TOKEN_IDENTIFIER;
        while ((src[i] >= 'a' && src[i] <= 'z') || (src[i] >= 'A' && src[i] <= 'Z')) {
            i++;
        }
        int len = macro_max(i - from, 1);
        memcpy(token->val, src + from, len);
        token->val[len] = '\0';
        to = from + len;
    } break;
    }
    return to;
}

char *io_read_file(const char *path) {
    FILE *file = fopen(path, "rb");
    if (!file) {
        return NULL;
    }
    fseek(file, 0, SEEK_END);
    long size = ftell(file);
    fseek(file, 0, SEEK_SET);
    char *buffer = (char *)malloc(size + 1);
    fread(buffer, 1, size, file);
    fclose(file);
    buffer[size] = '\0';
    return buffer;
}

ns_value ns_eval(ns_context_t *ctx, const char *source) {
    int len = strlen(source);
    int i = 0;
    ns_token_t token = {0};
    do {
        i = tokenize(&token, (char *)source, i);
        if (token.type == NS_TOKEN_SPACE) {
            continue;
        } else {
            printf("%s %s\n", ns_token_to_string(token.type), token.val);
        }
    } while (token.type != NS_TOKEN_EOF && i < len);
    return NS_NIL;
}

int main(int argc, char **argv) {
    ns_runtime_t *rt = ns_make_runtime();
    ns_context_t *ctx = ns_make_context(rt);

    if (argc < 2) {
        printf("Usage: ns [file.ns]\n");
        return 1;
    }

    const char *source = io_read_file(argv[1]);
    if (source == NULL) {
        printf("Failed to read file: %s\n", argv[1]);
        return 1;
    }

    ns_value val = ns_eval(ctx, source);
    return 0;
}