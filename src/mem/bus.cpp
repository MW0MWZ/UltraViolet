// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (C) 2026 Andy Taylor
#include "mem/bus.h"

#include <algorithm>
#include <cstring>
#include <stdexcept>

#include "core/log.h"

namespace uv {

Bus::Bus() : empty_(std::make_unique<Chunk>()), chunks_(kNumChunks, empty_.get()) {}

void Bus::check_range(u64 base, u64 size) const {
    if ((base & kPageMask) != 0 || (size & kPageMask) != 0 || size == 0 || base > kAddressMask ||
        size > kAddressMask + 1 - base) {
        throw std::invalid_argument("Bus mapping must be 64 KiB aligned and within the 41-bit space");
    }
}

Bus::Page& Bus::page_for_write(u64 paddr) {
    Chunk*& c = chunks_[paddr >> kChunkShift];
    if (c == empty_.get()) {
        owned_.push_back(std::make_unique<Chunk>());
        c = owned_.back().get();
    }
    return (*c)[(paddr >> kPageShift) & (kPagesPerChunk - 1)];
}

const Bus::Region* Bus::find_region(const u8* host) const {
    for (const auto& r : regions_)
        if (host >= r->host && host < r->host + r->size) return r.get();
    return nullptr;
}

// Aliases of memory already mapped share its region (and code flags).
Bus::Region& Bus::region_for(u8* host, u64 size) {
    for (auto& r : regions_)
        if (host >= r->host && host + size <= r->host + r->size) return *r;
    const u64 pages = (size + (u64{1} << kCodePageShift) - 1) >> kCodePageShift;
    const u64 lines = (size + (u64{1} << kCodeLineShift) - 1) >> kCodeLineShift;
    auto r = std::make_unique<Region>(Region{host, size, std::make_unique<u8[]>(pages), std::make_unique<u8[]>(lines)});
    regions_.push_back(std::move(r));
    return *regions_.back();
}

void Bus::map_ram(u64 base, u64 size, u8* host, bool writable) {
    check_range(base, size);
    Region& r = region_for(host, size);
    for (u64 off = 0; off < size; off += kPageSize) {
        u8* const code = r.code.get() + ((host + off - r.host) >> kCodePageShift);
        page_for_write(base + off) = Page{host + off, writable, nullptr, 0, code};
    }
    if (remap_handler_) remap_handler_();
}

u8* Bus::code_flag(const u8* host) const {
    const Region* r = find_region(host);
    return r ? r->code.get() + ((host - r->host) >> kCodePageShift) : nullptr;
}

void Bus::mark_code(const u8* host, std::size_t size) {
    const Region* r = find_region(host);
    if (!r || size == 0) return;
    const u64 off = static_cast<u64>(host - r->host);
    r->code[off >> kCodePageShift] = 1;
    for (u64 l = off >> kCodeLineShift; l <= (off + size - 1) >> kCodeLineShift; ++l) r->lines[l] = 1;
}

bool Bus::code_lines(const u8* host, std::size_t size) const {
    const Region* r = find_region(host);
    if (!r || size == 0) return false;
    const u64 off = static_cast<u64>(host - r->host);
    for (u64 l = off >> kCodeLineShift; l <= (off + size - 1) >> kCodeLineShift; ++l)
        if (r->lines[l]) return true;
    return false;
}

void Bus::clear_code(const u8* host_page) {
    const Region* r = find_region(host_page);
    if (!r) return;
    const u64 off = static_cast<u64>(host_page - r->host) & ~((u64{1} << kCodePageShift) - 1);
    r->code[off >> kCodePageShift] = 0;
    std::memset(r->lines.get() + (off >> kCodeLineShift), 0, std::size_t{1} << (kCodePageShift - kCodeLineShift));
}

bool Bus::write_block(u64 paddr, const u8* data, std::size_t length) {
    constexpr u64 kCodeMask = (u64{1} << kCodePageShift) - 1;
    ++dma_writes_;
    while (length) {
        if (paddr > kAddressMask) return false;
        const Page& p = page(paddr);
        if (!p.host || !p.writable) return false;
        const u64 in_page = paddr & kPageMask;
        const std::size_t chunk = static_cast<std::size_t>(std::min<u64>(length, kCodeMask + 1 - (in_page & kCodeMask)));
        u8* const h = p.host + in_page;
        std::memcpy(h, data, chunk);
        if (p.code[in_page >> kCodePageShift]) code_written(h - (in_page & kCodeMask), h, chunk);
        paddr += chunk;
        data += chunk;
        length -= chunk;
    }
    return true;
}

bool Bus::read_block(u64 paddr, u8* data, std::size_t length) const {
    while (length) {
        if (paddr > kAddressMask) return false;
        const Page& p = page(paddr);
        if (!p.host) return false;
        const u64 in_page = paddr & kPageMask;
        const std::size_t chunk = static_cast<std::size_t>(std::min<u64>(length, kPageSize - in_page));
        std::memcpy(data, p.host + in_page, chunk);
        paddr += chunk;
        data += chunk;
        length -= chunk;
    }
    return true;
}

void Bus::map_device(u64 base, u64 size, Device* device) {
    check_range(base, size);
    for (u64 off = 0; off < size; off += kPageSize) page_for_write(base + off) = Page{nullptr, false, device, base};
    if (remap_handler_) remap_handler_();
}

void Bus::unmap(u64 base, u64 size) {
    check_range(base, size);
    for (u64 off = 0; off < size; off += kPageSize) {
        if (chunks_[(base + off) >> kChunkShift] != empty_.get()) page_for_write(base + off) = Page{};
    }
    if (remap_handler_) remap_handler_();
}

u8* Bus::host_read_ptr(u64 paddr) const {
    if (paddr > kAddressMask) return nullptr;
    const Page& p = page(paddr);
    return p.host ? p.host + (paddr & kPageMask) : nullptr;
}

u8* Bus::host_write_ptr(u64 paddr) const {
    if (paddr > kAddressMask) return nullptr;
    const Page& p = page(paddr);
    return (p.host && p.writable) ? p.host + (paddr & kPageMask) : nullptr;
}

u64 Bus::stable_until(u64 paddr, unsigned size) const {
    if (paddr > kAddressMask) return 0;
    const Page& p = page(paddr);
    if (p.host) return Device::kForever;
    return p.device ? p.device->stable_until(paddr - p.device_base, size) : 0;
}

void Bus::set_read_stall(u64 base, u64 size, u16 cycles) {
    check_range(base, size);
    for (u64 a = base; a < base + size; a += kPageSize) page_for_write(a).read_stall = cycles;
}

bool Bus::read_slow(u64 paddr, unsigned size, u64& out) {
    ++slow_accesses_;
    if (paddr <= kAddressMask) {
        const Page& p = page(paddr);
        if (p.device && p.device->read(paddr - p.device_base, size, out)) {
            read_stall += size > 4 ? 2u * p.read_stall : p.read_stall;
            if (device_read_hook) [[unlikely]]
                device_read_hook(paddr, size, out);
            return true;
        }
    }
    UV_LOG(Debug, "bus", "bus error: read%u @ %011llx", size * 8, static_cast<unsigned long long>(paddr));
    if (error_handler_) error_handler_(paddr, size, false);
    return false;
}

bool Bus::write_slow(u64 paddr, unsigned size, u64 value) {
    ++slow_accesses_;
    if (paddr <= kAddressMask) {
        const Page& p = page(paddr);
        if (p.device) {
            if (device_write_hook) [[unlikely]]
                device_write_hook(paddr, size, value);
            if (p.device->write(paddr - p.device_base, size, value)) return true;
            UV_LOG(Debug, "bus", "bus error: write%u @ %011llx", size * 8, static_cast<unsigned long long>(paddr));
            if (error_handler_) error_handler_(paddr, size, true);
            return true;
        }
        if (p.host) {
            UV_LOG(Debug, "bus", "write%u to ROM @ %011llx ignored", size * 8, static_cast<unsigned long long>(paddr));
            if (rom_write_handler_) rom_write_handler_(paddr, size, value);
            return true;
        }
    }
    UV_LOG(Debug, "bus", "bus error: write%u @ %011llx = %llx", size * 8, static_cast<unsigned long long>(paddr),
           static_cast<unsigned long long>(value));
    if (error_handler_) error_handler_(paddr, size, true);
    return true;
}

}  // namespace uv
