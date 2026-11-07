#!/bin/sh
# Builds m64.rom, a PCI expansion ROM image of the FCode: the logo
# (logo.ras) as FCode data (logo.fth), then m64.fth tokenized behind a PCI
# header. TOKE names the tokenizer (default: toke on the PATH).
set -e
cd "$(dirname "$0")"
size=$(wc -c < logo.ras)
{
    echo "\\ Made by build.sh from logo.ras: its bytes, put into logo-ras."
    echo "d# $size constant /logo"
    echo "create logo-ras /logo allot"
    echo "0 value logo-len"
    echo ": +logo  ( adr len -- )  tuck  logo-ras logo-len + swap move  logo-len + to logo-len  ;"
    od -An -v -tx1 logo.ras | awk '
        { for (i = 1; i <= NF; i++) { b[n++] = $i } }
        END {
            for (i = 0; i < n; i += 64) {
                s = ""
                for (j = i; j < i + 64 && j < n; j++) s = s (j > i ? " " : "") b[j]
                printf "\" \"(%s)\" +logo\n", s
            }
        }'
} > logo.fth
${TOKE:-toke} -o m64.rom m64.fth
rm logo.fth
