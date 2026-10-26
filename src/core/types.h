// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (C) 2026 Andy Taylor
// UltraViolet - fundamental types and helpers.
#pragma once

#include <cstddef>
#include <cstdint>

namespace uv {

using u8 = std::uint8_t;
using u16 = std::uint16_t;
using u32 = std::uint32_t;
using u64 = std::uint64_t;
using s8 = std::int8_t;
using s16 = std::int16_t;
using s32 = std::int32_t;
using s64 = std::int64_t;

constexpr u64 sext8(u64 v) { return static_cast<u64>(static_cast<s64>(static_cast<s8>(v))); }
constexpr u64 sext16(u64 v) { return static_cast<u64>(static_cast<s64>(static_cast<s16>(v))); }
constexpr u64 sext32(u64 v) { return static_cast<u64>(static_cast<s64>(static_cast<s32>(v))); }

}  // namespace uv

#if defined(__GNUC__) || defined(__clang__)
#define UV_LIKELY(x) __builtin_expect(!!(x), 1)
#define UV_UNLIKELY(x) __builtin_expect(!!(x), 0)
#define UV_FORCEINLINE inline __attribute__((always_inline))
#define UV_NOINLINE __attribute__((noinline))
// MinGW checks formats against the C99 printf it is built with
// (__USE_MINGW_ANSI_STDIO), not Microsoft's.
#if defined(__MINGW32__)
#define UV_PRINTF(fmt_idx, arg_idx) __attribute__((format(gnu_printf, fmt_idx, arg_idx)))
#else
#define UV_PRINTF(fmt_idx, arg_idx) __attribute__((format(printf, fmt_idx, arg_idx)))
#endif
#elif defined(_MSC_VER)
#define UV_LIKELY(x) (x)
#define UV_UNLIKELY(x) (x)
#define UV_FORCEINLINE __forceinline
#define UV_NOINLINE __declspec(noinline)
#define UV_PRINTF(fmt_idx, arg_idx)
#else
#define UV_LIKELY(x) (x)
#define UV_UNLIKELY(x) (x)
#define UV_FORCEINLINE inline
#define UV_NOINLINE
#define UV_PRINTF(fmt_idx, arg_idx)
#endif
