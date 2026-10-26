# UltraViolet

An open-source emulator for Sun **UltraSPARC** workstations, starting with the
**Ultra 5** and **Ultra 10**: the UltraSPARC IIi processor with its on-chip
memory controller and PCI host bridge, the PCI I/O chipset, onboard ATI Rage
graphics and, on the Ultra 10, the UPA slot. The aim is to run the original
OpenBoot PROM and Solaris unmodified, at or above the speed of the real
machine, with a just-in-time compiler for the UltraSPARC IIi. Linux is the
primary host; macOS and Windows builds are planned from the start.

## Status

UltraViolet is at the very beginning. In place so far:

- **Ultra 5/10 machine:** the original OpenBoot PROM (3.31) runs its power-on
  sequence to the `ok` prompt on the serial console (ttya): it probes memory
  and the PCI buses, finds no keyboard or screen and uses ttya, prints its
  banner and answers commands. Modelled so far: the UltraSPARC IIi's PCI bus
  module (Sabre) with its IOMMU and interrupt mapping, the Advanced PCI
  Bridge (Simba), the PCIO EBus with the flash PROM, the M48T59 NVRAM and
  clock, the SuperIO's keyboard and mouse UARTs and floppy controller, and
  the SAB82532 serial ports.
- **UltraSPARC IIi interpreter:** the SPARC V9 integer unit with register
  windows, trap levels and the global register sets, the I- and D-MMU with
  their TLBs and TSB pointers, the internal ASIs, TICK and interrupts, the
  floating-point unit and VIS 1.0, and a disassembler.
- **Foundations:** the 41-bit physical address space, the event scheduler and
  host pacing, logging, guest memory, the x86-64 and AArch64 code emitters for
  the JIT, and user-mode (libslirp) and TAP networking.

Next come the on-board IDE (CMD646), Ethernet (HME) and graphics (ATI Rage
Pro); see [docs/ARCHITECTURE.md](docs/ARCHITECTURE.md).

## Building

UltraViolet needs a C++20 compiler (GCC 11+, Clang 14+), CMake 3.21+ and,
optionally, libslirp for user-mode networking.

```sh
cmake -S . -B build -G Ninja
cmake --build build
ctest --test-dir build --output-on-failure
```

On Debian or Ubuntu the dependencies are:

```sh
sudo apt install build-essential cmake ninja-build pkg-config libslirp-dev
```

## Running

```sh
./build/ultraviolet --prom-dir roms --nvram disks/ultra10.nvram
```

The newest `ultra5_10-obp-*.bin` image in the PROM folder is used. The first
serial port (ttya) is the terminal; Ctrl-] then q quits.

## Firmware and media

UltraViolet will need an OpenBoot PROM image from a real Ultra 5/10, and
Solaris (or another sparc64 operating system) media. Neither is included:
they are not part of this project and must come from your own machines and
media. The `roms/`, `iso/` and `disks/` folders are ignored by git for that
purpose.

## Legal

Copyright (C) 2026 Andy Taylor (MW0MWZ).

UltraViolet is free software; you can redistribute it and/or modify it under
the terms of the GNU General Public License as published by the Free Software
Foundation; either version 2 of the License, or (at your option) any later
version. See [LICENSE](LICENSE).

Sun, Ultra, Solaris and OpenBoot are trademarks of Oracle and/or its
affiliates. SPARC and UltraSPARC are trademarks of SPARC International, Inc.
UltraViolet is not affiliated with or endorsed by either.
