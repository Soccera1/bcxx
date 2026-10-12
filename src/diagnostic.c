#include "bcxx.h"
#include <stdarg.h>
#include <stdlib.h>

static void (*cleanup_hook)(void);
void diagnostic_cleanup(void (*hook)(void)) { cleanup_hook = hook; }

static void diagnostic(Location loc, const char *kind, const char *format, va_list ap)
{
    if (loc.file) fprintf(stderr, "%s:%u:%u: %s: ", loc.file, loc.line, loc.column, kind);
    else fprintf(stderr, "bcxx: %s: ", kind);
    vfprintf(stderr, format, ap);
    fputc('\n', stderr);
}

void error_at(Location loc, const char *format, ...)
{
    va_list ap;
    va_start(ap, format);
    diagnostic(loc, "error", format, ap);
    va_end(ap);
}

void warning_at(Location loc, const char *format, ...)
{
    va_list ap;
    va_start(ap, format);
    diagnostic(loc, "warning", format, ap);
    va_end(ap);
}

void fatal(const char *format, ...)
{
    Location loc = {0};
    va_list ap;
    va_start(ap, format);
    diagnostic(loc, "error", format, ap);
    va_end(ap);
    if (cleanup_hook) cleanup_hook();
    exit(1);
}
