/**
 * @file include/alias.hpp
 * @author modleTR (mxtian_ran@163.com)
 * @brief Alias for signed/unsigned intergal, atomic signed/unsigned intergal
 * @version 0.2
 * @date 2026-11-23
 * @copyright MIT License (c) 2026
 * 
 */

#pragma once

/// @note For std::int8_t, std::int16_t, ..., std::uint8_t, std::uint16_t
#include <cstdint>

/// @note For template std::atomic<T>
#include <atomic>
#include <string>
#include <string_view>
#include <vector>


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

/// Others

using uintptr = std::uintptr_t;

/// Containers

template<class T>
using vec = std::vector<T>;

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

    using size_t = std::atomic_size_t;
    using flag = std::atomic_flag;
    using uintptr = std::atomic_uintptr_t;
}

using memorder = std::memory_order;

using str = std::string;
using str_vw = std::string_view;