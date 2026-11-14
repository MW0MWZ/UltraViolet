// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (C) 2026 Andy Taylor
#include "devices/ecpp.h"

#include "devices/ebus_dma.h"
#include "devices/ebus_misc.h"

namespace uv {

namespace {
constexpr unsigned kPcr = 4;            // SuperIO configuration: printer control
constexpr u8 kPcrEcp = 0x04;            // PCR: ECP enable
constexpr u8 kStatusIdle = 0x7F;        // the status lines with nothing connected (an Ultra 10's)
constexpr u8 kControlDir = 0x20;        // DCR: backward direction
constexpr u8 kEcrDma = 0x08, kEcrService = 0x04, kEcrFull = 0x02, kEcrEmpty = 0x01;
constexpr u8 kCnfgA = 0x10, kCnfgB = 0x08;  // CNFGB: IRQ7 configured, its line low
constexpr unsigned kModeStandard = 0, kModePs2 = 1, kModeParallelFifo = 2, kModeEcpFifo = 3, kModeTest = 6,
                   kModeConfig = 7;
}  // namespace

Ecpp::Ecpp(const SuperIoConfig& config, EbusDma& dma) : config_(config), dma_(dma) { reset(); }

void Ecpp::reset() {
    data_ = 0;
    control_ = 0;
    for (u8& b : epp_) b = 0;
    ecr_ = 0x15;  // (the ECR's reset value)
    fifo_.clear();
}

bool Ecpp::ecp_enabled() const { return config_.reg(kPcr) & kPcrEcp; }

// The direction bit counts in modes other than 000 and 010.
bool Ecpp::backward() const {
    return (control_ & kControlDir) && mode() != kModeStandard && mode() != kModeParallelFifo;
}

bool Ecpp::read(u64 offset, unsigned size, u64& value) {
    value = 0;
    for (unsigned i = 0; i < size; ++i) value = value << 8 | read_byte(static_cast<unsigned>(offset + i) % kSize);
    return true;
}

bool Ecpp::write(u64 offset, unsigned size, u64 value) {
    for (unsigned i = 0; i < size; ++i)
        write_byte(static_cast<unsigned>(offset + i) % kSize, static_cast<u8>(value >> (8 * (size - 1 - i))));
    return true;
}

u8 Ecpp::read_byte(unsigned offset) {
    switch (offset) {
        case 0: return data_;
        case 1: return kStatusIdle;
        case 2:
            // Bits 7:6 read 1; bit 5 too while the ECP is off (an Ultra 10's
            // reads 0xE0), else the direction as written.
            if (!ecp_enabled()) return control_ | 0xE0;
            return static_cast<u8>((control_ & 0x3F & (backward() ? 0xFF : ~kControlDir)) | 0xC0);
        default: return epp_[offset - 3];
    }
}

void Ecpp::write_byte(unsigned offset, u8 v) {
    switch (offset) {
        case 0: data_ = v; break;
        case 1: break;
        case 2:
            control_ = v & 0x3F;
            service();
            break;
        default: epp_[offset - 3] = v; break;
    }
}

bool Ecpp::Extended::read(u64 offset, unsigned size, u64& value) {
    value = 0;
    for (unsigned i = 0; i < size; ++i) value = value << 8 | port_.read_extended(static_cast<unsigned>(offset + i));
    return true;
}

bool Ecpp::Extended::write(u64 offset, unsigned size, u64 value) {
    for (unsigned i = 0; i < size; ++i)
        port_.write_extended(static_cast<unsigned>(offset + i), static_cast<u8>(value >> (8 * (size - 1 - i))));
    return true;
}

// With the ECP off its registers are not decoded: they float as the
// SuperIO's undecoded addresses do (an Ultra 10's reads 0x76).
u8 Ecpp::read_extended(unsigned offset) {
    if (!ecp_enabled()) return FloatingBus::kSuperIo;
    switch (offset) {
        case 0:
            if (mode() == kModeConfig) return kCnfgA;
            if (mode() == kModeTest || (mode() == kModeEcpFifo && backward())) {
                if (fifo_.empty()) return FloatingBus::kSuperIo;  // (an underrun: undefined)
                const u8 b = fifo_.front();
                fifo_.pop_front();
                service();
                return b;
            }
            return FloatingBus::kSuperIo;  // (write-only or not accessible in this mode)
        case 1: return mode() == kModeConfig ? kCnfgB : FloatingBus::kSuperIo;
        case 2: {
            service();
            u8 v = ecr_ & 0xFC;
            if (fifo_.size() >= kFifo) v |= kEcrFull;
            if (fifo_.empty()) v |= kEcrEmpty;
            return v;
        }
        default: return FloatingBus::kSuperIo;
    }
}

void Ecpp::write_extended(unsigned offset, u8 v) {
    if (!ecp_enabled()) return;
    switch (offset) {
        case 0: {
            const bool fifo_write = mode() == kModeTest || mode() == kModeParallelFifo ||
                                    (mode() == kModeEcpFifo && !backward());
            if (fifo_write && fifo_.size() < kFifo) fifo_.push_back(v);  // (an overrun is ignored)
            break;
        }
        case 2: {
            // Modes 000 and 001 empty the FIFO; the full and empty bits are
            // read-only.
            ecr_ = v & 0xFC;
            if (mode() == kModeStandard || mode() == kModePs2) fifo_.clear();
            service();
            break;
        }
        default: break;
    }
}

// DMA (ECR bit 3 set, the service bit clear) in the FIFO modes: forward
// fills the FIFO from memory, backward empties it to memory (in test mode,
// the only backward mode with data to move here). The channel's terminal
// count sets the service bit, which stops the DMA.
void Ecpp::service() {
    if (!ecp_enabled() || !(ecr_ & kEcrDma) || (ecr_ & kEcrService)) return;
    const unsigned m = mode();
    if (m != kModeTest && m != kModeParallelFifo && m != kModeEcpFifo) return;
    u8 buf[kFifo];
    if (!backward()) {
        if (const std::size_t room = kFifo - fifo_.size()) {
            const std::size_t got = dma_.from_memory(buf, room);
            fifo_.insert(fifo_.end(), buf, buf + got);
        }
    } else if (m == kModeTest && !fifo_.empty()) {
        std::size_t n = 0;
        for (u8 b : fifo_) buf[n++] = b;
        const std::size_t sent = dma_.to_memory(buf, n);
        fifo_.erase(fifo_.begin(), fifo_.begin() + static_cast<std::ptrdiff_t>(sent));
    }
    if (dma_.csr() & EbusDma::kTc) ecr_ |= kEcrService;
}

}  // namespace uv
