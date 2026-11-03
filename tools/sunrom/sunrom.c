/*
 * SPDX-License-Identifier: GPL-2.0-or-later
 * Copyright (C) 2026 Andy Taylor
 *
 * sunrom: get the firmware images UltraViolet runs from what a Sun owner
 * already has, and say what an image is.
 *
 *   sunrom extract UPDATER OUT
 *       The flash PROM image (OpenBoot and POST) carried by a Sun
 *       "flash-update-*" program, as shipped in the flash PROM patches
 *       (106121 for the Ultra 5/10, 119235 for the Blade 100/150, 118323
 *       for the Blade 1000/2000).
 *   sunrom log MESSAGES UNIT OUT
 *       A card's PROM as the ffbprom kernel module (solaris/ffbprom.c)
 *       logged it, put back together from the system log: UNIT is the
 *       card ("ffb0" for the first); the last complete dump is taken.
 *   sunrom info FILE...
 *       What each file is: an OpenBoot flash image (its blocks and version),
 *       an FCode PROM (its checksum, version and any Sun raster logo), or
 *       neither.
 *
 * Written in ANSI C (C89) with nothing but the standard library, so that
 * it builds with old compilers on Solaris, Linux and the BSDs:
 *     cc -o sunrom sunrom.c
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define FLASH_TRAILER 0x20 /* bytes after each block in an updater */

static unsigned long be32(const unsigned char *p)
{
    return ((unsigned long)p[0] << 24) | ((unsigned long)p[1] << 16) | ((unsigned long)p[2] << 8) |
           (unsigned long)p[3];
}

static unsigned int be16(const unsigned char *p)
{
    return ((unsigned int)p[0] << 8) | (unsigned int)p[1];
}

/* The whole of a file, or NULL (with a message). */
static unsigned char *read_file(const char *path, unsigned long *size)
{
    FILE *f;
    unsigned char *buf = NULL, *grown;
    unsigned long have = 0, room = 0;
    size_t n;

    f = fopen(path, "rb");
    if (f == NULL) {
        fprintf(stderr, "sunrom: cannot open %s\n", path);
        return NULL;
    }
    for (;;) {
        if (have == room) {
            room = room ? room * 2 : 65536;
            grown = (unsigned char *)realloc(buf, room);
            if (grown == NULL) {
                fprintf(stderr, "sunrom: out of memory reading %s\n", path);
                free(buf);
                fclose(f);
                return NULL;
            }
            buf = grown;
        }
        n = fread(buf + have, 1, (size_t)(room - have), f);
        if (n == 0) break;
        have += (unsigned long)n;
    }
    if (ferror(f)) {
        fprintf(stderr, "sunrom: cannot read %s\n", path);
        free(buf);
        fclose(f);
        return NULL;
    }
    fclose(f);
    *size = have;
    return buf;
}

static int write_file(const char *path, const unsigned char *data, unsigned long size)
{
    FILE *f;

    f = fopen(path, "wb");
    if (f == NULL) {
        fprintf(stderr, "sunrom: cannot create %s\n", path);
        return 0;
    }
    if (fwrite(data, 1, (size_t)size, f) != (size_t)size || fclose(f) != 0) {
        fprintf(stderr, "sunrom: cannot write %s\n", path);
        return 0;
    }
    return 1;
}

/* The offset of `what` (len bytes) in data from `from`, or -1. */
static long find(const unsigned char *data, unsigned long size, unsigned long from, const char *what, unsigned long len)
{
    unsigned long i;

    for (i = from; i + len <= size; i++)
        if (data[i] == (unsigned char)what[0] && memcmp(data + i, what, (size_t)len) == 0) return (long)i;
    return -1;
}

/* A dropin's name: 16 bytes from offset 16 of its header. */
static void dropin_name(const unsigned char *block, char *name)
{
    int i;

    for (i = 0; i < 16 && block[16 + i] >= 0x20 && block[16 + i] < 0x7F; i++) name[i] = (char)block[16 + i];
    name[i] = '\0';
}

/* --- extract --------------------------------------------------------- */

/* The updater's initialised data: its largest PROGBITS section (ELF32,
 * big-endian, as Sun's standalone updaters are). */
static int data_section(const unsigned char *elf, unsigned long size, unsigned long *off, unsigned long *len)
{
    unsigned long shoff, i, best = 0;
    unsigned int shentsize, shnum;

    if (size < 52 || memcmp(elf, "\177ELF", 4) != 0 || elf[4] != 1 || elf[5] != 2) {
        fprintf(stderr, "sunrom: not a 32-bit big-endian ELF program\n");
        return 0;
    }
    shoff = be32(elf + 32);
    shentsize = be16(elf + 46);
    shnum = be16(elf + 48);
    if (shentsize < 24 || shoff + (unsigned long)shnum * shentsize > size) {
        fprintf(stderr, "sunrom: bad section table\n");
        return 0;
    }
    for (i = 0; i < shnum; i++) {
        const unsigned char *sh = elf + shoff + i * shentsize;
        unsigned long type = be32(sh + 4), o = be32(sh + 16), l = be32(sh + 20);
        if (type == 1 && l > best && o + l <= size) best = l, *off = o, *len = l;
    }
    if (best < 0x80000) {
        fprintf(stderr, "sunrom: no flash image in this program\n");
        return 0;
    }
    *len = best;
    return 1;
}

static int extract(const char *in, const char *out)
{
    static const unsigned long sizes[2] = {0x100000UL, 0x80000UL};
    unsigned char *file, *data, *img;
    unsigned long size, off, len, span, bsize = 0, count, keep[8], kept = 0, i;
    long first;
    char name[17], prev[17];
    int ok;

    file = read_file(in, &size);
    if (file == NULL) return 1;
    if (!data_section(file, size, &off, &len)) {
        free(file);
        return 1;
    }
    data = file + off;
    /* The image starts at the dropin header ("OBMD") of the OBP block. */
    first = -1;
    for (;;) {
        first = find(data, len, (unsigned long)(first + 1), "OBMD", 5);
        if (first < 0) {
            fprintf(stderr, "sunrom: no OpenBoot block found\n");
            free(file);
            return 1;
        }
        if (memcmp(data + first + 16, "OBP", 4) == 0) break;
    }
    span = len - (unsigned long)first;
    for (i = 0; i < 2; i++)
        if (span % (sizes[i] + FLASH_TRAILER) == 0) bsize = sizes[i];
    if (bsize == 0) {
        fprintf(stderr, "sunrom: unknown block layout (0x%lx bytes)\n", span);
        free(file);
        return 1;
    }
    count = span / (bsize + FLASH_TRAILER);
    if (count > 8) {
        fprintf(stderr, "sunrom: too many blocks (%lu)\n", count);
        free(file);
        return 1;
    }
    /* Each block, a dropin; the Ultra 5/10 updaters carry each half of the
     * flash twice (OBP, OBP, POST, POST): a block the same as the one
     * before it is left out. */
    prev[0] = '\0';
    for (i = 0; i < count; i++) {
        const unsigned char *b = data + first + i * (bsize + FLASH_TRAILER);
        if (memcmp(b, "OBMD", 4) != 0) {
            fprintf(stderr, "sunrom: block %lu is not a dropin\n", i);
            free(file);
            return 1;
        }
        dropin_name(b, name);
        if (kept > 0 && strcmp(name, prev) == 0 &&
            memcmp(b, data + first + keep[kept - 1] * (bsize + FLASH_TRAILER), (size_t)bsize) == 0)
            continue;
        keep[kept++] = i;
        strcpy(prev, name);
    }
    img = (unsigned char *)malloc((size_t)(kept * bsize));
    if (img == NULL) {
        fprintf(stderr, "sunrom: out of memory\n");
        free(file);
        return 1;
    }
    printf("%s:", in);
    for (i = 0; i < kept; i++) {
        const unsigned char *b = data + first + keep[i] * (bsize + FLASH_TRAILER);
        memcpy(img + i * bsize, b, (size_t)bsize);
        dropin_name(b, name);
        printf(" %s", name);
    }
    printf(" (%lu blocks of %lu KiB) -> %s\n", kept, bsize / 1024, out);
    ok = write_file(out, img, kept * bsize);
    free(img);
    free(file);
    return ok ? 0 : 1;
}

/* --- log ------------------------------------------------------------ */

static int hexval(int c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    return -1;
}

static int from_log(const char *in, const char *unit, const char *out)
{
    FILE *f;
    char line[1024], tag[64];
    unsigned char *cur = NULL, *done = NULL, *have = NULL;
    unsigned long total = 0, done_total = 0, got = 0, at, i;
    int hi, lo, ok;
    char *p;

    if (strlen(unit) > 32) {
        fprintf(stderr, "sunrom: unit name too long\n");
        return 1;
    }
    sprintf(tag, "sunrom: %s ", unit);
    f = fopen(in, "r");
    if (f == NULL) {
        fprintf(stderr, "sunrom: cannot open %s\n", in);
        return 1;
    }
    while (fgets(line, (int)sizeof line, f) != NULL) {
        p = strstr(line, tag);
        if (p == NULL) continue;
        p += strlen(tag);
        if (strncmp(p, "start ", 6) == 0) {
            /* "start ADDRESS BYTES": a new dump. */
            p = strchr(p + 6, ' ');
            total = p ? strtoul(p + 1, NULL, 16) : 0;
            free(cur);
            free(have);
            cur = total && total <= 0x100000UL ? (unsigned char *)malloc((size_t)total) : NULL;
            have = cur ? (unsigned char *)calloc((size_t)(total / 64 + 1), 1) : NULL;
            got = 0;
            if (cur == NULL || have == NULL) total = 0;
            continue;
        }
        if (total == 0) continue;
        if (strncmp(p, "end", 3) == 0) {
            if (got == (total + 63) / 64) {
                free(done);
                done = cur;
                done_total = total;
                cur = NULL;
            }
            total = 0;
            continue;
        }
        /* "OFFSET HEX": 64 bytes at OFFSET. */
        at = strtoul(p, &p, 16);
        if (*p != ' ' || at % 64 != 0 || at >= total) continue;
        p++;
        for (i = 0; i < 64 && at + i < total; i++) {
            hi = hexval(p[2 * i]);
            lo = hi < 0 ? -1 : hexval(p[2 * i + 1]);
            if (lo < 0) break;
            cur[at + i] = (unsigned char)(hi << 4 | lo);
        }
        if (i == 64 || at + i == total) {
            if (!have[at / 64]) got++;
            have[at / 64] = 1;
        }
    }
    fclose(f);
    free(cur);
    free(have);
    if (done == NULL) {
        fprintf(stderr, "sunrom: no complete dump of %s in %s\n", unit, in);
        return 1;
    }
    printf("%s: %s, %lu bytes -> %s\n", in, unit, done_total, out);
    ok = write_file(out, done, done_total);
    free(done);
    return ok ? 0 : 1;
}

/* --- info ------------------------------------------------------------ */

/* The first "@(#)" version string at or after `from`, printed. */
static long print_version(const unsigned char *d, unsigned long size, unsigned long from, const char *label)
{
    long at = find(d, size, from, "@(#)", 4);
    unsigned long i;

    if (at < 0) return -1;
    printf("  %s ", label);
    for (i = (unsigned long)at + 4; i < size && i < (unsigned long)at + 100 && d[i] >= 0x20 && d[i] < 0x7F; i++)
        putchar(d[i]);
    putchar('\n');
    return at;
}

/* A Sun raster image (magic 0x59a66a95) in the bytes from `from`. */
static void print_logo(const unsigned char *d, unsigned long size, unsigned long from)
{
    unsigned long i;

    for (i = (from + 3) & ~3UL; i + 32 <= size; i += 4)
        if (be32(d + i) == 0x59A66A95UL) {
            printf("  Sun raster image at 0x%lx: %lu x %lu, %lu bits a pixel\n", i, be32(d + i + 4), be32(d + i + 8),
                   be32(d + i + 12));
            return;
        }
}

static void info_flash(const unsigned char *d, unsigned long size)
{
    unsigned long bsize = size >= 0x200000UL ? 0x100000UL : 0x80000UL, i;
    char name[17];

    printf("  OpenBoot flash image, %lu KiB, dropins:", size / 1024);
    for (i = 0; i + 32 <= size; i += bsize)
        if (memcmp(d + i, "OBMD", 4) == 0) {
            dropin_name(d + i, name);
            printf(" %s at 0x%lx", name, i);
        }
    putchar('\n');
    print_version(d, size, 0, "version:");
}

static void info_fcode(const unsigned char *d, unsigned long size)
{
    unsigned long len = be32(d + 4), sum = 0, i;
    long name = find(d, size, 0, "SUNW,", 5);

    printf("  FCode PROM image, %lu bytes, the program %lu bytes", size, len);
    if (len >= 8 && len <= size) {
        for (i = 8; i < len; i++) sum += d[i];
        printf(", header checksum 0x%04x (its bytes add up to 0x%04lx)", be16(d + 2), sum & 0xFFFF);
    }
    putchar('\n');
    if (name >= 0) {
        printf("  names ");
        for (i = (unsigned long)name; i < size && d[i] > 0x20 && d[i] < 0x7F; i++) putchar(d[i]);
        putchar('\n');
    }
    print_version(d, size, 0, "version:");
    if (size > len) print_logo(d, size, len);
}

static int info(const char *path)
{
    unsigned long size;
    unsigned char *d = read_file(path, &size);

    if (d == NULL) return 1;
    printf("%s:\n", path);
    if (size >= 0x80000UL && memcmp(d, "OBMD", 4) == 0)
        info_flash(d, size);
    else if (size >= 8 && d[0] >= 0xF0 && d[0] <= 0xF3)
        info_fcode(d, size);
    else
        printf("  not a flash or FCode image (%lu bytes)\n", size);
    free(d);
    return 0;
}

static void usage(void)
{
    fprintf(stderr, "usage: sunrom extract UPDATER OUT\n"
                    "       sunrom log MESSAGES UNIT OUT\n"
                    "       sunrom info FILE...\n");
}

int main(int argc, char **argv)
{
    int i, rc = 0;

    if (argc == 4 && strcmp(argv[1], "extract") == 0) return extract(argv[2], argv[3]);
    if (argc == 5 && strcmp(argv[1], "log") == 0) return from_log(argv[2], argv[3], argv[4]);
    if (argc >= 3 && strcmp(argv[1], "info") == 0) {
        for (i = 2; i < argc; i++) rc |= info(argv[i]);
        return rc;
    }
    usage();
    return 2;
}
