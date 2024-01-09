#include "ns_tokenize.h"
#include "ns.h"

const char *ns_token_to_string(NS_TOKEN type) {
    switch (type) {
        case NS_TOKEN_INT_LITERIAL:
            return "NS_TOKEN_INT_LITERIAL";
        case NS_TOKEN_FLOAT_LITERIAL:
            return "NS_TOKEN_FLOAT_LITERIAL";
        case NS_TOKEN_STRING_LITERIAL:
            return "NS_TOKEN_STRING_LITERIAL";
        case NS_TOKEN_CONST:
            return "NS_TOKEN_CONST";
        case NS_TOKEN_LET:
            return "NS_TOKEN_LET";
        case NS_TOKEN_FN:
            return "NS_TOKEN_FN";
        case NS_TOKEN_IN:
            return "NS_TOKEN_IN";
        case NS_TOKEN_SPACE:
            return "NS_TOKEN_SPACE";
        case NS_TOKEN_STRUCT:
            return "NS_TOKEN_STRUCT";
        case NS_TOKEN_IDENTIFIER:
            return "NS_TOKEN_IDENTIFIER";
        case NS_TOKEN_ASYNC:
            return "NS_TOKEN_ASYNC";
        case NS_TOKEN_AWAIT:
            return "NS_TOKEN_AWAIT";
        case NS_TOKEN_TYPE:
            return "NS_TOKEN_TYPE";
        case NS_TOKEN_ASSIGN:
            return "NS_TOKEN_ASSIGN";
        case NS_TOKEN_COLON:
            return "NS_TOKEN_COLON";
        case NS_TOKEN_SEMICOLON:
            return "NS_TOKEN_SEMICOLON";
        case NS_TOKEN_OPEATOR:
            return "NS_TOKEN_OPEATOR";
        case NS_TOKEN_BITWISE_OPEATOR:
            return "NS_TOKEN_BITWISE_OPEATOR";
        case NS_TOKEN_BOOL_OPEATOR:
            return "NS_TOKEN_BOOL_OPEATOR";
        case NS_TOKEN_OPEN_BRACE:
            return "NS_TOKEN_OPEN_BRACE";
        case NS_TOKEN_CLOSE_BRACE:
            return "NS_TOKEN_CLOSE_BRACE";
        case NS_TOKEN_OPEN_PAREN:
            return "NS_TOKEN_OPEN_PAREN";
        case NS_TOKEN_CLOSE_PAREN:
            return "NS_TOKEN_CLOSE_PAREN";
        case NS_TOKEN_OPEN_BRACKET:
            return "NS_TOKEN_OPEN_BRACKET";
        case NS_TOKEN_CLOSE_BRACKET:
            return "NS_TOKEN_CLOSE_BRACKET";
        case NS_TOKEN_EOF:
            return "NS_TOKEN_EOF";
        default:
            return "Unknown token";
    }
}

int ns_token_float_literal(ns_token_t *t, char *s, int i) {
    int j = i;
    while (s[j] >= '0' && s[j] <= '9') {
        j++;
    }
    if (s[j] == '.') {
        j++;
        while (s[j] >= '0' && s[j] <= '9') {
            j++;
        }
    }
    t->type = NS_TOKEN_FLOAT_LITERIAL;
    int len = j - i;
    t->val = ns_str_range(s + i, len);
    return j;
}

int ns_token_int_literal(ns_token_t *t, char *s, int i) {
    int j = i;
    while (s[j] >= '0' && s[j] <= '9') {
        j++;
    }
    t->type = NS_TOKEN_INT_LITERIAL;
    int len = j - i;
    t->val = ns_str_range(s + i, len);
    return j;
}

int ns_tokenize(ns_token_t *t, char *s, int f) {
    int i = f;
    int to = f + 1;
    int l;
    char lead = s[i]; // TODO parse utf8 characters
    switch (lead) {
    case 'a': // as async await
    {
        if (s[i + 1] == 's' && s[i + 2] == 'y' && s[i + 3] == 'n' && s[i + 4] == 'c') {
            t->type = NS_TOKEN_ASYNC;
            
            to = i + 5;
        } else if (s[i + 1] == 'w' && s[i + 2] == 'a' && s[i + 3] == 'i' && s[i + 4] == 't') {
            t->type = NS_TOKEN_AWAIT;
            strcpy(t->str, "await");
            to = i + 5;
        } else {
            goto identifer;
        }
    } break;
    } break;

    case '0' ... '9': {
        i = ns_token_int_literal(t, s, i);
        if (s[i] == '.') {
            i = ns_token_float_literal(t, s, i);
        }
        to = i;
    } break;    
    case '(': {
        token->type = NS_TOKEN_OPEN_BRACE;
        strcpy(us_str, "(");
        to = i + 1;
    } break;
    case ')': {
        token->type = NS_TOKEN_CLOSE_BRACE;
        strcpy(us_str, ")");
        to = i + 1;
    } break;
    case '{': {
        token->type = NS_TOKEN_OPEN_PAREN;
        strcpy(us_str, "{");
        to = i + 1;
    } break;
    case '}': {
        token->type = NS_TOKEN_CLOSE_PAREN;
        strcpy(us_str, "}");
        to = i + 1;
    } break;
    case '[': {
        token->type = NS_TOKEN_OPEN_BRACKET;
        strcpy(us_str, "[");
        to = i + 1;
    } break;
    case ']': {
        token->type = NS_TOKEN_CLOSE_BRACKET;
        strcpy(us_str, "]");
        to = i + 1;
    } break;
    case '=': {
        if (src[i + 1] == '=') {
            token->type = NS_TOKEN_BOOL_OPEATOR;
            us_str[0] = '=';
            us_str[1] = '=';
            us_str[2] = '\0';
            to = i + 2;
        } else {
            token->type = NS_TOKEN_ASSIGN;
            us_str[0] = '=';
            us_str[1] = '\0';
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
        memcpy(us_str, src + from, len + 1);
        us_str[len + 1] = '\0';
        to = i + 1;
    } break;
    // try parse key words
    case 'a': {
        if (src[i + 1] == 's' && src[i + 2] == 'y' && src[i + 3] == 'n' && src[i + 4] == 'c') {
            token->type = NS_TOKEN_ASYNC;
            strcpy(us_str, "async");
            to = i + 5;
        } else if (src[i + 1] == 'w' && src[i + 2] == 'a' && src[i + 3] == 'i' && src[i + 4] == 't') {
            token->type = NS_TOKEN_AWAIT;
            strcpy(us_str, "await");
            to = i + 5;
        } else {
            goto identifer;
        }
    } break;
    case 'c': {
        if (src[i + 1] == 'o' && src[i + 2] == 'n' && src[i + 3] == 's' && src[i + 4] == 't') {
            token->type = NS_TOKEN_CONST;
            strcpy(us_str, "const");
            to = i + 5;
        } else {
            goto identifer;
        }
    } break;
    case 'f': {
        if (src[i + 1] == 'n') {
            token->type = NS_TOKEN_FN;
            strcpy(us_str, "fn");
            to = i + 2;
        } else if (src[i + 1] == '3' && src[i + 2] == '2') {
            token->type = NS_TOKEN_TYPE;
            strcpy(us_str, "f32");
            to = i + 3;
        } else if (src[i + 1] == '6' && src[i + 2] == '4') {
            token->type = NS_TOKEN_TYPE;
            strcpy(us_str, "f64");
            to = i + 3;
        } else {
            goto identifer;
        }
    } break;
    case 'i':
        if (src[i + 1] == 'n') {
            token->type = NS_TOKEN_IN;
            strcpy(us_str, "in");
            to = i + 2;
        } else if (src[i + 1] == '3' && src[i + 2] == '2') {
            token->type = NS_TOKEN_TYPE;
            strcpy(us_str, "i32");
            to = i + 3;
        } else if (src[i + 1] == '6' && src[i + 2] == '4') {
            token->type = NS_TOKEN_TYPE;
            strcpy(us_str, "i64");
            to = i + 3;
        } else {
            goto identifer;
        }
        break;
    case 'l': {
        if (src[i + 1] == 'e' && src[i + 2] == 't') {
            token->type = NS_TOKEN_LET;
            strcpy(us_str, "let");
            to = i + 3;
        } else {
            goto identifer;
        }
    } break;
    case 'u': {
        if (src[i + 1] == '3' && src[i + 2] == '2') {
            token->type = NS_TOKEN_TYPE;
            strcpy(us_str, "u32");
            to = i + 3;
        } else if (src[i + 1] == '6' && src[i + 2] == '4') {
            token->type = NS_TOKEN_TYPE;
            strcpy(us_str, "u64");
            to = i + 3;
        } else {
            goto identifer;
        }
    } break;
    case 's':
        if (src[i + 1] == 't' && src[i + 2] == 'r' && src[i + 3] == 'u' && src[i + 4] == 'c' && src[i + 5] == 't') {
            token->type = NS_TOKEN_STRUCT;
            strcpy(us_str, "struct");
            to = i + 6;
        } else if (src[i + 1] == 't' && src[i + 2] == 'r') {
            token->type = NS_TOKEN_TYPE;
            strcpy(us_str, "str");
            to = i + 3;
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
        memcpy(us_str, src + from, len);
        us_str[len] = '\0';
        to = i;
    } break;
    case '\0': {
        token->type = NS_TOKEN_EOF;
        us_str[0] = '\0';
    } break;
    identifer:
    default: {
        token->type = NS_TOKEN_IDENTIFIER;
        while ((src[i] >= 'a' && src[i] <= 'z') || (src[i] >= 'A' && src[i] <= 'Z')) {
            i++;
        }
        int len = macro_max(i - from, 1);
        memcpy(us_str, src + from, len);
        us_str[len] = '\0';
        to = from + len;
    } break;
    }
    return to;
}
