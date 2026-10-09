#!/usr/bin/env python3
"""Level 1: the public tree is in English.

Fails if Spanish words come back into src/ and include/ (text) or into the
names of public files (src, include, mods, tests, tools, packaging, schema,
content). The word list is short and deliberate; it is not a general detector.
Positive control: --self-test feeds known Spanish samples and requires hits.
"""
import re
import sys
from pathlib import Path

TEXT_WORDS = [
    "ejemplo", "jugador", "rejilla", "fuera de", "falta el", "invalido",
    "sin fuegos", "solido", "posicion de", "celda invalida", "se resuelve",
    "empieza dentro", "raton", "llama",
]
NAME_WORDS = ["ejemplo", "jugador", "rejilla", "invalido", "raton"]
TEXT_DIRS = ("src/", "include/")
NAME_DIRS = ("src/", "include/", "mods/", "tests/", "tools/", "packaging/",
             "schema/", "content/")
SELF = "tests/english-tree.py"


def find(text, words):
    low = text.lower()
    return [w for w in words if re.search(r"(?<![a-z])" + re.escape(w), low)]


def self_test():
    ok = (find('"Falta el jugador"', TEXT_WORDS)
          and find("mods/ejemplo", NAME_WORDS)
          and find("/* Posicion de objeto fuera de la rejilla */", TEXT_WORDS)
          and not find("Missing player start, example mod", TEXT_WORDS))
    if not ok:
        print("english-tree: positive control FAILED (the detector is blind)")
        return 1
    print("english-tree: positive control OK")
    return 0


def main():
    if self_test():
        return 1
    # The files on disk, not the git index: a lab tree is assembled over an
    # index of older history.
    files = sorted(str(p) for p in Path(".").rglob("*")
                   if p.is_file() and str(p).startswith(NAME_DIRS)
                   and not str(p).startswith("content/base/"))
    bad = []
    for f in files:
        if f == SELF:
            continue
        if f.startswith(NAME_DIRS):
            for w in find(f, NAME_WORDS):
                bad.append(f"{f}: file name has '{w}'")
        if f.startswith(TEXT_DIRS) and Path(f).is_file():
            try:
                body = Path(f).read_text(encoding="utf-8")
            except (UnicodeDecodeError, OSError):
                continue
            for n, line in enumerate(body.splitlines(), 1):
                for w in find(line, TEXT_WORDS):
                    bad.append(f"{f}:{n}: '{w}'")
    for b in bad:
        print("english-tree: FAIL", b)
    print(f"english-tree: {len(files)} files scanned, {len(bad)} findings")
    return 1 if bad else 0


sys.exit(main())
