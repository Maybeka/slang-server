# Building this fork and running every Linux acceptance suite


`slang-server` requires the following tools in order to build:
- [python 3](https://www.python.org/)
- [CMake](https://cmake.org/) (3.28 or later)
- C++20 compatible compiler. Minimum supported compiler versions:
    - GCC 11
    - clang 17
    - Xcode 16
    - MSVC support is tested only against the most recent update of VS 2022


```bash
# Clone this fork; upstream does not contain its preparation helpers.
git clone --branch main https://github.com/Maybeka/slang-server.git
cd slang-server
git rev-parse HEAD

# Pull direct dependencies (slang, reflect-cpp, and ctre)
git submodule update --init

# Exact public dependencies; existing checkouts at other commits are refused.
export DEPS_ROOT="$PWD/../slang-deps"
export CXX=g++-14  # Select the actual compiler being validated.
python3 restoration/checkout-dependencies.py "$DEPS_ROOT"
python3 restoration/prepare-main-sdk.py --apply --deps-root "$DEPS_ROOT"

# Frozen Python/LSP environment; requires uv and Python 3.12.6.
uv sync --frozen --python 3.12.6

# SDK tests are opt-in: omitting ON produces an incomplete test inventory.
bash restoration/configure-offline.sh "$DEPS_ROOT" "$PWD/build" \
  -G Ninja -DCMAKE_CXX_COMPILER="$CXX" \
  -DSLANG_SERVER_INCLUDE_SDK_TESTS=ON
cmake --build build -j4
ctest --test-dir build --output-on-failure --no-tests=error -j4
uv run --frozen python -m pytest tests/system tests/method_scope \
  --binary "$PWD/build/bin/slang-server" -q
CXX="$CXX" bash tests/transport/run-diagnostic-transport-tests.sh

mkdir -p evidence
uv run --frozen python tests/method_scope/check.py \
  --binary build/bin/slang-server --output evidence/oracle.json
uv run --frozen python tests/method_scope/count.py \
  --binary build/bin/slang_server_counted --output evidence/counts.json
uv run --frozen python tests/method_scope/held_diagnostics.py \
  --binary build/bin/diagnostic_test_server --output evidence/held.json

# Record a plan before measurement; use a new filename for every experiment.
uv run --frozen python tests/method_scope/benchmark.py \
  --binary build/bin/slang-server --plan evidence/plan.json \
  --output evidence/timing.json --seed 20261018 --blocks 3 --plan-only
uv run --frozen python tests/method_scope/benchmark.py \
  --binary build/bin/slang-server --plan evidence/plan.json \
  --output evidence/timing.json
```

Only direct submodules are needed. Leave out `--recursive` to skip reflect-cpp's
optional `vcpkg` submodule; slang-server uses its bundled JSON support with
`REFLECTCPP_USE_VCPKG=OFF` by default.

The binary will be at `build/bin/slang-server`.

The wrapper selects Release, disables mimalloc and FetchContent networking,
and verifies every SDK/dependency file before configuration. Git attributes
must remain intact, including toml++'s CRLF project files. Exact commits and
all 1,296 dependency file hashes are in `restoration/offline-dependencies.json`:

| Dependency | Commit |
|---|---|
| Catch2 | `25319fd3047c6bdcf3c0170e76fa526c77f99ca9` |
| fmt | `1be298e1bd68957e4cd352e1f676f00e07dcfb57` |
| Boost.Regex | `2b3ac0834f31086c6e3c0e0ceb8516e427d5c39d` |
| toml++ | `30172438cee64926dc41fdd9c11fb3ba5b2ba9de` |

The SDK pin is `151a801ceaf6faf23d79a42d210ff67e50eb4660` plus
`restoration/slang.patch` (SHA256
`96550dbfbfd65a0a986c0e6e2769a3f96857b0d70ec6bb728eeb612b41da4bbe`).
Its expected modified state must remain after preparation. Do not reset it.

Keep the tested commit, tool versions, CMake cache, complete logs, raw oracle
and timing JSON, and binary/source hashes with every run. Test-only binaries
`slang_server_counted` and `diagnostic_test_server` are excluded from editor
deployment and timing. GCC 13 and Linux arm64 remain unverified until their own
complete build and acceptance run. The current Linux evidence uses GCC 14.2.0.
The protocol/oracle fixture is representative class/package/multi-include data,
not a production UVM validation or a historical benchmark replica.

See [the fork's LazyVim setup](../editors/lazyvim-fork.md) for editor checks.

### Nix Development Shell

If you use Nix with flakes enabled, the repository provides a development shell for x86-64 Linux, ARM64 Linux, and Apple Silicon macOS. It includes the compiler, CMake, Ninja, Python, `uv`, and build and test dependencies.

After cloning the repository and initializing its submodules, run from the repository root:

```bash
nix develop
python3 restoration/prepare-main-sdk.py --apply
cmake -B build -DCMAKE_BUILD_TYPE=Release -DSLANG_SERVER_INCLUDE_SDK_TESTS=ON
cmake --build build -j8 --target slang_server
```

The shell supplies the tools; the CMake commands build `build/bin/slang-server`. Use a separate build directory if an existing one was configured with a different compiler.
