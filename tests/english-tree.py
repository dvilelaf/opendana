#!/usr/bin/env python3
"""Level 1: the public tree is in English.

Fails if Spanish words come back into src/ and include/ (text) or into the
names of public files (src, include, mods, tests, tools, packaging, schema,
content). The word list is short and deliberate; it is not a general detector.
Positive control: --self-test feeds known Spanish samples and requires hits.
"""
import os
import re
import sys
from pathlib import Path

TEXT_WORDS = [
    "ejemplo", "jugador", "rejilla", "fuera de", "falta el", "invalido",
    "sin fuegos", "solido", "posicion de", "celda invalida", "se resuelve",
    "empieza dentro", "raton", "llama",
]
NAME_WORDS = [
    "ejemplo", "jugador", "rejilla", "invalido", "raton",
    # C6.85: words of the test file names that were Spanish
    "nivel", "unitario", "producto", "castillo", "mundo", "mapa", "ruta", "ganadora",
    "llamas", "escena", "fase", "fondo", "paso", "pausa", "pistas", "varita", "sonido",
    "restos", "flujo", "carga", "ronda", "cierre", "combate", "bordes", "caida",
    "imagen", "aparece", "sesion", "propietario", "fin-de", "fin_de",
]
TEXT_DIRS = ("src/", "include/")
NAME_DIRS = ("src/", "include/", "mods/", "tests/", "tools/", "packaging/",
             "schema/", "content/")
SELF = "tests/english-tree.py"
# Not published: the imported package, and the reference recordings that a lab tree puts under
# tests/data (the public repository has none; their names are the lab's).
SKIP = ("content/base/", "tests/data/", "tests/cinematic-reference/")
# Spanish ids that comments and printed labels used to contain
SPANISH_IDS = [
    "interfaz-v2-pantallas",
    "enemigos-integracion",
    "audio-jefe-reintento",
    "arena-presentacion",
    "varita-editor-hud",
    "castillo-aparece",
    "entrada-password",
    "final-transicion",
    "fin-de-mundo-mod",
    "plan-dos-tiempos",
    "aaaa-mm-dd-hh-mm",
    "review-fidelidad",
    "split-excepcion",
    "fin-de-mundo-r2",
    "objetos-rejilla",
    "aaaa-mm-dd-hhmm",
    "aaaammdd-hhmmss",
    "acta-cierre-h10",
    "arquitectura-v1",
    "combate-bordes",
    "ppu-derivacion",
    "limpieza-resto",
    "estimacion-e13",
    "estimacion-e15",
    "ruta-ganadora",
    "tramos-ataque",
    "bug-reintento",
    "mapa-estatico",
    "estimacion-e6",
    "estimacion-e7",
    "fin-de-mundo",
    "restos-flujo",
    "llamas-rutas",
    "rutas-r0-r10",
    "piedra-borde",
    "bug-flotando",
    "limpieza-dev",
    "imagen-caida",
    "informe-bugs",
    "mapa-mundos",
    "carga-ronda",
    "arena-flujo",
    "escena-mapa",
    "sonido-mapa",
    "fase-fondo",
    "dos-rondas",
    "bug-muerte",
    "audio-jefe",
    "aaaa-mm-dd",
    "cierre-c3",
    "mapa-paso",
    "mapa-fase",
    "pausa-hud",
    "espera-1b",
    "cierre-c2",
    "mapa-ram",
    "hud-mapa",
    "acta-v1",
    "acta-v2",
]
ID_DIRS = ("src/", "include/", "tests/", "tools/", "packaging/", "docs/MODDING.md", "docs/RELEASE.md", "README.md",
          "CONTRIBUTING.md")
ID_SKIP = ("tests/english-tree.py", "tools/dev/neutral-rename.sh", "tests/public-tests-contract.py",
           "tools/audit/public-allowlist.txt")
ID_RE = re.compile(r"(?<![A-Za-z])(" + "|".join(map(re.escape, (i.upper() for i in SPANISH_IDS))) + r")(?![A-Za-z])")


def find_ids(text):
    return [m.group(1) for m in ID_RE.finditer(text)]


def find(text, words):
    low = text.lower()
    return [w for w in words if re.search(r"(?<![a-z])" + re.escape(w), low)]


def self_test():
    ok = (find('"Falta el jugador"', TEXT_WORDS)
          and find("mods/ejemplo", NAME_WORDS)
          and find("tests/test_castillo_aparece.c", NAME_WORDS)
          and find("tests/data/fin-de-mundo-r2-input.txt", NAME_WORDS)
          and find("tests/nivel2-producto.sh", NAME_WORDS)
          and not find("tests/test_castle_appears.c", NAME_WORDS)
          and find("/* Posicion de objeto fuera de la rejilla */", TEXT_WORDS)
          and not find("Missing player start, example mod", TEXT_WORDS)
          and find_ids("/* " + "fin-de-mundo".upper() + ": the cinematic */")
          and find_ids("printf(\"- " + "castillo-aparece".upper() + ": x\n\");")
          and not find_ids("/* END-OF-WORLD (lab note c3/END-OF-WORLD) */"))
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
    # A lab tree is assembled over the public checkout; judge the public one (OD_PUBLIC_TREE).
    root = Path(os.environ.get("OD_PUBLIC_TREE") or ".")
    os.chdir(root)
    files = sorted(str(p) for p in Path(".").rglob("*")
                   if p.is_file() and str(p).startswith(NAME_DIRS + ID_DIRS)
                   and not str(p).startswith(SKIP))
    bad = []
    for f in files:
        if f == SELF:
            continue
        if f.startswith(NAME_DIRS):
            for w in find(f, NAME_WORDS):
                bad.append(f"{f}: file name has '{w}'")
        if f.startswith(ID_DIRS) and f not in ID_SKIP and Path(f).is_file():
            try:
                body = Path(f).read_text(encoding="utf-8")
            except (UnicodeDecodeError, OSError):
                body = ""
            for n, line in enumerate(body.splitlines(), 1):
                for w in find_ids(line):
                    bad.append(f"{f}:{n}: Spanish identifier '{w}'")
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
