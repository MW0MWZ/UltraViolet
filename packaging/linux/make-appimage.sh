#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-or-later
# Copyright (C) 2026 Andy Taylor
#
# In the packaging/linux container: builds /src and makes /out/UltraViolet-VERSION-linux-ARCH.AppImage,
# with the licences of everything built in or bundled.
set -e
b=/tmp/build
cmake -S /src -B $b -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX=/usr \
      -DCMAKE_EXE_LINKER_FLAGS='-static-libstdc++ -static-libgcc' 2>&1 | tee /tmp/configure.log
if grep -qi 'not found' /tmp/configure.log; then echo 'a library was not found'; exit 1; fi
cmake --build $b
ctest --test-dir $b --output-on-failure
v=$(sed -n 's/^project(UltraViolet VERSION \([0-9.]*\).*/\1/p' /src/CMakeLists.txt)
d=/tmp/AppDir
rm -rf $d && mkdir -p $d/usr/bin
install -s $b/ultraviolet $d/usr/bin/ultraviolet
cat > /tmp/ultraviolet.desktop <<D
[Desktop Entry]
Type=Application
Name=UltraViolet
Comment=Sun Ultra 5/10 emulator
Exec=ultraviolet
Icon=ultraviolet
Categories=System;Emulator;
Terminal=false
D
# A 256x256 icon (linuxdeploy takes the standard sizes, up to 512).
mkdir -p /tmp/icon && convert /src/graphics/UltraViolet_icon_1024.png -resize 256x256 /tmp/icon/ultraviolet.png
doc=$d/usr/share/doc/ultraviolet
l=$doc/licenses
mkdir -p $l
cp /src/LICENSE /src/README.md $doc/
cp /src/third_party/imgui/LICENSE.txt $l/imgui.txt
cp /usr/local/share/licenses/SDL3/LICENSE.txt $l/SDL3.txt
cd /tmp
linuxdeploy --appdir $d -d /tmp/ultraviolet.desktop -i /tmp/icon/ultraviolet.png
# Each bundled distribution library's copyright file, by package.
for f in $d/usr/lib/*.so*; do
  n=$(basename $f)
  [ $n = libSDL3.so.0 ] && continue
  p=
  for q in /lib/$(uname -m)-linux-gnu/$n /usr/lib/$(uname -m)-linux-gnu/$n; do
    p=$(dpkg -S $q 2>/dev/null | cut -d: -f1) && [ -n "$p" ] && break
  done
  [ -n "$p" ] || { echo "no package for $n"; exit 1; }
  cp /usr/share/doc/$p/copyright $l/$p.txt
done
LDAI_OUTPUT=/out/UltraViolet-$v-linux-$(uname -m).AppImage linuxdeploy --appdir $d --output appimage
ls -l /out
