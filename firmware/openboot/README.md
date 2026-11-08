# OpenBoot 4.23.0

Sun Microsystems' OpenBoot, the IEEE 1275 Open Firmware of Sun's SPARC
machines, from the source Sun released in 2006, being brought to the Sun
Ultra 5 and Ultra 10 as open source system firmware: a replacement for
their OpenBoot 3.x flash PROM, for UltraViolet and for the real machines.

The release is OpenBoot 4.23.0, written for Sun's sun4v machines (the
UltraSPARC T1 systems), and the version here keeps its number. It holds the
whole of OpenBoot's Forth system and Open Firmware (the device tree, the
client interface, FCode, the boot and device packages), sun4u code shared
with the later machines, and the build tools. What the Ultra 5 and 10 need
besides is being written for them: their processor's reset and memory set-up
(UltraSPARC IIi), their PCI bridges (Sabre and Simba), the RIO EBus and its
devices, and the machines' configuration, with UltraViolet's open source
FCode for the Creator3D and Rage Pro built in.

## Status

- **Building on Linux:** done. The release's tools are Solaris SPARC
  programs; here they are built from their sources for SPARC Linux and run
  under QEMU's user-mode emulation, in Docker. Every step of the release's
  build runs: the host Forth and C tools, OpenBoot's 64-bit Forth kernel,
  a platform's Forth image, its reset code, the FCode tokenizer, drop-ins
  and the PROM image. The release's own platform (the Sun Fire T2000)
  builds to a PROM image, as a check of the tool chain.
- **The Ultra 5 and 10:** under way. The platform builds to a 1 MiB flash
  PROM image that starts the machine from reset (the processor, the memory
  controller and the memory's size, the PCI bus numbers; OpenBoot unpacked
  into the top of memory and the MMU turned on) and runs OpenBoot to its
  `ok` prompt, with the device tree as the machine's own firmware builds
  it: the processor (its clock measured), the PCI bus module and its IOMMU,
  the APB's two buses, the EBus and all its devices, IDE, Ethernet, and the
  Rage Pro's and Creator3D's screens (with UltraViolet's open source FCode).
  The console runs on the keyboard and screen or on ttya; the NVRAM holds
  the configuration variables; OpenBoot boots from CD-ROM and from the
  network (DHCP, TFTP). The installation media of Solaris 8, 9 and 10
  boot to their installers, and installed systems of each boot from disk.
  The selftests and diagnostic commands are there, and a power-on
  selftest (POST) at a power-on with `diag-switch?` true.

## Building

With Docker:

```sh
./build.sh                 # the tool chain: the host tools and the kernel
./build.sh ultra5_10       # a system's PROM image (after the tool chain)
./build.sh all             # the tool chain and every system
./build.sh systems         # the systems there are
```

(or `make`, `make ultra5_10`, `make toolchain`). The build image comes
from [host/Dockerfile](host/Dockerfile) (Debian's cross compiler for 64-bit
SPARC Linux, with its 32-bit libraries, and QEMU), and everything built goes
in `build/`.

Each system is described by a file in [systems](systems), in make's
`NAME = value` form: its platform's folder of release files (with the
drop-ins it lists in `dropins.src`), its compile flags, its load file and
reset code, the size of its flash PROM (the image padded with erased bytes
to it) and the file the image is installed as. `ultra5_10` makes
`build/ultra5_10/ultra5_10.bin`, the 1 MiB flash PROM, and installs
`ultra5_10.prom`, which UltraViolet builds in; `t2000`, the release's own
platform (the Sun Fire T2000), checks the tool chain. Another machine is a
platform folder under `obp/arch` and a file in `systems`.

The release's makefiles are for Solaris's make; `build.sh` does what they
do, a stage at a time, and `host/reset.ld` lays out the reset code as the
release's linker map (`obp/arch/sun4u/openboot.map`) does.

The host Forth is the release's own (`obp/os/unix/wrapper.c`), loading its
dictionaries, which are SPARC code. Two things in [host](host) let it run on
Linux: the memory it loads a dictionary into is made executable, and each C
function the dictionary calls is reached through a stub that keeps the Forth
engine's registers (`%g2` to `%g7`) apart from the C library's (`%g7` is
Linux's thread pointer).

## Source and changes

The source is Sun's release of OpenBoot 4.23.0, published with OpenSPARC
in 2006, as kept by the OpenBIOS project
([github.com/openbios/openboot](https://github.com/openbios/openboot)).
It is here as released, but for:

- the prebuilt Solaris programs and libraries that came with it (`bin`,
  `lib`, `tools`, `obp/os/unix`), left out: the build makes them from their
  sources;
- UltraViolet's additions: this README, [LICENSE](LICENSE), [build.sh](build.sh),
  [Makefile](Makefile), [systems](systems) and [host](host).

The Ultra 5 and 10's own code is new: the platform in
`obp/arch/sun4u/ultra5_10` (reset code, console, memory, NVRAM and IDPROM,
processor node, configuration, root node, aliases, probing), the UltraSPARC
I/II/IIi's TLB and translation entries in `obp/cpu/sparc/ultra` (`map.fth`,
`tte.fth`, `tlb.fth`, `tlbasm.fth`, `loadultra.fth`), and the drivers:

| Driver | For |
| --- | --- |
| `obp/dev/sabre` | the IIi's PCI bus module (`/pci@1f,0`) and its IOMMU |
| `obp/dev/pci-bridge/simba` | the Advanced PCI Bridge (`pci@1`, `pci@1,1`) |
| `obp/dev/ebus/rio` | the RIO's EBus and its devices' nodes |
| `obp/dev/ebus/se` | the SAB82532: ttya and ttyb |
| `obp/dev/ebus/su` | the SuperIO's UARTs, and the Sun Type 5 keyboard |
| `obp/dev/ebus/flashprom` | the flash PROM's drop-ins, read as a device |
| `obp/dev/ebus/eeprom` | the M48T59's clock (`watch-clock`, `date`) |
| `obp/dev/ebus/fdthree` | the floppy disk controller and drive |
| `obp/dev/network/hme` | the RIO's Ethernet (Happy Meal) |
| `obp/dev/m64` | the Rage Pro: UltraViolet's FCode (`firmware/rage`) as a drop-in |

OpenBoot 3.x's configuration variables for the machine
(`loadconfig.fth`), its diagnostic commands the release has no form of
(`diag.fth`: `.speed`, `probe-pci`, `probe-pci-slot`, `eject-floppy`), and
a power-on selftest (`post.fth`: OpenBoot 3.31's tests at the level
`diag-level` gives: the NVRAM; the D-TLB's tags and entries; the E-cache's
data and tags; the processor's instructions, tick, soft traps and
interrupts; the MMUs' registers; the D- and I-caches' arrays; the FPU, its
registers, operations and trap; the UPA data bus; memory, through the
E-cache and around it, with block loads and stores; the PCI bus module's
PIO address decoding and byte enables, interrupt maps and states, IOMMU registers and TLB, and PCI registers; the
MMUs' misses, byte order and ASIs; the traps for errors, misaligned
accesses and protection; the RIO's configuration; a PCI master abort; with
`show-post-results`) are part of the platform;
the selftests (`test`, `test-all`) of the Rage Pro, the Ethernet and the
floppy drive are in their drivers.

They are built on the release's PCI, serial, keyboard and network code
(`obp/dev/pci`, `obp/dev/serial`, `obp/dev/usb-devices/kbd`,
`obp/pkg/keyboard`, `obp/dev/network/common`); the IDE controller uses the
release's IDE driver. Some files are derived from the release's sun4v and
other files, with Sun's notices kept and a note of what changed:

| New file | From |
| --- | --- |
| `obp/cpu/sparc/ultra/mmuregs.fth` | `obp/cpu/sparc/ultra4v/mmuregs.fth` |
| `obp/cpu/sparc/ultra/savecpu.fth` | `obp/cpu/sparc/ultra4v/savecpu.fth` |
| `obp/cpu/sparc/ultra/tte-lookup.fth` | `obp/cpu/sparc/ultra4v/tte-lookup.fth` |
| `obp/arch/sun4u/cpustruct.fth` | `obp/arch/sun4v/cpustruct.fth` |
| `obp/arch/sun4u/catchexc.fth` | `obp/arch/sun4v/catchexc.fth` |
| `obp/arch/sun4u/mmumiss.fth` | `obp/arch/sun4v/mmumiss.fth` |
| `obp/arch/sun4u/forthint.fth` | `obp/arch/sun4v/forthint.fth` |
| `obp/arch/sun4u/ultra5_10/boot.fth` | `obp/arch/sun4v/niagara/boot.fth` |
| `obp/arch/sun4u/ultra5_10/virtaddrs.fth` | `obp/arch/sun4v/niagara/virtaddrs.fth` |
| `obp/arch/sun4u/ultra5_10/cleanup.fth` | `obp/arch/sun4v/niagara/cleanup.fth` (unchanged) |
| `obp/arch/sun4u/ultra5_10/console-tty.fth` | `obp/arch/sun4v/console-tty.fth` |
| `obp/dev/ebus/su/io.fth` | `obp/dev/serial/su16550/io.fth` (the UARTs' clock) |

`build.sh` builds the drop-ins a platform's `dropins.src` lists with GNU
make (`host/dropins.mk`), and runs in Docker with the whole `firmware`
folder (for the Rage Pro's FCode).

The release's own files are unchanged, but for:

- `obp/dev/ide/ide.fth`: a wait for the IDE controller polls its status
  every 10 microseconds for the first millisecond before polling every
  millisecond, so that a drive's brief busy time between the blocks of a
  transfer doesn't cost a millisecond a block.
- `obp/fm/kernel/fileio.fth`: an error at the end of a string being
  evaluated shows the string ("Evaluating: boot"); the line was looked for
  from one past its end, and shown empty.

## Credits

- **Sun Microsystems, Inc.:** OpenBoot, and its release as open source.
- **Mitch Bradley (Bradley Forthware, FirmWorks):** the Forth system
  OpenBoot is built on, and Open Firmware's design (IEEE 1275-1994).
- **The OpenBIOS project:** keeping the release available.

## Licence

OpenBoot is under Sun's licence, a BSD-style licence, given in full in
[LICENSE](LICENSE), and UltraViolet's additions are under the same terms.
This folder is not under UltraViolet's GPL.

Sun, Ultra, Solaris and OpenBoot are trademarks of Oracle and/or its
affiliates. SPARC and UltraSPARC are trademarks of SPARC International, Inc.
