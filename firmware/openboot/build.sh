#!/bin/sh
# Copyright (C) 2026 Andy Taylor. Distributed under the same terms as the
# OpenBoot release it builds (see LICENSE).
#
# Builds OpenBoot on Linux. The release's tools are Solaris SPARC programs
# (its host Forth runs SPARC code from its dictionaries), so they are built
# here from their sources for SPARC Linux and run under QEMU's user-mode
# emulation, in the Docker image host/Dockerfile describes:
#
#   ./build.sh            the tool chain (tools, kernel), in Docker (the
#                         image is made if need be)
#   ./build.sh STAGE...   those stages: tools, kernel, or a system (as
#                         systems/NAME.mk describes it); all: every one
#   ./build.sh systems    the systems there are
#
# Everything built goes in build/.
set -e
ROOT=$(cd "$(dirname "$0")" && pwd)
IMAGE=uv-openboot-build

if [ -z "$OPENBOOT_IN_DOCKER" ]; then
    docker build -q -t $IMAGE "$ROOT/host" > /dev/null 2>&1
    # (The firmware folder: the Rage Pro's FCode is built beside this one.)
    FIRMWARE=$(dirname "$ROOT")
    exec docker run --rm -u "$(id -u):$(id -g)" -e OPENBOOT_IN_DOCKER=1 \
        -v "$FIRMWARE:$FIRMWARE" -w "$ROOT" $IMAGE sh "$ROOT/build.sh" "$@"
fi

OUT=$ROOT/build
BP=$ROOT/obp
export BP
mkdir -p "$OUT/tools" "$OUT/obj32" "$OUT/obj64"

CC32="sparc64-linux-gnu-gcc -m32 -mcpu=v9 -static -O1 -w -fcommon -include $ROOT/host/compat.h"
CC64="sparc64-linux-gnu-gcc -m64 -static -O1 -w -fcommon -include $ROOT/host/compat.h"
FORTH32="qemu-sparc32plus-static $OUT/tools/forth"
FORTH64="qemu-sparc64-static $OUT/tools/forth64"
RUN32="qemu-sparc32plus-static"

say() { echo "== $*"; }

# The host Forth (os/unix: the C wrapper that loads a dictionary and runs
# it), 32- and 64-bit, and the C tools.
tools() {
    say "host tools"
    for bits in 32 64; do
        eval cc=\$CC$bits
        o=$OUT/obj$bits
        for f in "$ROOT"/tools/libxref/*.c; do $cc -c "$f" -I"$ROOT/tools/libxref" -I"$ROOT/lib" -o "$o/$(basename "${f%.c}").o"; done
        $cc -c "$ROOT/tools/libdef/defines.c" -I"$ROOT/tools/libdef" -o "$o/defines.o"
        name=forth; [ $bits = 64 ] && name=forth64
        $cc -Wl,--wrap=sbrk -DSYS5 -DUNIX -DDLOPEN -DDEF_DICT="(512*1024L)" \
            -I"$ROOT/lib" -I"$ROOT/tools/libxref" -o "$OUT/tools/$name" \
            "$BP/os/unix/wrapper.c" "$BP/os/unix/fsys.c" "$ROOT/host/hostcompat.c" \
            "$o"/xref*.o "$o"/datafile.o "$o"/filepath.o "$o"/find.o "$o/defines.o" -ldl 2>&1 |
            grep -v 'statically linked\|dlopen\|getwd' || true
    done
    o=$OUT/obj32
    $CC32 -c "$ROOT/tools/checksum/checksum.c" -o "$o/checksum.o"
    $CC32 -c "$ROOT/tools/checksum/crc32.c" -o "$o/crc32.o"
    $CC32 "$ROOT/tools/makeprom.c" -I"$ROOT/include" "$o/checksum.o" "$o/crc32.o" -o "$OUT/tools/makeprom"
    $CC32 -DNOMAIN -I"$ROOT/include" "$ROOT/tools/makedi.c" "$ROOT/tools/comp.c" "$o/checksum.o" "$o/crc32.o" -o "$OUT/tools/makedi"
    $CC32 "$ROOT/tools/comp.c" -o "$OUT/tools/comp"
    $CC32 -I"$ROOT/lib" "$ROOT/tools/didepend.c" "$o/defines.o" -o "$OUT/tools/didepend"
    echo "3 4 + . bye" | $FORTH32 -d "$BP/os/sun/sparc/forth.dic" | grep -q 7
    say "host Forth runs"
}

# The target kernel (64-bit cells, 32-bit tokens), metacompiled by the
# host Forth from the release's bootstrap dictionary, and its header list.
kernel() {
    say "kernel (k64t32)"
    mkdir -p "$OUT/kernel"
    cd "$OUT/kernel"
    $FORTH32 -d "$BP/os/sun/sparc/forth.dic" -D dic-file-name=k64t32.dic -D KERNEL \
        -D nheads-dic-name=nheads.k64t32.dic "$BP/fm/meta/conf64.fth" "$BP/fm/kernel/sparc/loadkern.fth" < /dev/null
    $FORTH32 -d nheads.k64t32.dic -s " nheads" < /dev/null | sort -k2 > k64t32.fth
    $FORTH32 -d nheads.k64t32.dic -s " nheadless" < /dev/null | sort -k2 > k64t32.headless.fth
    rm -f nheads.k64t32.dic
    echo "3 4 + . bye" | $FORTH64 -d k64t32.dic | grep -q 7
    say "kernel runs"
}

# --- A platform's PROM -------------------------------------------------------
#
# In $OUT/$NAME, with the platform's release folder (its debug.fth,
# dropins.src) as the working folder, as the release's makefiles have it:
#   image   the Forth image (bootprom.bin) and its headers, from LOADFILE
#   reset   the reset code (reset.bin), from RESET, linked with the
#           decompressor as obp/arch/sun4u/openboot.map lays it out
#   prom    the drop-ins the platform's dropins.src lists, and the PROM
#           image (openboot.bin)
# with FLAGS the platform's -D symbols.

version_fth() {
    {
        echo "headerless"
        date "+td %M td %H bwjoin td %d td %m bwjoin wljoin td %Y lxjoin"
        echo "constant compile-signature"
        echo "create (sub-release ,\" \""
        echo "create (obp-release ,\" $(cat "$BP/firmware.rel")\""
        echo "' (sub-release is sub-release"
        echo "' (obp-release is obp-release"
        echo "headers"
    } > version.fth
}

platform_dir() {
    mkdir -p "$OUT/$NAME"
    cd "$OUT/$NAME"
    cp "$RELEASE"/* .
}

image() {
    say "$NAME: Forth image"
    platform_dir
    version_fth
    $RUN32 "$OUT/tools/didepend" $FLAGS -c dropins.src > builtin.fth
    $FORTH64 -e 800 -d "$OUT/kernel/k64t32.dic" $FLAGS -D kernel-hdr-file="$OUT/kernel/k64t32.fth" \
        -D LOADFILE="$LOADFILE" -D SAVEFILE=bootprom.bin "$BP/arch/preload.fth" < /dev/null
    sort -k2 headers > acfheaders
}

reset() {
    say "$NAME: reset code"
    cd "$OUT/$NAME"
    sparc64-linux-gnu-gcc -m32 -mcpu=v9 -O2 -DSTANDALONE -ffreestanding -fno-pic \
        -fno-asynchronous-unwind-tables -c "$ROOT/tools/decomp.c" -o decomp.o
    size=$(sparc64-linux-gnu-size decomp.o | awk 'NR == 2 { print $1 + $2 }')
    grep headerless acfheaders > headerless
    $FORTH64 -D dropin-mode -D TARGET-FILE=reset.o -D RESET -D LOADFILE="$RESET" -e 900 -d stand.dic \
        $FLAGS -s "d# $size constant decomp-size" -s " warning off" headerless "$BP/arch/preload.fth" < /dev/null
    rm -f headerless
    sparc64-linux-gnu-ld -m elf32_sparc --no-warn-execstack -T "$ROOT/host/reset.ld" reset.o decomp.o -o reset.elf
    sparc64-linux-gnu-objcopy -O binary reset.elf reset.bin
}

prom() {
    say "$NAME: drop-ins and PROM"
    cd "$OUT/$NAME"
    # The drop-ins dropins.src lists, made as its rules say (host/dropins.mk),
    # then put together in its order.
    $RUN32 "$OUT/tools/didepend" $FLAGS -d dropins.src |
        sed 's/^\([^ \t:]*\):=[ \t]*/\1: /' > dropins.mk
    targets=$($RUN32 "$OUT/tools/didepend" $FLAGS -t dropins.src)
    make -s -f "$ROOT/host/dropins.mk" ROOT="$ROOT" OUT="$OUT" FLAGS="$FLAGS" $targets
    cat $targets > builtin.di
    $RUN32 "$OUT/tools/makeprom" -n -o openboot.bin reset.bin builtin.di
    say "$NAME: $OUT/$NAME/openboot.bin"
}

# A tokenized FCode program: tokenize SOURCE.tok OUTPUT.fc
tokenize() {
    $FORTH32 -d "$BP/pkg/tokenizr/sparc/tokenize.dic" -D tokenizer? $FLAGS \
        -s "aout-header? off silent on" -s "tokenize $1 $2" < /dev/null
}

# A system: its OpenBoot built as systems/NAME.mk describes it (the
# platform's release folder, flags, load and reset files), then padded to
# its flash PROM's size and installed if it says so.
system() {
    desc=$ROOT/systems/$1.mk
    val() { sed -n "s/^$1[ \t]*=[ \t]*//p" "$desc" | sed 's/[ \t]*$//'; }
    NAME=$(val NAME) FLAGS=$(val FLAGS)
    RELEASE=$BP/$(val RELEASE) LOADFILE=$BP/$(val LOADFILE) RESET=$BP/$(val RESET)
    image; reset; prom
    cd "$OUT/$NAME"
    flash=$(val FLASH)
    if [ -n "$flash" ]; then
        size=$(wc -c < openboot.bin)
        { cat openboot.bin; head -c $((flash - size)) /dev/zero | tr '\000' '\377'; } > "$NAME.bin"
        say "$NAME: $OUT/$NAME/$NAME.bin"
    fi
    install=$(val INSTALL)
    if [ -n "$install" ]; then
        cp openboot.bin "$ROOT/$install"
        say "$NAME: $ROOT/$install"
    fi
}

# The systems there are descriptions of
systems() {
    for d in "$ROOT"/systems/*.mk; do basename "$d" .mk; done
}

# A stage is a step (tools, kernel, systems) or a system's name; "all" is
# the tool chain and every system.
stages=${*:-tools kernel}
[ "$stages" = all ] && stages="tools kernel $(systems)"
for s in $stages; do
    if [ -f "$ROOT/systems/$s.mk" ]; then system "$s"; else $s; fi
done
