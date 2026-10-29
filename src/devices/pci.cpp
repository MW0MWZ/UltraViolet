// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (C) 2026 Andy Taylor
#include "devices/pci.h"

#include "core/log.h"

namespace uv::pci {

namespace {
u32 g_decode_generation = 1;
}  // namespace

u32 decode_generation() { return g_decode_generation; }
void decode_changed() {
    if (++g_decode_generation == 0) ++g_decode_generation;
}

Function::Function(u16 vendor, u16 device, u8 revision, u32 class_code, u8 header_type) {
    set_cfg16(kVendorId, vendor);
    set_cfg16(kDeviceId, device);
    set_cfg8(kRevision, revision);
    set_cfg8(kClassProg, static_cast<u8>(class_code));
    set_cfg16(kClassProg + 1, static_cast<u16>(class_code >> 8));
    set_cfg8(kHeaderType, header_type);
    set_wmask(kCommand, 2, 0x0147);  // I/O, memory, master, parity response, SERR
    set_w1c(kStatus, 2, 0xF900);     // the error bits
    set_wmask(kCacheLine, 1, 0xFF);
    set_wmask(kLatency, 1, 0xFF);
    if ((header_type & 0x7F) == 0) set_wmask(kInterruptLine, 1, 0xFF);
    save_reset_state();
}

void Function::reset() {
    cfg_ = reset_cfg_;
    decode_changed();
}

void Function::set_wmask(u8 reg, unsigned size, u32 mask) {
    for (unsigned i = 0; i < size; ++i) wmask_[reg + i] = static_cast<u8>(mask >> (8 * i));
}

void Function::set_w1c(u8 reg, unsigned size, u32 mask) {
    for (unsigned i = 0; i < size; ++i) w1c_[reg + i] = static_cast<u8>(mask >> (8 * i));
}

void Function::define_bar(unsigned bar, u32 size, bool io, u32 reset_address, bool prefetchable) {
    bars_[bar] = {size, io};
    const u8 reg = static_cast<u8>(kBar0 + 4 * bar);
    const u32 type = io ? 1u : (prefetchable ? 8u : 0u);
    set_cfg32(reg, (reset_address & ~(size - 1)) | type);
    set_wmask(reg, 4, ~(size - 1));
    save_reset_state();
}

u32 Function::bar_address(unsigned bar) const {
    const u32 v = cfg32(static_cast<u8>(kBar0 + 4 * bar));
    return bars_[bar].io ? (v & ~3u) : (v & ~15u);
}

u32 Function::config_read(u8 reg, unsigned size) {
    u32 v = 0;
    for (unsigned i = 0; i < size; ++i) v |= u32{cfg_[reg + i]} << (8 * i);
    return v;
}

void Function::config_write(u8 reg, unsigned size, u32 value) {
    for (unsigned i = 0; i < size; ++i) {
        const u8 b = static_cast<u8>(value >> (8 * i));
        const unsigned r = reg + i;
        cfg_[r] = static_cast<u8>((cfg_[r] & ~wmask_[r]) | (b & wmask_[r]));
        cfg_[r] &= static_cast<u8>(~(b & w1c_[r]));
    }
    decode_changed();
    if (on_config_write) on_config_write();
}

int Function::decode(u32 addr, bool io) const {
    const u16 cmd = command();
    if (io ? !(cmd & kCmdIo) : !(cmd & kCmdMemory)) return -1;
    for (unsigned i = 0; i < 6; ++i) {
        const Bar& b = bars_[i];
        if (b.size == 0 || b.io != io) continue;
        const u32 base = bar_address(i);
        if (addr - base < b.size) return static_cast<int>(i);
    }
    return -1;
}

Function::Claim Function::claim(u32 addr, bool io, Decoded& d) const {
    const int b = decode(addr, io);
    if (b < 0) return Claim::None;
    d = {static_cast<unsigned>(b), bar_address(static_cast<unsigned>(b)), bars_[static_cast<unsigned>(b)].size};
    return Claim::Plain;
}

bool Function::mem_read(u32 addr, unsigned size, u64& value) {
    const int b = decode(addr, false);
    return b >= 0 && bar_read(static_cast<unsigned>(b), addr - bar_address(static_cast<unsigned>(b)), size, value);
}

bool Function::mem_write(u32 addr, unsigned size, u64 value) {
    const int b = decode(addr, false);
    return b >= 0 && bar_write(static_cast<unsigned>(b), addr - bar_address(static_cast<unsigned>(b)), size, value);
}

bool Function::io_read(u32 addr, unsigned size, u32& value) {
    const int b = decode(addr, true);
    u64 v = 0;
    if (b < 0 || !bar_read(static_cast<unsigned>(b), addr - bar_address(static_cast<unsigned>(b)), size, v)) return false;
    value = static_cast<u32>(v);
    return true;
}

bool Function::io_write(u32 addr, unsigned size, u32 value) {
    const int b = decode(addr, true);
    return b >= 0 && bar_write(static_cast<unsigned>(b), addr - bar_address(static_cast<unsigned>(b)), size, value);
}

bool Function::bar_read(unsigned, u32, unsigned, u64&) { return false; }
bool Function::bar_write(unsigned, u32, unsigned, u64) { return false; }

void Bus::attach(unsigned device, unsigned function, Function* f) {
    slots_[device & 31][function & 7] = f;
    functions_.push_back(f);
    if (auto* b = dynamic_cast<Bridge*>(f)) bridges_.push_back(b);
    decode_changed();
}

Function::Claim Bus::resolve(u32 addr, bool io, Function*& f, Function::Decoded& d) const {
    for (Function* fn : functions_) {
        if (auto* b = dynamic_cast<Bridge*>(fn)) {
            if (io ? b->forwards_io(addr) : b->forwards_mem(addr)) {
                const Function::Claim c = b->secondary_.resolve(addr, io, f, d);
                if (c != Function::Claim::None) return c;
            }
            continue;
        }
        const Function::Claim c = fn->claim(addr, io, d);
        if (c != Function::Claim::None) {
            f = fn;
            return c;
        }
    }
    return Function::Claim::None;
}

const Bus::CacheEntry* Bus::decoded(u32 addr, bool io) {
    const u32 granule = addr >> (io ? 2 : 4);
    CacheEntry& e = (io ? io_cache_ : mem_cache_)[granule & (kCacheEntries - 1)];
    const u32 gen = decode_generation();
    if (e.gen == gen && e.granule == granule) return &e;
    Function* f = nullptr;
    Function::Decoded d;
    if (resolve(addr, io, f, d) != Function::Claim::Plain) return nullptr;
    e = {granule, gen, f, d};
    return &e;
}

void Bus::reset() {
    for (Function* f : functions_) f->reset();
}

u64 Bus::config_stable(unsigned bus, unsigned device, unsigned function, u8 reg, unsigned size) const {
    if (bus == number) {
        const Function* f = slots_[device & 31][function & 7];
        return f ? f->config_stable(reg, size) : Device::kForever;  // (no function: all ones)
    }
    for (const Bridge* b : bridges_)
        if (b->claims_bus(bus)) return b->secondary_.config_stable(bus, device, function, reg, size);
    return Device::kForever;
}

u32 Bus::config_read(unsigned bus, unsigned device, unsigned function, u8 reg, unsigned size) {
    const u32 none = size == 4 ? 0xFFFFFFFFu : (1u << (8 * size)) - 1;
    if (bus == number) {
        Function* f = slots_[device & 31][function & 7];
        // A multi-function device answers for function 0 only unless it says otherwise.
        return f ? f->config_read(reg, size) : none;
    }
    for (Bridge* b : bridges_)
        if (b->claims_bus(bus)) return b->secondary().config_read(bus, device, function, reg, size);
    return none;
}

void Bus::config_write(unsigned bus, unsigned device, unsigned function, u8 reg, unsigned size, u32 value) {
    if (bus == number) {
        if (Function* f = slots_[device & 31][function & 7]) f->config_write(reg, size, value);
        return;
    }
    for (Bridge* b : bridges_)
        if (b->claims_bus(bus)) return b->secondary().config_write(bus, device, function, reg, size, value);
}

bool Bus::mem_read(u32 addr, unsigned size, u64& value) {
    // A claimed cycle the function refuses goes on to the others, as uncached.
    if (const CacheEntry* e = decoded(addr, false))
        if (e->f->bar_read(e->d.bar, addr - e->d.base, size, value)) return true;
    for (Function* f : functions_)
        if (f->mem_read(addr, size, value)) return true;
    return false;
}

u64 Bus::mem_stable(u32 addr, unsigned size) {
    const CacheEntry* e = decoded(addr, false);
    return e ? e->f->bar_stable(e->d.bar, addr - e->d.base, size) : 0;
}

bool Bus::mem_write(u32 addr, unsigned size, u64 value) {
    if (const CacheEntry* e = decoded(addr, false))
        if (e->f->bar_write(e->d.bar, addr - e->d.base, size, value)) return true;
    for (Function* f : functions_)
        if (f->mem_write(addr, size, value)) return true;
    return false;
}

bool Bus::io_read(u32 addr, unsigned size, u32& value) {
    if (const CacheEntry* e = decoded(addr, true)) {
        u64 v = 0;
        if (e->f->bar_read(e->d.bar, addr - e->d.base, size, v)) {
            value = static_cast<u32>(v);
            return true;
        }
    }
    for (Function* f : functions_)
        if (f->io_read(addr, size, value)) return true;
    return false;
}

bool Bus::io_write(u32 addr, unsigned size, u32 value) {
    if (const CacheEntry* e = decoded(addr, true))
        if (e->f->bar_write(e->d.bar, addr - e->d.base, size, value)) return true;
    for (Function* f : functions_)
        if (f->io_write(addr, size, value)) return true;
    return false;
}

Bridge::Bridge(u16 vendor, u16 device, u8 revision) : Function(vendor, device, revision, 0x060400, 0x01) {
    set_wmask(kPrimaryBus, 4, 0xFFFFFFFF);  // bus numbers and secondary latency
    set_wmask(kIoBase, 2, 0xF0F0);
    set_w1c(kSecStatus, 2, 0xF900);
    set_wmask(kMemBase, 4, 0xFFF0FFF0);
    set_wmask(kPrefBase, 4, 0xFFF0FFF0);
    set_wmask(kIoBaseUpper, 4, 0xFFFFFFFF);
    set_wmask(kBridgeControl, 2, 0x0FFF);
    save_reset_state();
}

void Bridge::reset() {
    Function::reset();
    secondary_.number = 0;
    secondary_.reset();
}

void Bridge::config_write(u8 reg, unsigned size, u32 value) {
    Function::config_write(reg, size, value);
    secondary_.number = cfg8(kSecondaryBus);
}

bool Bridge::forwards_mem(u32 addr) const {
    if (!(command() & kCmdMemory)) return false;
    const u32 base = u32{cfg16(kMemBase) & 0xFFF0u} << 16;
    const u32 limit = (u32{cfg16(kMemLimit) & 0xFFF0u} << 16) | 0xFFFFF;
    const u32 pbase = u32{cfg16(kPrefBase) & 0xFFF0u} << 16;
    const u32 plimit = (u32{cfg16(kPrefLimit) & 0xFFF0u} << 16) | 0xFFFFF;
    return (addr >= base && addr <= limit) || (addr >= pbase && addr <= plimit);
}

bool Bridge::forwards_io(u32 addr) const {
    if (!(command() & kCmdIo)) return false;
    const u32 base = (u32{cfg16(kIoBaseUpper)} << 16) | (u32{cfg8(kIoBase) & 0xF0u} << 8);
    const u32 limit = (u32{cfg16(kIoLimitUpper)} << 16) | (u32{cfg8(kIoLimit) & 0xF0u} << 8) | 0xFFF;
    return addr >= base && addr <= limit;
}

bool Bridge::mem_read(u32 addr, unsigned size, u64& value) {
    return forwards_mem(addr) && secondary_.mem_read(addr, size, value);
}
bool Bridge::mem_write(u32 addr, unsigned size, u64 value) {
    return forwards_mem(addr) && secondary_.mem_write(addr, size, value);
}
bool Bridge::io_read(u32 addr, unsigned size, u32& value) {
    return forwards_io(addr) && secondary_.io_read(addr, size, value);
}
bool Bridge::io_write(u32 addr, unsigned size, u32 value) {
    return forwards_io(addr) && secondary_.io_write(addr, size, value);
}

}  // namespace uv::pci
