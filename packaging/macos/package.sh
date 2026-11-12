#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-or-later
# Copyright (C) 2026 Andy Taylor
#
# On macOS, from the source folder, with the README's Homebrew packages: builds and makes
# out/UltraViolet-VERSION-macos-ARCH.dmg, the app with its libraries copied in and signed
# (cmake --install), a link to Applications to drag it to, and the licences of everything in it.
# MACOS_MIN, if set, is the oldest macOS the download is said to run on: it stops if any of the
# app's code needs a newer one.
set -e
b=build-release
cmake -S . -B $b -G Ninja -DCMAKE_BUILD_TYPE=Release 2>&1 | tee configure.log
if grep -qi 'not found' configure.log; then echo 'a library was not found'; exit 1; fi
cmake --build $b
ctest --test-dir $b --output-on-failure
v=$(sed -n 's/^project(UltraViolet VERSION \([0-9.]*\).*/\1/p' CMakeLists.txt)
top=UltraViolet-$v-macos-$(uname -m)
s=stage/$top
rm -rf stage && mkdir -p $s/licenses out
cmake --install $b --prefix "$PWD/$s"  # (an absolute path, as fixup_bundle needs)
app=$s/UltraViolet.app
codesign --verify --deep --strict $app
cp README.md $s/
cp LICENSE $s/LICENSE.txt
cp third_party/imgui/LICENSE.txt $s/licenses/imgui.txt
# The firmware built into the program: OpenBoot (Sun's licence and FirmWorks'), and the
# Rage Pro and Creator3D FCode.
cp firmware/openboot/LICENSE $s/licenses/openboot.txt
cp firmware/rage/LICENSE $s/licenses/rage-fcode.txt
cp firmware/creator3d/LICENSE $s/licenses/creator3d-fcode.txt
# The Homebrew formulas the libraries come from, through their own imports.
brew_libs() { otool -L "$1" | awk 'NR > 1 {print $1}' | grep "^$(brew --prefix)/" || true; }
seen= todo=$(brew_libs $b/UltraViolet.app/Contents/MacOS/UltraViolet)
while [ -n "$todo" ]; do
  set -- $todo; l=$1; shift; todo="$*"
  case " $seen " in *" $l "*) continue;; esac
  seen="$seen $l"; todo="$todo $(brew_libs $l)"
done
for f in $(for l in $seen; do realpath $l; done | sed -n "s|^$(brew --prefix)/Cellar/\([^/]*\)/.*|\1|p" | sort -u); do
  n=0
  for lf in $(brew --prefix $f)/LICENSE* $(brew --prefix $f)/LICENCE* $(brew --prefix $f)/COPYING* $(brew --prefix $f)/COPYRIGHT*; do
    [ -f "$lf" ] && cp "$lf" "$s/licenses/$f-$(basename "$lf")" && n=1
  done
  # Without a file in the keg, the standard text of the formula's licence.
  if [ $n = 0 ]; then
    id=$(brew info --json=v2 $f | python3 -c 'import json, sys; print(json.load(sys.stdin)["formulae"][0]["license"])')
    curl -fsSL -o "$s/licenses/$f-$id.txt" "https://raw.githubusercontent.com/spdx/license-list-data/main/text/$id.txt" ||
      { echo "no licence for $f ($id)"; exit 1; }
  fi
done
ls $s/licenses
# The newest macOS any of the app's code was built for.
need=$(find $app/Contents -type f \( -perm -u+x -o -name '*.dylib' \) | while read -r f; do
  otool -l "$f" | awk '/LC_BUILD_VERSION/ {b = 1} b && /minos/ {print $2; exit}'
done | sort -t. -k1,1n -k2,2n | tail -1)
echo "needs macOS $need"
if [ -n "$MACOS_MIN" ] && [ "$(printf '%s\n%s\n' "$need" "$MACOS_MIN" | sort -t. -k1,1n -k2,2n | tail -1)" != "$MACOS_MIN" ]; then
  echo "needs macOS $need, newer than $MACOS_MIN"; exit 1
fi
ln -s /Applications $s/Applications
hdiutil create -volname UltraViolet -srcfolder $s -ov -format UDZO out/$top.dmg
