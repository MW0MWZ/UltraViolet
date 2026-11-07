# Copyright (C) 2026 Andy Taylor. Distributed under the same terms as the
# OpenBoot release it builds (see LICENSE).
#
# The OpenBoot build: a cross compiler for 64-bit SPARC Linux and QEMU's
# user-mode emulation to run what it builds (the host Forth and tools).
FROM debian:bookworm
RUN apt-get update && apt-get install -y --no-install-recommends \
      gcc-sparc64-linux-gnu gcc-12-multilib-sparc64-linux-gnu libc6-dev-sparc64-cross libc6-dev-sparc-sparc64-cross binutils-sparc64-linux-gnu \
      qemu-user-static make gcc libc6-dev file xxd ca-certificates \
    && rm -rf /var/lib/apt/lists/*
