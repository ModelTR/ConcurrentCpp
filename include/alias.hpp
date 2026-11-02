/**
 * @file include/alias.hpp
 * @author modleTR (mxtian_ran@163.com)
 * @brief Alias for signed/unsigned intergal, atomic signed/unsigned intergal
 * @version 0.1
 * @date 2026-11-02
 * @copyright MIT License (c) 2026
 * 
 */

/// @note For std::int8_t, std::int16_t, ..., std::uint8_t, std::uint16_t
#include <cstdint>

/// @note For template std::atomic<T>
#include <atomic>

/// Signed intergal
using i8 = std::int8_t;
using i16 = std::int16_t;
using i32 = std::int32_t;
using i64 = std::int64_t;

/// Unsigned intergal
using u8 = std::uint8_t;
using u16 = std::uint16_t;
using u32 = std::uint32_t;
using u64 = std::uint64_t;

/// Atomic signed and unsigned intergal
namespace atomic {

    using i8 = std::atomic<i8>;
    using i16 = std::atomic<i16>;
    using i32 = std::atomic<i32>;
    using i64 = std::atomic<i64>;

    using u8 = std::atomic<u8>;
    using u16 = std::atomic<u16>;
    using u32 = std::atomic<u32>;
    using u64 = std::atomic<u64>;
}