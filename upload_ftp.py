#!/usr/bin/env python3
"""Kopiert den Inhalt von out/ per FTP auf die SD-Karte der Switch.

Aufruf:  python upload_ftp.py <IP> [Port] [Benutzer] [Passwort]
Beispiel: python upload_ftp.py 192.168.2.50 5000
Ohne Benutzer wird anonym angemeldet (Standard bei ftpd).
Die Logdatei vom Switch holen:  python upload_ftp.py <IP> [Port] --log
"""
import ftplib
import pathlib
import sys

ROOT = pathlib.Path(__file__).resolve().parent
OUT = ROOT / "out"


def connect(host, port, user, pw):
    ftp = ftplib.FTP()
    ftp.connect(host, port, timeout=15)
    ftp.login(user, pw) if user else ftp.login()
    return ftp


def ensure_dir(ftp, path):
    cur = ""
    for part in path.strip("/").split("/"):
        cur += "/" + part
        try:
            ftp.mkd(cur)
        except ftplib.error_perm:
            pass  # existiert schon


def upload(ftp):
    files = [p for p in OUT.rglob("*") if p.is_file()]
    if not files:
        sys.exit("out/ ist leer - zuerst bauen (build.bat).")
    for f in files:
        rel = f.relative_to(OUT).as_posix()
        ensure_dir(ftp, "/" + rel.rsplit("/", 1)[0])
        with open(f, "rb") as fh:
            ftp.storbinary(f"STOR /{rel}", fh)
        print(f"  /{rel}  ({f.stat().st_size} Bytes)")


def fetch_log(ftp):
    dst = ROOT / "switch-log.txt"
    with open(dst, "wb") as fh:
        ftp.retrbinary("RETR /config/sys-airplay/log.txt", fh.write)
    print(f"Log gespeichert: {dst}")


def main():
    args = [a for a in sys.argv[1:] if a != "--log"]
    if not args:
        sys.exit(__doc__)
    host = args[0]
    port = int(args[1]) if len(args) > 1 else 5000
    user = args[2] if len(args) > 2 else ""
    pw = args[3] if len(args) > 3 else ""
    ftp = connect(host, port, user, pw)
    try:
        if "--log" in sys.argv:
            fetch_log(ftp)
        else:
            print(f"Lade nach {host}:{port} hoch ...")
            upload(ftp)
            print("Fertig. Switch neu starten.")
    finally:
        try:
            ftp.quit()
        except Exception:  # noqa: BLE001
            ftp.close()


if __name__ == "__main__":
    main()
