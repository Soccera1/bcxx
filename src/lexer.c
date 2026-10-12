#include "bcxx.h"
#include <ctype.h>
#include <stdlib.h>
#include <string.h>

static int ident_start(unsigned char c) { return isalpha(c) || c == '_' || c >= 128; }
static int ident_part(unsigned char c) { return ident_start(c) || isdigit(c); }

/* Translation phases 1 and 2 precede comment and token recognition. */
static char *translate(Arena *a, const char *src, Location **map, size_t **offsets, const char *file)
{
    size_t n = strlen(src), i = 0, j = 0;
    char *out = allocate(a, n + 1);
    Location *positions = allocate(a, (n + 1) * sizeof(*positions));
    size_t *original = allocate(a, (n + 1) * sizeof(*original));
    Location loc = {file, 1, 1};
    while (i < n) {
        size_t width = 1;
        char c = src[i];
        if (i + 2 < n && src[i] == '?' && src[i + 1] == '?') {
            const char *keys = "=/'()!<>-";
            const char *values = "#\\^[]|{}~";
            const char *key = strchr(keys, src[i + 2]);
            if (key) { c = values[key - keys]; width = 3; }
        }
        if (c == '\r') {
            c = '\n';
            if (i + 1 < n && src[i + 1] == '\n') width = 2;
        }
        if (c == '\\' && i + width < n &&
            (src[i + width] == '\n' || src[i + width] == '\r')) {
            i += width;
            if (src[i] == '\r' && src[i + 1] == '\n') i++;
            i++;
            loc.line++;
            loc.column = 1;
            continue;
        }
        positions[j] = loc;
        original[j] = i;
        out[j++] = c;
        i += width;
        if (c == '\n') { loc.line++; loc.column = 1; }
        else loc.column += (unsigned)width;
    }
    positions[j] = loc;
    original[j] = n;
    *map = positions;
    *offsets = original;
    return out;
}

static int literal_prefix(const char *s, size_t *prefix, int *raw, int cplusplus)
{
    size_t i = 0;
    *raw = 0;
    if (cplusplus && s[i] == 'u' && s[i + 1] == '8') i += 2;
    else if ((cplusplus && (s[i] == 'u' || s[i] == 'U')) || s[i] == 'L') i++;
    if (cplusplus && s[i] == 'R' && s[i + 1] == '"') { *raw = 1; i++; }
    if (s[i] != '"' && s[i] != '\'') return 0;
    *prefix = i;
    return 1;
}

int lex(Arena *a, const char *source, const char *file, int cplusplus, Tokens *out)
{
    Location *positions;
    size_t *offsets;
    char *s = translate(a, source, &positions, &offsets, file);
    size_t i = 0;
    unsigned space = 0;
    while (s[i]) {
        size_t start = i, prefix = 0;
        int raw = 0;
        const char *raw_text = NULL;
        Token t;
        memset(&t, 0, sizeof(t));
        t.loc = positions[i];
        t.space = space;
        if (s[i] == '\n') {
            t.kind = TOK_NEWLINE;
            t.text = "\n";
            tokens_push(out, t);
            i++;
            space = 1;
            continue;
        }
        if (isspace((unsigned char)s[i])) { i++; space = 1; continue; }
        if (s[i] == '/' && s[i + 1] == '/') {
            i += 2;
            while (s[i] && s[i] != '\n') i++;
            space = 1;
            continue;
        }
        if (s[i] == '/' && s[i + 1] == '*') {
            i += 2;
            while (s[i] && !(s[i] == '*' && s[i + 1] == '/')) {
                if (s[i] == '\n') {
                    t.kind = TOK_NEWLINE;
                    t.text = "\n";
                    t.loc = positions[i];
                    tokens_push(out, t);
                }
                i++;
            }
            if (!s[i]) { error_at(positions[start], "unterminated comment"); return 0; }
            i += 2;
            space = 1;
            continue;
        }
        if (literal_prefix(s + i, &prefix, &raw, cplusplus)) {
            char quote = s[i + prefix];
            i += prefix + 1;
            t.kind = quote == '"' ? TOK_STRING : TOK_CHAR;
            if (raw && cplusplus) {
                size_t delimiter = offsets[i - 1] + 1, length, cursor = delimiter;
                int closed = 0;
                while (source[cursor] && source[cursor] != '(' && cursor - delimiter <= 16) {
                    if (isspace((unsigned char)source[cursor]) || source[cursor] == ')' || source[cursor] == '\\') break;
                    cursor++;
                }
                length = cursor - delimiter;
                if (source[cursor] != '(' || length > 16) {
                    error_at(t.loc, "invalid raw string delimiter"); return 0;
                }
                cursor++;
                while (source[cursor]) {
                    if (source[cursor] == ')' && !strncmp(source + cursor + 1, source + delimiter, length) &&
                        source[cursor + 1 + length] == '"') {
                        cursor += length + 2;
                        closed = 1;
                        break;
                    }
                    cursor++;
                }
                if (!closed) { error_at(t.loc, "unterminated raw string"); return 0; }
                {
                    Buffer literal = {0};
                    size_t suffix_start;
                    buffer_n(&literal, s + start, i - start);
                    buffer_n(&literal, source + delimiter, cursor - delimiter);
                    while (s[i] && offsets[i] < cursor) i++;
                    suffix_start = i;
                    if (ident_start((unsigned char)s[i])) while (ident_part((unsigned char)s[i])) i++;
                    buffer_n(&literal, s + suffix_start, i - suffix_start);
                    raw_text = copy_string(a, literal.data);
                    buffer_free(&literal);
                }
            } else {
                while (s[i] && s[i] != quote && s[i] != '\n') {
                    if (s[i] == '\\' && s[i + 1]) i++;
                    i++;
                }
                if (s[i] != quote) { error_at(t.loc, "unterminated literal"); return 0; }
                i++;
            }
            /* A user-defined suffix is part of the preprocessing token. */
            if (cplusplus && ident_start((unsigned char)s[i])) while (ident_part((unsigned char)s[i])) i++;
        } else if (ident_start((unsigned char)s[i])) {
            t.kind = TOK_IDENT;
            do { i++; } while (ident_part((unsigned char)s[i]));
            if (cplusplus) {
                static const char *const operators[] = {"and", "or", "not", "bitand", "bitor", "xor", "compl",
                                                       "not_eq", "and_eq", "or_eq", "xor_eq", NULL};
                size_t p;
                for (p = 0; operators[p]; p++)
                    if (strlen(operators[p]) == i - start && !strncmp(s + start, operators[p], i - start)) t.kind = TOK_PUNCT;
            }
        } else if (isdigit((unsigned char)s[i]) || (s[i] == '.' && isdigit((unsigned char)s[i + 1]))) {
            t.kind = TOK_NUMBER;
            do {
                char c = s[i++];
                if ((c == 'e' || c == 'E' || c == 'p' || c == 'P') &&
                    (s[i] == '+' || s[i] == '-')) i++;
                if (cplusplus && s[i] == '\'' && ident_part((unsigned char)s[i + 1])) i++;
            } while (ident_part((unsigned char)s[i]) || s[i] == '.');
        } else {
            static const char *const punct[] = {
                "%:%:", ">>=", "<<=", "...", "->*", "##", "::", ".*", "->",
                "++", "--", "<<", ">>", "<=", ">=", "==", "!=", "&&", "||",
                "*=", "/=", "%=", "+=", "-=", "&=", "^=", "|=",
                "<:", ":>", "<%", "%>", "%:", NULL
            };
            size_t p;
            if (s[i] == '\\' && (s[i + 1] == 'u' || s[i + 1] == 'U')) {
                error_at(t.loc, "universal-character names in identifiers are not supported yet");
                return 0;
            }
            t.kind = TOK_PUNCT;
            for (p = 0; punct[p]; p++) {
                size_t width = strlen(punct[p]);
                if (cplusplus && !strcmp(punct[p], "<:") && !strncmp(s + i, "<::", 3) &&
                    s[i + 3] != ':' && s[i + 3] != '>') continue;
                if (!strncmp(s + i, punct[p], width)) { i += width; break; }
            }
            if (!punct[p]) i++;
        }
        t.text = raw_text ? raw_text : copy_n(a, s + start, i - start);
        tokens_push(out, t);
        space = 0;
    }
    return 1;
}

static void quoted_file(FILE *f, const char *file)
{
    fputc('"', f);
    while (*file) {
        if (*file == '"' || *file == '\\') fputc('\\', f);
        fputc(*file++, f);
    }
    fputc('"', f);
}

void emit_tokens(FILE *f, const Tokens *tokens, int markers)
{
    size_t i;
    const char *file = NULL;
    unsigned line = 0;
    int at_start = 1;
    for (i = 0; i < tokens->len; i++) {
        Token t = tokens->data[i];
        if (t.kind == TOK_EMPTY) continue;
        if (t.kind == TOK_NEWLINE) {
            fputc('\n', f);
            at_start = 1;
            line++;
            continue;
        }
        if (markers && (!file || strcmp(file, t.loc.file) || line != t.loc.line)) {
            if (!at_start) fputc('\n', f);
            fprintf(f, "# %u ", t.loc.line);
            quoted_file(f, t.loc.file);
            fputc('\n', f);
            file = t.loc.file;
            line = t.loc.line;
            at_start = 1;
        }
        if (!at_start) fputc(' ', f);
        fputs(t.text, f);
        at_start = 0;
    }
    if (!at_start) fputc('\n', f);
}
