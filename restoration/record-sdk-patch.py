"""Record the staged SDK tree as a patch against the server's official gitlink."""

import argparse
import hashlib
import json
import os
import subprocess
import tempfile
from pathlib import Path

HERE = Path(__file__).resolve().parent


def git(repo, *args):
    return subprocess.check_output(["git", "-C", str(repo), *args])


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("checkout", type=Path, help="SDK checkout with the complete patch staged")
    checkout = parser.parse_args().checkout.resolve()
    base = git(HERE.parent, "rev-parse", ":external/slang").decode().strip()
    if git(checkout, "rev-parse", "HEAD").decode().strip() != base:
        raise SystemExit("Stage the intended official SDK gitlink in the server checkout first")
    if git(checkout, "diff", "--name-only") or git(checkout, "ls-files", "--others", "--exclude-standard"):
        raise SystemExit("The SDK checkout contains unstaged files. Stage the patch in a clean checkout.")

    patch = git(
        checkout, "diff", "--cached", "--binary", "--full-index", "--no-renames",
        "--no-ext-diff", "--no-textconv", "--src-prefix=a/", "--dst-prefix=b/", "HEAD",
    )
    paths = os.fsdecode(git(checkout, "ls-files", "-z")).split("\0")
    with tempfile.TemporaryDirectory() as directory:
        git(checkout, "checkout-index", "--all", "--prefix=" + directory + os.sep)
        files = {
            path: hashlib.sha256((Path(directory) / path).read_bytes()).hexdigest()
            for path in paths if path
        }
    manifest = {
        "format": 1,
        "base_commit": base,
        "patch_sha256": hashlib.sha256(patch).hexdigest(),
        "files": files,
    }
    (HERE / "slang.patch").write_bytes(patch)
    (HERE / "main-sdk.json").write_bytes(
        (json.dumps(manifest, indent=2, sort_keys=True) + "\n").encode()
    )
    print(f"Recorded {len(files)} SDK files at {base}")


if __name__ == "__main__":
    main()
