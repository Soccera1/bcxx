#define _POSIX_C_SOURCE 200809L
#include "bcxx.h"
#include <stdlib.h>
#include <string.h>
#include <errno.h>

struct Allocation { void *memory; Allocation *next; };

static void *checked_realloc(void *p, size_t size)
{
    void *result = realloc(p, size ? size : 1);
    if (!result) fatal("out of memory");
    return result;
}

void *allocate(Arena *arena, size_t size)
{
    Allocation *a = checked_realloc(NULL, sizeof(*a));
    a->memory = checked_realloc(NULL, size);
    memset(a->memory, 0, size);
    a->next = arena->head;
    arena->head = a;
    return a->memory;
}

char *copy_n(Arena *arena, const char *s, size_t n)
{
    char *p = allocate(arena, n + 1);
    memcpy(p, s, n);
    return p;
}

char *copy_string(Arena *arena, const char *s)
{
    return copy_n(arena, s, strlen(s));
}

void arena_destroy(Arena *arena)
{
    Allocation *a = arena->head;
    while (a) {
        Allocation *next = a->next;
        free(a->memory);
        free(a);
        a = next;
    }
    arena->head = NULL;
}

void buffer_n(Buffer *b, const char *s, size_t n)
{
    if (b->len + n + 1 > b->cap) {
        b->cap = (b->len + n + 1) * 2;
        b->data = checked_realloc(b->data, b->cap);
    }
    memcpy(b->data + b->len, s, n);
    b->len += n;
    b->data[b->len] = 0;
}

void buffer_char(Buffer *b, char c) { buffer_n(b, &c, 1); }
void buffer_text(Buffer *b, const char *s) { buffer_n(b, s, strlen(s)); }
void buffer_free(Buffer *b) { free(b->data); memset(b, 0, sizeof(*b)); }

void strings_push(Strings *v, const char *s)
{
    if (v->len == v->cap) {
        v->cap = v->cap ? v->cap * 2 : 16;
        v->data = checked_realloc(v->data, v->cap * sizeof(*v->data));
    }
    v->data[v->len++] = s;
}

void strings_free(Strings *v) { free(v->data); memset(v, 0, sizeof(*v)); }

int strings_contains(const Strings *v, const char *s)
{
    size_t i;
    for (i = 0; i < v->len; i++) if (!strcmp(v->data[i], s)) return 1;
    return 0;
}

void tokens_push(Tokens *v, Token t)
{
    if (v->len == v->cap) {
        v->cap = v->cap ? v->cap * 2 : 32;
        v->data = checked_realloc(v->data, v->cap * sizeof(*v->data));
    }
    v->data[v->len++] = t;
}

void tokens_append(Tokens *v, const Tokens *src)
{
    size_t i;
    for (i = 0; i < src->len; i++) tokens_push(v, src->data[i]);
}

void tokens_free(Tokens *v) { free(v->data); memset(v, 0, sizeof(*v)); }
int token_is(Token t, const char *s)
{
    const char *spelling = t.text;
    if (t.kind == TOK_PUNCT || t.kind == TOK_PASTE) {
        if (!strcmp(spelling, "%:")) spelling = "#";
        else if (!strcmp(spelling, "%:%:")) spelling = "##";
        else if (!strcmp(spelling, "<:")) spelling = "[";
        else if (!strcmp(spelling, ":>")) spelling = "]";
        else if (!strcmp(spelling, "<%")) spelling = "{";
        else if (!strcmp(spelling, "%>")) spelling = "}";
        else if (!strcmp(spelling, "and")) spelling = "&&";
        else if (!strcmp(spelling, "or")) spelling = "||";
        else if (!strcmp(spelling, "not")) spelling = "!";
        else if (!strcmp(spelling, "bitand")) spelling = "&";
        else if (!strcmp(spelling, "bitor")) spelling = "|";
        else if (!strcmp(spelling, "xor")) spelling = "^";
        else if (!strcmp(spelling, "compl")) spelling = "~";
        else if (!strcmp(spelling, "not_eq")) spelling = "!=";
        else if (!strcmp(spelling, "and_eq")) spelling = "&=";
        else if (!strcmp(spelling, "or_eq")) spelling = "|=";
        else if (!strcmp(spelling, "xor_eq")) spelling = "^=";
    }
    return !strcmp(spelling, s);
}

char *read_file(Arena *arena, const char *path, size_t *length)
{
    FILE *f = !strcmp(path, "-") ? stdin : fopen(path, "rb");
    Buffer b = {0};
    char chunk[8192];
    size_t n;
    if (!f) return NULL;
    while ((n = fread(chunk, 1, sizeof(chunk), f))) buffer_n(&b, chunk, n);
    if (ferror(f)) {
        if (f != stdin) fclose(f);
        buffer_free(&b);
        return NULL;
    }
    if (f != stdin) fclose(f);
    if (length) *length = b.len;
    {
        char *result = copy_n(arena, b.data ? b.data : "", b.len);
        buffer_free(&b);
        return result;
    }
}

char *path_join(Arena *a, const char *dir, const char *file)
{
    Buffer b = {0};
    char *s;
    if (*file == '/') return copy_string(a, file);
    buffer_text(&b, dir);
    if (b.len && b.data[b.len - 1] != '/') buffer_char(&b, '/');
    buffer_text(&b, file);
    s = copy_string(a, b.data);
    buffer_free(&b);
    return s;
}

char *path_directory(Arena *a, const char *path)
{
    const char *slash = strrchr(path, '/');
    return slash ? copy_n(a, path, slash == path ? 1 : (size_t)(slash - path)) : copy_string(a, ".");
}

char *replace_suffix(Arena *a, const char *path, const char *suffix, int basename)
{
    const char *start = basename && strrchr(path, '/') ? strrchr(path, '/') + 1 : path;
    const char *dot = strrchr(start, '.');
    Buffer b = {0};
    char *s;
    if (dot && strchr(dot, '/')) dot = NULL;
    buffer_n(&b, start, dot ? (size_t)(dot - start) : strlen(start));
    buffer_text(&b, suffix);
    s = copy_string(a, b.data);
    buffer_free(&b);
    return s;
}
