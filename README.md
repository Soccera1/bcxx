# bcxx

bcxx is an unoptimizing bootstrap compiler project for x86_64 Linux. Its
permanent purpose is bootstrapping GCC, including future releases, and
potentially Clang. General application compilation is outside the project's
scope. The intended first complete compiler target is unmodified GCC 16.2.0
with C and C++ enabled, using TCC as the seed C compiler.

**The driver and internal preprocessor milestone is implemented.** The
implementation is handwritten C99, built with Make and TCC. C inputs are
preprocessed internally, compiled to objects with TCC, and linked with TCC.
C++14 inputs can be preprocessed. Attempting C++ compilation or C++ `--emit-c`
produces a source-located error. There is no C++ parser, semantic analyzer,
translator, bootstrap standard library, or GCC bootstrap proof yet.

Build and verify:

```sh
make                        # build/bcxx; defaults to TCC
make check                  # offline end-to-end suite
make check-self             # rebuild through bcxx -> C -> TCC, then test
make check-gcc              # development: strict GCC warnings
make check-clang            # development: strict Clang warnings
make check-sanitize         # development: Clang ASan + UBSan
```

The default build needs only Make, TCC, and libc development files. Tests need
Python 3. GCC provides an optional preprocessing oracle for differential tests;
it is never used as bcxx's backend. `check-self` uses a temporary PATH exposing
TCC, Python, and `ar`, rebuilds every C99 module, and tests the resulting binary.
Its differential tests compare the self-built binary with the TCC-built seed.
It checks independence from host compiler commands; it is not the isolated GCC
bootstrap proof required by the complete compiler plan.

Examples:

```sh
build/bcxx -E -P -std=c++14 -I include source.cc
build/bcxx -c -MMD -MP seed.c -o seed.o
build/bcxx seed.o other.o -L lib -l seed -o seed-tool
build/bcxx --emit-c seed.c -o seed-expanded.c
build/bcxx --save-temps seed.c -o seed-tool
```

The preprocessor implements object and function macros, argument prescan and
rescanning with hide sets, stringizing, validated token pasting, empty
arguments, standard variadic macros, and GNU named variadics and comma elision.
It handles line splicing, comments, trigraphs, digraphs, C++ named operators,
raw strings, user-defined literal tokens, binary literals, and digit separators.
`#if` uses 64-bit signed/unsigned integer semantics with precedence,
`defined`, conditional expressions, and short-circuit evaluation. Runtime
expressions remain tokens; there is no optimization or runtime constant folding.

Include processing supports quoted and angled headers, macro header names,
`#include_next`, guards, `#pragma once`, `_Pragma`, forced includes, and macro-only
includes. Source locations survive includes and `#line`. Dependencies escape
Make syntax, distinguish system headers (including their children), and support
phony header rules. Compiler queries never pretend bcxx is GCC.

| Options | Behavior |
| --- | --- |
| `-E`, `-P`, `-dM` | Internal preprocessing, suppress line markers, dump macros |
| `-c`, `-o FILE` | C object compilation, select output |
| `-x c`, `-x c++`, `-x none` | Select/reset language for following inputs |
| `-std=c99`, `-std=gnu99`, `-std=c++14`, `-std=gnu++14` | Dialect and `__STRICT_ANSI__` selection; C++ translation remains pending |
| `-I`, `-iquote`, `-isystem`, `-idirafter`, `-nostdinc` | Include search control |
| `-D`, `-U`, `-undef`, `-include`, `-imacros` | Macro and forced-include control |
| `-M`, `-MM`, `-MD`, `-MMD`, `-MF`, `-MT`, `-MQ`, `-MP` | Dependency rules/files and target spelling |
| `-L`, `-l`, `-Wl,...`, `-static`, `-shared`, `-rdynamic`, `-nostdlib`, `-r` | Native link arguments, retaining source/library order |
| `-g`, `-Wall`, `-Wextra`, `-Werror`, `-w`, `-fPIC`, `-fpic` | TCC options; `-Werror` and `-w` also control preprocessor warnings |
| `--emit-c` | Write preprocessed C; use `-o` to avoid replacing a C input |
| `--save-temps`, `-save-temps` | Retain basename `.i` inputs and, for links, `.o` files |
| `-v`, `--version`, `-dumpmachine`, `-dumpversion`, `-dumpfullversion` | Diagnostics and compiler identity |
| `-print-search-dirs`, `-print-file-name=...`, `-print-prog-name=...` | TCC search paths, file lookup in native library directories, bcxx/TCC program identity |

`-O`, `-O0`, `-O1`, `-O2`, `-O3`, `-Os`, `-Og`, `-Ofast`, and `-Oz` are
accepted with no effect and never forwarded to TCC. Compatibility options
`-pipe`, `-fno-strict-aliasing`, and `-fno-omit-frame-pointer` also have no effect
on this unoptimizing pipeline. `-fno-exceptions` and `-fno-rtti` are accepted;
neither feature exists. `-nostdinc++` currently has no effect because no C++
standard-library directories are searched. Other options are rejected.

`BCXX_TCC` selects one executable path; commands use `execvp`, without a shell.
System C include paths are read from that TCC's `-print-search-dirs`, avoiding
host C++ headers. `CPATH`, `C_INCLUDE_PATH`, and `CPLUS_INCLUDE_PATH` are
supported. `TMPDIR` selects temporary storage. Translation-unit allocations and
ordinary temporary files are cleaned up after processing.

`__cplusplus=201402L` selects the preprocessing dialect and is **not a claim
that the pending translator passes C++14 probes**. `__BCXX_DRIVER_STAGE__=1`
identifies this milestone. GNU compiler identity macros, `__SIZEOF_INT128__`,
exception/RTTI macros, and optimization macros are absent. Dynamic file, line,
date/time, counter, and include-level macros are supported. `__has_include`
searches real include paths; attribute and builtin queries conservatively
return zero until capabilities can be verified.

Current boundaries: x86_64 Linux only; no response files, assembly output,
universal-character names in identifiers, or complete Unicode character
evaluation in `#if`. Include/conditional nesting and expression/macro-argument
nesting are limited to 256; expansion is limited to one million replacements
per input token block. Directives inside an unfinished macro invocation are
rejected. One `-o` in a source-only mode and one `-MF` require a single source
input. Backend-specific pragmas pass through to TCC. Unknown directives and
unsupported options fail explicitly. Input files and included headers are
protected against output aliases, including symlinks and hard links.

The suite includes a licensed, revision-pinned copy of GCC's actual C++11/C++14
configure probe bodies, tested for preprocessing only. Its source and checksum
are recorded in [the fixture](tests/fixtures/gcc_cxx14_probe.cc). It is derived
from [GCC's probe source](https://github.com/gcc-mirror/gcc/blob/2b65b1a0df9d32e0182dd81db1899c4d110c83f3/config/ax_cxx_compile_stdcxx.m4).
`tests/update_gcc_fixture.py` reproduces it; normal builds and tests never fetch
sources or hydrate archives.

The remaining compiler work follows the accepted plan: typed C++ semantics and
C lowering; bootstrap headers and C99 runtime; GCC stage 1; and the full
stage-2/stage-3 bootstrap comparison without a host C++ toolchain. Archive/LFS
integration and the official GCC archive/checksum belong to the later bootstrap
milestone. No milestone introduces optimization levels or expands the permanent
scope to general application workloads.
