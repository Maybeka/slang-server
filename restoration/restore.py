#!/usr/bin/env python3
"""Apply the pinned Slang patch and verify archived source bytes; no downloads."""
import argparse
import hashlib
import json
import subprocess
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
HERE = Path(__file__).resolve().parent


def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def verify(root, entries):
    failed = []
    for name, entry in entries.items():
        path = root / name
        if not path.is_file() or digest(path) != entry["sha256"]:
            failed.append(name)
    return failed


def git(*args):
    return subprocess.check_output(["git", "-C", str(ROOT / "external/slang"), *args], text=True).strip()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--apply-slang", action="store_true", help="Apply only to the exact clean pinned Slang checkout; safe to repeat after successful restoration")
    parser.add_argument("--deps-root", type=Path, help="Also verify the four NAME-src dependency checkouts against archived bytes")
    args = parser.parse_args()
    manifest = json.loads((HERE / "manifest.json").read_text())
    for key in ("slang_patch", "source_manifest", "offline_dependencies"):
        entry = manifest[key]
        if digest(ROOT / entry["path"]) != entry["sha256"]:
            raise SystemExit("Restoration metadata hash mismatch: " + entry["path"])
    expected = json.loads((HERE / "source-sha256.json").read_text())
    slang_files = {p.removeprefix("external/slang/"): v for p, v in expected.items() if p.startswith("external/slang/")}
    if args.apply_slang:
        if git("rev-parse", "HEAD") != manifest["submodules"]["external/slang"]:
            raise SystemExit("Slang HEAD differs from the pinned base; refusing to patch")
        if verify(ROOT / "external/slang", slang_files):
            if git("status", "--porcelain", "--untracked-files=all"):
                raise SystemExit("Slang checkout is dirty or has untracked files; refusing to patch")
            patch = str(HERE / "slang.patch")
            git("apply", "--check", patch)
            git("apply", patch)
    failed = verify(ROOT, expected)
    if failed:
        raise SystemExit("Source verification failed (initialize submodules and apply Slang patch first):\n" + "\n".join(failed))
    print(f"Verified {len(expected)} archived source files byte-for-byte. Additional upstream assets and restoration files are outside that inventory.")
    if args.deps_root:
        deps = json.loads((HERE / "offline-dependencies.json").read_text())
        for dep in deps["dependencies"]:
            entries = {f["path"]: f for f in dep["files"]}
            failed = verify(args.deps_root / (dep["name"] + "-src"), entries)
            if failed:
                raise SystemExit("Dependency verification failed: " + dep["name"] + "\n" + "\n".join(failed))
            print(f"Verified {dep['name']}: {len(entries)} archived files")
    print("Integrity verification only; no compiler, runtime, or performance tests were run.")


if __name__ == "__main__":
    main()
