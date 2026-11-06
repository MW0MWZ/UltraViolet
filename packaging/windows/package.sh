#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-or-later
# Copyright (C) 2026 Andy Taylor
#
# In an MSYS2 UCRT64 shell, from the source folder, with the README's Windows packages and zip:
# builds and makes out/UltraViolet-VERSION-windows-x86_64.zip, with the DLLs the program loads
# and the licences of everything in it.
set -e
b=build-release
cmake -S . -B $b -G Ninja -DCMAKE_BUILD_TYPE=Release \
      -DCMAKE_EXE_LINKER_FLAGS='-static-libstdc++ -static-libgcc' 2>&1 | tee configure.log
if grep -qi 'not found' configure.log; then echo 'a library was not found'; exit 1; fi
cmake --build $b
ctest --test-dir $b --output-on-failure
v=$(sed -n 's/^project(UltraViolet VERSION \([0-9.]*\).*/\1/p' CMakeLists.txt)
top=UltraViolet-$v-windows-x86_64
s=stage/$top
rm -rf stage && mkdir -p $s/licenses out
strip -o $s/ultraviolet.exe $b/ultraviolet.exe
cp README.md $s/
cp LICENSE $s/LICENSE.txt
cp third_party/imgui/LICENSE.txt $s/licenses/imgui.txt
# The DLLs (all of them, through their own imports) and the packages they come from.
for dll in $(ldd $b/ultraviolet.exe | awk '$3 ~ /^\/ucrt64\// {print $3}' | sort -u); do
  cp $dll $s/
  pacman -Qqo $dll
done | sort -u | while read -r pkg; do
  n=0
  for f in $(pacman -Qlq $pkg | grep '/share/licenses/.' | grep -v '/$'); do
    cp $f "$s/licenses/${pkg#mingw-w64-ucrt-x86_64-}-$(basename $f)" && n=1
  done
  [ $n = 1 ] || echo "warning: no licence file in $pkg"
done
ls $s $s/licenses
(cd stage && zip -9 -r ../out/$top.zip $top)
