// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (C) 2026 Andy Taylor
#include "devices/ebus_misc.h"

#include <algorithm>

#include "core/log.h"

namespace uv {

namespace {
// Registers 0x00-0x0F as a real Ultra 10 reads them at the ok prompt. OBP
// writes 0x00-0x07 (FER 0x0F, FAR 0x11, PTR 0x80, the others 0) yet some of
// their bits read back as 1: those bits are taken as reading 1 whatever is
// written. 0x09-0x0F are not written by OBP and are the power-on values.
constexpr u8 kSuperIoReadOnes[8] = {0xC0, 0x00, 0x00, 0x85, 0x80, 0x00, 0x09, 0x40};
constexpr u8 kSuperIoPowerOn[16] = {0x00, 0x00, 0x00, 0x00, 0x00, 0x36, 0x00, 0x00,
                                    0x00, 0xCA, 0x00, 0xC7, 0x00, 0xC7, 0x0C, 0x3E};
}  // namespace

SuperIoConfig::SuperIoConfig(u8 sid) : sid_(sid) { reset(); }

void SuperIoConfig::reset() {
    std::fill(std::begin(regs_), std::end(regs_), 0);
    std::copy(std::begin(kSuperIoPowerOn), std::end(kSuperIoPowerOn), regs_);
    regs_[8] = sid_;  // SID: the chip's identification
    index_ = 0;
    first_read_ = true;
}

bool SuperIoConfig::read(u64 offset, unsigned, u64& value) {
    if ((offset & 1) == 0) {
        value = first_read_ ? 0x88 : index_;
        first_read_ = false;
    } else {
        value = regs_[index_] | (index_ < 8 ? kSuperIoReadOnes[index_] : 0);
    }
    return true;
}

bool SuperIoConfig::write(u64 offset, unsigned, u64 value) {
    const u8 v = static_cast<u8>(value);
    if ((offset & 1) == 0) {
        index_ = v;
        first_read_ = false;
    } else if (index_ != 8) {
        // Data writes are made twice in succession on the real chip; the
        // second carries the value, so taking each is equivalent.
        regs_[index_] = v;
    }
    return true;
}

FlashProm::FlashProm(std::vector<u8> image, u8 manufacturer, u8 device, u32 sector_size)
    : image_(std::move(image)), manufacturer_(manufacturer), device_(device), sector_size_(sector_size) {}

u8 FlashProm::read_byte(u32 addr) const {
    if (image_.empty()) return 0xFF;
    if (state_ == State::Autoselect) {
        switch (addr & 3) {
            case 0: return manufacturer_;
            case 1: return device_;
            case 2: return 0;  // sector protection: none
            default: return 0;
        }
    }
    return image_[addr % image_.size()];
}

void FlashProm::write_byte(u32 addr, u8 v) {
    const bool was_array = array_mode();
    write_command(addr, v);
    if (array_mode() != was_array && on_mode_change) on_mode_change();
}

void FlashProm::write_command(u32 addr, u8 v) {
    const u32 a11 = addr & 0x7FF;
    auto is555 = [&] { return a11 == 0x555 || (addr & 0x7FFF) == 0x5555; };
    auto is2aa = [&] { return a11 == 0x2AA || (addr & 0x7FFF) == 0x2AAA; };
    if (v == 0xF0) {
        state_ = State::Read;
        return;
    }
    switch (state_) {
        case State::Read:
        case State::Autoselect:
            state_ = (v == 0xAA && is555()) ? State::Unlock1 : (state_ == State::Autoselect ? State::Autoselect : State::Read);
            break;
        case State::Unlock1: state_ = (v == 0x55 && is2aa()) ? State::Unlock2 : State::Read; break;
        case State::Unlock2:
            if (!is555()) state_ = State::Read;
            else if (v == 0xA0) state_ = State::Program;
            else if (v == 0x80) state_ = State::Erase1;
            else if (v == 0x90) state_ = State::Autoselect;
            else state_ = State::Read;
            break;
        case State::Program:
            if (!image_.empty()) {
                u8& b = image_[addr % image_.size()];
                b &= v;  // programming only clears bits
                dirty_ = true;
            }
            state_ = State::Read;
            break;
        case State::Erase1: state_ = (v == 0xAA && is555()) ? State::Erase2 : State::Read; break;
        case State::Erase2: state_ = (v == 0x55 && is2aa()) ? State::Erase3 : State::Read; break;
        case State::Erase3:
            if (v == 0x10 && is555()) {
                std::fill(image_.begin(), image_.end(), 0xFF);
                dirty_ = true;
            } else if (v == 0x30 && !image_.empty()) {
                const u32 start = (addr % static_cast<u32>(image_.size())) / sector_size_ * sector_size_;
                std::fill(image_.begin() + start, image_.begin() + std::min<std::size_t>(start + sector_size_, image_.size()), 0xFF);
                dirty_ = true;
            }
            state_ = State::Read;
            break;
    }
}

bool FlashProm::read(u64 offset, unsigned size, u64& value) {
    value = 0;
    for (unsigned i = 0; i < size; ++i) value = (value << 8) | read_byte(static_cast<u32>(offset + i));
    return true;
}

bool FlashProm::write(u64 offset, unsigned size, u64 value) {
    for (unsigned i = 0; i < size; ++i) write_byte(static_cast<u32>(offset + i), static_cast<u8>(value >> (8 * (size - 1 - i))));
    return true;
}

RegisterBlock::RegisterBlock(std::string name, std::size_t size, u8 fill)
    : name_(std::move(name)), bytes_(size, fill), seen_(size, false) {}

bool RegisterBlock::read(u64 offset, unsigned size, u64& value) {
    value = 0;
    for (unsigned i = 0; i < size; ++i) value = (value << 8) | bytes_[(offset + i) % bytes_.size()];
    const std::size_t at = static_cast<std::size_t>(offset % bytes_.size());
    if (!seen_[at]) {
        seen_[at] = true;
        UV_LOG(Debug, "ebus", "%s: read%u +%llx -> %llx", name_.c_str(), size * 8, static_cast<unsigned long long>(offset),
               static_cast<unsigned long long>(value));
    }
    return true;
}

bool RegisterBlock::write(u64 offset, unsigned size, u64 value) {
    for (unsigned i = 0; i < size; ++i) bytes_[(offset + i) % bytes_.size()] = static_cast<u8>(value >> (8 * (size - 1 - i)));
    const std::size_t at = static_cast<std::size_t>(offset % bytes_.size());
    if (!seen_[at]) {
        seen_[at] = true;
        UV_LOG(Debug, "ebus", "%s: write%u +%llx <- %llx", name_.c_str(), size * 8, static_cast<unsigned long long>(offset),
               static_cast<unsigned long long>(value));
    }
    return true;
}

}  // namespace uv
