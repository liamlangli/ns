#include "ns_lsp.h"
#include "ns_json.h"
#include "ns_net.h"
#include "ns_ast.h"
#include <stdarg.h>
#include <time.h>

#if NS_DARWIN
#include <mach-o/dyld.h>
#elif NS_LINUX
#include <unistd.h>
#elif NS_WIN
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#endif

typedef enum {
    NS_LSP_INVALID = 0,
    NS_LSP_STDIO,
    NS_LSP_SOCKET
} ns_lsp_mode;

typedef struct {
    ns_lsp_mode mode;
    union {
        struct {
            ns_conn *conn;
            u16 port;
        } socket; // For socket mode
    };

    struct {
        ns_bool connected;
        i32 process_id;
        ns_str name;
    } client;

    struct {
        ns_str root_path;
        ns_str root_uri;
    } workspace;
} ns_lsp_client;

typedef enum {
    NS_LSP_METHOD_UNKNOWN = 0,
    NS_LSP_METHOD_INITIALIZE,
    NS_LSP_METHOD_INITIALIZED,
    NS_LSP_METHOD_SHUTDOWN,
    NS_LSP_METHOD_EXIT,
    NS_LSP_METHOD_TEXT_DOCUMENT_DID_OPEN,
    NS_LSP_METHOD_TEXT_DOCUMENT_DID_CLOSE,
    NS_LSP_METHOD_TEXT_DOCUMENT_DID_SAVE,
    NS_LSP_METHOD_TEXT_DOCUMENT_DID_CHANGE,
    NS_LSP_METHOD_COMPLETION,
    NS_LSP_METHOD_HOVER,
    NS_LSP_METHOD_DEFINITION,
    NS_LSP_METHOD_REFERENCES,
    NS_LSP_METHOD_DOCUMENT_SYMBOL,
    NS_LSP_METHOD_WORKSPACE_SYMBOL,
} ns_lsp_method;

static ns_lsp_client _client = {0};
static ns_str _in = (ns_str){0, 0, 1};
static ns_str _out = (ns_str){0, 0, 1};
static FILE *_log_file = NULL;

typedef struct {
    ns_str uri;
    ns_str text;
    i32 version;
} ns_lsp_doc;

static ns_lsp_doc *_docs = ns_null;

void ns_lsp_response(i32 r);
static ns_bool ns_lsp_localtime(time_t now, struct tm *out);

static ns_bool ns_lsp_has_ref(i32 ref) {
    return ref > 0 && ns_json_get(ref)->type != NS_JSON_INVALID;
}

static i32 ns_lsp_make_id_value(i32 req) {
    i32 id_ref = ns_json_get_prop(req, ns_str_cstr("id"));
    if (!ns_lsp_has_ref(id_ref)) return 0;

    ns_json *id = ns_json_get(id_ref);
    if (id->type == NS_JSON_NUMBER) {
        return ns_json_make_number(id->n);
    }
    if (id->type == NS_JSON_STRING) {
        return ns_json_make_string(id->str);
    }
    return 0;
}

static ns_bool ns_lsp_exe_path(i8 *out, szt cap) {
    if (!out || cap == 0) return false;
#if NS_DARWIN
    u32 size = (u32)cap;
    if (_NSGetExecutablePath(out, &size) != 0) {
        return false;
    }
    out[cap - 1] = '\0';
    return true;
#elif NS_LINUX
    ssize_t n = readlink("/proc/self/exe", out, cap - 1);
    if (n <= 0) return false;
    out[n] = '\0';
    return true;
#elif NS_WIN
    DWORD n = GetModuleFileNameA(NULL, out, (DWORD)(cap - 1));
    if (n == 0 || n >= cap) return false;
    out[n] = '\0';
    return true;
#else
    ns_unused(out); ns_unused(cap);
    return false;
#endif
}

static i8 ns_lsp_sep(void) {
#if NS_WIN
    return '\\';
#else
    return '/';
#endif
}

static ns_bool ns_lsp_localtime(time_t now, struct tm *out) {
    if (!out) return false;
#if NS_WIN
    return localtime_s(out, &now) == 0;
#else
    return localtime_r(&now, out) != NULL;
#endif
}

static void ns_lsp_rotate_log_if_needed(const i8 *dir, i8 sep) {
    if (!dir) return;
    i8 path[4608] = {0};
    snprintf(path, sizeof(path), "%s%cns_lsp.log", dir, sep);

    FILE *rf = fopen(path, "rb");
    if (!rf) return;
    if (fseek(rf, 0, SEEK_END) != 0) {
        fclose(rf);
        return;
    }
    long sz = ftell(rf);
    fclose(rf);
    if (sz < 0 || sz <= (4L * 1024L * 1024L)) return;

    time_t now = time(NULL);
    struct tm tm_now;
    if (!ns_lsp_localtime(now, &tm_now)) return;

    i8 backup[4608] = {0};
    snprintf(
        backup,
        sizeof(backup),
        "%s%cns_lsp_%04d%02d%02d_%02d%02d%02d.log",
        dir,
        sep,
        tm_now.tm_year + 1900,
        tm_now.tm_mon + 1,
        tm_now.tm_mday,
        tm_now.tm_hour,
        tm_now.tm_min,
        tm_now.tm_sec
    );
    rename(path, backup);
}

static void ns_lsp_log_init(void) {
    i8 exe[4096] = {0};
    if (!ns_lsp_exe_path(exe, sizeof(exe))) {
        return;
    }

    i8 sep = ns_lsp_sep();
    i8 *last = strrchr(exe, sep);
    if (!last) return;
    *last = '\0';

    ns_lsp_rotate_log_if_needed(exe, sep);

    i8 path[4608] = {0};
    snprintf(path, sizeof(path), "%s%cns_lsp.log", exe, sep);
    _log_file = fopen(path, "a");
    if (!_log_file) return;

    time_t now = time(NULL);
    struct tm tm_now;
    if (ns_lsp_localtime(now, &tm_now)) {
        fprintf(
            _log_file,
            "\n==== ns_lsp start %04d-%02d-%02d %02d:%02d:%02d (local) ====\n",
            tm_now.tm_year + 1900,
            tm_now.tm_mon + 1,
            tm_now.tm_mday,
            tm_now.tm_hour,
            tm_now.tm_min,
            tm_now.tm_sec
        );
    } else {
        fprintf(_log_file, "\n==== ns_lsp start (localtime unavailable) ====\n");
    }
    fflush(_log_file);
}

static void ns_lsp_logf(const i8 *fmt, ...) {
    if (!_log_file) return;
    time_t now = time(NULL);
    struct tm tm_now;
    if (!ns_lsp_localtime(now, &tm_now)) return;
    fprintf(_log_file, "[%04d-%02d-%02d %02d:%02d:%02d] ",
        tm_now.tm_year + 1900, tm_now.tm_mon + 1, tm_now.tm_mday,
        tm_now.tm_hour, tm_now.tm_min, tm_now.tm_sec);

    va_list ap;
    va_start(ap, fmt);
    vfprintf(_log_file, fmt, ap);
    va_end(ap);
    fprintf(_log_file, "\n");
    fflush(_log_file);
}

static void ns_lsp_log_payload(const i8 *tag, ns_str payload) {
    if (!_log_file) return;
    ns_lsp_logf("%s len=%d", tag, payload.len);
    if (payload.len > 0 && payload.data) {
        fprintf(_log_file, "%.*s\n", payload.len, payload.data);
        fflush(_log_file);
    }
}

static ns_lsp_doc *ns_lsp_doc_find(ns_str uri) {
    for (i32 i = 0, l = (i32)ns_array_length(_docs); i < l; ++i) {
        if (ns_str_equals(_docs[i].uri, uri)) return &_docs[i];
    }
    return ns_null;
}

static ns_lsp_doc *ns_lsp_doc_upsert(ns_str uri) {
    ns_lsp_doc *d = ns_lsp_doc_find(uri);
    if (d) return d;
    ns_lsp_doc nd = {.uri = ns_str_slice(uri, 0, uri.len), .text = ns_str_null, .version = 0};
    ns_array_push(_docs, nd);
    return ns_array_last(_docs);
}

static void ns_lsp_doc_set_text(ns_lsp_doc *d, ns_str text) {
    if (!d) return;
    ns_str_free(d->text);
    // JSON parser keeps escaped sequences. Convert to real source text.
    d->text = ns_str_unescape(text);
}

static void ns_lsp_doc_remove(ns_str uri) {
    for (i32 i = 0, l = (i32)ns_array_length(_docs); i < l; ++i) {
        if (ns_str_equals(_docs[i].uri, uri)) {
            ns_str_free(_docs[i].uri);
            ns_str_free(_docs[i].text);
            ns_array_splice(_docs, i);
            return;
        }
    }
}

static i32 ns_lsp_make_range(i32 sl, i32 so, i32 el, i32 eo) {
    i32 range = ns_json_make_object();
    i32 start = ns_json_make_object();
    i32 end = ns_json_make_object();
    ns_json_set(start, ns_str_cstr("line"), ns_json_make_number(sl));
    ns_json_set(start, ns_str_cstr("character"), ns_json_make_number(so));
    ns_json_set(end, ns_str_cstr("line"), ns_json_make_number(el));
    ns_json_set(end, ns_str_cstr("character"), ns_json_make_number(eo));
    ns_json_set(range, ns_str_cstr("start"), start);
    ns_json_set(range, ns_str_cstr("end"), end);
    return range;
}

static void ns_lsp_push_diag(i32 diagnostics, i32 line, i32 ch, ns_str msg) {
    i32 d = ns_json_make_object();
    ns_json_set(d, ns_str_cstr("range"), ns_lsp_make_range(line, ch, line, ch + 1));
    ns_json_set(d, ns_str_cstr("severity"), ns_json_make_number(1)); // Error
    ns_json_set(d, ns_str_cstr("source"), ns_json_make_string(ns_str_cstr("ns_lsp")));
    ns_json_set(d, ns_str_cstr("message"), ns_json_make_string(msg));
    ns_json_push(diagnostics, d);
}

static void ns_lsp_collect_debug_diagnostics(ns_str source, i32 diagnostics) {
    i32 line = 0, col = 0;
    i32 last_non_ws_col = -1;
    i8 last_non_ws = 0;

    i8 *st = ns_null;
    i32 *st_line = ns_null;
    i32 *st_col = ns_null;

    for (i32 i = 0; i < source.len; ++i) {
        i8 c = source.data[i];
        if (c == '\r') continue;
        if (c == '\n') {
            if (last_non_ws == '=') {
                ns_lsp_push_diag(diagnostics, line, last_non_ws_col, ns_str_cstr("expected expression after '='"));
            }
            line++;
            col = 0;
            last_non_ws = 0;
            last_non_ws_col = -1;
            continue;
        }

        if (c == '(' || c == '[' || c == '{') {
            ns_array_push(st, c);
            ns_array_push(st_line, line);
            ns_array_push(st_col, col);
        } else if (c == ')' || c == ']' || c == '}') {
            if (ns_array_length(st) == 0) {
                ns_lsp_push_diag(diagnostics, line, col, ns_str_cstr("unmatched closing delimiter"));
            } else {
                i8 open = ns_array_pop(st);
                i32 ol = ns_array_pop(st_line);
                i32 oc = ns_array_pop(st_col);
                ns_bool ok = (open == '(' && c == ')') || (open == '[' && c == ']') || (open == '{' && c == '}');
                if (!ok) {
                    ns_lsp_push_diag(diagnostics, ol, oc, ns_str_cstr("mismatched delimiter"));
                }
            }
        }

        if (c != ' ' && c != '\t') {
            last_non_ws = c;
            last_non_ws_col = col;
        }
        col++;
    }

    if (last_non_ws == '=') {
        ns_lsp_push_diag(diagnostics, line, ns_max(0, last_non_ws_col), ns_str_cstr("expected expression after '='"));
    }

    while (ns_array_length(st) > 0) {
        ns_array_pop(st);
        i32 ol = ns_array_pop(st_line);
        i32 oc = ns_array_pop(st_col);
        ns_lsp_push_diag(diagnostics, ol, oc, ns_str_cstr("unclosed delimiter"));
    }

    ns_array_free(st);
    ns_array_free(st_line);
    ns_array_free(st_col);
}

static void ns_lsp_publish_diagnostics(ns_str uri, ns_str source) {
    i32 params = ns_json_make_object();
    ns_json_set(params, ns_str_cstr("uri"), ns_json_make_string(uri));
    i32 diagnostics = ns_json_make_array();

#ifdef NS_DEBUG
    ns_lsp_collect_debug_diagnostics(source, diagnostics);
#else
    ns_ast_ctx ctx = {0};
    ns_return_bool ret = ns_ast_parse(&ctx, source, uri);
    if (ns_return_is_error(ret)) {
        i32 line = ret.e.loc.l > 0 ? ret.e.loc.l - 1 : 0;
        i32 chr = ret.e.loc.o > 0 ? ret.e.loc.o : 0;
        ns_lsp_push_diag(diagnostics, line, chr, ret.e.msg);
    }
#endif

    ns_json_set(params, ns_str_cstr("diagnostics"), diagnostics);

    i32 msg = ns_json_make_object();
    ns_json_set(msg, ns_str_cstr("jsonrpc"), ns_json_make_string(ns_str_cstr("2.0")));
    ns_json_set(msg, ns_str_cstr("method"), ns_json_make_string(ns_str_cstr("textDocument/publishDiagnostics")));
    ns_json_set(msg, ns_str_cstr("params"), params);
    ns_lsp_response(msg);
}

static void ns_lsp_publish_clear(ns_str uri) {
    i32 params = ns_json_make_object();
    ns_json_set(params, ns_str_cstr("uri"), ns_json_make_string(uri));
    ns_json_set(params, ns_str_cstr("diagnostics"), ns_json_make_array());

    i32 msg = ns_json_make_object();
    ns_json_set(msg, ns_str_cstr("jsonrpc"), ns_json_make_string(ns_str_cstr("2.0")));
    ns_json_set(msg, ns_str_cstr("method"), ns_json_make_string(ns_str_cstr("textDocument/publishDiagnostics")));
    ns_json_set(msg, ns_str_cstr("params"), params);
    ns_lsp_response(msg);
}

static ns_bool ns_lsp_is_ident_char(i8 c) {
    return (c >= 'a' && c <= 'z') ||
           (c >= 'A' && c <= 'Z') ||
           (c >= '0' && c <= '9') ||
           c == '_';
}

static ns_bool ns_lsp_word_boundary(ns_str s, i32 from, i32 len) {
    i32 l = from - 1;
    i32 r = from + len;
    ns_bool left_ok = l < 0 || !ns_lsp_is_ident_char(s.data[l]);
    ns_bool right_ok = r >= s.len || !ns_lsp_is_ident_char(s.data[r]);
    return left_ok && right_ok;
}

static i32 ns_lsp_position_to_offset(ns_str s, i32 line, i32 ch) {
    if (line < 0 || ch < 0) return -1;
    i32 cur_line = 0;
    i32 off = 0;
    while (off < s.len && cur_line < line) {
        if (s.data[off] == '\n') cur_line++;
        off++;
    }
    if (cur_line != line) return -1;
    return ns_min(s.len, off + ch);
}

static void ns_lsp_offset_to_position(ns_str s, i32 off, i32 *line, i32 *ch) {
    i32 l = 0, c = 0;
    for (i32 i = 0; i < off && i < s.len; ++i) {
        if (s.data[i] == '\n') {
            l++;
            c = 0;
        } else {
            c++;
        }
    }
    *line = l;
    *ch = c;
}

static ns_str ns_lsp_word_at(ns_str s, i32 off, i32 *start_out) {
    if (off < 0 || off > s.len) return ns_str_null;
    i32 pos = off;
    if (pos == s.len) pos--;
    if (pos >= 0 && !ns_lsp_is_ident_char(s.data[pos]) && pos > 0 && ns_lsp_is_ident_char(s.data[pos - 1])) {
        pos--;
    }
    if (pos < 0 || !ns_lsp_is_ident_char(s.data[pos])) return ns_str_null;

    i32 a = pos;
    i32 b = pos + 1;
    while (a > 0 && ns_lsp_is_ident_char(s.data[a - 1])) a--;
    while (b < s.len && ns_lsp_is_ident_char(s.data[b])) b++;
    if (start_out) *start_out = a;
    return ns_str_range(s.data + a, b - a);
}

static i32 ns_lsp_line_start(ns_str s, i32 off) {
    i32 i = ns_clamp(off, 0, s.len);
    while (i > 0 && s.data[i - 1] != '\n') i--;
    return i;
}

static i32 ns_lsp_line_end(ns_str s, i32 off) {
    i32 i = ns_clamp(off, 0, s.len);
    while (i < s.len && s.data[i] != '\n') i++;
    return i;
}

static ns_bool ns_lsp_line_has_prefix(ns_str s, i32 occ, ns_str prefix) {
    i32 ls = ns_lsp_line_start(s, occ);
    while (ls < occ && (s.data[ls] == ' ' || s.data[ls] == '\t')) ls++;
    if (ls + prefix.len > occ) return false;
    return strncmp(s.data + ls, prefix.data, prefix.len) == 0;
}

static ns_bool ns_lsp_find_decl(ns_str source, ns_str word, i32 *decl_off, ns_str *kind_out) {
    i32 i = 0;
    while (i + word.len <= source.len) {
        if (strncmp(source.data + i, word.data, word.len) == 0 && ns_lsp_word_boundary(source, i, word.len)) {
            if (ns_lsp_line_has_prefix(source, i, ns_str_cstr("let "))) {
                *decl_off = i; if (kind_out) *kind_out = ns_str_cstr("variable"); return true;
            }
            if (ns_lsp_line_has_prefix(source, i, ns_str_cstr("fn "))) {
                *decl_off = i; if (kind_out) *kind_out = ns_str_cstr("function"); return true;
            }
            if (ns_lsp_line_has_prefix(source, i, ns_str_cstr("struct "))) {
                *decl_off = i; if (kind_out) *kind_out = ns_str_cstr("struct"); return true;
            }
            if (ns_lsp_line_has_prefix(source, i, ns_str_cstr("type "))) {
                *decl_off = i; if (kind_out) *kind_out = ns_str_cstr("type"); return true;
            }
        }
        i++;
    }
    return false;
}

static void ns_lsp_send_result(i32 req, i32 result_ref) {
    i32 out_id = ns_lsp_make_id_value(req);
    if (!ns_lsp_has_ref(out_id)) return;
    i32 r = ns_json_make_object();
    ns_json_set(r, ns_str_cstr("jsonrpc"), ns_json_make_string(ns_str_cstr("2.0")));
    ns_json_set(r, ns_str_cstr("id"), out_id);
    ns_json_set(r, ns_str_cstr("result"), result_ref);
    ns_lsp_response(r);
}

void ns_lsp_handle_hover(i32 req) {
    i32 params = ns_json_get_prop(req, ns_str_cstr("params"));
    if (params == 0) { ns_lsp_send_result(req, ns_json_make_null()); return; }
    i32 td = ns_json_get_prop(params, ns_str_cstr("textDocument"));
    i32 pos = ns_json_get_prop(params, ns_str_cstr("position"));
    if (td == 0 || pos == 0) { ns_lsp_send_result(req, ns_json_make_null()); return; }

    ns_str uri = ns_json_to_string(ns_json_get_prop(td, ns_str_cstr("uri")));
    ns_lsp_doc *d = ns_lsp_doc_find(uri);
    if (!d || ns_str_is_empty(d->text)) { ns_lsp_send_result(req, ns_json_make_null()); return; }

    i32 line = ns_json_to_i32(ns_json_get_prop(pos, ns_str_cstr("line")));
    i32 ch = ns_json_to_i32(ns_json_get_prop(pos, ns_str_cstr("character")));
    i32 off = ns_lsp_position_to_offset(d->text, line, ch);
    i32 start = -1;
    ns_str word = ns_lsp_word_at(d->text, off, &start);
    if (ns_str_is_empty(word)) { ns_lsp_send_result(req, ns_json_make_null()); return; }

    i32 decl = -1;
    ns_str kind = ns_str_cstr("symbol");
    if (!ns_lsp_find_decl(d->text, word, &decl, &kind)) {
        ns_lsp_send_result(req, ns_json_make_null());
        return;
    }

    i32 ls = ns_lsp_line_start(d->text, decl);
    i32 le = ns_lsp_line_end(d->text, decl);
    ns_str line_text = ns_str_range(d->text.data + ls, le - ls);

    ns_str md = ns_str_null;
    ns_str_append(&md, ns_str_cstr("**"));
    ns_str_append_len(&md, kind.data, kind.len);
    ns_str_append(&md, ns_str_cstr("** `"));
    ns_str_append_len(&md, word.data, word.len);
    ns_str_append(&md, ns_str_cstr("`\n\n```ns\n"));
    ns_str_append_len(&md, line_text.data, line_text.len);
    ns_str_append(&md, ns_str_cstr("\n```"));

    i32 contents = ns_json_make_object();
    ns_json_set(contents, ns_str_cstr("kind"), ns_json_make_string(ns_str_cstr("markdown")));
    ns_json_set(contents, ns_str_cstr("value"), ns_json_make_string(md));

    i32 hover = ns_json_make_object();
    ns_json_set(hover, ns_str_cstr("contents"), contents);
    ns_lsp_send_result(req, hover);
}

void ns_lsp_handle_definition(i32 req) {
    i32 params = ns_json_get_prop(req, ns_str_cstr("params"));
    if (params == 0) { ns_lsp_send_result(req, ns_json_make_null()); return; }
    i32 td = ns_json_get_prop(params, ns_str_cstr("textDocument"));
    i32 pos = ns_json_get_prop(params, ns_str_cstr("position"));
    if (td == 0 || pos == 0) { ns_lsp_send_result(req, ns_json_make_null()); return; }

    ns_str uri = ns_json_to_string(ns_json_get_prop(td, ns_str_cstr("uri")));
    ns_lsp_doc *d = ns_lsp_doc_find(uri);
    if (!d || ns_str_is_empty(d->text)) { ns_lsp_send_result(req, ns_json_make_null()); return; }

    i32 line = ns_json_to_i32(ns_json_get_prop(pos, ns_str_cstr("line")));
    i32 ch = ns_json_to_i32(ns_json_get_prop(pos, ns_str_cstr("character")));
    i32 off = ns_lsp_position_to_offset(d->text, line, ch);
    i32 start = -1;
    ns_str word = ns_lsp_word_at(d->text, off, &start);
    if (ns_str_is_empty(word)) { ns_lsp_send_result(req, ns_json_make_null()); return; }

    i32 decl = -1;
    ns_str kind = ns_str_null;
    if (!ns_lsp_find_decl(d->text, word, &decl, &kind)) {
        ns_lsp_send_result(req, ns_json_make_null());
        return;
    }

    i32 dl = 0, dc = 0;
    ns_lsp_offset_to_position(d->text, decl, &dl, &dc);

    i32 loc = ns_json_make_object();
    ns_json_set(loc, ns_str_cstr("uri"), ns_json_make_string(uri));
    ns_json_set(loc, ns_str_cstr("range"), ns_lsp_make_range(dl, dc, dl, dc + word.len));

    i32 arr = ns_json_make_array();
    ns_json_push(arr, loc);
    ns_lsp_send_result(req, arr);
}


static ns_bool ns_lsp_read_stdio_message(void) {
    i8 line[1024];
    i32 content_len = -1;

    while (fgets(line, (i32)sizeof(line), stdin)) {
        i32 n = (i32)strlen(line);
        if (n == 0) continue;
        if ((n == 1 && line[0] == '\n') ||
            (n == 2 && line[0] == '\r' && line[1] == '\n')) {
            break;
        }

        if (strncmp(line, "Content-Length:", 15) == 0) {
            i32 p = 15;
            while (line[p] == ' ' || line[p] == '\t') p++;
            i32 len = 0;
            ns_bool has_digit = false;
            while (line[p] >= '0' && line[p] <= '9') {
                has_digit = true;
                len = len * 10 + (line[p] - '0');
                p++;
            }
            if (has_digit) content_len = len;
        }
    }

    if (content_len < 0) {
        return false;
    }

    ns_array_set_length(_in.data, content_len);
    if ((i32)fread(_in.data, 1, content_len, stdin) != content_len) {
        return false;
    }
    _in.len = content_len;
    _in.data[content_len] = '\0';
    return true;
}

void ns_lsp_response(i32 r) {
    ns_str str = ns_json_stringify(ns_json_get(r));

    ns_str_append(&_out, ns_str_cstr("Content-Length: "));
    ns_str_append_i32(&_out, (i32)str.len);
    ns_str_append(&_out, ns_str_cstr("\r\n\r\n"));
    ns_str_append(&_out, str);
    ns_lsp_log_payload("SEND", str);

    if (_client.mode == NS_LSP_STDIO) {
        fprintf(stdout, "%.*s", _out.len, _out.data);
        fflush(stdout);
    } else if (_client.mode == NS_LSP_SOCKET) {
        ns_tcp_write(_client.socket.conn, (ns_data){_out.data, _out.len});
    } else {
        ns_warn("lsp", "Invalid LSP mode.\n");
        return;
    }
    ns_str_clear(&_out);
}

ns_lsp_method ns_lsp_parse_method(ns_str str) {
    if (ns_str_equals_STR(str, "initialized")) {
        return NS_LSP_METHOD_INITIALIZED;
    } else if (ns_str_equals_STR(str, "initialize")) {
        return NS_LSP_METHOD_INITIALIZE;
    } else if (ns_str_equals_STR(str, "shutdown")) {
        return NS_LSP_METHOD_SHUTDOWN;
    } else if (ns_str_equals_STR(str, "exit")) {
        return NS_LSP_METHOD_EXIT;
    } else if (ns_str_equals_STR(str, "textDocument/didOpen")) {
        return NS_LSP_METHOD_TEXT_DOCUMENT_DID_OPEN;
    } else if (ns_str_equals_STR(str, "textDocument/didClose")) {
        return NS_LSP_METHOD_TEXT_DOCUMENT_DID_CLOSE;
    } else if (ns_str_equals_STR(str, "textDocument/didSave")) {
        return NS_LSP_METHOD_TEXT_DOCUMENT_DID_SAVE;
    } else if (ns_str_equals_STR(str, "textDocument/didChange")) {
        return NS_LSP_METHOD_TEXT_DOCUMENT_DID_CHANGE;
    } else if (ns_str_equals_STR(str, "textDocument/completion")) {
        return NS_LSP_METHOD_COMPLETION;
    } else if (ns_str_equals_STR(str, "textDocument/hover")) {
        return NS_LSP_METHOD_HOVER;
    } else if (ns_str_equals_STR(str, "textDocument/definition")) {
        return NS_LSP_METHOD_DEFINITION;
    } else if (ns_str_equals_STR(str, "textDocument/references")) {
        return NS_LSP_METHOD_REFERENCES;
    } else if (ns_str_equals_STR(str, "textDocument/documentSymbol")) {
        return NS_LSP_METHOD_DOCUMENT_SYMBOL;
    } else if (ns_str_equals_STR(str, "workspace/symbol")) {
        return NS_LSP_METHOD_WORKSPACE_SYMBOL;
    } else {
        return NS_LSP_METHOD_UNKNOWN;
    }
}

void ns_lsp_handle_init(i32 req) {
    i32 params = ns_json_get_prop(req, ns_str_cstr("params"));
    if (params == 0) {
        ns_warn("lsp", "No params in request.\n");
    }

    i32 r = ns_json_make_object();
    ns_json_set(r, ns_str_cstr("jsonrpc"), ns_json_make_string(ns_str_cstr("2.0")));

    // set id
    i32 out_id = ns_lsp_make_id_value(req);
    if (ns_lsp_has_ref(out_id)) {
        ns_json_set(r, ns_str_cstr("id"), out_id);
    } else {
        ns_json_set(r, ns_str_cstr("id"), ns_json_make_null());
    }

    // set result
    i32 result = ns_json_make_object();
    i32 caps = ns_json_make_object();
    i32 sync = ns_json_make_object();
    ns_json_set(sync, ns_str_cstr("openClose"), ns_json_make_bool(true));
    ns_json_set(sync, ns_str_cstr("change"), ns_json_make_number(1)); // Full sync
    i32 save = ns_json_make_object();
    ns_json_set(save, ns_str_cstr("includeText"), ns_json_make_bool(true));
    ns_json_set(sync, ns_str_cstr("save"), save);
    ns_json_set(caps, ns_str_cstr("textDocumentSync"), sync);
    ns_json_set(caps, ns_str_cstr("hoverProvider"), ns_json_make_bool(true));
    ns_json_set(caps, ns_str_cstr("definitionProvider"), ns_json_make_bool(true));
    ns_json_set(result, ns_str_cstr("capabilities"), caps);

    ns_json_set(r, ns_str_cstr("result"), result);

    _client.client.process_id = ns_json_to_i32(ns_json_get_prop(params, ns_str_cstr("processId")));
    i32 client_info = ns_json_get_prop(params, ns_str_cstr("clientInfo"));
    if (client_info != 0) {
        ns_str name = ns_json_to_string(ns_json_get_prop(client_info, ns_str_cstr("name")));
        if (!ns_str_is_empty(name)) {
            _client.client.name = name;
        }
    }
    i32 root_path = ns_json_get_prop(params, ns_str_cstr("rootPath"));
    if (root_path != 0) {
        _client.workspace.root_path = ns_json_to_string(root_path);
    }
    i32 root_uri = ns_json_get_prop(params, ns_str_cstr("rootUri"));
    if (root_uri != 0) {
        _client.workspace.root_uri = ns_json_to_string(root_uri);
    }
    ns_lsp_response(r);
    ns_info("lsp", "Initialized LSP server with process ID: %d, client name: %.*s\n", _client.client.process_id, _client.client.name.len, _client.client.name.data);
}

void ns_lsp_handle_inited(i32 req) {
    ns_unused(req);
    _client.client.connected = true;
}

void ns_lsp_handle_shutdown(i32 req) {
    i32 out_id = ns_lsp_make_id_value(req);
    if (!ns_lsp_has_ref(out_id)) return;

    i32 r = ns_json_make_object();
    ns_json_set(r, ns_str_cstr("jsonrpc"), ns_json_make_string(ns_str_cstr("2.0")));
    ns_json_set(r, ns_str_cstr("id"), out_id);
    ns_json_set(r, ns_str_cstr("result"), ns_json_make_null());
    ns_lsp_response(r);
}

void ns_lsp_handle_did_open(i32 req) {
    i32 params = ns_json_get_prop(req, ns_str_cstr("params"));
    if (params == 0) return;
    i32 td = ns_json_get_prop(params, ns_str_cstr("textDocument"));
    if (td == 0) return;

    ns_str uri = ns_json_to_string(ns_json_get_prop(td, ns_str_cstr("uri")));
    ns_str text = ns_json_to_string(ns_json_get_prop(td, ns_str_cstr("text")));
    i32 version = ns_json_to_i32(ns_json_get_prop(td, ns_str_cstr("version")));

    ns_lsp_doc *d = ns_lsp_doc_upsert(uri);
    if (!d) return;
    ns_lsp_doc_set_text(d, text);
    d->version = version;
    ns_lsp_publish_diagnostics(d->uri, d->text);
}

void ns_lsp_handle_did_change(i32 req) {
    i32 params = ns_json_get_prop(req, ns_str_cstr("params"));
    if (params == 0) return;
    i32 td = ns_json_get_prop(params, ns_str_cstr("textDocument"));
    i32 changes = ns_json_get_prop(params, ns_str_cstr("contentChanges"));
    if (td == 0 || changes == 0) return;

    ns_str uri = ns_json_to_string(ns_json_get_prop(td, ns_str_cstr("uri")));
    i32 version = ns_json_to_i32(ns_json_get_prop(td, ns_str_cstr("version")));
    i32 c0 = ns_json_get_item(changes, 0);
    if (c0 == 0) return;
    ns_str text = ns_json_to_string(ns_json_get_prop(c0, ns_str_cstr("text")));

    ns_lsp_doc *d = ns_lsp_doc_upsert(uri);
    if (!d) return;
    ns_lsp_doc_set_text(d, text);
    d->version = version;
    ns_lsp_publish_diagnostics(d->uri, d->text);
}

void ns_lsp_handle_did_save(i32 req) {
    i32 params = ns_json_get_prop(req, ns_str_cstr("params"));
    if (params == 0) return;
    i32 td = ns_json_get_prop(params, ns_str_cstr("textDocument"));
    if (td == 0) return;

    ns_str uri = ns_json_to_string(ns_json_get_prop(td, ns_str_cstr("uri")));
    ns_lsp_doc *d = ns_lsp_doc_find(uri);
    i32 text_ref = ns_json_get_prop(params, ns_str_cstr("text"));
    if (text_ref != 0) {
        ns_str text = ns_json_to_string(text_ref);
        if (!d) d = ns_lsp_doc_upsert(uri);
        if (d) {
            ns_lsp_doc_set_text(d, text);
        }
    }

    if (d) ns_lsp_publish_diagnostics(d->uri, d->text);
}

void ns_lsp_handle_did_close(i32 req) {
    i32 params = ns_json_get_prop(req, ns_str_cstr("params"));
    if (params == 0) return;
    i32 td = ns_json_get_prop(params, ns_str_cstr("textDocument"));
    if (td == 0) return;
    ns_str uri = ns_json_to_string(ns_json_get_prop(td, ns_str_cstr("uri")));
    ns_lsp_publish_clear(uri);
    ns_lsp_doc_remove(uri);
}

void ns_lsp_handle_method_not_found(i32 req, ns_str method_str) {
    i32 out_id = ns_lsp_make_id_value(req);
    if (!ns_lsp_has_ref(out_id)) {
        ns_warn("lsp", "Ignoring unsupported notification method: %.*s\n", method_str.len, method_str.data);
        return;
    }

    i32 r = ns_json_make_object();
    ns_json_set(r, ns_str_cstr("jsonrpc"), ns_json_make_string(ns_str_cstr("2.0")));
    ns_json_set(r, ns_str_cstr("id"), out_id);

    i32 err = ns_json_make_object();
    ns_json_set(err, ns_str_cstr("code"), ns_json_make_number(-32601));
    ns_json_set(err, ns_str_cstr("message"), ns_json_make_string(ns_str_cstr("Method not found")));
    ns_json_set(r, ns_str_cstr("error"), err);
    ns_lsp_response(r);
}

void ns_lsp_handle_request(i32 req) {
    ns_str req_json = ns_json_stringify(ns_json_get(req));
    ns_lsp_log_payload("RECV", req_json);

    i32 method = ns_json_get_prop(req, ns_str_cstr("method"));
    if (method == 0) {
        // dump input json
        ns_str input_json = ns_json_stringify(ns_json_get(req));
        ns_exit(1, "lsp", "Received request without method: %.*s\n", input_json.len, input_json.data);
    }
    ns_str method_str = ns_json_to_string(method);
    ns_lsp_logf("METHOD %.*s", method_str.len, method_str.data);
    ns_lsp_method lsp_method = ns_lsp_parse_method(method_str);
    switch (lsp_method) {
        case NS_LSP_METHOD_INITIALIZE:
            ns_lsp_handle_init(req);
            break;
        case NS_LSP_METHOD_INITIALIZED:
            ns_lsp_handle_inited(req);
            ns_info("lsp", "Initialized method called.\n");
            break;
        case NS_LSP_METHOD_SHUTDOWN:
            ns_lsp_handle_shutdown(req);
            _client.client.connected = false;
            ns_info("lsp", "Shutdown method called.\n");
            break;
        case NS_LSP_METHOD_EXIT:
            // In socket mode, keep server process alive and only close current connection.
            ns_info("lsp", "Exit method called.\n");
            if (_client.mode == NS_LSP_SOCKET && _client.socket.conn) {
                ns_conn_close(_client.socket.conn);
                _client.socket.conn = ns_null;
                break;
            }
            if (_client.mode == NS_LSP_STDIO) {
                exit(0);
            }
            break;
        case NS_LSP_METHOD_TEXT_DOCUMENT_DID_OPEN:
            ns_lsp_handle_did_open(req);
            break;
        case NS_LSP_METHOD_TEXT_DOCUMENT_DID_CLOSE:
            ns_lsp_handle_did_close(req);
            break;
        case NS_LSP_METHOD_TEXT_DOCUMENT_DID_SAVE:
            ns_lsp_handle_did_save(req);
            break;
        case NS_LSP_METHOD_TEXT_DOCUMENT_DID_CHANGE:
            ns_lsp_handle_did_change(req);
            break;
        case NS_LSP_METHOD_COMPLETION:
            ns_lsp_handle_method_not_found(req, method_str);
            break;
        case NS_LSP_METHOD_HOVER:
            ns_lsp_handle_hover(req);
            break;
        case NS_LSP_METHOD_DEFINITION:
            ns_lsp_handle_definition(req);
            break;
        case NS_LSP_METHOD_REFERENCES:
        case NS_LSP_METHOD_DOCUMENT_SYMBOL:
        case NS_LSP_METHOD_WORKSPACE_SYMBOL:
            ns_lsp_handle_method_not_found(req, method_str);
            break;
        default:
            // Unknown method from newer clients/extensions.
            ns_lsp_handle_method_not_found(req, method_str);
            break;
    }
}

ns_bool ns_lsp_parse_header(ns_str s, i32 *head_len, i32 *body_len) {
    i32 i = ns_str_index_of(s, ns_str_cstr("\r\n\r\n"));
    if (i == -1) {
        return false;
    }
    ns_str header = (ns_str){s.data, i, 0};
    i32 l = ns_str_index_of(header, ns_str_cstr("Content-Length:"));
    if (l == -1) {
        return false;
    }

    i32 p = l + 15;
    while (p < header.len && (header.data[p] == ' ' || header.data[p] == '\t')) p++;
    if (p >= header.len) return false;

    i32 len = 0;
    ns_bool has_digit = false;
    while (p < header.len && header.data[p] >= '0' && header.data[p] <= '9') {
        has_digit = true;
        len = len * 10 + (header.data[p] - '0');
        p++;
    }
    if (!has_digit || len < 0) {
        return false;
    }
    *head_len = i + 4; // Length of header including "\r\n\r\n"
    *body_len = len;
    return true;
}

void ns_lsp_on_connect(ns_conn *conn) {
    ns_info("lsp", "New connection established.\n");
    _client.socket.conn = conn;

    i32 head = 0, body = 0;
    ns_str rx = (ns_str){0, 0, 1};
    while(1) {
        ns_data data = ns_tcp_read(conn);
        if (data.len == 0) {
            ns_info("lsp", "Connection closed.\n");
            _client.socket.conn = ns_null;
            break;
        }

        ns_str_append_len(&rx, data.data, (i32)data.len);
        while (ns_lsp_parse_header(rx, &head, &body)) {
            if (rx.len < head + body) {
                break; // wait for more bytes
            }

            ns_str_clear(&_in);
            ns_str_append_len(&_in, rx.data + head, body);
            ns_lsp_handle_request(ns_json_parse(_in));

            i32 consumed = head + body;
            i32 remain = rx.len - consumed;
            if (remain > 0) {
                memmove(rx.data, rx.data + consumed, remain);
            }
            rx.len = remain;
            if (rx.data) {
                ns_array_set_length(rx.data, remain);
                if (remain > 0) rx.data[remain] = '\0';
            }
        }
    }

    ns_str_free(rx);
}

void ns_lsp_parse_arg(i32 argc, i8 **argv) {
    _client.mode = NS_LSP_INVALID;
    for (i32 i = 1; i < argc; i++) {
        if (ns_str_equals_STR(ns_str_cstr("--socket"), argv[i])) {
            _client.mode = NS_LSP_SOCKET;
        } else if (ns_str_equals_STR(ns_str_cstr("--stdio"), argv[i])) {
            _client.mode = NS_LSP_STDIO;
        } else if (ns_str_equals_STR(ns_str_cstr("--port"), argv[i])) {
            if (i + 1 < argc) {
                i8 *port_str = argv[i + 1];
                szt len = strlen(port_str);
                ns_str port_ns_str = ns_str_range(port_str, len);
                _client.socket.port = (u16)ns_str_to_i32(port_ns_str);
                ++i;
            } else {
                ns_exit(1, "lsp", "Error: --port requires a value.");
            }
        } else {
            ns_exit(1, "lsp", "Unknown argument: %s.", argv[i]);
        }
    }
    ns_lsp_logf("MODE=%d PORT=%d", (i32)_client.mode, (i32)_client.socket.port);
}

i32 main(i32 argc, i8 **argv) {
    ns_lsp_log_init();
    if (argc < 2) {
        ns_exit(1, "lsp", "Usage: lsp --socket|--stdio [--port <port>]");
    } else {
        ns_lsp_parse_arg(argc, argv);
    }

    if (_client.mode == NS_LSP_SOCKET) {
        u16 port = _client.socket.port;
        ns_info("lsp", "Listening on port %d.\n", port);
        ns_tcp_serve(port, ns_lsp_on_connect);
    } else if (_client.mode == NS_LSP_STDIO) {
        while (1) {
            if (!ns_lsp_read_stdio_message()) break;
            if (ns_str_is_empty(_in)) continue;
            ns_lsp_handle_request(ns_json_parse(_in));
        }
    } else {
        ns_exit(1, "lsp", "Error: No valid mode specified. Use --socket or --stdio.");
    }
    return 0;
}
