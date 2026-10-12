#!/usr/bin/env python3
"""Laedt die externen Bausteine fuer sys-airplay herunter.

* Apples ALAC-Decoder (Apache 2.0)        -> sysmodule/source/alac/
* libtesla (Overlay-Framework, GPLv2)     -> overlay/libs/libtesla/
* den oeffentlich bekannten AirPort-Express-RSA-Schluessel, den jeder
  AirPlay-1-Empfaenger braucht (aus dem Quelltext von shairport-sync)
  -> sysmodule/source/airport_key.h

Aufruf:  python fetch_deps.py
"""
import base64
import io
import pathlib
import re
import sys
import urllib.request
import zipfile

ROOT = pathlib.Path(__file__).resolve().parent

ALAC_FILES = [
    "ALACAudioTypes.h", "ALACBitUtilities.c", "ALACBitUtilities.h",
    "ALACDecoder.cpp", "ALACDecoder.h", "EndianPortable.c", "EndianPortable.h",
    "ag_dec.c", "aglib.h", "dp_dec.c", "dplib.h", "matrix_dec.c", "matrixlib.h",
]
ALAC_BASES = [
    "https://raw.githubusercontent.com/macosforge/alac/master/codec/",
    "https://raw.githubusercontent.com/macosforge/alac/main/codec/",
]
TESLA_ZIPS = [
    "https://codeload.github.com/WerWolv/libtesla/zip/refs/heads/master",
    "https://codeload.github.com/WerWolv/libtesla/zip/refs/heads/main",
]
KEY_SOURCES = [
    "https://raw.githubusercontent.com/mikebrady/shairport-sync/master/common.c",
    "https://raw.githubusercontent.com/mikebrady/shairport-sync/development/common.c",
    "https://raw.githubusercontent.com/mikebrady/shairport-sync/master/rtsp.c",
]


def fetch(url):
    req = urllib.request.Request(url, headers={"User-Agent": "sys-airplay-fetch"})
    with urllib.request.urlopen(req, timeout=60) as r:
        return r.read()


def fetch_first(urls):
    last = None
    for u in urls:
        try:
            return fetch(u)
        except Exception as e:  # noqa: BLE001
            last = e
    raise RuntimeError(f"Download fehlgeschlagen: {urls[0]} ({last})")


def get_alac():
    dst = ROOT / "sysmodule" / "source" / "alac"
    dst.mkdir(parents=True, exist_ok=True)
    for name in ALAC_FILES:
        data = fetch_first([b + name for b in ALAC_BASES])
        (dst / name).write_bytes(data)
    print(f"  ALAC-Decoder: {len(ALAC_FILES)} Dateien")


def get_tesla():
    dst = ROOT / "overlay" / "libs" / "libtesla"
    data = fetch_first(TESLA_ZIPS)
    with zipfile.ZipFile(io.BytesIO(data)) as z:
        for info in z.infolist():
            parts = info.filename.split("/", 1)
            if len(parts) < 2 or not parts[1] or info.is_dir():
                continue
            out = dst / parts[1]
            out.parent.mkdir(parents=True, exist_ok=True)
            out.write_bytes(z.read(info))
    if not (dst / "include" / "tesla.hpp").exists():
        raise RuntimeError("libtesla: include/tesla.hpp fehlt im Archiv")
    print("  libtesla: ok")


def c_string_literals(src):
    """Alle C-String-Literale der Datei aneinandergehaengt (mit aufgeloesten Escapes)."""
    out = []
    for m in re.finditer(r'"((?:[^"\\\n]|\\.)*)"', src):
        out.append(m.group(1).encode().decode("unicode_escape"))
    return "".join(out)


def der_ints(der):
    """Parst RSAPrivateKey ::= SEQUENCE { version, n, e, d, p, q, dp, dq, qinv }."""
    def read_len(buf, i):
        b = buf[i]
        i += 1
        if b < 0x80:
            return b, i
        n = b & 0x7F
        return int.from_bytes(buf[i:i + n], "big"), i + n

    if der[0] != 0x30:
        raise ValueError("kein SEQUENCE")
    _, i = read_len(der, 1)
    ints = []
    while i < len(der):
        if der[i] != 0x02:
            raise ValueError("kein INTEGER")
        ln, i = read_len(der, i + 1)
        ints.append(int.from_bytes(der[i:i + ln], "big"))
        i += ln
    return ints


def get_key():
    pem = None
    for url in KEY_SOURCES:
        try:
            text = fetch(url).decode("utf-8", "replace")
        except Exception:  # noqa: BLE001
            continue
        lits = c_string_literals(text)
        m = re.search(r"-----BEGIN RSA PRIVATE KEY-----(.*?)-----END RSA PRIVATE KEY-----", lits, re.S)
        if m:
            pem = m.group(1)
            break
    if pem is None:
        raise RuntimeError("AirPort-Schluessel nicht gefunden (shairport-sync-Quelltext geaendert?)")

    der = base64.b64decode("".join(pem.split()))
    _ver, n, e, d, p, q, dp, dq, qi = der_ints(der)[:9]
    # Selbsttest: Ver- und Entschluesseln muss zusammenpassen.
    probe = 0x1234567890ABCDEF
    if pow(pow(probe, e, n), d, n) != probe or p * q != n:
        raise RuntimeError("Schluessel-Selbsttest fehlgeschlagen")
    if n.bit_length() != 2048:
        raise RuntimeError(f"unerwartete Schluessellaenge {n.bit_length()}")

    def arr(name, v):
        b = v.to_bytes((v.bit_length() + 7) // 8, "big")
        rows = [", ".join(f"0x{x:02x}" for x in b[i:i + 16]) for i in range(0, len(b), 16)]
        return f"static const unsigned char {name}[{len(b)}] = {{\n    " + ",\n    ".join(rows) + "\n};\n"

    hdr = ["// Automatisch erzeugt von fetch_deps.py - nicht von Hand bearbeiten.",
           "// Oeffentlich bekannter AirPort-Express-Schluessel (AirPlay 1 / RAOP).",
           "#pragma once", ""]
    for name, v in (("kKeyN", n), ("kKeyE", e), ("kKeyD", d), ("kKeyP", p), ("kKeyQ", q)):
        hdr.append(arr(name, v))
    out = ROOT / "sysmodule" / "source" / "airport_key.h"
    out.write_text("\n".join(hdr), encoding="utf-8")
    print("  AirPlay-Schluessel: ok (Selbsttest bestanden)")


def main():
    print("Lade Abhaengigkeiten ...")
    try:
        get_alac()
        get_tesla()
        get_key()
    except Exception as e:  # noqa: BLE001
        print(f"FEHLER: {e}")
        return 1
    print("Fertig.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
