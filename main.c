#include <clang-c/Index.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>

typedef struct {
    char **data;
    size_t n, cap;
} StrSet;

static void die(const char *msg) { fprintf(stderr, "error: %s\n", msg); exit(1); }

static bool cursor_is_in_main_file(CXCursor c) {
    CXSourceLocation loc = clang_getCursorLocation(c);
    return clang_Location_isFromMainFile(loc);
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
    if (path && *path) printf("\n*Defined at*: `%s:%u`\n\n", path, line);
    free(path);
}

static void print_md_comment(CXCursor c) {
    char *raw = dup_cx(clang_Cursor_getRawCommentText(c));
    if (raw && *raw) {
        // Strip leading /** or /// a bit (minimal cleanup)
        printf("%s\n\n", raw);
    }
    free(raw);
}

static void print_code_block(const char *code) {
    printf("```c\n%s\n```\n\n", code);
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
    CXType ft = clang_getCursorType(c);
    CXType rt = clang_getResultType(ft);
    char *rts = type_spelling(rt);
    char *disp = dup_cx(clang_getCursorDisplayName(c)); // name(params)
    printf("### Function: `%s`\n\n", name);
    print_md_comment(c);
    char line[4096];
    snprintf(line, sizeof(line), "%s %s;", rts, disp);
    print_code_block(line);
    print_location(c);
    free(name); free(rts); free(disp);
}

/* Collect struct/union fields or enum constants. */
static enum CXChildVisitResult struct_enum_visitor(CXCursor c, CXCursor parent, CXClientData cd) {
    enum CXCursorKind k = clang_getCursorKind(c);
    if (k == CXCursor_FieldDecl) {
        char *nm = cursor_name(c);
        char *ts = type_spelling(clang_getCursorType(c));
        printf("- `%s %s;`\n", ts, nm);
        free(nm); free(ts);
    } else if (k == CXCursor_EnumConstantDecl) {
        char *nm = cursor_name(c);
        long long val = clang_getEnumConstantDeclValue(c);
        printf("- `%s = %lld`\n", nm, val);
        free(nm);
    }
    return CXChildVisit_Continue;
}

static void emit_record(CXCursor c, const char *what) {
    char *name = cursor_name(c);
    printf("### %s: `%s`\n\n", what, *name ? name : "(anonymous)");
    print_md_comment(c);
    // List members
    clang_visitChildren(c, struct_enum_visitor, NULL);
    printf("\n");
    print_location(c);
    free(name);
}

static void emit_typedef(CXCursor c) {
    char *name = cursor_name(c);
    CXType ut = clang_getTypedefDeclUnderlyingType(c);
    char *uts = type_spelling(ut);
    printf("### Typedef: `%s`\n\n", name);
    print_md_comment(c);
    char line[4096];
    snprintf(line, sizeof(line), "typedef %s %s;", uts, name);
    print_code_block(line);
    print_location(c);
    free(name); free(uts);
}

static void emit_macro(CXCursor c, CXTranslationUnit tu) {
    char *name = cursor_name(c);
    printf("### Macro: `%s`\n\n", name);
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
            // Only include macros actually defined in the main file passed to clang
            if (cursor_is_in_main_file(c)) {
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
    printf("## File: %s\n\n", path);
    clang_visitChildren(clang_getTranslationUnitCursor(tu), tu_visitor, &ctx);
    clang_disposeTranslationUnit(tu);

    // cleanup set
    for (size_t i = 0; i < ctx.seen.n; ++i) free(ctx.seen.data[i]);
    free(ctx.seen.data);
}

int main(int argc, const char **argv) {
    if (argc < 2) {
        fprintf(stderr, "usage: %s <file.c|file.h>... [-- <clang-args...>]\n", argv[0]);
        return 2;
    }
    int split = argc;
    for (int i = 1; i < argc; ++i) if (strcmp(argv[i], "--") == 0) { split = i; break; }

    int nfiles = split - 1;
    if (nfiles <= 0) die("no input files");
    int cargc = (split < argc) ? (argc - split - 1) : 0;
    const char **cargv = (cargc > 0) ? (argv + split + 1) : NULL;

    printf("# API Documentation\n\n");

    CXIndex idx = clang_createIndex(/*excludeDeclsFromPCH=*/0, /*displayDiagnostics=*/0);
    for (int i = 1; i <= nfiles; ++i) {
        process_file(idx, argv[i], cargc, cargv);
    }
    clang_disposeIndex(idx);
    return 0;
}
