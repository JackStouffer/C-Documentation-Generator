#include <clang-c/Index.h>
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>

static void die(const char *msg);
static char *dup_range(const char *src, size_t len);

typedef struct {
    char **data;
    size_t n, cap;
} StrSet;

typedef struct {
    char *name;
    char *anchor;
    char *kind;
} Entry;

typedef struct {
    Entry *data;
    size_t n, cap;
} EntryVec;

typedef struct {
    char *buf;
    size_t len, cap;
} StrBuf;

static void sb_free(StrBuf *sb) {
    free(sb->buf);
    sb->buf = NULL;
    sb->len = sb->cap = 0;
}

static void sb_reserve(StrBuf *sb, size_t extra) {
    size_t need = sb->len + extra + 1;
    if (need <= sb->cap) return;
    size_t newcap = sb->cap ? sb->cap * 2 : 64;
    while (newcap < need) newcap *= 2;
    sb->buf = (char*)realloc(sb->buf, newcap);
    if (!sb->buf) die("out of memory");
    sb->cap = newcap;
}

static void sb_append_n(StrBuf *sb, const char *s, size_t n) {
    if (n == 0) return;
    sb_reserve(sb, n);
    memcpy(sb->buf + sb->len, s, n);
    sb->len += n;
    sb->buf[sb->len] = '\0';
}

static void sb_append(StrBuf *sb, const char *s) {
    if (!s) return;
    sb_append_n(sb, s, strlen(s));
}

static void sb_append_char(StrBuf *sb, char c) {
    sb_reserve(sb, 1);
    sb->buf[sb->len++] = c;
    sb->buf[sb->len] = '\0';
}

static void sb_trim_trailing_space(StrBuf *sb) {
    while (sb->len > 0) {
        char c = sb->buf[sb->len - 1];
        if (c != ' ' && c != '\t' && c != '\n' && c != '\r') break;
        sb->len--;
    }
    if (sb->buf) sb->buf[sb->len] = '\0';
}

static void sb_ensure_blank_line(StrBuf *sb) {
    if (sb->len == 0) return;
    if (sb->len >= 2 && sb->buf[sb->len - 1] == '\n' && sb->buf[sb->len - 2] == '\n')
        return;
    if (sb->buf[sb->len - 1] != '\n') sb_append_char(sb, '\n');
    if (sb->len < 2 || sb->buf[sb->len - 2] != '\n') sb_append_char(sb, '\n');
}

static void sb_append_code_block(StrBuf *sb, const char *code, const char *lang) {
    if (!code) code = "";
    sb_ensure_blank_line(sb);
    sb_append(sb, "```");
    if (lang && *lang) {
        if (lang[0] == '.') lang++;
        sb_append(sb, lang);
    }
    sb_append(sb, "\n");
    sb_append(sb, code);
    size_t clen = strlen(code);
    if (clen == 0 || code[clen - 1] != '\n') sb_append_char(sb, '\n');
    sb_append(sb, "```\n");
}

static char *sb_detach(StrBuf *sb) {
    if (!sb->buf) return NULL;
    sb_reserve(sb, 0);
    char *out = sb->buf;
    sb->buf = NULL;
    sb->len = sb->cap = 0;
    return out;
}

typedef struct {
    char *name;
    StrBuf desc;
} ParamDoc;

typedef struct {
    ParamDoc *data;
    size_t n, cap;
} ParamVec;

static ParamDoc *paramvec_push(ParamVec *vec, const char *name) {
    if (vec->n == vec->cap) {
        vec->cap = vec->cap ? vec->cap * 2 : 8;
        vec->data = (ParamDoc*)realloc(vec->data, vec->cap * sizeof(ParamDoc));
        if (!vec->data) die("out of memory");
    }
    ParamDoc *entry = &vec->data[vec->n++];
    entry->name = strdup(name ? name : "");
    entry->desc.buf = NULL;
    entry->desc.len = entry->desc.cap = 0;
    return entry;
}

static void paramvec_free(ParamVec *vec) {
    for (size_t i = 0; i < vec->n; ++i) {
        free(vec->data[i].name);
        sb_free(&vec->data[i].desc);
    }
    free(vec->data);
    vec->data = NULL;
    vec->n = vec->cap = 0;
}

typedef struct {
    StrBuf text;
    bool has;
} SectionDoc;

static StrBuf *section_begin(SectionDoc *sec) {
    if (sec->has && sec->text.len > 0) sb_append(&sec->text, "\n\n");
   sec->has = true;
   return &sec->text;
}

static void append_param_desc(StrBuf *dest, const char *desc) {
    if (!desc || !*desc) return;
    for (const char *p = desc; *p; ++p) {
        if (*p == '\n') {
            sb_append_char(dest, '\n');
            sb_append(dest, "  ");
        } else {
            sb_append_char(dest, *p);
        }
    }
}

static char *doxygen_to_markdown(const char *text) {
    if (!text || !*text) return NULL;

    StrBuf general = {0};
    ParamVec params = {0};
    SectionDoc returns = {0}, notes = {0}, warnings = {0};
    StrBuf *current_buf = &general;

    char *copy = strdup(text);
    if (!copy) die("out of memory");
    char *cursor = copy;
    while (cursor) {
        char *line = cursor;
        char *newline = strchr(cursor, '\n');
        if (newline) { *newline = '\0'; cursor = newline + 1; }
        else cursor = NULL;

        char *trim = line;
        while (*trim == ' ' || *trim == '\t') trim++;

        if (strncmp(trim, "@code", 5) == 0 && (trim[5] == '\0' || isspace((unsigned char)trim[5]) || trim[5] == '{')) {
            char *lang = NULL;
            const char *lang_start = trim + 5;
            while (*lang_start == ' ' || *lang_start == '\t') lang_start++;
            if (*lang_start == '{') {
                const char *close = strchr(lang_start, '}');
                if (close && close > lang_start + 1) lang = dup_range(lang_start + 1, (size_t)(close - lang_start - 1));
            } else if (*lang_start) {
                lang = strdup(lang_start);
            }

            StrBuf code = {0};
            while (cursor) {
                char *code_line = cursor;
                char *code_newline = strchr(cursor, '\n');
                if (code_newline) { *code_newline = '\0'; cursor = code_newline + 1; }
                else cursor = NULL;
                char *code_trim = code_line;
                while (*code_trim == ' ' || *code_trim == '\t') code_trim++;
                if (strncmp(code_trim, "@endcode", 8) == 0) {
                    break;
                }
                sb_append(&code, code_line);
                sb_append_char(&code, '\n');
            }
            sb_append_code_block(current_buf, code.buf ? code.buf : "", lang);
            sb_free(&code);
            free(lang);
            continue;
        }

        bool handled = false;
        if ((strncmp(trim, "@param", 6) == 0 && (trim[6] == '\0' || isspace((unsigned char)trim[6]))) ||
            (strncmp(trim, "@params", 7) == 0 && (trim[7] == '\0' || isspace((unsigned char)trim[7])))) {
            const char *rest = trim + ((trim[6] == 's') ? 7 : 6);
            while (*rest == ' ' || *rest == '\t') rest++;
            const char *name_start = rest;
            while (*name_start == ' ' || *name_start == '\t') name_start++;
            const char *name_end = name_start;
            while (*name_end && !isspace((unsigned char)*name_end)) name_end++;
            char *name = NULL;
            if (name_end > name_start) name = dup_range(name_start, (size_t)(name_end - name_start));
            ParamDoc *param = paramvec_push(&params, name ? name : "");
            free(name);
            const char *desc_start = name_end;
            while (*desc_start == ' ' || *desc_start == '\t') desc_start++;
            if (*desc_start) {
                sb_append(&param->desc, desc_start);
                sb_append_char(&param->desc, '\n');
            }
            current_buf = &param->desc;
            handled = true;
        } else if ((strncmp(trim, "@return", 7) == 0 && (trim[7] == '\0' || isspace((unsigned char)trim[7]))) ||
                   (strncmp(trim, "@returns", 8) == 0 && (trim[8] == '\0' || isspace((unsigned char)trim[8])))) {
            const char *desc = trim + ((trim[7] == 's') ? 8 : 7);
            while (*desc == ' ' || *desc == '\t') desc++;
            StrBuf *target = section_begin(&returns);
            if (*desc) {
                sb_append(target, desc);
                sb_append_char(target, '\n');
            }
            current_buf = target;
            handled = true;
        } else if (strncmp(trim, "@note", 5) == 0 && (trim[5] == '\0' || isspace((unsigned char)trim[5]))) {
            const char *desc = trim + 5;
            while (*desc == ' ' || *desc == '\t') desc++;
            StrBuf *target = section_begin(&notes);
            if (*desc) {
                sb_append(target, desc);
                sb_append_char(target, '\n');
            }
            current_buf = target;
            handled = true;
        } else if (strncmp(trim, "@warning", 8) == 0 && (trim[8] == '\0' || isspace((unsigned char)trim[8]))) {
            const char *desc = trim + 8;
            while (*desc == ' ' || *desc == '\t') desc++;
            StrBuf *target = section_begin(&warnings);
            if (*desc) {
                sb_append(target, desc);
                sb_append_char(target, '\n');
            }
            current_buf = target;
            handled = true;
        }

        if (handled) continue;

        if (*trim == '\0') {
            if (current_buf) sb_append_char(current_buf, '\n');
        } else {
            if (!current_buf) current_buf = &general;
            sb_append(current_buf, trim);
            sb_append_char(current_buf, '\n');
        }
    }
    free(copy);

    sb_trim_trailing_space(&general);
    if (returns.has) sb_trim_trailing_space(&returns.text);
    if (notes.has) sb_trim_trailing_space(&notes.text);
    if (warnings.has) sb_trim_trailing_space(&warnings.text);
    for (size_t i = 0; i < params.n; ++i) sb_trim_trailing_space(&params.data[i].desc);

    StrBuf final = {0};
    if (general.len) sb_append(&final, general.buf);

    if (params.n) {
        if (final.len) sb_append(&final, "\n\n");
        sb_append(&final, "#### Parameters\n\n");
        for (size_t i = 0; i < params.n; ++i) {
            ParamDoc *p = &params.data[i];
            sb_append(&final, "**");
            sb_append(&final, p->name);
            sb_append(&final, "** \xE2\x80\x94 ");
            if (p->desc.len) append_param_desc(&final, p->desc.buf);
            sb_append_char(&final, '\n');
        }
    }

    if (returns.has && returns.text.len) {
        if (final.len) sb_append(&final, "\n\n");
        sb_append(&final, "#### Returns\n\n");
        sb_append(&final, returns.text.buf);
    }
    if (notes.has && notes.text.len) {
        if (final.len) sb_append(&final, "\n\n");
        sb_append(&final, "#### Note\n\n");
        sb_append(&final, notes.text.buf);
    }
    if (warnings.has && warnings.text.len) {
        if (final.len) sb_append(&final, "\n\n");
        sb_append(&final, "#### Warning\n\n");
        sb_append(&final, warnings.text.buf);
    }

    sb_trim_trailing_space(&final);

    char *result = NULL;
    if (final.len) result = sb_detach(&final);

    sb_free(&general);
    paramvec_free(&params);
    sb_free(&returns.text);
    sb_free(&notes.text);
    sb_free(&warnings.text);
    sb_free(&final);
    return result;
}

typedef struct {
    char *name;
    char *doc;
} FileDoc;

typedef struct {
    FileDoc *data;
    size_t n, cap;
} FileDocVec;

static void filedocs_add(FileDocVec *vec, const char *path, char *doc) {
    if (!doc) return;
    if (vec->n == vec->cap) {
        vec->cap = vec->cap ? vec->cap * 2 : 8;
        vec->data = (FileDoc*)realloc(vec->data, vec->cap * sizeof(FileDoc));
        if (!vec->data) die("out of memory");
    }
    const char *slash = strrchr(path, '/');
    const char *bslash = strrchr(path, '\\');
    const char *base = path;
    if (slash && bslash) base = (slash > bslash) ? slash + 1 : bslash + 1;
    else if (slash) base = slash + 1;
    else if (bslash) base = bslash + 1;
    char *name = strdup(base);
    if (!name) die("out of memory");
    vec->data[vec->n].name = name;
    vec->data[vec->n].doc = doc;
    vec->n++;
}

static void filedocs_free(FileDocVec *vec) {
    for (size_t i = 0; i < vec->n; ++i) {
        free(vec->data[i].name);
        free(vec->data[i].doc);
    }
    free(vec->data);
    vec->data = NULL;
    vec->n = vec->cap = 0;
}

static FILE *g_out;
static EntryVec g_macros, g_types, g_functions;
static StrSet g_ignore_patterns;
static FileDocVec g_file_docs;

static void die(const char *msg) { fprintf(stderr, "error: %s\n", msg); exit(1); }

static bool cursor_is_in_system_header(CXCursor c) {
    CXSourceLocation loc = clang_getCursorLocation(c);
    return clang_Location_isInSystemHeader(loc);
}

static void entryvec_add(EntryVec *vec, const char *name, const char *anchor, const char *kind) {
    if (vec->n == vec->cap) {
        vec->cap = vec->cap ? vec->cap * 2 : 64;
        vec->data = (Entry*)realloc(vec->data, vec->cap * sizeof(Entry));
    }
    vec->data[vec->n].name = strdup(name);
    vec->data[vec->n].anchor = strdup(anchor);
    vec->data[vec->n].kind = (kind && *kind) ? strdup(kind) : NULL;
    vec->n++;
}

static void entryvec_free(EntryVec *vec) {
    for (size_t i = 0; i < vec->n; ++i) {
        free(vec->data[i].name);
        free(vec->data[i].anchor);
        free(vec->data[i].kind);
    }
    free(vec->data);
    vec->data = NULL;
    vec->n = vec->cap = 0;
}

static char *make_anchor(const char *prefix, const char *name) {
    size_t plen = strlen(prefix);
    size_t nlen = strlen(name);
    size_t cap = plen + nlen * 2 + 4;
    char *buf = (char*)malloc(cap);
    size_t pos = 0;
    for (size_t i = 0; i < plen; ++i) {
        buf[pos++] = (char)tolower((unsigned char)prefix[i]);
    }
    if (pos && buf[pos - 1] != '-') buf[pos++] = '-';
    bool last_dash = (pos && buf[pos - 1] == '-');
    for (size_t i = 0; i < nlen; ++i) {
        unsigned char c = (unsigned char)name[i];
        if (isalnum(c) || c == '_') {
            buf[pos++] = (char)tolower(c);
            last_dash = false;
        } else {
            if (!last_dash && pos > 0) {
                buf[pos++] = '-';
                last_dash = true;
            }
        }
        if (pos + 2 >= cap) {
            cap *= 2;
            buf = (char*)realloc(buf, cap);
        }
    }
    while (pos > 0 && buf[pos - 1] == '-') pos--;
    if (pos == 0) {
        buf[pos++] = 'x';
    }
    buf[pos] = '\0';
    return buf;
}

static void print_summary_section(const char *title, EntryVec *vec, bool include_kind) {
    printf("## %s\n\n", title);
    if (vec->n == 0) {
        printf("- (none)\n\n");
        return;
    }
    for (size_t i = 0; i < vec->n; ++i) {
        Entry *e = &vec->data[i];
        if (include_kind && e->kind) {
            printf("- [%s `%s`](#%s)\n", e->kind, e->name, e->anchor);
        } else {
            printf("- [`%s`](#%s)\n", e->name, e->anchor);
        }
    }
    printf("\n");
}

static void set_add(StrSet *s, const char *key) {
    for (size_t i = 0; i < s->n; ++i) if (strcmp(s->data[i], key) == 0) return;
    if (s->n == s->cap) { s->cap = s->cap ? s->cap * 2 : 64; s->data = (char**)realloc(s->data, s->cap * sizeof(char*)); }
    s->data[s->n++] = strdup(key);
}
static bool set_has(StrSet *s, const char *key) {
    for (size_t i = 0; i < s->n; ++i) if (strcmp(s->data[i], key) == 0) return true;
    return false;
}
static void set_free(StrSet *s) {
    for (size_t i = 0; i < s->n; ++i) free(s->data[i]);
    free(s->data);
    s->data = NULL;
    s->n = s->cap = 0;
}

static bool pattern_match(const char *pat, const char *text) {
    if (!pat || !text) return false;
    if (*pat == '\0') return *text == '\0';
    if (*pat == '*') {
        while (*pat == '*') pat++;
        if (*pat == '\0') return true;
        for (; *text; ++text) {
            if (pattern_match(pat, text)) return true;
        }
        return pattern_match(pat, text);
    }
    if (*pat == '?') {
        if (*text == '\0') return false;
        return pattern_match(pat + 1, text + 1);
    }
    if (*pat == *text) {
        return pattern_match(pat + 1, text + 1);
    }
    return false;
}

static char *dup_cx(CXString s) {
    const char *c = clang_getCString(s);
    char *r = strdup(c ? c : "");
    clang_disposeString(s);
    return r;
}

static char *dup_range(const char *src, size_t len) {
    char *out = (char*)malloc(len + 1);
    if (!out) die("out of memory");
    memcpy(out, src, len);
    out[len] = '\0';
    return out;
}

static bool should_ignore(const char *name) {
    if (!name || !*name || g_ignore_patterns.n == 0) return false;
    for (size_t i = 0; i < g_ignore_patterns.n; ++i) {
        if (pattern_match(g_ignore_patterns.data[i], name)) return true;
    }
    return false;
}

static char *extract_macro_comment(CXTranslationUnit tu, CXCursor cursor) {
    CXSourceRange range = clang_getCursorExtent(cursor);
    CXSourceLocation start_loc = clang_getRangeStart(range);
    CXFile file = NULL;
    unsigned line = 0, col = 0;
    unsigned offset = 0;
    clang_getSpellingLocation(start_loc, &file, &line, &col, &offset);
    if (!file) return NULL;
    size_t buf_len = 0;
    const char *buf = clang_getFileContents(tu, file, &buf_len);
    if (!buf || offset == 0 || offset > buf_len) return NULL;

    size_t pos = offset;
    while (pos > 0 && buf[pos - 1] != '\n') pos--;
    // Skip whitespace between comment and macro; stop if blank line encountered
    size_t idx = pos;
    while (idx > 0 && isspace((unsigned char)buf[idx - 1])) {
        if (buf[idx - 1] == '\n') {
            size_t line_start = idx - 1;
            while (line_start > 0 && buf[line_start - 1] != '\n') line_start--;
            bool non_ws = false;
            for (size_t i = line_start; i < idx - 1; ++i) {
                if (!isspace((unsigned char)buf[i])) { non_ws = true; break; }
            }
            if (!non_ws) {
                return NULL; // blank line separating comment
            }
        }
        idx--;
    }
    if (idx == 0) return NULL;
    size_t end = idx;

    // Attempt block comment detection
    if (end >= 2 && buf[end - 2] == '*' && buf[end - 1] == '/') {
        size_t start_pos = end - 2;
        while (start_pos > 0) {
            if (buf[start_pos - 1] == '/' && buf[start_pos] == '*') {
                start_pos--;
                break;
            }
            start_pos--;
        }
        if (buf[start_pos] != '/' || buf[start_pos + 1] != '*') return NULL;
        size_t len = end - start_pos;
        return dup_range(buf + start_pos, len);
    }

    // Attempt line comment detection (// or ///) accumulating contiguous block
    size_t comment_start = end;
    size_t cur = end;
    bool saw_comment = false;
    while (cur > 0) {
        size_t line_end = cur;
        size_t line_start = cur;
        while (line_start > 0 && buf[line_start - 1] != '\n') line_start--;
        size_t i = line_start;
        while (i < line_end && isspace((unsigned char)buf[i])) i++;
        if (i + 1 < line_end && buf[i] == '/' && buf[i + 1] == '/') {
            saw_comment = true;
            comment_start = line_start;
            cur = line_start;
            if (cur > 0 && buf[cur - 1] == '\n') cur--;
            continue;
        }
        break;
    }
    if (saw_comment) {
        size_t len = end - comment_start;
        return dup_range(buf + comment_start, len);
    }

    return NULL;
}

static char *normalize_comment(const char *raw) {
    if (!raw || !*raw) return NULL;
    size_t raw_len = strlen(raw);
    if (raw_len == 0) return NULL;
    bool is_block = raw_len >= 2 && raw[0] == '/' && raw[1] == '*';
    bool is_line = raw_len >= 2 && raw[0] == '/' && raw[1] == '/';
    if (!is_block && !is_line) return dup_range(raw, raw_len);

    const char *p = raw;
    const char *end = raw + raw_len;
    if (is_block) {
        p += 2;
        while (p < end && *p == '*') p++;
        if (p < end && *p == ' ') p++;
    }

    char **lines = NULL;
    size_t *lens = NULL;
    size_t n = 0, cap = 0;

    while (p < end) {
        const char *line_end = memchr(p, '\n', end - p);
        bool has_newline = line_end != NULL;
        const char *line_stop = has_newline ? line_end : end;
        if (line_stop > p && line_stop[-1] == '\r') line_stop--;

        const char *s = p;
        if (is_block) {
            while (s < line_stop && (*s == ' ' || *s == '\t')) s++;
            if (s < line_stop && *s == '*') {
                s++;
                if (s < line_stop && *s == ' ') s++;
            }
            if (!has_newline) {
                const char *tmp_end = line_stop;
                while (tmp_end > s && isspace((unsigned char)tmp_end[-1])) tmp_end--;
                if (tmp_end >= s + 2 && tmp_end[-2] == '*' && tmp_end[-1] == '/') tmp_end -= 2;
                while (tmp_end > s && isspace((unsigned char)tmp_end[-1])) tmp_end--;
                line_stop = tmp_end;
            }
        } else {
            while (s < line_stop && (*s == ' ' || *s == '\t')) s++;
            if (s < line_stop && s[0] == '/' && s + 1 < line_stop && s[1] == '/') {
                s += 2;
                while (s < line_stop && *s == '/') s++;
                if (s < line_stop && *s == ' ') s++;
            }
        }

        const char *trim_end = line_stop;
        while (trim_end > s && trim_end[-1] == '\r') trim_end--;
        size_t seg = trim_end > s ? (size_t)(trim_end - s) : 0;
        if (is_block && seg == 1 && s[0] == '/') {
            seg = 0;
        }

        if (cap == n) {
            cap = cap ? cap * 2 : 8;
            lines = (char**)realloc(lines, cap * sizeof(char*));
            lens = (size_t*)realloc(lens, cap * sizeof(size_t));
            if (!lines || !lens) die("out of memory");
        }
        lines[n] = (seg > 0) ? dup_range(s, seg) : strdup("");
        lens[n] = seg;
        n++;

        if (!has_newline) break;
        p = line_end + 1;
    }

    size_t start_idx = 0;
    while (start_idx < n && lens[start_idx] == 0) start_idx++;
    size_t end_idx = n;
    while (end_idx > start_idx && lens[end_idx - 1] == 0) end_idx--;

    if (start_idx == end_idx) {
        for (size_t i = 0; i < n; ++i) free(lines[i]);
        free(lines);
        free(lens);
        return NULL;
    }
    for (size_t i = 0; i < start_idx; ++i) free(lines[i]);
    for (size_t i = end_idx; i < n; ++i) free(lines[i]);

    size_t total = 0;
    for (size_t i = start_idx; i < end_idx; ++i) {
        total += lens[i];
        if (i + 1 < end_idx) total++;
    }
    char *result = (char*)malloc(total + 1);
    if (!result) die("out of memory");
    size_t pos = 0;
    for (size_t i = start_idx; i < end_idx; ++i) {
        if (i > start_idx) result[pos++] = '\n';
        if (lens[i] > 0) {
            memcpy(result + pos, lines[i], lens[i]);
            pos += lens[i];
        }
        free(lines[i]);
    }
    result[pos] = '\0';
    free(lines);
    free(lens);
    char *md = doxygen_to_markdown(result);
    free(result);
    return md;
}

static char *extract_file_doc(const char *path) {
    FILE *fp = fopen(path, "rb");
    if (!fp) return NULL;
    if (fseek(fp, 0, SEEK_END) != 0) { fclose(fp); return NULL; }
    long sz = ftell(fp);
    if (sz <= 0) { fclose(fp); return NULL; }
    if (fseek(fp, 0, SEEK_SET) != 0) { fclose(fp); return NULL; }
    size_t len = (size_t)sz;
    char *buf = (char*)malloc(len + 1);
    if (!buf) { fclose(fp); die("out of memory"); }
    size_t readn = fread(buf, 1, len, fp);
    fclose(fp);
    if (readn != len) { free(buf); return NULL; }
    buf[len] = '\0';

    size_t pos = 0;
    if (len >= 3 && (unsigned char)buf[0] == 0xEF && (unsigned char)buf[1] == 0xBB && (unsigned char)buf[2] == 0xBF)
        pos = 3;

    while (pos < len && (buf[pos] == ' ' || buf[pos] == '\t' || buf[pos] == '\r' || buf[pos] == '\n'))
        pos++;
    if (pos >= len) { free(buf); return NULL; }

    char *raw = NULL;
    if (buf[pos] == '/' && pos + 1 < len) {
        if (buf[pos + 1] == '*') {
            size_t cur = pos + 2;
            while (cur + 1 < len && !(buf[cur] == '*' && buf[cur + 1] == '/')) cur++;
            if (cur + 1 >= len) { free(buf); return NULL; }
            cur += 2;
            raw = dup_range(buf + pos, cur - pos);
        } else if (buf[pos + 1] == '/') {
            size_t cur = pos;
            while (cur < len) {
                if (buf[cur] == '\r' || buf[cur] == '\n') {
                    size_t after = cur;
                    if (buf[cur] == '\r' && cur + 1 < len && buf[cur + 1] == '\n') after = cur + 2;
                    else after = cur + 1;
                    size_t peek = after;
                    while (peek < len && (buf[peek] == ' ' || buf[peek] == '\t')) peek++;
                    if (peek + 1 < len && buf[peek] == '/' && buf[peek + 1] == '/') {
                        cur = after;
                        continue;
                    } else {
                        cur = after;
                        break;
                    }
                } else {
                    cur++;
                }
            }
            raw = dup_range(buf + pos, cur - pos);
        }
    }

    free(buf);
    if (!raw) return NULL;
    char *md = normalize_comment(raw);
    free(raw);
    return md;
}

static char *bump_markdown_headers(const char *text) {
    if (!text || !*text) return NULL;
    StrBuf out = {0};
    const char *p = text;
    bool in_code_block = false;

    while (*p) {
        const char *line_end = strchr(p, '\n');
        size_t len = line_end ? (size_t)(line_end - p) : strlen(p);
        const char *line_start = p;
        const char *trim = line_start;
        while ((size_t)(trim - line_start) < len && (*trim == ' ' || *trim == '\t')) trim++;
        size_t trimmed_len = len - (size_t)(trim - line_start);

        bool is_fence = (trimmed_len >= 3 && strncmp(trim, "```", 3) == 0);
        if (is_fence) {
            in_code_block = !in_code_block;
        }

        if (!in_code_block && !is_fence && trimmed_len > 0 && trim[0] == '#') {
            size_t hash_count = 0;
            while (hash_count < trimmed_len && trim[hash_count] == '#') hash_count++;
            size_t new_level = hash_count < 6 ? hash_count + 1 : 6;
            sb_append_n(&out, line_start, (size_t)(trim - line_start));
            for (size_t i = 0; i < new_level; ++i) sb_append_char(&out, '#');
            const char *rest = trim + hash_count;
            size_t rest_len = len - (size_t)(rest - line_start);
            if (rest_len > 0) sb_append_n(&out, rest, rest_len);
        } else {
            sb_append_n(&out, line_start, len);
        }

        if (line_end) {
            sb_append_char(&out, '\n');
            p = line_end + 1;
        } else {
            break;
        }
    }

    char *result = sb_detach(&out);
    sb_free(&out);
    return result;
}

static void print_location(CXCursor c) {
    CXSourceLocation loc = clang_getCursorLocation(c);
    CXFile file; unsigned line, col, off;
    clang_getSpellingLocation(loc, &file, &line, &col, &off);
    char *path = dup_cx(clang_getFileName(file));
    if (path && *path) fprintf(g_out, "\n*Defined at*: `%s:%u`\n\n", path, line);
    free(path);
}

static bool print_md_comment(CXCursor c) {
    char *raw = dup_cx(clang_Cursor_getRawCommentText(c));
    char *norm = normalize_comment(raw);
    free(raw);
    if (norm && *norm) {
        fprintf(g_out, "%s\n\n", norm);
        free(norm);
        return true;
    }
    free(norm);
    return false;
}

static void print_code_block(const char *code) {
    fprintf(g_out, "```c\n%s\n```\n\n", code);
}

static char *cursor_usr(CXCursor c) { return dup_cx(clang_getCursorUSR(c)); }
static char *cursor_name(CXCursor c) { return dup_cx(clang_getCursorSpelling(c)); }

static char *type_spelling(CXType t) { return dup_cx(clang_getTypeSpelling(t)); }

typedef struct {
    CXTranslationUnit tu;
    StrSet seen; // USR dedupe
} Ctx;

/* Join tokens in a source range into a single line of text (for macros/prototypes). */
static char *range_text(CXTranslationUnit tu, CXSourceRange range) {
    CXToken *toks = NULL; unsigned ntok = 0;
    clang_tokenize(tu, range, &toks, &ntok);
    size_t cap = 256, len = 0;
    char *buf = (char*)malloc(cap);
    buf[0] = 0;
    for (unsigned i = 0; i < ntok; ++i) {
        char *sp = dup_cx(clang_getTokenSpelling(tu, toks[i]));
        size_t sl = strlen(sp);
        if (len + sl + 2 >= cap) { cap = (len + sl + 2) * 2; buf = (char*)realloc(buf, cap); }
        if (len && sp[0] != ',' && sp[0] != ';' && sp[0] != ')' && sp[0] != ']' &&
            strcmp(sp, ">") && strcmp(sp, "::")) {
            // add space before most tokens except common punct
            buf[len++] = ' ';
        }
        memcpy(buf + len, sp, sl); len += sl; buf[len] = 0;
        free(sp);
    }
    clang_disposeTokens(tu, toks, ntok);
    // Collapse newlines/extra spaces
    for (size_t i = 0; i < len; ++i) if (buf[i] == '\n' || buf[i] == '\r' || buf[i] == '\t') buf[i] = ' ';
    return buf;
}

/* Print function prototype */
static void emit_function(CXCursor c) {
    char *name = cursor_name(c);
    if (should_ignore(name)) {
        free(name);
        return;
    }
    const char *anchor_key = (*name) ? name : "anonymous";
    char *anchor = make_anchor("function", anchor_key);
    entryvec_add(&g_functions, anchor_key, anchor, NULL);
    fprintf(g_out, "<a id=\"%s\"></a>\n", anchor);
    CXType ft = clang_getCursorType(c);
    CXType rt = clang_getResultType(ft);
    char *rts = type_spelling(rt);
    char *disp = dup_cx(clang_getCursorDisplayName(c)); // name(params)
    fprintf(g_out, "### Function: `%s`\n\n", name);
    print_md_comment(c);
    char line[4096];
    snprintf(line, sizeof(line), "%s %s;", rts, disp);
    print_code_block(line);
    print_location(c);
    fprintf(g_out, "---\n\n");
    free(anchor);
    free(name); free(rts); free(disp);
}

/* Collect struct/union fields or enum constants. */
static enum CXChildVisitResult struct_enum_visitor(CXCursor c, CXCursor parent, CXClientData cd) {
    enum CXCursorKind k = clang_getCursorKind(c);
    if (k == CXCursor_FieldDecl) {
        char *nm = cursor_name(c);
        char *ts = type_spelling(clang_getCursorType(c));
        fprintf(g_out, "- `%s %s;`\n", ts, nm);
        free(nm); free(ts);
    } else if (k == CXCursor_EnumConstantDecl) {
        char *nm = cursor_name(c);
        long long val = clang_getEnumConstantDeclValue(c);
        fprintf(g_out, "- `%s = %lld`\n", nm, val);
        free(nm);
    }
    return CXChildVisit_Continue;
}

static void emit_record(CXCursor c, const char *what) {
    char *name = cursor_name(c);
    const char *display = (*name) ? name : "(anonymous)";
    if (should_ignore(display)) {
        free(name);
        return;
    }
    char prefix[64];
    snprintf(prefix, sizeof(prefix), "type-%s", what);
    char *anchor = make_anchor(prefix, display);
    entryvec_add(&g_types, display, anchor, what);
    fprintf(g_out, "<a id=\"%s\"></a>\n", anchor);
    fprintf(g_out, "### %s: `%s`\n\n", what, display);
    print_md_comment(c);
    // List members
    clang_visitChildren(c, struct_enum_visitor, NULL);
    fprintf(g_out, "\n");
    print_location(c);
    fprintf(g_out, "---\n\n");
    free(anchor);
    free(name);
}

static void emit_typedef(CXCursor c) {
    char *name = cursor_name(c);
    CXType ut = clang_getTypedefDeclUnderlyingType(c);
    char *uts = type_spelling(ut);
    const char *display = (*name) ? name : "(anonymous)";
    if (should_ignore(display)) {
        free(name);
        free(uts);
        return;
    }
    char *anchor = make_anchor("type-typedef", display);
    entryvec_add(&g_types, display, anchor, "Typedef");
    fprintf(g_out, "<a id=\"%s\"></a>\n", anchor);
    fprintf(g_out, "### Typedef: `%s`\n\n", name);
    print_md_comment(c);
    char line[4096];
    snprintf(line, sizeof(line), "typedef %s %s;", uts, name);
    print_code_block(line);
    print_location(c);
    fprintf(g_out, "---\n\n");
    free(anchor);
    free(name); free(uts);
}

static void emit_macro(CXCursor c, CXTranslationUnit tu) {
    char *name = cursor_name(c);
    const char *display = (*name) ? name : "(anonymous)";
    if (should_ignore(display)) {
        free(name);
        return;
    }
    char *anchor = make_anchor("macro", display);
    entryvec_add(&g_macros, display, anchor, NULL);
    fprintf(g_out, "<a id=\"%s\"></a>\n", anchor);
    fprintf(g_out, "### Macro: `%s`\n\n", name);
    // libclang rarely attaches raw comments to macros; still try:
    if (!print_md_comment(c)) {
        char *manual = extract_macro_comment(tu, c);
        char *norm = normalize_comment(manual);
        if (norm && *norm) {
            fprintf(g_out, "%s\n\n", norm);
        }
        free(norm);
        free(manual);
    }
    // Reconstruct the #define line/body
    CXSourceRange r = clang_getCursorExtent(c);
    char *txt = range_text(tu, r);
    // Ensure it includes "#define"; if not, synthesize.
    if (strstr(txt, "#define") == NULL) {
        char *def = (char*)malloc(strlen(name) + strlen(txt) + 16);
        sprintf(def, "#define %s %s", name, txt);
        free(txt); txt = def;
    }
    print_code_block(txt);
    print_location(c);
    fprintf(g_out, "---\n\n");
    free(anchor);
    free(txt); free(name);
}

static enum CXChildVisitResult tu_visitor(CXCursor c, CXCursor parent, CXClientData client_data) {
    Ctx *ctx = (Ctx*)client_data;
    enum CXCursorKind k = clang_getCursorKind(c);

    // Only handle top-level decls
    if (!clang_isDeclaration(k) && k != CXCursor_MacroDefinition && k != CXCursor_EnumDecl)
        return CXChildVisit_Recurse;

    // Dedup by USR when available (macros often lack USR)
    char *usr = cursor_usr(c);
    bool have_usr = usr && *usr;
    if (have_usr && set_has(&ctx->seen, usr)) { free(usr); return CXChildVisit_Continue; }
    if (have_usr) set_add(&ctx->seen, usr);
    free(usr);

    switch (k) {
        case CXCursor_FunctionDecl:
            // Emit the first declaration/definition we encounter; USR dedupe avoids repeats.
            emit_function(c);
            break;
        case CXCursor_StructDecl: emit_record(c, "Struct"); break;
        case CXCursor_UnionDecl:  emit_record(c, "Union");  break;
        case CXCursor_EnumDecl:   emit_record(c, "Enum");   break;
        case CXCursor_TypedefDecl: emit_typedef(c); break;
        case CXCursor_MacroDefinition:
            // Skip system headers, but allow project/local headers included by the file.
            if (!cursor_is_in_system_header(c)) {
                emit_macro(c, ctx->tu);
            }
            break;
        default: break;
    }
    return CXChildVisit_Recurse;
}

static void process_file(CXIndex idx, const char *path, int clang_argc, const char **clang_argv) {
    char *file_doc = extract_file_doc(path);
    bool have_file_doc = file_doc && *file_doc;
    if (have_file_doc) {
        filedocs_add(&g_file_docs, path, file_doc);
    } else {
        free(file_doc);
        file_doc = NULL;
    }
    unsigned opts = CXTranslationUnit_DetailedPreprocessingRecord |
                    CXTranslationUnit_IncludeBriefCommentsInCodeCompletion;
    CXTranslationUnit tu = NULL;
    enum CXErrorCode ec = clang_parseTranslationUnit2(
        idx, path, clang_argv, clang_argc, NULL, 0, opts, &tu);
    if (ec != CXError_Success || !tu) {
        fprintf(stderr, "failed to parse: %s (ec=%d)\n", path, ec);
        return;
    }

    Ctx ctx = {0};
    ctx.tu = tu;
    fprintf(g_out, "## File: %s\n\n", path);
    if (have_file_doc) {
        char *adjusted = bump_markdown_headers(file_doc);
        fprintf(g_out, "%s\n\n", adjusted ? adjusted : file_doc);
        free(adjusted);
    }
    clang_visitChildren(clang_getTranslationUnitCursor(tu), tu_visitor, &ctx);
    clang_disposeTranslationUnit(tu);

    // cleanup set
    set_free(&ctx.seen);
}

static void print_help(const char *prog) {
    printf("Usage: %s [options] <file.c|file.h>... [-- <clang-args...>]\n", prog);
    printf("Generate Markdown documentation for C headers or sources.\n\n");
    printf("Options:\n");
    printf("  -h, --help          Show this help message and exit\n");
    printf("  --ignore PATTERN    Skip symbols whose names match PATTERN (* and ? supported)\n");
}

int main(int argc, const char **argv) {
    for (int i = 1; i < argc && strcmp(argv[i], "--") != 0; ++i) {
        if (strcmp(argv[i], "-h") == 0 || strcmp(argv[i], "--help") == 0) {
            print_help(argv[0]);
            return 0;
        }
        if (strcmp(argv[i], "--ignore") == 0) {
            ++i; // skip pattern placeholder if present
        }
    }

    if (argc < 2) {
        fprintf(stderr, "usage: %s <file.c|file.h>... [-- <clang-args...>]\n", argv[0]);
        fprintf(stderr, "Try '%s --help' for more information.\n", argv[0]);
        return 2;
    }
    int argi = 1;
    while (argi < argc && strcmp(argv[argi], "--") != 0) {
        if (strcmp(argv[argi], "--ignore") == 0) {
            if (argi + 1 >= argc) die("missing pattern after --ignore");
            set_add(&g_ignore_patterns, argv[argi + 1]);
            argi += 2;
            continue;
        }
        break;
    }

    int split = argc;
    for (int i = argi; i < argc; ++i) if (strcmp(argv[i], "--") == 0) { split = i; break; }

    for (int i = argi; i < split; ++i) {
        if (strcmp(argv[i], "--ignore") == 0) die("--ignore must appear before input files");
    }

    int nfiles = split - argi;
    if (nfiles <= 0) die("no input files");
    int cargc = (split < argc) ? (argc - split - 1) : 0;
    const char **cargv = (cargc > 0) ? (argv + split + 1) : NULL;

    FILE *body = tmpfile();
    if (!body) die("failed to allocate temporary buffer");
    g_out = body;
    printf("# API Documentation\n\n");
    CXIndex idx = clang_createIndex(/*excludeDeclsFromPCH=*/0, /*displayDiagnostics=*/0);
    for (int i = 0; i < nfiles; ++i) {
        process_file(idx, argv[argi + i], cargc, cargv);
    }
    clang_disposeIndex(idx);
    print_summary_section("Macros", &g_macros, false);
    print_summary_section("Types", &g_types, true);
    print_summary_section("Functions", &g_functions, false);
    rewind(body);
    char buf[4096];
    size_t read_bytes;
    while ((read_bytes = fread(buf, 1, sizeof(buf), body)) > 0) {
        fwrite(buf, 1, read_bytes, stdout);
    }
    fclose(body);
    entryvec_free(&g_macros);
    entryvec_free(&g_types);
    entryvec_free(&g_functions);
    filedocs_free(&g_file_docs);
    set_free(&g_ignore_patterns);
    return 0;
}
