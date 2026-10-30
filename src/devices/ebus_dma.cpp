// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (C) 2026 Andy Taylor
#include "devices/ebus_dma.h"

#include <algorithm>

namespace uv {

namespace {
constexpr unsigned kCsr = 0x0, kAddr = 0x4, kCount = 0x8;
// CSR bits software sets and clears: the enables, the direction, reset,
// burst size, diagnostics, error and terminal-count interrupt disables.
constexpr u32 kWritable = EbusDma::kIntEn | EbusDma::kReset | EbusDma::kWrite | EbusDma::kEnDma | EbusDma::kEnCnt |
                          (1u << 16) | (3u << 18) | (1u << 20) | (1u << 22) | EbusDma::kTciDis | EbusDma::kEnNext;
}  // namespace

void EbusDma::reset() {
    control_ = 0;
    addr_ = count_ = staged_count_ = next_addr_ = next_count_ = 0;
    loaded_ = next_loaded_ = tc_ = err_ = false;
    update_irq();
}

u32 EbusDma::csr() const {
    u32 v = kDevId | control_;
    if (device_irq_ || (tc_ && !(control_ & kTciDis))) v |= kIntPend;
    if (err_) v |= kErrPend;
    if (tc_) v |= kTc;
    if (loaded_) v |= kALoaded;
    if (next_loaded_) v |= kNaLoaded;
    if (loaded_ && (control_ & kEnDma)) v |= kDmaOn;
    return v;
}

void EbusDma::update_irq() {
    const bool on = (control_ & kIntEn) && (csr() & (kIntPend | kErrPend));
    if (on == irq_out_) return;
    irq_out_ = on;
    if (irq_) irq_(on);
}

void EbusDma::set_device_irq(bool asserted) {
    device_irq_ = asserted;
    update_irq();
}

bool EbusDma::read(u64 offset, unsigned size, u64& value) {
    // Little-endian registers: the byte at the lowest address is bit 7:0.
    value = 0;
    for (unsigned i = 0; i < size; ++i) {
        const unsigned a = static_cast<unsigned>(offset + i) & (kSize - 1);
        u32 r = 0;
        switch (a & ~3u) {
            case kCsr: r = csr(); break;
            case kAddr: r = addr_; break;
            case kCount: r = count_; break;
            default: break;
        }
        value = (value << 8) | ((r >> (8 * (a & 3))) & 0xFF);
    }
    return true;
}

bool EbusDma::write(u64 offset, unsigned size, u64 value) {
    // Gather the bytes written to each register, then apply each register once.
    u32 data[4]{}, mask[4]{};
    for (unsigned i = 0; i < size; ++i) {
        const unsigned a = static_cast<unsigned>(offset + i) & (kSize - 1);
        const u32 b = static_cast<u32>(value >> (8 * (size - 1 - i))) & 0xFF;
        data[a >> 2] |= b << (8 * (a & 3));
        mask[a >> 2] |= 0xFFu << (8 * (a & 3));
    }
    if (mask[kCsr >> 2]) {
        const u32 v = data[0], m = mask[0];
        control_ = (control_ & ~(m & kWritable)) | (v & m & kWritable);
        // Terminal count and the error clear when written with 1.
        if (m & v & (kTc | kIntPend)) tc_ = false;
        if (m & v & kErrPend) err_ = false;
        if (control_ & kReset) {
            loaded_ = next_loaded_ = tc_ = err_ = false;
            count_ = next_count_ = 0;
        }
    }
    // The count first, then the address, which loads the transfer.
    if (mask[kCount >> 2]) staged_count_ = ((staged_count_ & ~mask[2]) | (data[2] & mask[2])) & 0xFFFFFF;
    if (mask[kAddr >> 2] && !(control_ & kReset)) {
        if (loaded_ && (control_ & kEnNext)) {
            next_addr_ = (next_addr_ & ~mask[1]) | (data[1] & mask[1]);
            next_count_ = staged_count_;
            next_loaded_ = true;
        } else {
            addr_ = (addr_ & ~mask[1]) | (data[1] & mask[1]);
            count_ = staged_count_;
            loaded_ = true;
        }
    }
    update_irq();
    return true;
}

std::size_t EbusDma::grant(std::size_t length, bool to_memory) const {
    if (!loaded_ || err_ || !(control_ & kEnDma) || (control_ & kReset)) return 0;
    if (static_cast<bool>(control_ & kWrite) != to_memory) return 0;
    return (control_ & kEnCnt) ? std::min<std::size_t>(length, count_) : length;
}

std::size_t EbusDma::complete(std::size_t n, bool ok) {
    if (!ok) {
        err_ = true;
        update_irq();
        return 0;
    }
    addr_ += static_cast<u32>(n);
    if (control_ & kEnCnt) {
        count_ -= static_cast<u32>(n);
        if (count_ == 0) {
            tc_ = true;
            // The next transfer, if one is loaded, becomes the current one.
            loaded_ = next_loaded_;
            next_loaded_ = false;
            if (loaded_) {
                addr_ = next_addr_;
                count_ = next_count_;
            }
        }
    }
    update_irq();
    return n;
}

// Each runs on through a chained next transfer.
std::size_t EbusDma::to_memory(const u8* data, std::size_t length) {
    std::size_t done = 0;
    while (const std::size_t n = grant(length - done, true)) {
        if (!complete(n, mem_write_ && mem_write_(addr_, data + done, n))) break;
        done += n;
    }
    return done;
}

std::size_t EbusDma::from_memory(u8* data, std::size_t length) {
    std::size_t done = 0;
    while (const std::size_t n = grant(length - done, false)) {
        if (!complete(n, mem_read_ && mem_read_(addr_, data + done, n))) break;
        done += n;
    }
    return done;
}

}  // namespace uv
