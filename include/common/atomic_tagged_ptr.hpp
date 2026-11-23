/**
 * @file atomic_tagged_ptr.hpp
 * @author modelTR (mxtian_ran@163.com)
 * @brief A simple impl of atomic tagged pointer(simliar to Java's
 * AtomicStampedReference)
 * @version 0.2
 * @date 2026-11-23
 *
 * @copyright MIT License (c) 2026
 *
 */

#pragma once

#include <atomic>
#include <bit>
#include <cassert>
#include <functional>
#include <limits>

#include "alias.hpp"

namespace concur::details {
inline constexpr unsigned UINTPTR_BITS = std::numeric_limits<uintptr>::digits;

template <typename T>
concept CompleteObjectType = std::is_object_v<T> and requires {
	sizeof(T);
	alignof(T);
};

template <typename T>
concept AtLeastTwoAlignedObjectType =
	CompleteObjectType<T> and (alignof(T) >= 2);

template <typename T>
inline constexpr unsigned TAG_BITS_FOR =
	static_cast<unsigned>(std::countr_zero(static_cast<uintptr>(alignof(T))));

template <typename T>
concept PointerTaggableObjectType =
	AtLeastTwoAlignedObjectType<T> and (TAG_BITS_FOR<T> > 0) and
	(TAG_BITS_FOR<T> < UINTPTR_BITS);

/// @attention Change tag from storing in lowbits of pointer into highbits.

template <PointerTaggableObjectType T, unsigned HighTagBits = 0>
class AtomicTaggedPtr {
public:
	static_assert(HighTagBits < UINTPTR_BITS,
				  "HighTagBits must fit in the machine word.");

	static constexpr unsigned LOW_TAG_BITS = TAG_BITS_FOR<T>;
	static constexpr unsigned TAG_BITS = LOW_TAG_BITS + HighTagBits;
	static constexpr unsigned WORD_BITS = UINTPTR_BITS;

	static_assert(LOW_TAG_BITS >= 1,
				  "alignof(T) must be >= 2 to pack a stamp into pointer bits. "
				  "Wrap T in an over-aligned type, or use HighTagBits.");
	static_assert(TAG_BITS < WORD_BITS,
				  "Combined tag must leave at least one bit for the pointer.");
	static_assert(atomic::uintptr::is_always_lock_free,
				  "AtomicTaggedPtr requires lock-free uintptr_t atomics.");

	static constexpr uintptr LOW_TAG_MASK =
		(uintptr{1} << LOW_TAG_BITS) - uintptr{1};

	static constexpr uintptr HIGH_TAG_MASK =
		HighTagBits > 0 ? (((uintptr{1} << HighTagBits) - uintptr{1})
						   << (WORD_BITS - HighTagBits))
						: uintptr{0};

	static constexpr uintptr PACKED_TAG_MASK = LOW_TAG_MASK | HIGH_TAG_MASK;

	static constexpr uintptr LOGICAL_TAG_MASK =
		(uintptr{1} << TAG_BITS) - uintptr{1};

	using value_type = T;
	using pointer = T *;

	[[nodiscard]]
	static auto pointer_has_free_tag_bits(pointer ptr) noexcept -> bool {
		return (reinterpret_cast<uintptr>(ptr) & PACKED_TAG_MASK) == 0;
	}

private:
	atomic::uintptr packed_{pack(nullptr, 0)};

	[[nodiscard]]
	static auto pack(pointer ptr, uintptr tag) noexcept -> uintptr {
		const auto raw = reinterpret_cast<uintptr>(ptr);
		assert((tag & ~LOGICAL_TAG_MASK) == 0 &&
			   "tag exceeds the configured tag bit budget.");
		assert((raw & PACKED_TAG_MASK) == 0 &&
			   "Pointer representation does not provide the required "
			   "tag bits (a bit reserved for tagging is already set).");

		const auto low = tag & LOW_TAG_MASK;

		uintptr hi = 0;
		if constexpr (HighTagBits > 0) {
			const auto high_tag =
				(tag >> LOW_TAG_BITS) & ((uintptr{1} << HighTagBits) - 1);
			hi = high_tag << (WORD_BITS - HighTagBits);
		}

		return raw | low | hi;
	}

	[[nodiscard]]
	static auto ptr_of(uintptr packed) noexcept -> pointer {
		return reinterpret_cast<pointer>(packed & (~PACKED_TAG_MASK));
	}

	[[nodiscard]]
	static constexpr auto tag_of(uintptr packed) noexcept -> uintptr {
		const auto low = packed & LOW_TAG_MASK;

		uintptr hi = 0;
		if constexpr (HighTagBits > 0) {
			hi = ((packed >> (WORD_BITS - HighTagBits)) &
				  ((uintptr{1} << HighTagBits) - 1))
				 << LOW_TAG_BITS;
		}

		return low | hi;
	}

	template <bool Weak>
	auto compare_exchange_impl(pointer &expected_p, uintptr &expected_t,
							   pointer desired_p, uintptr desired_t,
							   std::memory_order succ,
							   std::memory_order fail) noexcept -> bool {
		auto expected = pack(expected_p, expected_t);
		const auto desired = pack(desired_p, desired_t);

		bool result = false;

		if constexpr (Weak) {
			result =
				packed_.compare_exchange_weak(expected, desired, succ, fail);
		} else {
			result =
				packed_.compare_exchange_strong(expected, desired, succ, fail);
		}

		if (!result) {
			expected_p = ptr_of(expected);
			expected_t = tag_of(expected);
		}

		return result;
	}

	template <bool Weak>
	auto compare_exchange_inc_tag_impl(pointer &expected_p, uintptr &expected_t,
									   pointer desired_p,
									   std::memory_order succ,
									   std::memory_order fail) noexcept
		-> bool {
		const auto next_tag = (expected_t + uintptr{1}) & LOGICAL_TAG_MASK;
		return compare_exchange_impl<Weak>(expected_p, expected_t, desired_p,
										   next_tag, succ, fail);
	}

	template <bool Weak, class NextTag>
	auto compare_exchange_next(pointer &expected_p, uintptr &expected_t,
							   pointer desired_p, NextTag &&next_t,
							   std::memory_order succ,
							   std::memory_order fail) noexcept -> bool {
		const auto desired_tag = static_cast<uintptr>(
			std::invoke(std::forward<NextTag>(next_t), expected_t));
		return compare_exchange_impl<Weak>(expected_p, expected_t, desired_p,
										   desired_tag, succ, fail);
	}

public:
	AtomicTaggedPtr() noexcept = default;

	AtomicTaggedPtr(const AtomicTaggedPtr &) = delete;
	auto operator=(const AtomicTaggedPtr &) -> AtomicTaggedPtr & = delete;

	AtomicTaggedPtr(AtomicTaggedPtr &&) = delete;
	auto operator=(AtomicTaggedPtr &&) -> AtomicTaggedPtr & = delete;

	~AtomicTaggedPtr() = default;

	explicit AtomicTaggedPtr(pointer ptr, uintptr tag = 0) noexcept
		: packed_(pack(ptr, tag)) {}

	[[nodiscard]]
	auto get(std::memory_order order = memorder::acquire) const noexcept
		-> pointer {
		return ptr_of(packed_.load(order));
	}

	[[nodiscard]]
	auto get_tag(std::memory_order order = memorder::relaxed) const noexcept
		-> uintptr {
		return tag_of(packed_.load(order));
	}

	struct Snapshot {
		pointer ptr;
		uintptr tag;
	};

	/// @attention API changed.
	[[nodiscard]]
	auto snapshot(std::memory_order order = memorder::acquire) const noexcept
		-> Snapshot {
		auto packed = packed_.load(order);
		return {ptr_of(packed), tag_of(packed)};
	}

	auto store(pointer ptr, uintptr tag,
			   std::memory_order order = memorder::release) noexcept -> void {
		packed_.store(pack(ptr, tag), order);
	}

	template <class NextTag>
		requires std::is_invocable_r_v<uintptr, NextTag, uintptr>
	auto store(pointer ptr, NextTag &&next_t,
			   std::memory_order order = memorder::release) noexcept -> void {
		const auto current_tag = tag_of(packed_.load(memorder::relaxed));
		const auto next_tag_value = static_cast<uintptr>(
			std::invoke(std::forward<NextTag>(next_t), current_tag));
		packed_.store(pack(ptr, next_tag_value), order);
	}

	auto compare_exchange_weak(
		pointer &expected_p, uintptr &expected_t, pointer desired_p,
		std::memory_order succ = memorder::seq_cst,
		std::memory_order fail = memorder::seq_cst) noexcept -> bool {
		return compare_exchange_inc_tag_impl<true>(expected_p, expected_t,
												   desired_p, succ, fail);
	}

	auto compare_exchange_strong(
		pointer &expected_p, uintptr &expected_t, pointer desired_p,
		std::memory_order succ = memorder::seq_cst,
		std::memory_order fail = memorder::seq_cst) noexcept -> bool {
		return compare_exchange_inc_tag_impl<false>(expected_p, expected_t,
													desired_p, succ, fail);
	}

	auto compare_exchange_weak(
		pointer &expected_p, uintptr &expected_t, pointer desired_p,
		uintptr desired_t, std::memory_order succ = memorder::seq_cst,
		std::memory_order fail = memorder::seq_cst) noexcept -> bool {
		return compare_exchange_impl<true>(expected_p, expected_t, desired_p,
										   desired_t, succ, fail);
	}

	auto compare_exchange_strong(
		pointer &expected_p, uintptr &expected_t, pointer desired_p,
		uintptr desired_t, std::memory_order succ = memorder::seq_cst,
		std::memory_order fail = memorder::seq_cst) noexcept -> bool {
		return compare_exchange_impl<false>(expected_p, expected_t, desired_p,
											desired_t, succ, fail);
	}

	template <class NextTag>
		requires std::is_invocable_r_v<uintptr, NextTag, uintptr>
	auto
	compare_exchange_weak(pointer &expected_p, uintptr &expected_t,
						  pointer desired_p, NextTag &&next_t,
						  std::memory_order succ = memorder::seq_cst,
						  std::memory_order fail = memorder::seq_cst) noexcept
		-> bool {
		return compare_exchange_next<true>(expected_p, expected_t, desired_p,
										   std::forward<NextTag>(next_t), succ,
										   fail);
	}

	template <class NextTag>
		requires std::is_invocable_r_v<uintptr, NextTag, uintptr>
	auto
	compare_exchange_strong(pointer &expected_p, uintptr &expected_t,
							pointer desired_p, NextTag &&next_t,
							std::memory_order succ = memorder::seq_cst,
							std::memory_order fail = memorder::seq_cst) noexcept
		-> bool {
		return compare_exchange_next<false>(expected_p, expected_t, desired_p,
											std::forward<NextTag>(next_t), succ,
											fail);
	}
};

template <unsigned TotalTagBits, unsigned PayloadBits> struct StampedTag {
	static_assert(PayloadBits >= 1, "payload needs at least 1 bit");
	static_assert(TotalTagBits > PayloadBits,
				  "need at least 1 version bit for ABA protection");

	static constexpr unsigned PAYLOAD_BITS = PayloadBits;
	static constexpr unsigned VERSION_BITS = TotalTagBits - PayloadBits;

	static constexpr uintptr PAYLOAD_MASK = (uintptr{1} << PayloadBits) - 1;
	static constexpr uintptr VERSION_MASK = (uintptr{1} << VERSION_BITS) - 1;

	uintptr version;
	uintptr payload;

	[[nodiscard]] static constexpr auto decode(uintptr tag) noexcept
		-> StampedTag {
		return {tag >> PayloadBits, tag & PAYLOAD_MASK};
	}

	[[nodiscard]] constexpr auto encode() const noexcept -> uintptr {
		return ((version & VERSION_MASK) << PayloadBits) |
			   (payload & PAYLOAD_MASK);
	}

	[[nodiscard]] constexpr auto next_tag(uintptr new_payload) const noexcept
		-> uintptr {
		const auto v = (version + 1) & VERSION_MASK;
		return (v << PayloadBits) | (new_payload & PAYLOAD_MASK);
	}
};

template <unsigned TotalTagBits, unsigned PayloadBits>
auto tag_transition(uintptr new_payload) {
	return [new_payload](uintptr expected_tag) -> uintptr {
		return StampedTag<TotalTagBits, PayloadBits>::decode(expected_tag)
			.next_tag(new_payload);
	};
}

template <unsigned TotalTagBits, unsigned PayloadBits, class F>
auto tag_transition_with(F &&f) {
	return [f = std::forward<F>(f)](uintptr expected_tag) mutable -> uintptr {
		const auto st =
			StampedTag<TotalTagBits, PayloadBits>::decode(expected_tag);
		const auto new_payload =
			static_cast<uintptr>(std::invoke(f, st.payload, st.version));
		return st.next_tag(new_payload);
	};
}

} // namespace concur::details