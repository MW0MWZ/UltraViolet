// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (C) 2026 Andy Taylor
// Physical address space: the UltraSPARC IIi's 41-bit physical addresses.
// A two-level page table (256 MiB chunks of 64 KiB pages) resolves each
// physical page to either host memory (RAM/ROM, accessed directly) or a
// Device (MMIO, dispatched virtually). Chunks nothing is mapped in share one
// empty table, so the fast path has no null checks; RAM accesses never leave
// it.
#pragma once

#include <array>
#include <functional>
#include <memory>
#include <vector>

#include "core/endian.h"
#include "core/types.h"

namespace uv {

class Device {
public:
    virtual ~Device() = default;
    virtual const char* name() const = 0;

    // `offset` is relative to the base address the device was mapped at.
    // `size` is 1, 2, 4 or 8 and accesses are naturally aligned. Values are
    // plain integers in the bus's (big-endian) order: a little-endian access
    // has already been swapped back by the CPU. Return false to signal a bus
    // error to the requester.
    virtual bool read(u64 offset, unsigned size, u64& value) = 0;
    virtual bool write(u64 offset, unsigned size, u64 value) = 0;

    // The guest time (in cycles) until which reading here returns the value
    // it does now and changes nothing: kForever if only a write, a scheduled
    // event or input delivered between run slices can change it; 0 if the
    // device promises nothing (the default). The processor may then run a
    // loop polling it many iterations at once.
    // kClock: the register counts guest time: reading it changes nothing,
    // and its value, as read (big-endian), never falls as guest time goes
    // on (and depends on nothing else that can change but by a write or a
    // scheduled event); kClockLittle: the same with its bytes reversed
    // (read little-endian).
    static constexpr u64 kForever = ~u64{0}, kClock = ~u64{0} - 1, kClockLittle = ~u64{0} - 2;
    virtual u64 stable_until(u64 offset, unsigned size) const {
        (void)offset;
        (void)size;
        return 0;
    }
};

class Bus {
public:
    static constexpr unsigned kAddressBits = 41;
    static constexpr u64 kAddressMask = (u64{1} << kAddressBits) - 1;
    static constexpr unsigned kPageShift = 16;
    static constexpr u64 kPageSize = u64{1} << kPageShift;
    static constexpr u64 kPageMask = kPageSize - 1;
    static constexpr unsigned kChunkShift = 28;
    static constexpr unsigned kPagesPerChunk = 1u << (kChunkShift - kPageShift);
    static constexpr unsigned kNumChunks = 1u << (kAddressBits - kChunkShift);

    Bus();
    Bus(const Bus&) = delete;
    Bus& operator=(const Bus&) = delete;

    // Reads of the devices mapped at [base, base + size) stall the reader
    // for `cycles` CPU cycles a word (twice that for a doubleword): the
    // time an uncached read across a slower bus takes. The stall is left in
    // `read_stall` for the CPU to charge. Set after the devices are mapped.
    void set_read_stall(u64 base, u64 size, u16 cycles);
    u32 read_stall = 0;

    // Base and size must be multiples of kPageSize and lie within the 41-bit
    // space. Later mappings replace earlier ones; the remap handler is told
    // (cached host pointers, such as the CPU's soft TLB, must then be dropped).
    void map_ram(u64 base, u64 size, u8* host, bool writable = true);
    void map_device(u64 base, u64 size, Device* device);
    void unmap(u64 base, u64 size);
    void set_remap_handler(std::function<void()> handler) { remap_handler_ = std::move(handler); }

    // Host pointer to `paddr` if it is backed by host memory, else nullptr.
    // The pointer is valid to the end of the containing bus page.
    u8* host_read_ptr(u64 paddr) const;
    u8* host_write_ptr(u64 paddr) const;  // nullptr for ROM as well as MMIO

    // Called for accesses that nothing answers (unmapped space, or a device
    // returning false). Reads then fail (the CPU takes a data access error);
    // writes are posted and complete normally, so error reporting for them is
    // up to the handler.
    using ErrorHandler = std::function<void(u64 paddr, unsigned size, bool is_write)>;
    void set_error_handler(ErrorHandler handler) { error_handler_ = std::move(handler); }
    // Called for writes to ROM (host memory mapped read-only); they change
    // nothing in it (a flash device may take them as commands).
    using RomWriteHandler = std::function<void(u64 paddr, unsigned size, u64 value)>;
    void set_rom_write_handler(RomWriteHandler handler) { rom_write_handler_ = std::move(handler); }

    // Code tracking for the CPU's decoded-block cache. Host-backed memory has
    // one flag per 4 KiB ("decoded code was taken from here"), shared by all
    // physical aliases of the same host memory. Any write to a flagged page,
    // by the CPU or a device, calls the handler with the host address of the
    // start of that 4 KiB page so cached code can be dropped.
    static constexpr unsigned kCodePageShift = 12;
    using CodeWriteHandler = std::function<void(const u8* host_page)>;
    void set_code_write_handler(CodeWriteHandler handler) { code_write_handler_ = std::move(handler); }
    // Declares a block of host memory that will be mapped (in pieces, or more
    // than once). Optional, but it guarantees every mapping of it shares one
    // set of code flags however the pieces are later arranged.
    void add_host_region(u8* host, u64 size) { region_for(host, size); }
    // Flag byte for the 4 KiB page holding `host` (host-backed memory only).
    u8* code_flag(const u8* host) const;
    void code_written(const u8* page) const {
        if (code_write_handler_) code_write_handler_(page);
    }
    // Device DMA into memory: copies and invalidates cached code it overwrites.
    // Returns false if any of it is not writable host memory.
    bool write_block(u64 paddr, const u8* data, std::size_t length);
    // Device DMA from memory. Returns false if any of it is not host memory.
    bool read_block(u64 paddr, u8* data, std::size_t length) const;

    // See Device::stable_until. Host memory (RAM and ROM) changes only by
    // writes and DMA, which run between slices or are writes themselves.
    u64 stable_until(u64 paddr, unsigned size) const;

    // Accesses that went past host memory (to a device, ROM writes and bus
    // errors): one that leaves this unchanged reached host memory.
    u64 slow_accesses() const { return slow_accesses_; }
    // Device DMA into memory (write_block calls).
    u64 dma_writes() const { return dma_writes_; }

    // Debugger hook: every device (MMIO) read, after it completes.
    std::function<void(u64 paddr, unsigned size, u64 value)> device_read_hook;
    // Debugger hook: every device (MMIO) write, before the device sees it.
    std::function<void(u64 paddr, unsigned size, u64 value)> device_write_hook;

    template <typename T>
    UV_FORCEINLINE bool read(u64 paddr, T& out) {
        if (UV_LIKELY(paddr <= kAddressMask)) {
            const Page& p = page(paddr);
            if (UV_LIKELY(p.host != nullptr)) {
                out = load_be<T>(p.host + (paddr & kPageMask));
                return true;
            }
        }
        u64 v = 0;
        if (!read_slow(paddr, sizeof(T), v)) return false;
        out = static_cast<T>(v);
        return true;
    }

    template <typename T>
    UV_FORCEINLINE bool write(u64 paddr, T value) {
        if (UV_LIKELY(paddr <= kAddressMask)) {
            const Page& p = page(paddr);
            if (UV_LIKELY(p.host != nullptr && p.writable)) {
                u8* const h = p.host + (paddr & kPageMask);
                store_be<T>(h, value);
                if (UV_UNLIKELY(p.code[(paddr & kPageMask) >> kCodePageShift]))
                    code_written(h - (paddr & ((u64{1} << kCodePageShift) - 1)));
                return true;
            }
        }
        return write_slow(paddr, sizeof(T), value);
    }

private:
    struct Page {
        u8* host = nullptr;
        bool writable = false;
        Device* device = nullptr;
        u64 device_base = 0;
        u8* code = nullptr;  // code flags for this page's 4 KiB pieces (host-backed only)
        u16 read_stall = 0;  // CPU cycles a device word read waits (see set_read_stall)
    };
    using Chunk = std::array<Page, kPagesPerChunk>;
    // A distinct piece of host memory, however many times it is mapped.
    struct Region {
        u8* host;
        u64 size;
        std::unique_ptr<u8[]> code;
    };

    UV_FORCEINLINE const Page& page(u64 paddr) const {
        return (*chunks_[paddr >> kChunkShift])[(paddr >> kPageShift) & (kPagesPerChunk - 1)];
    }
    Page& page_for_write(u64 paddr);
    Region& region_for(u8* host, u64 size);
    const Region* find_region(const u8* host) const;

    void check_range(u64 base, u64 size) const;
    bool read_slow(u64 paddr, unsigned size, u64& out);
    bool write_slow(u64 paddr, unsigned size, u64 value);
    u64 slow_accesses_ = 0;
    u64 dma_writes_ = 0;

    std::unique_ptr<Chunk> empty_;               // shared by chunks nothing is mapped in
    std::vector<Chunk*> chunks_;                 // kNumChunks entries, each empty_ or owned
    std::vector<std::unique_ptr<Chunk>> owned_;
    std::vector<std::unique_ptr<Region>> regions_;
    ErrorHandler error_handler_;
    RomWriteHandler rom_write_handler_;
    CodeWriteHandler code_write_handler_;
    std::function<void()> remap_handler_;
};

}  // namespace uv
