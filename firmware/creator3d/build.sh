#!/bin/sh
# Builds ffb.fc, the card's PROM image: the FCode tokenized from ffb.fth,
# then the logo (logo.ras) at PROM byte 0x4000. TOKE names the tokenizer
# (default: toke on the PATH).
set -e
cd "$(dirname "$0")"
${TOKE:-toke} -o ffb.fcode ffb.fth
size=$(wc -c < ffb.fcode)
if [ "$size" -gt 16384 ]; then
    echo "build.sh: the FCode ($size bytes) runs past the logo at 0x4000" >&2
    exit 1
fi
cp ffb.fcode ffb.fc
dd if=/dev/zero bs=1 count=$((16384 - size)) 2>/dev/null >> ffb.fc
cat logo.ras >> ffb.fc
rm ffb.fcode
