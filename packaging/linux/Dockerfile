# SPDX-License-Identifier: GPL-2.0-or-later
# Copyright (C) 2026 Andy Taylor
#
# Builds the Linux AppImage on Ubuntu 22.04 (glibc 2.35, so it runs on that and newer), for the
# host's architecture. SDL3 is built from source, as 22.04 does not package it, with every video
# and audio back end it loads at run time; libslirp and glib come from the distribution and are
# bundled by linuxdeploy.
#   docker build -t uv-appimage packaging/linux
#   docker run --rm -v "$PWD:/src:ro" -v "$PWD/out:/out" uv-appimage make-appimage.sh
FROM ubuntu:22.04
ARG DEBIAN_FRONTEND=noninteractive
RUN apt-get update && apt-get install -y --no-install-recommends \
        g++-12 cmake ninja-build pkg-config ca-certificates curl file git xz-utils imagemagick \
        libslirp-dev libglib2.0-dev \
        libx11-dev libxext-dev libxrandr-dev libxcursor-dev libxi-dev libxss-dev libxfixes-dev libxtst-dev \
        libxkbcommon-dev libwayland-dev wayland-protocols libdecor-0-dev libegl1-mesa-dev libgl1-mesa-dev \
        libgles2-mesa-dev libdrm-dev libgbm-dev libpulse-dev libasound2-dev libpipewire-0.3-dev \
        libdbus-1-dev libudev-dev libibus-1.0-dev \
    && rm -rf /var/lib/apt/lists/*
ENV CC=gcc-12 CXX=g++-12
ARG SDL=3.4.18
RUN curl -fsSL https://github.com/libsdl-org/SDL/releases/download/release-$SDL/SDL3-$SDL.tar.gz | tar -xz -C /tmp \
    && cmake -S /tmp/SDL3-$SDL -B /tmp/sdl-build -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX=/usr/local \
         -DSDL_TESTS=OFF -DSDL_EXAMPLES=OFF -DSDL_STATIC=OFF \
    && cmake --build /tmp/sdl-build && cmake --install /tmp/sdl-build && ldconfig \
    && install -D -m 644 /tmp/SDL3-$SDL/LICENSE.txt /usr/local/share/licenses/SDL3/LICENSE.txt \
    && rm -rf /tmp/SDL3-$SDL /tmp/sdl-build
ARG LINUXDEPLOY=1-alpha-20251107-1
RUN curl -fsSL -o /usr/local/bin/linuxdeploy \
        https://github.com/linuxdeploy/linuxdeploy/releases/download/$LINUXDEPLOY/linuxdeploy-$(uname -m).AppImage \
    && chmod +x /usr/local/bin/linuxdeploy
ENV APPIMAGE_EXTRACT_AND_RUN=1
COPY make-appimage.sh /usr/local/bin/
WORKDIR /src
