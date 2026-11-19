#!/bin/sh
# 在 Linux / macOS 上编译并跑核心测试（src/core + src/posix）
set -e
cd "$(dirname "$0")/.."
out="${TMPDIR:-/tmp}/desknote-coretest"
mkdir -p "$out"
c++ -std=c++17 -O1 -Wall -Wextra -Wno-unused-parameter -Isrc/core src/core/*.cpp src/posix/*.cpp tests/coretest.cpp -lpthread -o "$out/coretest"
"$out/coretest" "$out"
