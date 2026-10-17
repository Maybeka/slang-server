#!/usr/bin/env python3
"""Apply and verify the restored SDK patch on the integrated main SDK pin.

This verifies SDK bytes only. Legacy restore.py verifies the historical archive,
not the current server tree. No downloads or builds are performed here.
"""

import argparse
import hashlib
import json
import subprocess
from pathlib import Path

HERE = Path(__file__).resolve().parent
SDK = HERE.parent / "external/slang"


def git(*args):
    return subprocess.check_output(["git", "-C", str(SDK), *args], text=True).strip()


def sha(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--apply", action="store_true")
    parser.add_argument(
        "--deps-root",
        type=Path,
        help="Verify the four pinned offline dependency exports",
    )
    args = parser.parse_args()
    inventory = json.loads((HERE / "main-sdk.json").read_text())
    if git("rev-parse", "HEAD") != inventory["base_commit"]:
        raise SystemExit("SDK HEAD differs from main pin; refusing")
    patch = HERE / "slang.patch"
    if sha(patch) != inventory["patch_sha256"]:
        raise SystemExit("SDK patch integrity mismatch")

    def mismatches():
        return [
            p
            for p, digest in inventory["files"].items()
            if not (SDK / p).is_file() or sha(SDK / p) != digest
        ]

    if mismatches() and args.apply:
        if git("status", "--porcelain", "--untracked-files=all"):
            raise SystemExit(
                "SDK differs from verified output and is dirty; refusing to patch"
            )
        git("apply", "--check", str(patch))
        git("apply", str(patch))
    failed = mismatches()
    if failed:
        raise SystemExit("SDK byte verification failed:\n" + "\n".join(failed))
    print(
        f"Verified {len(inventory['files'])} SDK files at {inventory['base_commit']} plus restored patch"
    )
    if args.deps_root:
        dependencies = json.loads((HERE / "offline-dependencies.json").read_text())
        for dependency in dependencies["dependencies"]:
            root = args.deps_root / (dependency["name"] + "-src")
            failed = [
                entry["path"]
                for entry in dependency["files"]
                if not (root / entry["path"]).is_file()
                or sha(root / entry["path"]) != entry["sha256"]
            ]
            if failed:
                raise SystemExit(
                    "Dependency verification failed: "
                    + dependency["name"]
                    + "\n"
                    + "\n".join(failed)
                )
            print(f"Verified {dependency['name']}: {len(dependency['files'])} files")
    print("Integrity check only; no test or performance result is implied.")


if __name__ == "__main__":
    main()
