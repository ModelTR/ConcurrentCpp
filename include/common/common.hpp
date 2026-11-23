/**
 * @file common.hpp
 * @author modelTR (mxtian_ran@163.com)
 * @brief Some uilities.
 * @version 0.2
 * @date 2026-11-23
 *
 * @copyright MIT License (c) 2026
 *
 */

#pragma once

#include "alias.hpp"

namespace concur::details {
[[nodiscard]]
static constexpr auto pack(u32 idx, u32 tag) noexcept -> u64 {
	return (static_cast<u64>(tag) << 32) | static_cast<u64>(idx);
}
[[nodiscard]]
static constexpr auto idx_of(u64 v) noexcept -> u32 {
	return static_cast<u32>(v & 0xFFFFFFFFULL);
}
[[nodiscard]]
static constexpr auto tag_of(u64 v) noexcept -> u32 {
	return static_cast<u32>(v >> 32);
}

} // namespace concur::details