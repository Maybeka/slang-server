#!/usr/bin/env python3
"""Populate exact public FetchContent checkouts; never alter an existing checkout.

Run prepare-main-sdk.py --deps-root afterward to verify all source bytes.
Only new directories are cloned / checked out. No settings are changed.
"""

import argparse
import json
from pathlib import Path
import subprocess


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("deps_root", type=Path)
    args = parser.parse_args()
    root = args.deps_root.resolve()
    root.mkdir(parents=True, exist_ok=True)
    manifest = json.loads(
        (Path(__file__).parent / "offline-dependencies.json").read_text()
    )
    for dep in manifest["dependencies"]:
        target = root / (dep["name"] + "-src")
        if target.exists():
            actual = subprocess.check_output(
                ["git", "-C", str(target), "rev-parse", "HEAD"], text=True
            ).strip()
            if actual != dep["commit"]:
                raise SystemExit(
                    f"Refusing to alter {target}: unexpected HEAD {actual}"
                )
        else:
            subprocess.run(
                ["git", "clone", "--no-checkout", dep["repository"], str(target)],
                check=True,
            )
            subprocess.run(
                ["git", "-C", str(target), "checkout", "--detach", dep["commit"]],
                check=True,
            )
        print(f"{dep['name']}: {dep['commit']}")


if __name__ == "__main__":
    main()
