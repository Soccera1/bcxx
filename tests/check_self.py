#!/usr/bin/env python3
"""Rebuild all bcxx C99 modules through bcxx -> C -> TCC, then run its suite."""
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile


def main():
    compiler = Path(sys.argv[1]).resolve()
    root = Path(__file__).resolve().parent.parent
    with tempfile.TemporaryDirectory(prefix="bcxx-self-") as name:
        directory = Path(name)
        tools = directory / "tools"
        tools.mkdir()
        # Only seed tools appear on PATH. No gcc, g++, clang, cpp, or C++ runtime.
        for command in ("tcc", "python3", "ar"):
            path = shutil.which(command)
            if path:
                (tools / command).symlink_to(path)
        env = dict(os.environ, PATH=str(tools))
        for key in ("BCXX_TCC", "CPATH", "C_INCLUDE_PATH", "CPLUS_INCLUDE_PATH"):
            env.pop(key, None)
        objects = []
        for source in sorted((root / "src").glob("*.c")):
            target = directory / (source.stem + ".o")
            subprocess.run([str(compiler), "-std=c99", "-Wall", "-Wextra", "-I", str(root / "src"),
                            "-c", str(source), "-o", str(target)], env=env, check=True)
            objects.append(str(target))
        second = directory / "bcxx"
        subprocess.run([str(compiler), *objects, "-o", str(second)], env=env, check=True)
        fixture = root / "tests" / "fixtures" / "gcc_cxx14_probe.cc"
        arguments = ["-E", "-P", "-nostdinc", str(fixture)]
        before = subprocess.check_output([str(compiler), *arguments], env=env)
        after = subprocess.check_output([str(second), *arguments], env=env)
        if before != after:
            raise SystemExit("self-built compiler changed configure-probe preprocessing")
        print("Self-build succeeded using only TCC, libc headers, and C99 sources.", flush=True)
        # Differential checks compare with the TCC-built seed in this run,
        # while normal make check compares with GCC when available.
        env["BCXX_TEST_PP_ORACLE"] = str(compiler)
        subprocess.run([sys.executable, str(root / "tests" / "test_driver.py"), str(second)], env=env, check=True)


if __name__ == "__main__":
    main()
