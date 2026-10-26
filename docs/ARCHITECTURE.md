# UltraViolet architecture

UltraViolet is an emulator for the Sun **Ultra 5** and **Ultra 10**: the
UltraSPARC IIi processor (SPARC V9 with the VIS extensions), its on-chip
memory controller and PCI host bridge, and the PCI-based I/O of those
machines. It aims to run the original OpenBoot PROM and Solaris unmodified at
**100% of native speed or better**. Linux is the primary host; macOS and
Windows are supported targets.

## Guiding principles

1. **Correct first, fast second.** The interpreter is the reference
   implementation; every performance feature (soft MMU, fastmem, block cache,
   JIT) must produce exactly the same architectural results.
2. **Guest memory stays in guest byte order** (big-endian). PROM, DMA and disk
   images need no conversion; loads and stores do one byte swap, and
   little-endian ASIs and the PCI side swap where the hardware does.
3. **No global state.** A machine owns its bus, memory, CPU and devices, so
   tests can build many small machines and save states stay tractable.
4. **Portable core, platform code at the edges.** The emulation core is plain
   C++20 with no dependencies; SDL3, networking (TAP/slirp) and executable
   memory for the JIT live behind small interfaces.
5. **Documented behaviour is implemented,** whether or not a particular guest
   uses it.

## Source layout

```
src/core/            types, endian helpers, 128-bit arithmetic, logging, guest
                     memory allocation, event scheduler, host pacing, worker pool
src/mem/             physical address space: the 41-bit UltraSPARC IIi PA,
                     a two-level page table of host memory and devices
src/cpu/ultrasparc/  UltraSPARC IIi: interpreter, MMU and ASIs, FPU and VIS,
                     disassembler
src/devices/         PCI framework; Sabre, Simba, RIO EBus; M48T59, 16550,
                     SAB82532, 82077 floppy controller, SuperIO configuration,
                     flash PROM
src/machine/ultra5_10/  Ultra 5/10 assembly: memory banks, PCI topology, EBus map
src/app/             command-line front end (ttya on the terminal)
src/jit/             x86-64 and AArch64 code emitters, executable memory
src/net/             network back ends: user-mode (libslirp) and TAP
tests/               dependency-free unit tests and a small SPARC assembler
docs/                this document
```

Planned: the SDL3 window with Dear ImGui menus, settings and status bar,
and the native macOS menu bar, in `src/app/`.

## The machine as built

* **Memory:** four 256 MiB bank windows at physical 0, 0x1000_0000,
  0x2000_0000 and 0x3000_0000, filled by DIMM pairs in banks 0 and 2 (and 1
  and 3 for double-sided DIMMs). A bank repeats through its window, as the
  memory controller decodes only the address bits a bank needs; an empty
  bank reads zero.
* **PCI:** the Sabre at 0x1FE.0000.0000 (registers), 0x1FE.0100.0000
  (configuration), 0x1FE.0200.0000 (I/O) and 0x1FF.0000.0000 (memory), with
  the byte swap to little-endian PCI. Bus 0 holds the Sabre and the APB's two
  functions; bus A (APB function 1) carries the EBus at device 1, bus B the
  slots.
* **EBus:** BAR0 is the flash (the reset vector, 0x1FF.F000.0000, while BAR0
  is at its power-on 0xF000_0000); BAR1 holds the NVRAM (0x000000), audio
  (0x200000), the SuperIO devices (configuration 0x300398, floppy 0x3023F0,
  parallel 0x3043BC, mouse 0x3062F8, keyboard 0x3083F8), the serial ports
  (0x400000), the clock control (0x504000), DMA (0x700000), power
  (0x724000) and auxio (0x726000).
* **Interrupts:** each device's interrupt number (INO) goes through the
  Sabre's mapping registers to an interrupt vector (INR 0x7C0 + INO) for the
  processor.

## The emulated machine

| Part | Ultra 5/10 |
|---|---|
| CPU | UltraSPARC IIi, 270 to 440 MHz, with L2 (E-cache) and integrated memory controller |
| Host bridge | UltraSPARC IIi PCI bus module ("Sabre"), IOMMU, streaming buffer, interrupt mapping |
| PCI bridges | Advanced PCI Bridge ("Simba"): bus A (onboard devices) and bus B (slots) |
| I/O | PCIO ("RIO"): EBus and 10/100 Ethernet (HME) |
| EBus devices | OpenBoot flash PROM, M48T59 NVRAM and time-of-day clock, serial ports (SAB82532), keyboard and mouse UARTs, floppy, parallel port, CS4231 audio |
| Disk | CMD646 IDE controller |
| Graphics | Onboard ATI Rage (PGX), Ultra 10 UPA slot for Creator/Creator3D |

## CPU: UltraSPARC IIi

### Interpreter (implemented)

* The SPARC V9 integer instruction set: eight register windows with
  CANSAVE/CANRESTORE/CLEANWIN/OTHERWIN and spill, fill and clean-window
  traps; four trap levels with TPC/TNPC/TSTATE/TT, DONE and RETRY; the
  normal, alternate, MMU and interrupt global sets; annulled delay slots;
  BPcc, BPr, MOVcc, MOVr, 64-bit multiply and divide, CASA/CASXA, LDSTUB,
  SWAP and the 128-bit atomic quad load.
* Power-on reset into RED_state at the reset vector (physical
  0x1FF_F000_0000), with TL = MAXTL.
* The UltraSPARC I/II MMU: 64-entry fully associative I- and D-TLBs with 8K,
  64K, 512K and 4M pages, locked and global entries, tag access, synchronous
  fault status and address registers, TSB 8K/64K/direct pointer generation
  (split and unified), demap page and demap context, and the 44-bit virtual
  address range check. Nucleus, primary, secondary and as-if-user ASIs,
  little-endian ASIs, PSTATE.CLE and TTE.IE, non-faulting loads, and the
  physical bypass ASIs.
* Internal registers: LSU control, error enable, AFSR/AFAR, interrupt
  vector receive and dispatch data, E-cache diagnostic access (data and
  tags, aliasing like the real cache) and the L1 cache diagnostic ASIs.
* TICK, TICK_COMPARE and SOFTINT, with interrupt levels 1 to 15 against
  PIL, and interrupt vector traps.
* Floating point: single and double arithmetic, square root, compares into
  fcc0-3, conversions, FMOVcc and FMOVr, run on the host FPU under the
  guest rounding mode with SPARC NaN rules and the IEEE exception and trap
  enable fields. Quad instructions take fp_exception_other
  (unimplemented_FPop) for the operating system to emulate, as on the IIi.
  Block loads and stores, partial stores and short floating-point loads and
  stores.
* VIS 1.0: partitioned add and subtract, multiplies, pack, merge, expand,
  PDIST, partitioned compares, EDGE, ALIGNADDRESS, FALIGNDATA and the
  logical operations. ARRAY8/16/32 are not yet implemented.

### Next

1. **Speed without a JIT.** Soft TLB in front of the MMU, host-mapped guest
   RAM, a pre-decoded block cache invalidated by stores and DMA through
   per-page code flags.
2. **JIT.** Blocks and loop regions translated to x86-64 first, then AArch64
   (including macOS's `MAP_JIT`), with the integer registers of the current
   window cached in host registers, inline memory access through the soft
   TLB, and delay slots and annulled branches handled in the generated code.
   Each JIT feature is checked against the interpreter instruction by
   instruction.

## Timing

Guest time comes from the CPU's cycle counter, which drives every emulated
clock. The pacer (`src/core/pacer.h`) ties it to the host's clock in one of
four modes: Cycle (one cycle per instruction, reproducible), Realtime (guest
time follows the wall clock; the CPU runs as fast as the host allows),
Max (Realtime, with disk and network I/O at the host's speed) and Free (no
pacing, for fixed-length runs).
