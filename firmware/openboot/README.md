# OpenBoot 4.23.0

Sun Microsystems' OpenBoot, the IEEE 1275 Open Firmware of Sun's SPARC
machines, from the source Sun released in 2006, being brought to the Sun
Ultra 5 and Ultra 10 as open source system firmware: a replacement for
the firmware in their flash PROM, for UltraViolet and for the real
machines.

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
  `ok` prompt, with the device tree as an Ultra 10 has it: the processor (its clock measured), the PCI bus module and its IOMMU,
  the APB's two buses, the EBus and all its devices, IDE, Ethernet, and the
  Rage Pro's and Creator3D's screens (with UltraViolet's open source FCode).
  The console runs on the keyboard and screen or on ttya; the NVRAM holds
  the configuration variables an Ultra 10 has, in its layout (see
  below); OpenBoot boots from CD-ROM and from the
  network (DHCP, TFTP). The installation media of Solaris 8, 9 and 10
  boot to their installers, and installed systems of each boot from disk.
  The selftests and diagnostic commands are there, and a power-on
  selftest (POST) at a power-on with `diag-switch?` true. The POST is a
  program of its own in the flash PROM's second half ([post](post), C and
  assembler), as an Ultra 10 has it: the reset code runs it before
  OpenBoot, it probes the memory itself (from the PROM, as it sets the
  memory controller's column mode), copies itself into memory, reports on
  ttya and returns its result, and a software power-on reset follows;
  OpenBoot then has the result (`show-post-results`, "Power On Self Test
  Failed.  Cause: ...", and no booting by itself after a failure).
  In diagnostic mode it reports as an Ultra 10 does: from the reset code
  ("Hardware Power ON", "Probing keyboard", "Clearing E$ Tags",
  "Probing Memory", "MMUs ON" and the rest), and before probing (the reset
  control, the processor module, the memory's DIMM pairs) and while
  probing ("Probing UPA Slot at 1e,0 ", "Probing /pci@1f,0/pci@1,1 at
  Device 1  " and the name of each node made); the RIO's EBus is named by
  its IDs during the PCI probe and becomes `ebus`, with its devices, after
  it, as on an Ultra 10.

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
to it) and the file the image is installed as; and, if it has one, its POST
program's folder (`POST`), its place in the flash PROM (`POST_AT`) and
whether the image installed has it (`POST_ON`; the image with it is
`build/NAME/NAME-post.bin`). `ultra5_10` makes
`build/ultra5_10/ultra5_10.bin`, the 1 MiB flash PROM, and installs it as
`ultra5_10.prom`, which UltraViolet builds in; `t2000`, the release's own
platform (the Sun Fire T2000), checks the tool chain. Another machine is a
platform folder under `obp/arch` and a file in `systems`.

The release's shared files serve every machine. A change to one that only
makes it behave as an Ultra 10 does is made under the system's flag
(`[ifdef] Ultra5_10`, from `systems/ultra5_10.mk`) or kept in the
platform's folder, so that another machine built from them gets the
release's behaviour; fixes that hold for any machine are made for all. A
driver for a chip keeps the board's part (slots, their names and wiring)
under the flag in one place, as the APB's (`obp/dev/pci-bridge/simba`)
does.

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

The root node has the UPA methods a UPA card's FCode calls on an Ultra
10 (`upa-get-portid`, `upa-set-queue-sizes`, `upa-slave-interrupt` and the
rest, `rootnexus.fth`): the Creator3D's sets its request queue size in the
IIi's FFB_Config register through them. It also has `decode-space` (a
port ID as a UPA physical address's high cell) and the DMA methods
(`dma-alloc`, `dma-map-in` and the rest), which are the PCI bus module's:
all of an Ultra 10's DMA goes through its IOMMU. Mapping out a range that
isn't mapped in gives a warning. `reset-all` says "Resetting ..."
first, from the ok prompt or on a reboot, and leaves "soft-rst" in the
NVRAM for the reset code (at 0x10: "Software Power ON" in diagnostic
mode, and no power-on counted). The root's `reset-reason` is made once,
at the start, from the reset control's status bits as an Ultra 10 names
them: `POR`, `S-POR` (`reset-all`), `B-POR`, `S-XIR` or `B-XIR` for one
bit alone, `???` for none (a watchdog reset) or more than one; after a
button XIR the memory is only touched, not cleared. The other resets are
taken as an Ultra 10 takes them: a watchdog reset says "Watchdog Reset"
and starts again from power-on; a SIR ("SIR Reset") or a RED state
exception ("RED State Exception", then each trap level's TL, TT, TPC, TnPC
and TSTATE) is followed by a software XIR, and an XIR, from the button or
software, goes back to the ok prompt of the OpenBoot running ("Externally
Initiated Reset", `reset-reason` unchanged) while its TLB entries are
there, or starts again if they are gone. `watchdog-reboot?` is kept but,
as on an Ultra 10, has no effect. A trap that ends at the ok prompt is
shown by its name alone ("Memory Address not Aligned"). `watch-net` shows a
bad packet ('X') for each frame the Ethernet's MAC counts as an error (too long,
misaligned, bad CRC) or that overflowed its buffer.
The keyboard works in every Type 5 layout (21-36): the release's tables
by USB usage, and, for the layouts the release has none of, the Ultra 10's
own (`usb-japan.fth`, `usb-canadian-french.fth`, `usb-hungary.fth`,
`usb-poland.fth`, `usb-czech.fth`, in `obp/pkg/keyboard/tables/usb`, as
its keyboard gives them; Korea, Taiwan and Russia are the USA's).

The machine's configuration variables (`loadconfig.fth`) and its
diagnostic commands the release has no form of (`diag.fth`: `.speed`,
`probe-pci`, `probe-pci-slot`, `eject-floppy`) are part of the platform;
the power-on selftest ([post](post)) runs an Ultra 10's groups and tests,
in its order and words, at the level `diag-level` gives: the NVRAM; the D-TLB's tags and entries; the E-cache's
data and tags; the processor's instructions, tick, soft traps and
interrupts; the MMUs' registers; the D- and I-caches' arrays; the FPU, its
registers, operations and trap; the UPA data bus; memory, through the
E-cache and around it, with block loads and stores; the PCI bus module's
PIO address decoding and byte enables, interrupt maps and states, IOMMU registers and TLB, and PCI registers; the
MMUs' misses, byte order and ASIs; the traps for errors (a PCI master
abort among them), misaligned accesses and protection; DMA through the
IOMMU (reads and writes, TLB misses and hits, pass-through and the TLB's
locking), made by the RIO EBus's parallel port DMA channel in its
diagnostic mode; the RIO's PCI configuration headers (EBus and
Ethernet); PIO reads ending in a master abort and in a target abort (APB
B in master-abort mode). As an Ultra 10's, the first failure ends the run:
each test names a suspect part, and its class updates the board
descriptor (the processor module's bit 4 cleared and the test's sub-code
in bits 4:0, the RIO's bit 25, the NVRAM's bit 49, or a memory
miscompare's DIMM bit, from bit 32); the keyboard's LEDs show the run's
heartbeat and the suspect. `post.fth` takes its result, and the board
descriptor (0002.3333.0200.001b after a passing run, as an Ultra 10's,
kept in the NVRAM at 1d58 with the POST's other working data, 1cd8-1e0f;
OpenBoot then zeroes 1cd8-1cdf, the suspect code the POST left, as an
Ultra 10's does). The selftests (`test`, `test-all`) of the Rage Pro, the
Ethernet and the floppy drive are in their drivers. As an Ultra 10's,
`test` prints only the selftest's own lines, and on a failure a blank line
and "*device* selftest failed. Return code = *n*", and gives a device that
passes no `status` property (`obp/pkg/selftest/test.fth`); the Ethernet's transceiver
check needs a link (with `tpe-link-test?` true). The floppy drive is the
one the digital output register selects with its motor on (drive 0); the
driver's blocks are its 512-byte sectors, as the Ultra 10's are:
`read-blocks` and `write-blocks` move one whatever the count asked for
and return 1 or 0, with nothing shown, a read judged done when the
controller ends it abnormally (a non-DMA read's end) or with an overrun,
a write only when it ends normally; a data error alone is tried again,
the drive recalibrated, up to eight times. Its open and selftest check
the controller and drive (their reset, the timings read back, recalibrate,
seeks to tracks 2 and 0, track 0's signal and the heads) with an Ultra
10's messages.

`obdiag` (`obdiag.fth`, `obd-pci.fth`, `obd-ebus.fth`, `obd-misc.fth`,
`obdmenu.fth`) is the Ultra 10's diagnostics of its on-board I/O: the
EBus's PCI header and DMA channels, the Ethernet (its registers and four
loopbacks), the keyboard and mouse UARTs, the floppy drive, the parallel
port, both serial ports at each speed, the NVRAM, the audio codec, the IDE
controller and drives, and the Rage. With `mfg-mode` off it does nothing;
with `on` (or a bft or sys mode) it shows the Ultra 10's numbered menu,
with `menu-int` or `menu-ext` the sub-test menu, and a failure is reported
in the Ultra 10's failure block (`###OBDIAG_MFG_START###` ... `MESSAGE=`).
`mfg-mode` is read at the first `obdiag` of a session. After a power-on
selftest that passed, a bft or sys `mfg-mode` runs the manufacturing script
(`run-obd-scripts`) as the Ultra 10 does. The tests run in instances of
the devices' nodes made without their drivers' open, so the drivers are as
they were afterwards; the sub-test menu's Ethernet entries set the chip up
first.

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
| `obp/arch/sun4u/ultra5_10/ui-cvars-u10.fth` | `obp/pkg/confvar/interfaces/ui-cvars.fth` (printenv's columns and defaults as an Ultra 10's) |
| `obp/arch/sun4u/ultra5_10/nvramrc-u10.fth` | `obp/pkg/confvar/interfaces/nvramrc.fth` (nvramrc's size and nvrecover as an Ultra 10's) |

`build.sh` builds the drop-ins a platform's `dropins.src` lists with GNU
make (`host/dropins.mk`), and runs in Docker with the whole `firmware`
folder (for the Rage Pro's FCode).

### The NVRAM

The configuration variables are an Ultra 10's, no more (the release's
others are left out: `verbosity`, `error-reset-recovery`,
`auto-boot-on-error?`, `network-boot-arguments`), and each is kept where an
Ultra 10 keeps it in the M48T59, in its encoding, with its lengths and
defaults (`fixed-u10.fth`, `loadconfig.fth`), with its checksum (byte 0:
0x17 XOR bytes 0x00C-0x1CD7). So a machine's settings are read as they
are. Past that area, an Ultra 10's power-on selftest keeps its scratch test
and working data (0x1CD8-0x1E0F), and ours tests the NVRAM at the same
place (0x1D00-0x1D3F); the release's hashed store (for nothing an Ultra 10
keeps) and its keys follow (0x1E10-0x1FD7), where an Ultra 10 never writes
(zeros on one that has never run its power-on selftest). `printenv` lays
its table out as an Ultra 10 does.

The Ultra 10's quirks are followed, as there is no telling whether anything
relies on them:

- A string is cut to its field (the field's size less 2) and its length
  kept in a byte: `boot-device` and `diag-device` hold 256, so a value of
  256 bytes or more is stored empty. A length past its field reads as
  empty.
- `security-mode` and `system-board-date`'s length are the same byte
  (0x050): setting either changes the other; `security-mode` shows `none`
  for any byte but 1 (command) and 2 (full).
- `security-password` is kept as it is typed, its length and up to 8
  characters.
- `nvramrc` is at the top of the area (its text ending at 0x1CD5, up to
  5282 bytes), its length at 0x2BC and again at 0x1CD6: `set-defaults`
  zeroes the first only, and `nvrecover` puts the text it left in the
  editor (stored by `nvstore`).
- `mfg-mode`, `diag-switch?`, `diag-level` and `#power-cycles` are outside
  the checksum.
- At a bad checksum (or an all-zero area), "Setting NVRAM parameters to
  default values.": the defaults, `oem-banner` and `oem-logo` emptied, and
  `diag-switch?` true (which `set-defaults` alone does not do).

### The memory

The memory is found and sized as an Ultra 10 does it, in two steps.
The reset code (`reset.fth`), with the memory controller in 10-bit column
mode, looks for each DIMM of the two pairs (0, at 0, and 2, at 256 MiB),
sizes it, and with 11-bit columns tests a marker 16 KiB up (column bit 10)
to tell DIMMs with 11-bit column DRAMs from those with 10-bit (Sun's 16 MB
DIMM). It turns 11-bit mode on if the 11-bit DIMMs give at least the memory
10-bit columns would give all of them, and puts OpenBoot at the top of the
highest-addressed pair side the mode uses. OpenBoot's `probe-memory`
(`memprobe.fth`) then sizes each pair again, both sides, for the memory
node's `reg` (pair 0, then pair 2). In 11-bit mode a pair of 10-bit DIMMs
is left unused ("Unused 10 bit Column DIMMS - See User's Manual!" in
diagnostic mode); an unused or empty pair is switched off in Mem_Control0.
The Ultra 10's quirks are followed:

- the reset code looks for the second DIMM's second side at +512 MiB +
  0x10 (the first DIMM's place); `probe-memory` at +512 MiB + 8;
- a DIMM's second side is taken to be the size of its first;
- in 11-bit mode on an UltraSPARC-IIi of version 12.10, an unused pair is
  not switched off;
- the memory controller's timing (Mem_Control1) at reset is 0626.128a on a
  processor of mask version below 8.0, else 0c4a.ab14.

Once the memory is known, OpenBoot tunes the memory controller as an Ultra
10 does: ECC on (off while `probe-memory` probes), the refresh interval
from the measured clock and the pairs in use (clock in Hz / 2048000 /
pairs, the IIi manual's formula), and Mem_Control1's timings from a table
by the clock in MHz, for 50 ns DRAMs, or 60 ns ones when both pairs are in
use; the banner then names the speed ("256 MB (50 ns) memory installed").
The free memory's check bits are then set ("Initializing ... megs of
memory at addr ..." in diagnostic mode, "Initializing Memory" otherwise,
and nothing shown with `silent-mode?` true outside diagnostic mode).

OpenBoot lives in the top 1 MiB of that pair side, as on an Ultra 10: its
image in the top 512 KiB (mapped at f000.0000 by one locked 512 KiB TLB
entry), its first 64 KiB of RAM below that (mapped at fffe.0000, its
run-time mappings taken below it), and the memory it takes as it
runs (its heap, page tables, processor structures, the dictionary's
growth a page at a time, the IOMMU's table) from the rest of the 1 MiB;
the retained page (`arch/sun4u/retained.fth`) is the page just below.
A compressed drop-in's buffers come from the bottom of the 1 MiB (the
dictionary grows from its top), so that freeing them once its driver has
run leaves no hole: the memory is initialised in two ranges, as on an
Ultra 10.
The rest is free for the client (`/memory`'s `available`), the 1 MiB's
unused part with it. The IOMMU's table is the Ultra 10's 16 KiB, for the
16 MB of DVMA from c000.0000 OpenBoot uses (the IOMMU is told 128K entries;
the client sets up its own); as on an Ultra 10, a page's DVMA address is
c000.0000 plus the low 24 bits of its virtual address. As on an Ultra 10,
the flash PROM is mapped only while drop-ins are read, and of PCI
configuration space only bus 0's first page stays mapped (the PCI node's
`address`); any other page is mapped for the access alone.

### PCI

The PCI spaces are given out as an Ultra 10's own OpenBoot gives them. The
APB's bus A (`pci@1,1`, the machine's own devices) has memory from
e000.0000 and I/O from c0.0000: the Ethernet at e000.0000, the Rage Pro at
e100.0000 and e200.0000, the IDE controller's five I/O ranges from c0.0000
in order, each at its own size; the EBus keeps its fixed windows (f000.0000
and f100.0000). Bus B (`pci@1`, the slots) has memory from 1000 and I/O
from 400, given out the same way, and what it leaves is the Sabre's
`available` (to the top of the 4 GB and 16 MB spaces, as that firmware
lists it). Devices are left with their command registers clear but for the
EBus, and their drivers turn them on as they open; expansion ROMs keep the
address they were read at, turned off. Bus B's bridge is given a latency
timer of 0x50 and its subordinate bus number is left at ff.

The release's own files are unchanged, but for:

- `obp/dev/ide/ide.fth`: a wait for the IDE controller polls its status
  every 10 microseconds for the first millisecond before polling every
  millisecond, so that a drive's brief busy time between the blocks of a
  transfer doesn't cost a millisecond a block.
- `obp/fm/lib/suspend.fth`: on the Ultra 5/10 the pager's prompt is an
  Ultra 10's, "More [<space>,<cr>,q,n,p,c] ?" (the release's begins with
  a space).
- `obp/dev/pci/device-props.fth`, `probe-reg.fth`, `make-device.fth` and
  `pcibus.fth`: on the Ultra 5/10, PCI devices are given their addresses as
  an Ultra 10's own OpenBoot gives them. While a device is probed its BARs
  have temporary addresses from its bus's base up. Once probed, a device
  whose "reg" has fixed addresses (the EBus) keeps them, with command 0x146;
  any other has its BARs cleared, the relocatable spaces of its final "reg"
  placed at the sizes it gives, from the bus's base up and each above the
  last, `assigned-addresses` made from them, and its command register
  cleared (its driver turns it on as it opens). Expansion ROMs keep no space.
  Devices' cache line size and latency timer are left as reset and made no
  properties, and bridges get no `available`. A device's properties are
  made in the order an Ultra 10's are (`.properties` lists them newest
  first): those of its configuration header (vendor-id, device-id,
  revision-id, subsystem-id, subsystem-vendor-id, min-grant, max-latency,
  interrupts, class-code, devsel-speed, fast-back-to-back, 66mhz-capable)
  before its driver's, but a bridge's and the EBus's after their drivers'.
- `obp/os/bootprom/initdict.fth` and `obp/arch/sun4u/retained.fth`: a
  system may set the dictionary's growth step and the retained page's
  alignment (the Ultra 5/10's: a page, and 1 MiB).
- `obp/pkg/console/banner.fth`: on the Ultra 5/10 the banner has no
  copyright line, as an Ultra 10's has none.
- `obp/arch/sun4u/fieldberr.fth` (with a hook in `catchexc.fth`): a probe
  (`cprobe` and the like) that ends at the ok prompt with a trap it doesn't
  field, such as an MMU miss, no longer leaves its trap handlers installed;
  a later misaligned access gave "Error bed", and the next an illegal
  instruction.
- `obp/pkg/boot/bootparm.fth`: on the Ultra 5/10, with `silent-mode?` true
  and not in diagnostic mode, the "Boot device" and "Rebooting with
  command" lines are left out, as an Ultra 10 leaves them out.
- `obp/pkg/decompressor/decompress.fth`: on the Ultra 5/10 a compressed
  drop-in's buffers take their memory where the platform chooses
  (`memprobe.fth`), at the bottom of OpenBoot's 1 MiB.
- `obp/fm/kernel/fileio.fth`: an error at the end of a string being
  evaluated shows the string ("Evaluating: boot"); the line was looked for
  from one past its end, and shown empty.
- `obp/os/bootprom/testdevt.fth` and `obp/dev/pci/knownprops.fth`: on the
  Ultra 5/10, `.properties` shows values as an Ultra 10 shows them. The
  integer properties it knows by name (with `#address-cells`,
  `#size-cells`, `#interrupt-cells`, `#mapped-interrupt-cells`,
  `upa-portid`, `scsi-initiator-id`, `burst-sizes`, `up-burst-sizes`,
  `slot-address-bits`, `breakpoint-trap`, `pulse-interrupts`,
  `translations` six to a line and the interrupt maps a row to a line, but
  not `cache-line-size` or `latency-timer`) are shown as integers, every
  line as long as the others, the last one reading on past the value's
  end; any other value of eight bytes as two integers, one to a line; a
  single printable string, with or without its null, as text; anything
  else, composite strings and four-byte values included, as hex bytes, at
  most 16 and nothing after them. Its pager stops only between properties.
- `obp/os/bootprom/finddev.fth`: on the Ultra 5/10 a path is searched for
  as an Ultra 10 searches it. A unit address is read only by a node's own
  `decode-unit` (the two-number default is not used), and only at a node
  with children; a component is matched among the node's children by name
  and the first cells of `reg`, then by name among those with no `reg`,
  then in its whole subtree, depth first, the same at each node with
  children. A node with no `decode-unit`, or one that cannot read the
  address, is where the search ends, with no message, when the path names
  it (`/openprom/foo@0` is `/openprom`, `/memory@,0` the root); met in the
  subtree search it ends the search with "Package method not found"
  (`cd /memory@0,0`, `cd /foo@0,0`). Support packages and devices are
  treated alike.
- `obp/arch/sun4u/trans.fth` and `obp/cpu/sparc/ultra/mmu.fth`: on the
  Ultra 5/10 `translations` lists its ranges highest virtual address
  first, and two ranges are one only where both their virtual and their
  physical addresses carry on with the same attributes, as an Ultra 10
  lists them.
- On the Ultra 5/10 the data stack is emptied after an error at the ok
  prompt, but for an undefined word, as an Ultra 10 empties it (the
  release's interpreter, `obp/fm/kernel/ansio.fth`, keeps it; the
  platform's `loadprom.fth` patches it, the kernel being built for every
  system).

## Credits

- **Sun Microsystems, Inc.:** OpenBoot, and its release as open source.
- **Mitch Bradley (Bradley Forthware, FirmWorks):** the Forth system
  OpenBoot is built on, and Open Firmware's design (IEEE 1275-1994).
- **The OpenBIOS project:** keeping the release available.

## Licence

OpenBoot is under Sun's licence, a BSD-style licence, given in full in
[LICENSE](LICENSE), and UltraViolet's additions are under the same terms.
Some of its files are also FirmWorks' (Mitch Bradley's Forth system, which
OpenBoot is built on); LICENSE also gives the MIT licence FirmWorks
released its own Open Firmware under, from the same code.
This folder is not under UltraViolet's GPL.

Sun, Ultra, Solaris and OpenBoot are trademarks of Oracle and/or its
affiliates. SPARC and UltraSPARC are trademarks of SPARC International, Inc.
