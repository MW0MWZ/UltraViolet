# sunrom

Tools to make the firmware images UltraViolet runs from what a Sun owner
has: Sun's flash PROM patches, and a Creator3D card. They contain no Sun
code. See [roms/README.md](../../roms/README.md) for which images
UltraViolet expects and their checksums.

## sunrom

One file of ANSI C (C89) with nothing but the standard library, so it
builds with old and new compilers alike, on Solaris, Linux and the BSDs:

```sh
cc -o sunrom sunrom.c
```

- `sunrom extract UPDATER OUT` takes the flash PROM image (OpenBoot and
  POST) out of a Sun `flash-update-*` program from a flash PROM patch
  (106121 for the Ultra 5/10, 119235 for the Blade 100/150, 118323 for the
  Blade 1000/2000).
- `sunrom log MESSAGES UNIT OUT` puts a Creator3D's PROM back together from
  the system log, where the `ffbprom` module (below) wrote it. `UNIT` is the
  card, `ffb0` for the first. The last complete dump in the log is taken;
  one with a line missing is refused.
- `sunrom info FILE...` says what each file is: an OpenBoot flash image
  (its blocks and version), an FCode PROM (its name, version, header
  checksum and any logo it holds), or neither.

## Reading a Creator3D's PROM: solaris/ffbprom.c

A Creator3D's FCode, and the logo OpenBoot shows, are in the card's own
PROM, which only the kernel can reach. `ffbprom` is a Solaris kernel
module that reads it and writes it to the system log. It reads only the
PROM's own address range of each `SUNW,ffb` device (Creator and
Creator3D), found through the device tree; it writes nothing to the card,
and its `_init` returns an error once it has read the PROM, so it is never
left loaded.

On the Solaris system with the card (as root, with a C compiler and the
kernel headers, SUNWhea):

```sh
isainfo -kv          # a 64-bit kernel: build with -m64 as below
gcc -m64 -mcmodel=medlow -mno-app-regs -fno-pic -ffreestanding \
    -D_KERNEL -O2 -c ffbprom.c
ld -r -o ffbprom ffbprom.o
modload ./ffbprom    # "can't load module: I/O error" is expected
sunrom log /var/adm/messages ffb0 ffb2p-prom.bin
sunrom info ffb2p-prom.bin
```

With Sun's compiler, build with `cc -xarch=v9 -xregs=no%appl -D_KERNEL -O -c ffbprom.c`
instead of `gcc`. For a 32-bit kernel leave out `-m64` and
`-mcmodel=medlow` (or `-xarch=v9`).
