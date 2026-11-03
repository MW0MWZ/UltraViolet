# Firmware images

UltraViolet runs the machine's own firmware: the OpenBoot flash PROM of an
Ultra 5/10, and for a Creator3D the card's own FCode PROM. These are Sun's
copyrighted software (now Oracle's) and are not included here. This page
says which images UltraViolet expects, where they come from, and how to
make them from the original Sun files with the `sunrom` tool in
[tools/sunrom](../tools/sunrom). Use only images you have the right to use.

Put the images in this folder (`roms` beside the program), or in the `roms`
folder of the UltraViolet folder in your Documents folder, or in the folder
set in Machine > Settings. File names do not matter: UltraViolet looks at
what each file is and takes the newest version it finds.

## OpenBoot for the Ultra 5/10

The flash PROM image (1 MiB: OpenBoot and POST) comes from Sun's flash PROM
update patch for the Ultra 5/10, **106121**. Each revision carries
`flash-update-Ultra510-*` programs, one per OpenBoot version, with the image
inside. Sun published the patch; Oracle now keeps it on its support site
for customers with a support contract, and copies are kept by archive
sites, such as the Internet Archive's mirror of the SunShack archive
(the item `sunshack_bootroms`, which holds 106121-18).

| Patch file | Size | SHA-256 |
|---|---|---|
| `106121-18.zip` | 5207398 | `21561476a550286a0fc28fd331f231b59f9db2ab774779bb3aec164d02d8153f` |

Build the tool and extract the image:

```sh
cc -o sunrom tools/sunrom/sunrom.c
unzip 106121-18.zip
./sunrom extract 106121-18/flash-update-Ultra510-latest roms/ultra5_10-obp-3.31.bin
./sunrom info roms/ultra5_10-obp-3.31.bin
```

The images in 106121-18, and what each should be once extracted:

| Program in 106121-18 | OpenBoot | SHA-256 of the image |
|---|---|---|
| `flash-update-Ultra510-latest`, `-18` | 3.31 (2001/07/25), OBDiag 2.9 | `fa3fafed624d19562af2ff488154a8bf3bf16b77aecf305cb077800398abe616` |
| `flash-update-Ultra510-16` | 3.31 (2001/07/25), OBDiag 2.8 | `519522c1677d7a99676b66b76a3f7acd98d3909f50636b841bbfbaae1a3ed5f4` |
| `flash-update-Ultra510-14` | 3.29 (2000/12/20) | `3b0dcc5a8e82717125a5c8babd8e3d0c6daff18e33f1254a9061b37995df8574` |
| `flash-update-Ultra510-10` | 3.27 (2000/08/23) | `230b714f70fb1d13b957b43fe6e7e4df970642f0efd1daa205bdf8d2e9bd35c5` |

UltraViolet is developed against **OpenBoot 3.31** (the `-latest`
program).

## Creator3D FCode

A Creator3D's FCode is on the card itself, in its PROM, and in no Sun patch
or install media. The card's FCode draws OpenBoot's console and gives the
operating system the card's properties, and the rest of its PROM holds the
banner logo (the globe) OpenBoot shows. It is read from a card in a running
Solaris system with the `ffbprom` kernel module and `sunrom`: see
[tools/sunrom/README.md](../tools/sunrom/README.md).

UltraViolet models the Creator3D model `SUNW,501-4788` (FFB2+,
double-buffered), FCode `ffb2p.fth 2.9 98/07/14`. A read of that card's
whole PROM (64 KiB) should be:

| Image | Size | SHA-256 |
|---|---|---|
| FFB2+ PROM, FCode 2.9 | 65536 | `3eebb3f04a0460207ba7b8dd319a63682142244ca4e3641c3be477e65f810be3` |

The FCode alone (the first 17756 bytes) also works, with OpenBoot's own Sun
logo in place of the card's.

## Blade 100/150 and Blade 1000/2000

Not emulated yet. Their flash PROM images come from patches **119235**
(Blade 100/150) and **118323** (Blade 1000/2000) the same way, with
`sunrom extract`.

| Patch file | Size | SHA-256 |
|---|---|---|
| `119235-01.zip` | 2436550 | `91057af94d124feb8964a90f705cb2333dfdeda3b0a8f72ceb79c3867a41a76b` |
| `118323-01.zip` | 5500207 | `344ab44163051d13c0ca99974a536b877cdf02acf4a20907d3ee2e72f86afdaf` |
