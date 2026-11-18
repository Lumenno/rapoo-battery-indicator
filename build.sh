#!/usr/bin/env sh
set -eu
cd "$(dirname "$0")"
"${CXX:-x86_64-w64-mingw32-clang++}" -std=c++17 -Wall -Wextra -Werror -O2 \
    -static -municode -mwindows src/main.cpp -lhid -lsetupapi -lshell32 -lgdi32 \
    -o "${1:-Rapoo-VT2-Max-Battery.exe}"
