#!/bin/sh
# 在 Linux / macOS 上：编译并跑核心测试（src/core + src/posix），再检查跨平台的界面代码（src/ui）能不能编译
set -e
cd "$(dirname "$0")/.."
out="${TMPDIR:-/tmp}/desknote-coretest"
mkdir -p "$out"
c++ -std=c++17 -O1 -Wall -Wextra -Wno-unused-parameter -Isrc/core src/core/*.cpp src/posix/*.cpp tests/coretest.cpp -lpthread -o "$out/coretest"
"$out/coretest" "$out"
# 界面层还没有 macOS / Linux 的外壳（窗口、绘图、系统外壳），先只检查语法和类型
c++ -std=c++17 -fsyntax-only -Wall -Wextra -Wno-unused-parameter -Isrc/core -Isrc/ui src/ui/*.cpp
echo "UI COMPILES"
