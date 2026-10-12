#ifndef BCXX_H
#define BCXX_H

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

#define BCXX_VERSION "0.1.0"
#define BCXX_TARGET "x86_64-pc-linux-gnu"

/* All translation-unit allocations belong to this arena. */
typedef struct Allocation Allocation;
typedef struct { Allocation *head; } Arena;
void *allocate(Arena *, size_t);
char *copy_n(Arena *, const char *, size_t);
char *copy_string(Arena *, const char *);
void arena_destroy(Arena *);

typedef struct { char *data; size_t len, cap; } Buffer;
void buffer_char(Buffer *, char);
void buffer_text(Buffer *, const char *);
void buffer_n(Buffer *, const char *, size_t);
void buffer_free(Buffer *);

typedef struct { const char **data; size_t len, cap; } Strings;
void strings_push(Strings *, const char *);
void strings_free(Strings *);
int strings_contains(const Strings *, const char *);
char *read_file(Arena *, const char *, size_t *);
char *path_join(Arena *, const char *, const char *);
char *path_directory(Arena *, const char *);
char *replace_suffix(Arena *, const char *, const char *, int);

typedef struct { const char *file; unsigned line, column; } Location;
void error_at(Location, const char *, ...);
void warning_at(Location, const char *, ...);
void fatal(const char *, ...);
void diagnostic_cleanup(void (*)(void));

typedef enum { TOK_IDENT, TOK_NUMBER, TOK_STRING, TOK_CHAR, TOK_PUNCT,
               TOK_NEWLINE, TOK_EMPTY, TOK_PASTE } TokenKind;
typedef struct Hide Hide;
struct Hide { const char *name; Hide *next; };
typedef struct {
    TokenKind kind;
    const char *text;
    Location loc;
    Hide *hide;
    unsigned space;
} Token;
typedef struct { Token *data; size_t len, cap; } Tokens;
void tokens_push(Tokens *, Token);
void tokens_append(Tokens *, const Tokens *);
void tokens_free(Tokens *);
int token_is(Token, const char *);
int lex(Arena *, const char *, const char *, int, Tokens *);
void emit_tokens(FILE *, const Tokens *, int);

typedef struct Macro Macro;
struct Macro {
    const char *name;
    unsigned function, variadic, builtin;
    Strings parameters;
    Tokens body;
    Macro *next;
    Macro *hash_next;
};
typedef struct { const char *path, *key; unsigned system; } Dependency;
typedef struct {
    Arena arena;
    Macro *macros;
    Macro *macro_table[4096];
    Strings includes, quote_includes, system_includes, after_includes;
    Strings once_files;
    Dependency *dependencies;
    size_t dependency_count, dependency_cap;
    const char *base_file;
    const char *physical_file;
    const char *date, *time;
    unsigned counter, errors, include_depth;
    int cplusplus, no_standard_includes, undef_builtins;
    int warnings, warnings_as_errors;
} Preprocessor;
void pp_init(Preprocessor *, int, int);
void pp_destroy(Preprocessor *);
Macro *pp_macro(Preprocessor *, const char *);
int pp_define(Preprocessor *, const Tokens *, Location);
void pp_undef(Preprocessor *, const char *);
int pp_command_define(Preprocessor *, const char *);
int pp_expand(Preprocessor *, const Tokens *, Tokens *, unsigned);
int pp_expression(Preprocessor *, const Tokens *, int *);
int pp_file(Preprocessor *, const char *, int, int, Tokens *);
int pp_forced_include(Preprocessor *, const char *, Tokens *);
void pp_dump_macros(Preprocessor *, FILE *);
void pp_dependencies(Preprocessor *, FILE *, const Strings *, const Strings *, int, int);

#endif
