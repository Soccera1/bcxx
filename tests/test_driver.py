#!/usr/bin/env python3
"""End-to-end checks; GCC is a preprocessing oracle, never a bcxx backend."""
import os
from pathlib import Path
import random
import re
import shutil
import subprocess
import sys
import tempfile
import unittest

BCXX = str(Path(sys.argv.pop(1)).resolve())
GCC = shutil.which("gcc")
PP_ORACLE = os.environ.get("BCXX_TEST_PP_ORACLE") or GCC


def run(args, *, source=None, cwd=None, env=None):
    return subprocess.run(args, input=source, text=True, stdout=subprocess.PIPE,
                          stderr=subprocess.PIPE, cwd=cwd, env=env, timeout=30)


def tokens(text):
    # Token comparison tolerates whitespace choices without erasing literal contents.
    return re.findall(r'"(?:\\.|[^"\\])*"|\'(?:\\.|[^\'\\])*\'|'
                      r'[A-Za-z_]\w*|(?:\d|\.\d)[\w.\']*|'
                      r'##|\.\.\.|<<=|>>=|->\*|::|&&|\|\||<<|>>|'
                      r'<=|>=|==|!=|\+\+|--|->|\+=|-=|\*=|/=|[^\s]', text)


class DriverTests(unittest.TestCase):
    def setUp(self):
        self.directory = tempfile.TemporaryDirectory(prefix="bcxx-test-")
        self.cwd = Path(self.directory.name)
        self.env = dict(os.environ)
        for name in ("CPATH", "C_INCLUDE_PATH", "CPLUS_INCLUDE_PATH", "BCXX_TCC"):
            self.env.pop(name, None)

    def tearDown(self):
        self.directory.cleanup()

    def write(self, path, text):
        target = self.cwd / path
        target.parent.mkdir(parents=True, exist_ok=True)
        target.write_text(text)
        return target

    def bcxx(self, *args, source=None, env=None, ok=True):
        p = run([BCXX, *map(str, args)], source=source, cwd=self.cwd, env=env or self.env)
        if ok:
            self.assertEqual(p.returncode, 0, p.stderr)
        else:
            self.assertNotEqual(p.returncode, 0, p.stdout)
        self.assertNotIn("AddressSanitizer", p.stderr)
        self.assertNotIn("runtime error:", p.stderr)
        return p

    def pp(self, source, *args, language="c++", ok=True):
        return self.bcxx("-E", "-P", "-nostdinc", "-x", language, *args, "-", source=source, ok=ok)

    def compare(self, source, *args, language="c++"):
        p = self.pp(source, *args, language=language)
        if not PP_ORACLE:
            self.skipTest("GCC preprocessing oracle unavailable")
        oracle = run([PP_ORACLE, "-E", "-P", "-nostdinc", "-std=c++14" if language == "c++" else "-std=c99",
                      "-x", language, *args, "-"], source=source, cwd=self.cwd, env=self.env)
        self.assertEqual(oracle.returncode, 0, oracle.stderr)
        self.assertEqual(tokens(p.stdout), tokens(oracle.stdout), source)
        return p

    def test_identity_and_help(self):
        self.assertIn("0.1.0", self.bcxx("--version").stdout)
        self.assertIn("translation pending", self.bcxx("--version").stdout)
        self.assertEqual(self.bcxx("-dumpmachine").stdout.strip(), "x86_64-pc-linux-gnu")
        self.assertEqual(self.bcxx("-dumpfullversion").stdout.strip(), "0.1.0")
        self.assertEqual(self.bcxx("-dumpversion").stdout.strip(), "0.1")
        self.assertIn("Usage:", self.bcxx("--help").stdout)
        self.assertIn("bcxx 0.1.0", self.bcxx("-v").stderr)

    def test_search_and_file_queries(self):
        self.assertIn("include:", self.bcxx("-print-search-dirs").stdout)
        path = self.bcxx("-print-file-name=libtcc1.a").stdout.strip()
        self.assertTrue(Path(path).is_file(), path)
        self.assertEqual(self.bcxx("-print-file-name=does-not-exist.bcxx").stdout.strip(), "does-not-exist.bcxx")
        self.assertEqual(self.bcxx("-print-prog-name=tcc").stdout.strip(), "tcc")
        self.assertEqual(self.bcxx("-print-prog-name=cc1plus").stdout.strip(), BCXX)

    def test_missing_input_options_and_arguments(self):
        for args in ((), ("-x",), ("-o",), ("-D",), ("-MF",), ("-I",),
                     ("-fexceptions",), ("-O999",), ("-std=c++17",), ("-x", "assembler"),
                     ("-dM",), ("-E", "--emit-c")):
            with self.subTest(args=args):
                self.bcxx(*args, ok=False)

    def test_macro_capability_identity(self):
        p = self.pp("", "-dM")
        self.assertIn("#define __cplusplus 201402L", p.stdout)
        self.assertIn("#define __BCXX_DRIVER_STAGE__ 1", p.stdout)
        for macro in ("__GNUC__", "__GNUG__", "__SIZEOF_INT128__", "__EXCEPTIONS", "__GXX_RTTI", "__OPTIMIZE__"):
            self.assertNotIn(macro, p.stdout)
        c = self.pp("", "-dM", language="c").stdout
        self.assertIn("__STDC_VERSION__ 199901L", c)
        self.assertNotIn("__cplusplus", c)
        self.assertNotIn("__BCXX__", self.pp("", "-undef", "-dM").stdout)

    def test_cli_macro_order(self):
        self.compare("A B C FN(4)\n", "-DA=1", "-UA", "-DA=9", "-DB", "-DC=", "-DFN(x)=x+2")
        self.pp("", "-Ufoo bar", ok=False)
        self.pp("", "-D9bad=1", ok=False)

    def test_strict_standard_identity(self):
        source = "#ifdef __STRICT_ANSI__\nSTRICT\n#else\nGNU\n#endif\n"
        self.compare(source, "-std=c++14")
        self.compare(source, "-std=gnu++14")
        self.compare(source, "-std=c99", language="c")
        self.compare(source, "-std=gnu99", language="c")

    def test_preprocessor_warnings_and_werror(self):
        warning = "#warning a message\n"
        self.assertIn("warning: a message", self.pp(warning).stderr)
        self.pp(warning, "-Werror", ok=False)
        self.assertEqual(self.pp(warning, "-Werror", "-w").stderr, "")
        redefinition = "#define A 1\n#define A 2\nA\n"
        self.assertIn("redefined", self.pp(redefinition).stderr)
        self.pp(redefinition, "-Werror", ok=False)
        self.assertEqual(self.pp(redefinition, "-w").stderr, "")
        self.pp("#define A 1\n#define A 1\nA\n", "-Werror")

    def test_macro_objects_and_functions(self):
        self.compare("""
            #define F(x,y) x + y
            #define A 3
            #define G F
            #define ONE() 7
            F(A, F(2, 4)) G (ONE(), A)
            F(, ) ONE() F
        """)

    def test_recursion_and_unavailable_tokens(self):
        self.compare("""
            #define x (4 + y)
            #define y (2 * x)
            #define f(a) a * g
            #define g f
            #define z z[0]
            x y f(2)(9) f(f(1)) z
        """)

    def test_standard_rescan_example(self):
        self.compare("""
            #define x 3
            #define f(a) f(x * (a))
            #undef x
            #define x 2
            #define g f
            #define z z[0]
            #define h g(~
            #define m(a) a(w)
            #define w 0,1
            #define t(a) a
            f(y+1) + f(f(z)) % t(t(g)(0) + t)(1);
            g(x+(3,4)-w) | h 5) & m(f)^m(m);
        """)

    def test_stringizing_and_prescan(self):
        self.compare(r'''
            #define S(x) #x
            #define XS(x) S(x)
            #define X 19
            S( X  +/**/ 2 ) XS(X) S("a\\b" + '\n')
            S(a
            b)
            S(a+b) S()
        ''')

    def test_token_paste_and_empty_arguments(self):
        self.compare("""
            #define CAT(a,b) a##b
            #define CAT3(a,b,c) a##b##c
            #define name42 81
            CAT(name,42) CAT(+,=) CAT(,x) CAT(y,) CAT(,)
            CAT3(a,,c) CAT3(,,) CAT3(1,.,5)
            #define HASH # ## #
            #define ID(x) x
            ID(HASH)
        """)

    def test_variadics(self):
        self.compare("""
            #define V(a, ...) a __VA_ARGS__
            #define S(...) #__VA_ARGS__
            #define G(a, ...) call(a, ## __VA_ARGS__)
            #define N(args...) args
            V(1,2,3) V(9) V(8,) S(a, b, c) S()
            G(1) G(1,) G(1, 2,3) N(1,2,3)
        """)

    def test_cross_line_invocation(self):
        self.compare("#define F(x) x + x\nF\n(\n7\n)\n")

    def test_translation_phases(self):
        self.compare("#define AB 5\r\nA\\\r\nB // comment\\\r\ncontinued\r\n"
                     "a/**/b /*multi\nline*/ c\n??=define Z 7\nZ\n", language="c")
        self.compare("%:define X 4\nX <% 2 %> <: 0 :>\n")

    def test_literals(self):
        p = self.pp('L"x" u8"y" u"z" U\'a\' R"tag(a\nb)tag" "hi"_suffix 0b101 1\'000\n')
        for fragment in ('L"x"', 'u8"y"', 'R"tag(a\nb)tag"', '"hi"_suffix', "1'000"):
            self.assertIn(fragment, p.stdout)

    def test_raw_string_phase_reversion(self):
        source = 'R"tag(a\\\nb??=c)tag"_suffix\n'
        self.assertEqual(self.pp(source).stdout.strip(), source.strip())
        self.compare('#define S(x) #x\nS(R"(a\nb)")\n')

    def test_user_defined_literal_suffixes_and_adjacent_macros(self):
        self.compare('#define s WRONG\n#define tag WRONG\n#define _tag WRONG\n"text"s 12_tag R"(raw)"tag "text"_tag\n')
        self.assertIn('"text"s', self.pp('"text"s\n').stdout)

    def test_actual_gcc_cxx14_configure_probe_preprocessing(self):
        fixture = Path(__file__).parent / "fixtures" / "gcc_cxx14_probe.cc"
        source = fixture.read_text()
        p = self.pp(source)
        for feature in ("test_polymorphic_lambdas", "test_binary_literals", "test_generalized_constexpr",
                        "test_lambda_init_capture", "test_digit_separators", "test_return_type_deduction"):
            self.assertIn(feature, p.stdout)
        self.compare(source)

    def test_cpp_named_operators_and_angle_exception(self):
        self.compare('#if 1 and not 0 or false\nPASS\n#endif\n'
                     '#define S(x) #x\nS(and) S(<%)\nT<::N>\n')
        self.pp('#define and 1\n', ok=False)
        self.compare('#define and 3\nand\n', language="c")

    def test_pragma_operator(self):
        self.compare('#define DO(x) _Pragma(#x)\nDO(pack(push, 1))\nstruct S {char c; int i;};\nDO(pack(pop))\n')
        self.write("once.h", '_Pragma("once")\nONCE\n')
        self.compare('#include "once.h"\n#include "once.h"\n')
        self.pp('_Pragma(123)\n', ok=False)
        self.pp('_Pragma("pack" "pop")\n', ok=False)

    def test_conditional_operators(self):
        expressions = (
            "(2+3*4)==14", "-7/3 == -2 && -7%3 == -1", "(~0U >> 63)==1",
            "-1 < 1", "!(-1 < 1U)", "1 ? 1 : 1/0", "0 ? 1/0 : 1",
            "(0 && 1/0)==0", "1 || 1/0", "(1<<8)==256", "(-4 >> 1)==-2",
            "(6&3)==2 && (6|3)==7 && (6^3)==5", "0b101 == 5", "1'024 == 1024",
            "'a'==97 && '\\n'==10 && '\\x41'==65 && '\\101'==65", "true && !false",
            "'\\xff' == -1 && L'\\xff' == 255",
            "(0 ? -1 : 0U) >= 0", "0xFFFFFFFFFFFFFFFF == -1U",
            "(-9223372036854775807L-1)/-1 == (-9223372036854775807L-1)",
        )
        for expr in expressions:
            with self.subTest(expr=expr):
                self.compare(f"#if {expr}\nPASS\n#else\nFAIL\n#endif\n")

    def test_conditional_control(self):
        self.compare("""
            #define X 3
            #if defined X && defined(X) && !defined Y
            A
            #elif 1/0
            FAIL
            #else
            FAIL
            #endif
            #if 0
            #if gibberish / 0
            #error invisible
            #endif
            #include "missing.h"
            #define X 99
            #else
            X
            #endif
            #ifdef Y
            FAIL
            #elif X == 3
            B
            #endif
        """)

    def test_expression_errors(self):
        for expr in ("", "1/0", "1 << 64", "1 << -1", "1.2", "08", "0x", "1uu",
                     "0b2", "18446744073709551616U", "9223372036854775808", "(1", "1 ? 2", "defined()"):
            with self.subTest(expr=expr):
                self.pp(f"#if {expr}\nx\n#endif\n", ok=False)

    def test_conditional_errors(self):
        for source in ("#if 1\n", "#else\n", "#endif\n", "#elif 1\n", "#if 1\n#else\n#else\n#endif\n",
                       "#ifdef\n#endif\n", "#if 1\n#endif junk\n", "#unknown\n"):
            with self.subTest(source=source):
                self.pp(source, ok=False)

    def test_macro_errors(self):
        for source in ("#define F(x,x) x\n", "#define F(x,) x\n", "#define F(x) #y\n",
                       "#define F(x) ##x\n", "#define F(x) x##\n", "#define F(x) x\nF(1,2)\n",
                       "#define F(x) x\nF(\n", "#define C(a,b) a##b\nC(x,+)\n", "#define defined 3\n"):
            with self.subTest(source=source):
                self.pp(source, ok=False)

    def test_lexical_errors(self):
        for source in ("/* open", '"open\n', "'open\n", 'R"longlonglonglonglong(x)longlonglonglonglong"',
                       "a\0b", r"\u00e9"):
            with self.subTest(source=source):
                self.pp(source, ok=False)

    def test_include_search_order_and_next(self):
        self.write("local.h", "LOCAL\n")
        self.write("q/local.h", "QUOTE\n")
        self.write("a/local.h", '#define FIRST 1\n#include_next <local.h>\nUSER\n')
        self.write("s/local.h", "SYSTEM\n")
        self.write("after/local.h", "AFTER\n")
        self.compare('#include "local.h"\n#include <local.h>\n', "-iquote", "q", "-I", "a", "-isystem", "s", "-idirafter", "after")
        self.write("q/only.h", "QUOTEONLY\n")
        self.pp("#include <only.h>\n", "-iquote", "q", ok=False)

    def test_include_macro_and_header_tokens(self):
        self.write("foo.h", "FOUND\n")
        self.compare('#define H "foo.h"\n#include H\n#define foo wrong\n#include <foo.h>\n', "-I", ".")

    def test_guard_once_and_symlink(self):
        self.write("once.h", "#pragma once\nONCE\n")
        self.write("guard.h", "#ifndef GUARD\n#define GUARD\nGUARDED\n#include \"guard.h\"\n#endif\n")
        (self.cwd / "alias.h").symlink_to("once.h")
        self.compare('#include "once.h"\n#include "alias.h"\n#include "guard.h"\n#include "guard.h"\n')

    def test_missing_include_and_depth(self):
        self.write("recursive.h", '#include "recursive.h"\n')
        p = self.pp('#include "no.h"\n', ok=False)
        self.assertIn("header 'no.h' not found", p.stderr)
        p = self.pp('#include "recursive.h"\n', ok=False)
        self.assertIn("include nesting exceeds", p.stderr)

    def test_forced_include_and_imacros(self):
        self.write("pre.h", "#define X 2\nPRE\n")
        self.write("defs.h", "#define X 1\nDISCARD\n")
        self.compare("X __INCLUDE_LEVEL__\n", "-include", "pre.h", "-imacros", "defs.h", "-DX=0")

    def test_forced_include_dependency_order_and_level(self):
        self.write("forced.h", "__INCLUDE_LEVEL__\n")
        self.write("main.c", "int x;\n")
        p = self.bcxx("-E", "-P", "-nostdinc", "-include", "forced.h", "main.c")
        self.assertEqual(tokens(p.stdout)[0], "1")
        p = self.bcxx("-MM", "-MP", "-nostdinc", "-include", "forced.h", "main.c")
        self.assertLess(p.stdout.index("main.c"), p.stdout.index("forced.h"))
        self.assertIn("./forced.h:", p.stdout)
        self.assertEqual(p.stdout.count("main.c"), 1)

    def test_builtins_line_and_file(self):
        self.write("a.h", '__LINE__ __INCLUDE_LEVEL__ __BASE_FILE__\n')
        self.write("main.cc", '#include "a.h"\n#define L __LINE__\n#line 72 "virtual.cc"\nL __LINE__ __FILE__ __COUNTER__ __COUNTER__\n')
        p = self.bcxx("-E", "-P", "-nostdinc", "main.cc")
        self.assertEqual(tokens(p.stdout), ["1", "1", '"main.cc"', "72", "72", '"virtual.cc"', "0", "1"])
        self.compare('#line 40 "fake.cc"\n__LINE__ __FILE__\n#line 10\n__LINE__\n')

    def test_source_markers_and_locations(self):
        self.write("a.h", "int a;\n")
        self.write("main.c", '#include "a.h"\nint main(void) { return 0; }\n')
        p = self.bcxx("-E", "-nostdinc", "main.c")
        self.assertIn('# 1 "./a.h"', p.stdout)
        self.assertIn('# 2 "main.c"', p.stdout)
        self.assertNotIn("# ", self.bcxx("-E", "-P", "-nostdinc", "main.c").stdout)
        p = self.pp('#line 101 "virtual.cc"\n#error failure\n', ok=False)
        self.assertIn("virtual.cc:101:2: error: failure", p.stderr)

    def test_feature_queries(self):
        self.write("here.h", "")
        p = self.pp('#if __has_include("here.h") && !__has_include(<missing>) && !__has_attribute(nope) && !__has_builtin(nope) && !__has_cpp_attribute(nope)\nPASS\n#endif\n')
        self.assertIn("PASS", p.stdout)

    def test_feature_query_uses_physical_directory(self):
        self.write("dir/here.h", "")
        self.write("dir/main.cc", '#line 2 "elsewhere/fake.cc"\n#if __has_include("here.h")\nFOUND\n#endif\n')
        self.assertIn("FOUND", self.bcxx("-E", "-P", "-nostdinc", "dir/main.cc").stdout)

    def test_dependency_user_system_and_phony(self):
        self.write("user.h", '#include <sys.h>\n')
        self.write("sys/sys.h", '#include "child.h"\n')
        self.write("sys/child.h", "")
        self.write("main.c", '#include "user.h"\nint main(void){return 0;}\n')
        p = self.bcxx("-M", "-nostdinc", "-isystem", "sys", "main.c")
        for name in ("main.o:", "main.c", "user.h", "sys/sys.h", "sys/child.h"):
            self.assertIn(name, p.stdout)
        p = self.bcxx("-MM", "-MP", "-nostdinc", "-isystem", "sys", "main.c")
        self.assertNotIn("sys/", p.stdout)
        self.assertIn("./user.h:", p.stdout)
        self.assertEqual(p.stdout.count("main.c"), 1)

    def test_dependency_quoting(self):
        self.write("space #$.h", "")
        self.write("main.c", '#include "space #$.h"\n')
        p = self.bcxx("-MM", "-nostdinc", "-MT", "literal target", "-MQ", "space $#.o", "main.c")
        self.assertIn("literal target space\\ $$\\#.o:", p.stdout)
        self.assertIn("space\\ \\#$$.h", p.stdout)

    def test_depfile_with_preprocess(self):
        self.write("main.c", "int x;\n")
        self.bcxx("-E", "-MD", "-MF", "deps.d", "-o", "out.i", "main.c")
        self.assertIn("int x", (self.cwd / "out.i").read_text())
        self.assertIn("main.o:", (self.cwd / "deps.d").read_text())

    def test_c_compile_link_and_multiple_translation_units(self):
        self.write("a.c", "int answer(void) { return 42; }\n")
        self.write("main.c", '#include <stdio.h>\nint answer(void);\nint main(void) { printf("%d\\n",answer()); return answer()!=42; }\n')
        self.bcxx("-c", "a.c", "-o", "a.o")
        self.bcxx("main.c", "a.o", "-o", "program")
        p = run([str(self.cwd / "program")], cwd=self.cwd)
        self.assertEqual((p.returncode, p.stdout), (0, "42\n"))
        self.bcxx("a.c", "main.c", "-o", "program2")
        self.assertEqual(run([str(self.cwd / "program2")]).returncode, 0)

    def test_c_dependency_compile_and_saved_temps(self):
        self.write("header.h", "#define VALUE 7\n")
        self.write("main.c", '#include "header.h"\nint f(void){return VALUE;}\n')
        self.bcxx("-c", "-MMD", "--save-temps", "main.c", "-o", "result.o")
        self.assertTrue((self.cwd / "result.o").exists())
        self.assertTrue((self.cwd / "main.i").exists())
        self.assertIn("result.o:", (self.cwd / "result.d").read_text())
        self.assertIn("header.h", (self.cwd / "result.d").read_text())

    def test_emit_c_and_no_optimization(self):
        self.write("input.c", "int f(int x) { return (2+3) * x + 0; }\n")
        outputs = []
        for level in ("-O0", "-O1", "-O2", "-O3", "-Os", "-Og", "-Ofast", "-Oz"):
            self.bcxx("--emit-c", level, "input.c", "-o", "out.c")
            outputs.append((self.cwd / "out.c").read_text())
        self.assertTrue(all(s == outputs[0] for s in outputs))
        self.assertIn("( 2 + 3 ) * x + 0", outputs[0])

    def test_backend_argv_and_exit_status(self):
        log = self.cwd / "calls.log"
        script = self.write("fake tcc", '#!/usr/bin/env python3\nimport json, sys\nfrom pathlib import Path\n'
                            f'p=Path({str(log)!r})\n'
                            'with p.open("a") as f: f.write(json.dumps(sys.argv[1:])+"\\n")\n'
                            'if "-c" in sys.argv: sys.exit(23)\n')
        script.chmod(0o755)
        env = dict(self.env, BCXX_TCC=str(script))
        self.write("weird ;$().c", "int x;\n")
        p = self.bcxx("-nostdinc", "-O3", "-c", "weird ;$().c", env=env, ok=False)
        self.assertEqual(p.returncode, 23)
        self.assertNotIn("-O3", log.read_text())
        self.assertIn('"-std=c99"', log.read_text())

    def test_link_library_order(self):
        if not shutil.which("ar"):
            self.skipTest("ar unavailable")
        self.write("answer.c", "int answer(void){return 42;}\n")
        self.write("main.c", "int answer(void); int main(void){return answer()!=42;}\n")
        self.bcxx("-c", "answer.c")
        p = run(["ar", "rcs", "libanswer.a", "answer.o"], cwd=self.cwd, env=self.env)
        self.assertEqual(p.returncode, 0, p.stderr)
        self.bcxx("main.c", "-L", ".", "-l", "answer", "-o", "program")
        self.assertEqual(run([str(self.cwd / "program")]).returncode, 0)

    def test_builtin_argument_prescan_occurs_once(self):
        self.compare('#define F(x) x x\n#define IGNORE(x) 9\nF(__COUNTER__) IGNORE(__COUNTER__) __COUNTER__\n')

    def test_macro_state_is_per_translation_unit(self):
        self.write("a.c", "#define X 5\nint a=X;\n")
        self.write("b.c", "#ifdef X\n#error macro leaked\n#endif\nint b;\n")
        self.bcxx("-c", "a.c", "b.c")
        self.assertTrue((self.cwd / "a.o").exists())
        self.assertTrue((self.cwd / "b.o").exists())

    def test_large_macro_table_and_expansion(self):
        definitions = "".join(f"#define M{i} {i}\n" for i in range(4000))
        uses = " ".join(f"M{i}" for i in range(4000))
        p = self.pp(definitions + uses + "\n")
        self.assertEqual(p.stdout.split(), [str(i) for i in range(4000)])

    def test_cpp_compile_is_honest_and_does_not_invoke_tcc(self):
        self.write("main.cc", "int main(){return 0;}\n")
        env = dict(self.env, BCXX_TCC="/does/not/exist")
        for args in (("-c",), (), ("--emit-c",)):
            with self.subTest(args=args):
                p = self.bcxx(*args, "main.cc", env=env, ok=False)
                self.assertIn("main.cc:1:1: error: C++ translation is not implemented", p.stderr)
                self.assertFalse((self.cwd / "main.o").exists())
        self.bcxx("-E", "-nostdinc", "main.cc", env=env)

    def test_language_selection_and_stdin(self):
        self.write("source.txt", "__cplusplus\n")
        self.assertIn("201402L", self.bcxx("-E", "-P", "-nostdinc", "-x", "c++", "source.txt").stdout)
        self.bcxx("-E", "-", source="int x;", ok=False)
        self.bcxx("-x", "c", "-", "-o", "stdin-program", source="int main(void){return 0;}")
        self.assertEqual(run([str(self.cwd / "stdin-program")]).returncode, 0)

    def test_environment_paths(self):
        self.write("env/header.h", "ENVIRONMENT\n")
        env = dict(self.env, CPATH="env")
        p = self.bcxx("-E", "-P", "-nostdinc", "-x", "c", "-", source="#include <header.h>\n", env=env)
        self.assertIn("ENVIRONMENT", p.stdout)

    def test_no_archive_needed(self):
        # Neither identity nor internal preprocessing needs a backend or a GCC archive.
        env = dict(self.env, BCXX_TCC="/missing/tcc")
        self.bcxx("--version", env=env)
        self.bcxx("-E", "-nostdinc", "-x", "c++", "-", source="TOKEN\n", env=env)

    def test_no_output_overwrites_and_ambiguous_outputs(self):
        self.write("main.c", "int x;\n")
        self.write("other.c", "int y;\n")
        original = (self.cwd / "main.c").read_bytes()
        for args in (("-E", "main.c", "-o", "main.c"), ("--emit-c", "main.c"),
                     ("-c", "main.c", "other.c", "-o", "one.o")):
            self.bcxx(*args, ok=False)
        self.assertEqual((self.cwd / "main.c").read_bytes(), original)

    def test_output_aliases_and_headers_are_protected(self):
        self.write("main.c", '#include "header.h"\nint x;\n')
        self.write("header.h", "#define VALUE 5\n")
        (self.cwd / "alias.c").symlink_to("main.c")
        for args in (("-E", "main.c", "-o", "./main.c"), ("-E", "main.c", "-o", "alias.c"),
                     ("-E", "main.c", "-o", "header.h"), ("-MM", "main.c", "-MF", "main.c"),
                     ("-MM", "main.c", "-MF", "header.h"), ("-c", "main.c", "-o", "header.h")):
            with self.subTest(args=args):
                self.bcxx(*args, ok=False)
        self.assertEqual((self.cwd / "header.h").read_text(), "#define VALUE 5\n")

    def test_random_integer_expressions(self):
        rng = random.Random(4219)
        expressions = []
        for _ in range(150):
            a, b, c = [rng.randrange(-100, 100) for _ in range(3)]
            op = rng.choice(["+", "-", "*", "&", "|", "^", "==", "!=", "<", ">", "&&", "||"])
            expressions.append(f"(({a}) {op} ({b})) + ({c})")
        source = "".join(f"#if {expr}\nT{i}\n#else\nF{i}\n#endif\n" for i, expr in enumerate(expressions))
        self.compare(source)


if __name__ == "__main__":
    unittest.main(verbosity=2)
