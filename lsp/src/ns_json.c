#include "ns_json.h"

static ns_json *_ns_json_stack = ns_null;
static ns_json_ctx _ns_json_ctx = {0};

ns_json* ns_json_make() {
    ns_array_push(_ns_json_stack, (ns_json){.type = NS_JSON_INVALID});
    ns_json *json = ns_array_last(_ns_json_stack);
    json->child = ns_null;
    return json;
}

ns_json* ns_json_make_null() {
    ns_json *json = ns_json_make();
    return json;
}

ns_json* ns_json_make_bool(bool b) {
    ns_json *json = ns_json_make();
    json->type = b ? NS_JSON_TRUE : NS_JSON_FALSE;
    return json;
}

ns_json* ns_json_make_number(f64 n) {
    ns_json *json = ns_json_make();
    json->type = NS_JSON_NUMBER;
    json->n = n;
    return json;
}

ns_json* ns_json_make_string(ns_str s) {
    ns_json *json = ns_json_make();
    json->type = NS_JSON_STRING;
    json->str = s;
    return json;
}

ns_json* ns_json_make_array() {
    ns_json *json = ns_json_make();
    json->type = NS_JSON_ARRAY;
    return json;
}

ns_json* ns_json_make_object() {
    ns_json *json = ns_json_make();
    json->type = NS_JSON_OBJECT;
    return json;
}

f64 ns_json_get_number(ns_json *json) {
    if (json->type == NS_JSON_NUMBER) {
        return json->n;
    }
    return 0;
}

ns_str ns_json_get_string(ns_json *json) {
    if (json->type == NS_JSON_STRING) {
        return json->str;
    }
    return ns_str_null;
}

ns_str ns_json_to_string(ns_json *json) {
    if (json) {
        switch (json->type)
        {
        case NS_JSON_INVALID:
            return ns_str_cstr("invalid");
        case NS_JSON_FALSE:
            return ns_str_cstr("false");
        case NS_JSON_TRUE:
            return ns_str_cstr("true");
        case NS_JSON_NULL:
            return ns_str_cstr("null");
        case NS_JSON_NUMBER:
            return ns_str_null;
        case NS_JSON_STRING:
            return ns_str_null;
        case NS_JSON_ARRAY:
            return ns_str_cstr("array");
        case NS_JSON_OBJECT:
            return ns_str_cstr("object");
        case NS_JSON_RAW:
            return ns_str_cstr("raw");
        default:
            break;
        }
    }

    return ns_str_null;
}

ns_json *ns_json_top() {
    return ns_array_length(_ns_json_ctx.stack) > 0 ? ns_array_last(_ns_json_stack) : ns_null;
}

bool ns_json_push(ns_json *json, ns_json *child) {
    if (json->type == NS_JSON_ARRAY) {
        if (json->child) {
            ns_json *last = json->child;
            while (last->child) {
                last = last->child;
            }
            last->child = child;
        } else {
            json->child = child;
        }
        return true;
    }
    return false;
}

bool ns_json_set(ns_json *json, ns_str key, ns_json *child) {
    child->key = key;
    if (json->type == NS_JSON_OBJECT) {
        if (json->child) {
            ns_json *last = json->child;
            while (last->child) {
                last = last->child;
            }
            last->child = child;
        } else {
            json->child = child;
        }
        return true;
    }
    return false;
}

ns_json *ns_json_parse(ns_str s) {
    ns_json *json = ns_json_make();
    _ns_json_ctx.s = s;
    _ns_json_ctx.i = 0;
    ns_array_set_length(_ns_json_ctx.stack, 0);
    _ns_json_ctx.root = ns_null;

    i32 i = 0;
    while (i < s.len) {
        i8 c = s.data[i];

        switch (c)
        {
        case '{':  {// object 
            ns_json *obj = ns_json_make_object();
            ns_json *parent = ns_json_top();
            if (parent) {
                if (parent->type == NS_JSON_ARRAY) {
                    ns_json_push(parent, obj);
                } else {
                    if (!ns_str_empty(parent->key)) {
                        ns_json_set(parent, parent->key, obj);
                        parent->key = ns_str_null;
                    } else {
                        ns_error("ns_json", "json parse error: object key is empty");
                    }
                }
            } else {
                _ns_json_ctx.root = obj;
            }
            ns_array_push(_ns_json_ctx.stack, ns_array_length(_ns_json_stack));
        } break;
        case '}': { // object
            if (ns_array_length(_ns_json_ctx.stack) > 0) {
                ns_array_pop(_ns_json_ctx.stack);
            } else {
                ns_error("ns_json", "json parse error: stack is empty");
            }
        } break;

        case '[': { // array
            ns_json *arr = ns_json_make_array();
            ns_json *parent = ns_json_top();
            if (parent) {
                if (parent->type == NS_JSON_ARRAY) {
                    ns_json_push(parent, arr);
                } else {
                    if (!ns_str_empty(parent->key)) {
                        ns_json_set(parent, parent->key, arr);
                        parent->key = ns_str_null;
                    } else {
                        ns_error("ns_json", "json parse error: array key is empty");
                    }
                }
            } else {
                _ns_json_ctx.root = arr;
            }
            ns_array_push(_ns_json_ctx.stack, ns_array_length(_ns_json_stack));
        } break;
        case ']': { // array
            if (ns_array_length(_ns_json_ctx.stack) > 0) {
                ns_array_pop(_ns_json_ctx.stack);
            } else {
                ns_error("ns_json", "json parse error: stack is empty");
            }
        } break;

        case '"': { // string
            i32 start = i + 1;
            i32 end = start;
            while (end < s.len && s.data[end] != '"') {
                end++;
            }
            ns_str str = ns_str_range(s.data + start, end - start);
            ns_json *parent = ns_json_top();
            if (parent) {
                if (parent->type == NS_JSON_ARRAY) {
                    ns_json_push(parent, ns_json_make_string(str));
                } else {
                    if (ns_str_empty(parent->key)) {
                        parent->key = str;
                    } else {
                        ns_json_set(parent, parent->key, ns_json_make_string(str));
                        parent->key = ns_str_null;
                    }
                }
            } else {
                ns_error("ns_json", "json parse error: string is not in object or array.\n");
            }
            i = end;
        } break;

        case 't':  { // true
            ns_json *parent = ns_json_top();
            if (parent) {
                if (parent->type == NS_JSON_ARRAY) {
                    ns_json_push(parent, ns_json_make_bool(true));
                } else {
                    if (ns_str_empty(parent->key)) {
                        ns_json_set(parent, parent->key, ns_json_make_bool(true));
                        parent->key = ns_str_null;
                    } else {
                        ns_error("ns_json", "json parse error: true key is empty");
                    }
                }
            } else {
                ns_error("ns_json", "json parse error: true is not in object or array");
            }
            i += 3;
        } break;
        case 'f': { // false
            ns_json *parent = ns_json_top();
            if (parent) {
                if (parent->type == NS_JSON_ARRAY) {
                    ns_json_push(parent, ns_json_make_bool(false));
                } else {
                    if (ns_str_empty(parent->key)) {
                        ns_json_set(parent, parent->key, ns_json_make_bool(false));
                        parent->key = ns_str_null;
                    } else {
                        ns_error("ns_json", "json parse error: false key is empty.\n");
                    }
                }
            } else {
                ns_error("ns_json", "json parse error: false is not in object or array.\n");
            }
            i += 4;
        } break;

        case 'n': { // null
            ns_json *parent = ns_json_top();
            if (parent) {
                if (parent->type == NS_JSON_ARRAY) {
                    ns_json_push(parent, ns_json_make_null());
                } else {
                    if (ns_str_empty(parent->key)) {
                        ns_json_set(parent, parent->key, ns_json_make_null());
                        parent->key = ns_str_null;
                    } else {
                        ns_error("ns_json", "json parse error: null key is empty");
                    }
                }
            } else {
                ns_error("ns_json", "json parse error: null is not in object or array");
            }
            i += 3;
        } break;

        case '0' ... '9': { // number
            i32 start = i;
            i32 end = start;
            while (end < s.len && (s.data[end] >= '0' && s.data[end] <= '9')) {
                end++;
            }
            ns_str str = ns_str_slice(s, start, end);
            f64 n = ns_str_to_f64(str);
            ns_json *parent = ns_json_top();
            if (parent) {
                if (parent->type == NS_JSON_ARRAY) {
                    ns_json_push(parent, ns_json_make_number(n));
                } else {
                    if (ns_str_empty(parent->key)) {
                        ns_json_set(parent, parent->key, ns_json_make_number(n));
                        parent->key = ns_str_null;
                    } else {
                        ns_error("ns_json", "json parse error: number key is empty.\n");
                    }
                }
            } else {
                ns_error("ns_json", "json parse error: number is not in object or array.\n");
            }
            i = end;
        } break;

        case ':': {
            ns_json *parent = ns_json_top();
            if (parent) {
                if (parent->type == NS_JSON_OBJECT) {
                    i++;
                } else {
                    ns_error("ns_json", "json parse error: ':' is not in object.\n");
                }
            } else {
                ns_error("ns_json", "json parse error: ':' is not in object.\n");
            }
        } break;

        case ',': {
            i++;
        } break;

        default:
            break;
        }
        i++;
        _ns_json_ctx.i = i;
    }
    return json;
}

bool ns_json_print(ns_json *json) {
    if (json) {
        switch (json->type)
        {
        case NS_JSON_INVALID:
            ns_info("ns_json", "invalid json\n");
            break;
        case NS_JSON_FALSE:
            ns_info("ns_json", "false\n");
            break;
        case NS_JSON_TRUE:
            ns_info("ns_json", "true\n");
            break;
        case NS_JSON_NULL:
            ns_info("ns_json", "null\n");
            break;
        case NS_JSON_NUMBER:
            ns_info("ns_json", "%f\n", json->n);
            break;
        case NS_JSON_STRING:
            ns_info("ns_json", "%.*s\n", json->str.len, json->str.data);
            break;
        case NS_JSON_ARRAY:
            ns_info("ns_json", "array\n");
            break;
        case NS_JSON_OBJECT:
            ns_info("ns_json", "object\n");
            break;
        case NS_JSON_RAW:
            ns_info("ns_json", "raw\n");
            break;
        default:
            break;
        }
    }
    return true;
}
