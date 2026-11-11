/*
 * Copyright (C) 2026 Andy Taylor. Distributed under the same terms as the
 * OpenBoot release it is part of (see LICENSE).
 *
 * The power-on selftest: what its parts share.
 */
#ifndef POST_H
#define POST_H

typedef unsigned char u8;
typedef unsigned short u16;
typedef unsigned int u32;
typedef unsigned long long u64;

/* Physical addresses. */
#define TTYA_PA		0x1fff1400000ULL	/* the SAB82532's channel A */
#define NVRAM_PA	0x1fff1000000ULL	/* the M48T59 */

/* start.S: physical access, through no cache. */
u8 phys_ld8(u64 pa);
void phys_st8(u64 pa, u32 v);
u32 phys_ld32(u64 pa);
void phys_st32(u64 pa, u32 v);
u64 rd_tick(void);
u64 rd_ver(void);

/* cpu.S */
u64 asi_ldx(u64 va, u32 asi);
void asi_stx(u64 va, u32 asi, u64 v);
u64 asi_swap(u64 va, u32 asi, u64 pattern);
void dtlb_swap(u32 index, u64 tag, u64 data, u64 *out);
u64 dtlb_entry(u32 index, u32 which);
u32 softint_setclr(u32 bit, u32 *after_clear);
void softint_int(u32 level);
void tick_test(u64 pattern, u64 *out);
void do_ta(void);
void do_tn(void);
u64 lsu_get(void);
void lsu_set(u64 v);

/* The result, for start.S to hand back. */
extern const char *post_message;	/* the part suspected; 0 passed */
extern u64 post_board;			/* the board descriptor */
extern u64 post_prom_base;		/* where the image is in the PROM */

/* Traps a test expects: armed, then the type taken (0 none). */
extern volatile u32 trap_armed, trap_tt;
u32 trap_run(void (*fn)(void));

/* Output (ttya). A line ends "\n\r", as the Ultra 10's POST ends it. */
void post_putc(int c);
void post_puts(const char *s);
void post_putd(u32 n, int width, int zero);
void post_putx(u64 n, int digits);
void post_putx64(u64 n);		/* xxxxxxxx.xxxxxxxx */

/* The NVRAM. */
static inline u8 nvram_ld8(u32 offset) { return phys_ld8(NVRAM_PA + offset); }
static inline void nvram_st8(u32 offset, u32 v) { phys_st8(NVRAM_PA + offset, v); }

/* The run (post.c). */
extern int post_max;			/* diag-level max: every test */
extern u32 ecache_size;
void post_suspect(const char *part);
int post_failed(void);
void post_fail(const char *msg);
void post_compare(u64 observed, u64 expected, const char *msg);
void post_compare_at(u64 observed, u64 expected, u64 adr, const char *msg);

/* The tests (tests-cpu.c). */
void post_battery(void);
void post_nv_addr(void);
void post_nv_data(void);
void post_dtlb_tags(void);
void post_dtlb_data(void);
void post_probe_ec(void);
void post_ec_ram_addr(void);
void post_ec_tag_addr(void);
void post_ec_ram(void);
void post_ec_tag(void);
void post_v9(void);
void post_tick(void);
void post_soft_trap(void);
void post_softint(void);
void post_pctx(void);
void post_sctx(void);
void post_dtsb(void);
void post_dtag(void);
void post_vawp(void);
void post_pawp(void);
void post_itsb(void);
void post_itag(void);
void post_dcache_ram(void);
void post_dcache_tag(void);
void post_icache_ram(void);
void post_icache_tag(void);
void post_icache_next(void);
void post_icache_predecode(void);

/* tests-mem.c */
void post_banks(void);
void post_mcu_init(void);
void post_ec_access(void);
void post_area_addr(void);
void post_load(void);
void post_fpu_regs(void);
void post_fpu_move(void);
void post_fsr(void);
void post_fpu_func(void);
void post_fpu_trap(void);
void post_upa(void);
void post_init_memory(void);
void post_mem_addr_ec(void);
void post_mem_addr_ecc(void);
void post_blk_addr(void);
void post_blk(void);
void post_ecc_blk(void);

/* tests-sabre.c */
void post_init_sabre(void);
void post_pio_decoder(void);
void post_byte_enable(void);
void post_imap(void);
void post_iclr(void);
void post_iommu_regs(void);
void post_iommu_ram(void);
void post_iommu_cam(void);
void post_iommu_compare(void);
void post_iommu_flush(void);
void post_pbm_config(void);
void post_pbm_ctl(void);
void post_pbm_diag(void);
void post_sabre_io(void);

/* tests-adv.c */
void post_map(void);
void post_dmmu_hit_miss(void);
void post_immu_hit_miss(void);
void post_little(void);
void post_iu_asi(void);
void post_fpu_asi(void);
void post_ec_thrash(void);
void post_data_access(void);
void post_align(void);
void post_priv(void);
void post_protect(void);
void post_init_sabre_err(void);
void post_pio_master_abort(void);
void post_pio_target_abort(void);

/* tests-pci.c */
void post_rio_ebus_config(void);
void post_rio_net_config(void);

/* tests-ebus.c */
void post_dma_rd_miss(void);
void post_dma_rd_hit(void);
void post_dma_wr_miss(void);
void post_dma_wr_hit(void);
void post_dma_pass_rd(void);
void post_dma_pass_wr(void);
void post_dma_rd_lock(void);
void post_dma_wr_lock(void);

u32 post_main(u32 arg);
void post_unexpected_trap(u32 tt, u64 tpc);

#endif
