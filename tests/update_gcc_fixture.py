#!/usr/bin/env python3
"""Reproduce the checked-in configure probe; ordinary tests are offline."""
import argparse
import hashlib
from pathlib import Path
import re
import urllib.request

REVISION = "2b65b1a0df9d32e0182dd81db1899c4d110c83f3"
URL = f"https://raw.githubusercontent.com/gcc-mirror/gcc/{REVISION}/config/ax_cxx_compile_stdcxx.m4"
SHA256 = "dee2d187ca130d536ba1d8022ea0415b978ef545716b1177aca44a724e2a7567"


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--source", type=Path, help="Use a previously downloaded .m4 file")
    args = parser.parse_args()
    if args.source:
        data = args.source.read_bytes()
    else:
        with urllib.request.urlopen(URL, timeout=30) as response:
            data = response.read()
    if hashlib.sha256(data).hexdigest() != SHA256:
        raise SystemExit("GCC probe source checksum mismatch")
    text = data.decode()
    license_text = text.split("# LICENSE\n", 1)[1].split("#serial", 1)[0]
    notice = "\n".join("//" + line[1:] for line in license_text.strip().splitlines())
    bodies = []
    for version in (11, 14):
        match = re.search(r"m4_define\(\[_AX_CXX_COMPILE_STDCXX_testbody_new_in_" + str(version)
                          + r"\], \[\[(.*?)\]\]\)", text, re.S)
        if not match:
            raise SystemExit(f"C++{version} test body not found")
        bodies.append(match.group(1).strip())
    header = (f"// Extracted from GCC config/ax_cxx_compile_stdcxx.m4\n// Revision: {REVISION}\n"
              f"// Source: {URL}\n// Source SHA-256: {SHA256}\n"
              "// Only m4 wrappers were removed. This fixture exercises preprocessing,\n"
              "// not bcxx C++ translation. Redistribution notice follows.\n")
    path = Path(__file__).parent / "fixtures" / "gcc_cxx14_probe.cc"
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(header + notice + "\n\n" + "\n\n".join(bodies) + "\n")


if __name__ == "__main__":
    main()
