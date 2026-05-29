#!/usr/bin/env bash
# One-shot cross-build of vddsound.dll on macOS (Homebrew MinGW-w64 i686).
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
cd "$ROOT"

export PATH="$(brew --prefix)/bin:$PATH"

# 1. Toolchain (idempotent).
brew list mingw-w64 >/dev/null 2>&1 || brew install mingw-w64
command -v cmake >/dev/null 2>&1 || brew install cmake

# 2. Configure + build out-of-tree (clean each run for reproducibility).
rm -rf build
cmake -B build -G "Unix Makefiles" \
      -DCMAKE_TOOLCHAIN_FILE="$ROOT/cmake/toolchain-mingw-i686.cmake" \
      -DCMAKE_BUILD_TYPE=Release
cmake --build build -j"$(sysctl -n hw.ncpu)"

# 3. Verify no stray runtime-DLL dependencies.
echo "=== vddsound.dll DLL dependencies ==="
i686-w64-mingw32-objdump -p build/vddsound.dll | grep "DLL Name"

echo "=== built: $ROOT/build/vddsound.dll ==="
ls -la build/vddsound.dll

# 4. Publish everything the VM needs to the shared folder (./release).
PUBLISH="$ROOT/release"
mkdir -p "$PUBLISH"
cp -f build/vddsound.dll "$PUBLISH/vddsound.dll"
cp -f scripts/install.bat "$PUBLISH/install.bat"

# Surface the build tag so you can confirm what got published.
TAG="$(strings build/vddsound.dll | grep -m1 'build \[' || true)"
echo "=== published to $PUBLISH ==="
echo "    tag: ${TAG:-<none found>}"
ls -la "$PUBLISH"
echo
echo "On the VM: open the shared folder and double-click install.bat (as Administrator), then run your DOS program."
