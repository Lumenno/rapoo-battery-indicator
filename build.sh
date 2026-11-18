#!/usr/bin/env sh
set -eu
cd "$(dirname "$0")"
resource=$(mktemp)
trap 'rm -f "$resource"' EXIT
"${WINDRES:-x86_64-w64-mingw32-windres}" -I src -i src/app.rc -O coff -o "$resource"
"${CXX:-x86_64-w64-mingw32-clang++}" -std=c++17 -Wall -Wextra -Werror -O2 \
    -static -municode -mwindows src/main.cpp "$resource" -lhid -lsetupapi -lshell32 -lgdi32 -ladvapi32 \
    -o "${1:-Rapoo-VT2-Max-Battery.exe}"
