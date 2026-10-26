// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (C) 2026 Andy Taylor
// SPARC V9 disassembler (with the UltraSPARC registers and ASIs), in the
// usual assembler syntax. Branch and call targets are shown as addresses.
#pragma once

#include <string>

#include "core/types.h"

namespace uv::usparc {

std::string disassemble(u32 insn, u64 pc);

}  // namespace uv::usparc
