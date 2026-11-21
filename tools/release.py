#!/usr/bin/env python3
"""Create update.json for a new Prism Widgets release, signed with the private release key.

    pip install cryptography
    python tools/release.py --version 1.8.0 --notes "What changed, in a line or two"

Run it from the repository folder after building the new PrismWidgets.exe, then commit and push
PrismWidgets.exe and update.json together. Every running copy of the app finds the update within a
day (or at once with "Check now"). The private key (.release/update-signing-key.pem) must never be
committed or shared: keep a backup somewhere safe, because without it no further update can be signed.
"""
import argparse, base64, hashlib, json, sys

from cryptography.hazmat.primitives import hashes, serialization
from cryptography.hazmat.primitives.asymmetric import ec
from cryptography.hazmat.primitives.asymmetric.utils import decode_dss_signature

DEFAULT_URL = "https://raw.githubusercontent.com/mdadnanafzal/Prism-Widgets/main/PrismWidgets.exe"


def main():
    p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument("--version", required=True, help="the new version, e.g. 1.8.0 (must match APP_VERSION in src/common.h)")
    p.add_argument("--notes", default="", help="what's new, shown in Settings")
    p.add_argument("--notes-file", help="read the notes from a UTF-8 text file instead")
    p.add_argument("--exe", default="PrismWidgets.exe")
    p.add_argument("--key", default=".release/update-signing-key.pem")
    p.add_argument("--url", default=DEFAULT_URL, help="where the app downloads the exe from")
    p.add_argument("--out", default="update.json")
    a = p.parse_args()

    data = open(a.exe, "rb").read()
    if a.version.encode("utf-16-le") not in data:
        sys.exit(f"{a.exe} doesn't contain version {a.version}. Bump APP_VERSION in src/common.h and rebuild first.")
    sha = hashlib.sha256(data).hexdigest()
    key = serialization.load_pem_private_key(open(a.key, "rb").read(), password=None)
    msg = f"prism-update|{a.version}|{sha}|{len(data)}".encode()
    r, s = decode_dss_signature(key.sign(msg, ec.ECDSA(hashes.SHA256())))
    sig = base64.b64encode(r.to_bytes(32, "big") + s.to_bytes(32, "big")).decode()
    notes = open(a.notes_file, encoding="utf-8").read().strip() if a.notes_file else a.notes

    manifest = {"version": a.version, "url": a.url, "size": len(data), "sha256": sha, "notes": notes, "sig": sig}
    with open(a.out, "w", encoding="utf-8", newline="\n") as f:
        json.dump(manifest, f, indent=2, ensure_ascii=False)
        f.write("\n")
    print(f"Wrote {a.out}: version {a.version}, {len(data):,} bytes, sha256 {sha[:16]}...")


if __name__ == "__main__":
    main()
