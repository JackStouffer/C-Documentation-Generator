#include <clang-c/Index.h>
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>

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

static FILE *g_out;
static EntryVec g_macros, g_types, g_functions;
static StrSet g_ignore_patterns;

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

static bool should_ignore(const char *name) {
    if (!name || !*name || g_ignore_patterns.n == 0) return false;
    for (size_t i = 0; i < g_ignore_patterns.n; ++i) {
        if (pattern_match(g_ignore_patterns.data[i], name)) return true;
    }
    return false;
}

static char *dup_cx(CXString s) {
    const char *c = clang_getCString(s);
    char *r = strdup(c ? c : "");
    clang_disposeString(s);
    return r;
}

static void print_location(CXCursor c) {
    CXSourceLocation loc = clang_getCursorLocation(c);
    CXFile file; unsigned line, col, off;
    clang_getSpellingLocation(loc, &file, &line, &col, &off);
    char *path = dup_cx(clang_getFileName(file));
    if (path && *path) fprintf(g_out, "\n*Defined at*: `%s:%u`\n\n", path, line);
    free(path);
}

static void print_md_comment(CXCursor c) {
    char *raw = dup_cx(clang_Cursor_getRawCommentText(c));
    if (raw && *raw) {
        // Strip leading /** or /// a bit (minimal cleanup)
        fprintf(g_out, "%s\n\n", raw);
    }
    free(raw);
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
    print_md_comment(c);
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
    clang_visitChildren(clang_getTranslationUnitCursor(tu), tu_visitor, &ctx);
    clang_disposeTranslationUnit(tu);

    // cleanup set
    set_free(&ctx.seen);
}

int main(int argc, const char **argv) {
    if (argc < 2) {
        fprintf(stderr, "usage: %s <file.c|file.h>... [-- <clang-args...>]\n", argv[0]);
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
    set_free(&g_ignore_patterns);
    return 0;
}
