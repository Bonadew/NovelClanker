#!/usr/bin/env bash
# Builds NovelWriter on Linux and puts a self-contained copy in dist/: start it with dist/NovelWriter
# (or double-click it in a file manager), or copy the whole dist folder to another machine.
#
#   ./build.sh         dependencies (the first run builds Qt and RocksDB from source: can take an hour), build, dist/
#   ./build.sh test    the same, and runs the tests before making dist/
#
# Needs: g++ 11+ (or Clang 14+), Python 3 with venv, and the X11/OpenGL development packages Qt needs. Missing
# system packages are installed with sudo when run from a terminal (you are asked for your password); otherwise
# they are listed. CMake 3.23+, Conan 2 and Ninja are used from PATH when new enough, else installed into a
# private Python environment under build/ (nothing is installed system-wide).
set -euo pipefail
cd "$(dirname "$(readlink -f "$0")")"

run_tests=false
if [[ "${1:-}" == "test" ]]; then
    run_tests=true
fi

fail() {
    echo
    echo "ERROR: $*" >&2
    exit 1
}

version_at_least() { # version_at_least <have> <need>
    [[ "$(printf '%s\n%s\n' "$2" "$1" | sort -V | head -n1)" == "$2" ]]
}

# --- Tools ---------------------------------------------------------------------------------------------------
command -v c++ >/dev/null 2>&1 || fail "No C++ compiler found. Install one, e.g.: sudo apt install build-essential"

tools_venv=build/linux-tools
need_tools=()
if ! command -v cmake >/dev/null 2>&1 ||
    ! version_at_least "$(cmake --version | head -n1 | awk '{print $3}')" 3.23; then
    need_tools+=("cmake>=3.23,<4")
fi
if ! command -v conan >/dev/null 2>&1 || [[ "$(conan --version 2>/dev/null)" != *"version 2."* ]]; then
    need_tools+=("conan>=2.0,<3")
fi
if ! command -v ninja >/dev/null 2>&1; then
    need_tools+=("ninja")
fi
if ((${#need_tools[@]} > 0)); then
    echo "=== Installing build tools into $tools_venv: ${need_tools[*]} ==="
    if [[ ! -x "$tools_venv/bin/pip" ]]; then
        python3 -m venv "$tools_venv" ||
            fail "Could not create a Python virtual environment. Install it, e.g.: sudo apt install python3-venv"
    fi
    "$tools_venv/bin/pip" install --quiet --upgrade pip
    "$tools_venv/bin/pip" install --quiet "${need_tools[@]}"
fi
if [[ -d "$tools_venv/bin" ]]; then
    export PATH="$PWD/$tools_venv/bin:$PATH"
fi

# The executable can't be replaced while it runs.
if pgrep -x NovelWriter >/dev/null 2>&1; then
    fail "NovelWriter is running. Close it and run build.sh again."
fi

# --- Dependencies (Qt, RocksDB), built once and cached by Conan ----------------------------------------------
echo
echo "=== Installing dependencies (the first run builds Qt and RocksDB and can take an hour) ==="
conan profile detect --exist-ok >/dev/null

# Qt needs X11/OpenGL system packages. From a terminal Conan installs missing ones with sudo (asking for your
# password); without a terminal it only checks and lists what is missing.
if [[ -t 0 ]] || [[ "$(id -u)" == "0" ]]; then
    package_mode=install
else
    package_mode=check
fi

# build/linux/Release (presets "conan-linux-release"), so a Windows build in the same checkout is not touched.
# gnu20, not c++20: with GCC, Qt needs the GNU dialect for 128-bit integers and fails to build in strict mode.
conan install . --build=missing \
    -s build_type=Release -s compiler.cppstd=gnu20 \
    -c tools.cmake.cmaketoolchain:generator=Ninja \
    -c "tools.cmake.cmake_layout:build_folder_vars=['settings.os']" \
    -c tools.system.package_manager:mode=$package_mode \
    -c tools.system.package_manager:sudo=True ||
    fail "Installing the dependencies failed. If system packages are missing, install the ones listed above
(e.g. sudo apt install <packages>) or run build.sh from a terminal so it can install them."

# --- Build -------------------------------------------------------------------------------------------------------
echo
echo "=== Configuring ==="
cmake --preset conan-linux-release

echo
echo "=== Building ==="
cmake --build --preset conan-linux-release

if $run_tests; then
    echo
    echo "=== Testing ==="
    ctest --test-dir build/linux/Release --output-on-failure
fi

# --- Self-contained copy: the app, Qt libraries and plugins --------------------------------------------------
echo
echo "=== Creating dist/ ==="
rm -rf dist
cmake --install build/linux/Release --prefix dist
[[ -x dist/NovelWriter ]] || fail "dist/NovelWriter was not created."

echo
echo "Done. Start NovelWriter with dist/NovelWriter (or double-click it in a file manager)."
echo "The dist folder is self-contained: copy all of it to run NovelWriter on another machine."
