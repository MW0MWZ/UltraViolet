/*
 * Copyright (C) 2026 Andy Taylor. Distributed under the same terms as the
 * OpenBoot release it builds (see LICENSE).
 *
 * What the host Forth (os/unix/wrapper.c) needs on Linux besides its own
 * sources: no terminal control (the build is batch), executable memory for
 * the dictionary, and stubs that keep the Forth engine's registers apart
 * from the C library's.
 */

/* The terminal control (termcap.c's): none. */
long t_op(int op) { (void)op; return 0; }
long t_move(int col, int row) { (void)col; (void)row; return 0; }
long t_rows(void) { return 24; }
long t_cols(void) { return 80; }
long t_init(void) { return 0; }

/* The dictionary runs where sbrk puts it, so that memory is made
   executable (Linux does not make the heap so). */
#include <stdint.h>
#include <sys/mman.h>
#include <unistd.h>
void *__real_sbrk(intptr_t increment);
void *__wrap_sbrk(intptr_t increment) {
    char *p = __real_sbrk(increment);
    if (p != (char *)-1 && increment > 0) {
        const uintptr_t page = (uintptr_t)sysconf(_SC_PAGESIZE);
        const uintptr_t start = (uintptr_t)p & ~(page - 1);
        const uintptr_t end = ((uintptr_t)p + (uintptr_t)increment + page - 1) & ~(page - 1);
        mprotect((void *)start, end - start, PROT_READ | PROT_WRITE | PROT_EXEC);
    }
    return p;
}

/* The Forth engine keeps its state in %g2-%g7 (its stack pointer in %g7),
   where Linux's C library keeps its thread pointer (%g7) and gcc may keep
   values (%g2-%g4). Each entry of the table of C functions the dictionary
   calls is wrapped in a stub that saves those registers, gives %g7 back
   to the C library, calls the function and restores them. (The program is
   linked low, so the function's address and the saved thread pointer's
   fit in a sethi and an or.) */
extern long (*functions[])();
#define N_FUNCTIONS 57  /* entries 0-56, then the end of the table */
static uintptr_t c_thread_pointer;

static uint32_t op3(unsigned rd, unsigned op, unsigned rs1, int imm, unsigned rs2_or_simm) {
    return 2u << 30 | rd << 25 | op << 19 | rs1 << 14 | (imm ? 1u << 13 | (rs2_or_simm & 0x1FFF) : rs2_or_simm);
}
static uint32_t mov(unsigned from, unsigned to) { return op3(to, 0x02, 0, 0, from); }  /* or %g0, from, to */
static uint32_t sethi(unsigned rd, uint32_t v) { return rd << 25 | 4u << 22 | v >> 10; }

static long (*stub_for(long (*fn)(), uint32_t *code))() {
#ifdef __arch64__
    const int frame = -176;
    const unsigned load = 0x0B;  /* ldx */
#else
    const int frame = -96;
    const unsigned load = 0x00;  /* ld */
#endif
    const uint32_t tp = (uint32_t)(uintptr_t)&c_thread_pointer, f = (uint32_t)(uintptr_t)fn;
    uint32_t *p = code;
    *p++ = op3(14, 0x3C, 14, 1, (unsigned)frame);                    /* save %sp, frame, %sp */
    for (unsigned g = 2; g <= 7; ++g) *p++ = mov(g, 16 + g);         /* %g2-7 to %l2-7 */
    *p++ = sethi(7, tp);                                             /* sethi %hi(tp), %g7 */
    *p++ = 3u << 30 | 7u << 25 | load << 19 | 7u << 14 | 1u << 13 | (tp & 0x3FF);  /* ld[x] [%g7+%lo(tp)], %g7 */
    for (unsigned i = 0; i < 6; ++i) *p++ = mov(24 + i, 8 + i);      /* %i0-5 to %o0-5 */
    *p++ = sethi(1, f);                                              /* sethi %hi(fn), %g1 */
    *p++ = op3(1, 0x02, 1, 1, f & 0x3FF);                            /* or %g1, %lo(fn), %g1 */
    *p++ = op3(15, 0x38, 1, 1, 0);                                   /* jmpl %g1, %o7 */
    *p++ = 0x01000000;                                               /* nop */
    for (unsigned g = 2; g <= 7; ++g) *p++ = mov(16 + g, g);         /* %l2-7 back to %g2-7 */
    *p++ = op3(0, 0x38, 31, 1, 8);                                   /* ret (jmpl %i7+8, %g0) */
    *p++ = op3(8, 0x3D, 8, 0, 0);                                    /* restore %o0, %g0, %o0 */
    return (long (*)())(void *)code;
}

__attribute__((constructor)) static void wrap_functions(void) {
    __asm__ volatile("mov %%g7, %0" : "=r"(c_thread_pointer));
    uint32_t *code = mmap(0, N_FUNCTIONS * 32 * 4, PROT_READ | PROT_WRITE | PROT_EXEC, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    for (unsigned i = 0; i < N_FUNCTIONS; ++i)
        if (functions[i]) functions[i] = stub_for(functions[i], code + i * 32);
}
