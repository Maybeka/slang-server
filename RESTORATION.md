# Prepare the project-maintained slang SDK

The `external/slang` gitlink pins official release **v12.2** at `1130e4d42f799d2915b3253dfa9ff429f8cd3e14` in [the official slang repository](https://github.com/MikePopoloski/slang/commit/1130e4d42f799d2915b3253dfa9ff429f8cd3e14). The project maintains its compiler and editor integration in `restoration/slang.patch`. A fresh checkout needs the official repository and this project patch.

```sh
git submodule update --init
python3 restoration/prepare-main-sdk.py --apply
```

For a previously prepared checkout, keep any local SDK work and use a fresh checkout for migration. The helper verifies the base, the patch hash and all 742 resulting SDK files. CI, local builds and UVM preparse use the same source. The [build guide](docs/start/building.md) covers offline dependencies.

The initial official pin, `31a29606b`, was the common ancestor of the previous editor SDK. The combined patch retains Andrew Nolte's original 16 changes from `ab7f6ae08` through `151a801ce` along with this project's work. These provide source buffer updates and line mappings, parser metadata for ports and virtual interfaces, instance and type overrides, analysis queries and diagnostic recovery. This project now maintains those interfaces and their tests. The original copyrights and licenses remain with their sources.

## Maintain the patch

Use a disposable SDK checkout at the official pin. Apply the current patch, make the SDK changes, stage the complete intended SDK tree including new source and test files, and inspect the staged diff. From the server repository run:

```sh
python3 restoration/record-sdk-patch.py /path/to/staged-sdk
```

The recorder reads the SDK index and regenerates the patch and file hashes together. For an official SDK upgrade, stage the new `external/slang` gitlink first. Verify the regenerated pair by applying it in a clean checkout, then build the server, regenerate the bundled UVM artifact and run the acceptance suites. Commit the gitlink, patch and manifest together.

# Historical recovered Linux source baseline

**Historical provenance only. These commands are not the current main build
recipe. Use [the complete fork recipe](docs/start/building.md), with
`prepare-main-sdk.py`, for integrated main. `restore.py` describes a different
SDK pin and whole-server inventory; do not apply it to integrated main.**

The historical restoration branch restored the Linux fast-CWD candidate onto server
commit `a85abe34a802816f7dfded6e90d173c69ffc3364`. It is **not** a rebase onto
current `main`, and it does not merge or replace `main`.

Product and test source
bytes are unchanged from the recovered archive. The default remains
`fastCwdSnapshotChecks: false`.

## Restore the modified Slang dependency

The submodule records upstream Slang commit
`eb8a331e244f7f10d04c605aff438bc6303ba1ee`. This baseline also requires the
32-file `restoration/slang.patch`; the upstream gitlink alone is insufficient.
The patch includes the snapshot, macro/workspace-source, compilation-context,
and CWD-coordination changes present in the archive.

Only in a checkout of that historical restoration branch:

```sh
git submodule update --init --recursive
python3 restoration/restore.py --apply-slang
```

The helper checks the exact Slang base and refuses to patch an unrelated dirty
checkout. It verifies all 2,881 retained archive source files byte-for-byte after
patching. It can be rerun; the patched Slang submodule appearing dirty is expected.
Do not reset it or run a forced submodule update afterward without reapplying the
patch. Standard build/CI commands that omit this restoration step do not build
the recovered candidate.

## Restore the exact four FetchContent dependencies

The original archive supplied offline copies instead of relying on the versions
selected by a normal CMake configure. Do not assume that default dependency
resolution recreates the tested build. `restoration/offline-dependencies.json`
records the exact public repository URLs, commits and all 1,296 file hashes.
Populate an external directory with the following checkout names and commits:

| Directory | Repository | Commit |
|---|---|---|
| catch2-src | https://github.com/catchorg/Catch2.git | 25319fd3047c6bdcf3c0170e76fa526c77f99ca9 |
| fmt-src | https://github.com/fmtlib/fmt.git | 1be298e1bd68957e4cd352e1f676f00e07dcfb57 |
| boost_regex-src | https://github.com/boostorg/regex.git | 2b3ac0834f31086c6e3c0e0ceb8516e427d5c39d |
| tomlplusplus-src | https://github.com/marzer/tomlplusplus.git | 30172438cee64926dc41fdd9c11fb3ba5b2ba9de |

Use `git clone` followed by `git checkout --detach COMMIT` for each repository.
Normal Git attribute handling is required: the archived tomlplusplus Windows
project/solution files have CRLF line endings. The dependency verifier catches
any differences. Fetching requires network access once; configuring after all
sources are present does not.

```sh
python3 restoration/restore.py --deps-root "$DEPS_ROOT"
bash restoration/configure-offline.sh "$DEPS_ROOT" "$PWD/build-restored" \
  -DCMAKE_CXX_COMPILER=g++-14
cmake --build build-restored --target slang_server server_unittests -j2
```

`DEPS_ROOT` must name the directory containing those four checkouts. The wrapper
uses Release mode, disables mimalloc, and disables FetchContent network activity.
GCC 14 is the historical tested compiler; substituting GCC 13 requires fresh
validation. Standard project prerequisites still apply; no packages or user
installation are changed by the restoration helper.

## Provenance, omissions and limits

- Source archive SHA-256:
  `a0714d8e20284523d3e458dc1bb3d20247a7b9263c9ddcfde874c9ae2b89285e`
- `restoration/manifest.json` binds the patch and inventories by SHA-256 and
  records pinned dependencies and the historical tested binary identity.
- Four pytest-cache files and two historical documents (MACOS-SOURCE-PACKAGE.txt
  and docs/macos-diagnostics.md) were excluded. The original archive remains
  unchanged. No product-source redactions were
  necessary. Raw evidence, logs, binaries, and offline dependency copies are
  not published.
- Upstream documentation images/styles absent from the archive remain inherited
  from the pinned upstream commits; these are outside the archive inventory.
- Existing licenses and notices remain in the server and pinned dependencies.
  The patch preserves the Slang files' existing license headers.
- Restoration verification checks source integrity only. No new compilation,
  runtime tests, platform validation, or performance measurement was performed
  for this publication.
- Historical final validation was Linux x86_64, Debian 13, GCC 14.2.0, Release
  `-O3 -DNDEBUG`, mimalloc off. GCC 13 and ARM64 were not tested. The final reload-reporting change was
  not revalidated on macOS. Historical performance evidence did not demonstrate
  a general responsiveness improvement.

The historical restore branch retained the upstream workflow definitions without
applying the recovery patch. Its ordinary CI was not evidence that the exact
restored source and dependency configuration had been tested. Integrated main
now has an explicit SDK preparation step; current runtime results must still be
reported separately.
