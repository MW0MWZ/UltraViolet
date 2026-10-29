// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (C) 2026 Andy Taylor
// Test rig: a processor and RAM for running short instruction sequences.
#pragma once

#include <initializer_list>
#include <vector>

#include "cpu/ultrasparc/ultrasparc.h"
#include "mem/bus.h"
#include "sparc_asm.h"

namespace uv::test {

using namespace uv::usparc;
using namespace uv::test::sparc;

// A processor with 1 MiB of RAM at physical 0, in privileged mode at TL 0
// with the MMUs off, running code placed at 0x1000. Traps vector to TBA
// 0x8000.
struct Rig {
    static constexpr u64 kCode = 0x1000, kTba = 0x8000;
    std::vector<u8> ram = std::vector<u8>(1 << 20);
    Bus bus;
    UltraSparc cpu{bus};

    Rig() {
        bus.map_ram(0, ram.size(), ram.data());
        cpu.set_tl(0);
        cpu.set_pstate(pstate::PRIV | pstate::PEF);
        cpu.set_window_state(6, 0, 7, 0, 0);
        // TBA is out of simm13 range: build it in %g7, then clear %g7.
        put(0x100, {SETHI(g7, kTba), WRPR(prTBA, g7, g0), MOVi(g7, 0), WRASRi(6, g0, 4) /* FPRS.FEF */});
        run_at(0x100, 4);
    }
    void put(u64 addr, std::initializer_list<u32> code) {
        for (u32 w : code) {
            bus.write(addr, w);  // through the bus, so decoded copies of it are dropped
            addr += 4;
        }
    }
    void run_at(u64 addr, u64 n) {
        cpu.set_pc(addr);
        cpu.run(n);
    }
    // Places `code` at kCode and runs it (n instructions).
    void exec(std::initializer_list<u32> code, u64 n = 0) {
        put(kCode, code);
        run_at(kCode, n ? n : code.size());
    }
    u64 load64(u64 addr) const { return load_be<u64>(ram.data() + addr); }
};


}  // namespace uv::test
