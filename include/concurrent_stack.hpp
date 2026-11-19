/**
 * @file include/concurrent_stack.hpp
 * @author modelTR (mxtian_ran@163.com)
 * @brief Serval implemention of concurrent stack (descriptions need to be
 * enriched)
 * @version 0.2
 * @date 2026-11-19
 * @copyright MIT License (c) 2026
 *
 */

#pragma once

#include "alias.hpp"
#include "backoff.hpp"
#include "inplace_vector.hpp"
#include "random.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <bit>
#include <cassert>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <format>
#include <initializer_list>
#include <iostream>
#include <limits>
#include <memory>
#include <mutex>
#include <new>
#include <optional>
#include <ratio>
#include <stdexcept>
#include <thread>
#include <utility>

/// @todo Complete the std::optional< T >'s semantics
/// @todo Need new Benchmark. Proformance needed to be cleared
/// @todo Code structure needed to be refactored, which is too messy now.

namespace concur::details {
[[nodiscard]]
static constexpr u64 pack(u32 idx, u32 tag) noexcept {
	return (static_cast<u64>(tag) << 32) | static_cast<u64>(idx);
}
[[nodiscard]]
static constexpr u32 idx_of(u64 v) noexcept {
	return static_cast<u32>(v & 0xFFFFFFFFULL);
}
[[nodiscard]]
static constexpr u32 tag_of(u64 v) noexcept {
	return static_cast<u32>(v >> 32);
}

inline constexpr unsigned kUintptrBits = std::numeric_limits<uintptr>::digits;

template <typename T>
concept CompleteObjectType = std::is_object_v<T> && requires {
	sizeof(T);
	alignof(T);
};

template <typename T>
concept AtLeastTwoAlignedObjectType =
	CompleteObjectType<T> && (alignof(T) >= 2);

template <typename T>
inline constexpr unsigned kTagBitsFor =
	static_cast<unsigned>(std::countr_zero(static_cast<uintptr>(alignof(T))));

template <typename T>
concept PointerTaggableObjectType =
	AtLeastTwoAlignedObjectType<T> && (kTagBitsFor<T> > 0) &&
	(kTagBitsFor<T> < kUintptrBits);

template <PointerTaggableObjectType T> class AtomicTaggedPtr {
public:
	static constexpr unsigned kTagBits = kTagBitsFor<T>;

	static_assert(
		kTagBits >= 1,
		"alignof(T) must be >= 2 to pack a stamp into pointer bits. "
		"Wrap T in AlignedBox<T, 8> (or use a stronger-aligned type).");
	static_assert(atomic::uintptr::is_always_lock_free,
				  "Requires lock-free uintptr_t atomic.");

	static constexpr unsigned kWordBits = kUintptrBits;

	static constexpr uintptr kTagMask = (uintptr{1} << kTagBits) - uintptr{1};

	using value_type = T;
	using pointer = T *;

	static_assert(kTagBits >= 1,
				  "T must have at least one available pointer-tag bit. "
				  "Use an over-aligned wrapper type.");

	static_assert(kTagBits < kWordBits,
				  "Tag mask cannot shift by the width of uintptr_t.");

	static_assert(std::atomic<uintptr>::is_always_lock_free,
				  "AtomicTaggedPtr requires lock-free uintptr_t atomics.");

private:
	atomic::uintptr packed_{pack(nullptr, 0)};

	[[nodiscard]]
	static auto pack(pointer ptr, uintptr tag) noexcept -> uintptr {
		const auto raw = reinterpret_cast<uintptr>(ptr);

		assert((raw & kTagMask) == 0 &&
			   "Pointer representation does not provide the required "
			   "low-order tag bits.");

		return raw | (tag & kTagMask);
	}

	[[nodiscard]]
	static auto ptr_of(uintptr packed) noexcept -> pointer {
		return reinterpret_cast<pointer>(packed & (~kTagMask));
	}

	[[nodiscard]]
	static constexpr auto tag_of(uintptr packed) noexcept -> uintptr {
		return packed & kTagMask;
	}

	template <bool Weak>
	auto compare_exchange_impl(pointer &expected_ptr, uintptr &expected_tag,
							   pointer desired_ptr, uintptr desired_tag,
							   std::memory_order success,
							   std::memory_order failure) noexcept -> bool {
		auto expected = pack(expected_ptr, expected_tag);
		const auto desired = pack(desired_ptr, desired_tag);

		bool result = false;

		if constexpr (Weak) {
			result = packed_.compare_exchange_weak(expected, desired, success,
												   failure);
		} else {
			result = packed_.compare_exchange_strong(expected, desired, success,
													 failure);
		}

		if (!result) {
			expected_ptr = ptr_of(expected);
			expected_tag = tag_of(expected);
		}

		return result;
	}

	template <bool Weak>
	auto compare_exchange_impl(pointer &expected_ptr, uintptr &expected_tag,
							   pointer desired_ptr, std::memory_order success,
							   std::memory_order failure) noexcept -> bool {
		const auto current_tag = expected_tag & kTagMask;

		auto expected = pack(expected_ptr, current_tag);

		const auto next_tag = (current_tag + uintptr{1}) & kTagMask;

		const auto desired = pack(desired_ptr, next_tag);

		bool exchanged = false;

		if constexpr (Weak) {
			exchanged = packed_.compare_exchange_weak(expected, desired,
													  success, failure);
		} else {
			exchanged = packed_.compare_exchange_strong(expected, desired,
														success, failure);
		}

		if (!exchanged) {
			expected_ptr = ptr_of(expected);
			expected_tag = tag_of(expected);
		}

		return exchanged;
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

	[[nodiscard]]
	auto
	get_snapshot(std::memory_order order = memorder::acquire) const noexcept
		-> Snapshot {
		auto packed = packed_.load(order);
		return {ptr_of(packed), tag_of(packed)};
	}

	auto store(pointer ptr, uintptr tag,
			   std::memory_order order = memorder::release) noexcept -> void {
		packed_.store(pack(ptr, tag), order);
	}

	auto compare_exchange_weak(
		pointer &expected_ptr, uintptr &expected_tag, pointer desired_ptr,
		std::memory_order success = memorder::seq_cst,
		std::memory_order failure = memorder::seq_cst) noexcept -> bool {
		const auto next_tag = (expected_tag + uintptr{1}) & kTagMask;
		return compare_exchange_impl<true>(expected_ptr, expected_tag,
										   desired_ptr, next_tag, success,
										   failure);
	}

	auto compare_exchange_strong(
		pointer &expected_ptr, uintptr &expected_tag, pointer desired_ptr,
		std::memory_order success = std::memory_order_seq_cst,
		std::memory_order failure = std::memory_order_seq_cst) noexcept
		-> bool {
		const auto next_tag = (expected_tag + uintptr{1}) & kTagMask;

		return compare_exchange_impl<false>(expected_ptr, expected_tag,
											desired_ptr, next_tag, success,
											failure);
	}

	auto compare_exchange_weak(pointer &expected_ptr, uintptr &expected_tag,
							   pointer desired_ptr, uintptr desired_tag,
							   std::memory_order success,
							   std::memory_order failure) noexcept -> bool {
		return compare_exchange_impl<true>(expected_ptr, expected_tag,
										   desired_ptr, desired_tag, success,
										   failure);
	}

	auto compare_exchange_strong(pointer &expected_ptr, uintptr &expected_tag,
								 pointer desired_ptr, uintptr desired_tag,
								 std::memory_order success,
								 std::memory_order failure) noexcept -> bool {
		return compare_exchange_impl<false>(expected_ptr, expected_tag,
											desired_ptr, desired_tag, success,
											failure);
	}
};

} // namespace concur::details

namespace concur {
/// stack with fixed capacity, using mutex
template <class T, size_t N> class Stack {
public:
	using value_type = T;
	using pointer = value_type *;
	using Container = inplace_vector<T, N>;
	using size_type = std::size_t;
	using difference_type = std::ptrdiff_t;

	using scoped_lock = std::scoped_lock<std::mutex>;

	struct Current {
		mutable std::mutex cur_mt_;
		int idx = -1;
	};

	constexpr Stack(std::initializer_list<value_type> elems)
		: current_({}, static_cast<int>(elems.size()) - 1), size_(elems.size()),
		  elems_(elems) {}
	Stack(const size_type n, const value_type &elem) : size_(n) {
		if (n > capacity_) {
			throw std::length_error{"Too many elems to initialize stack"};
		}
		if (n != 0) {
			elems_.fill(n, elem);
			current_.idx = static_cast<int>(n) - 1;
		}
	}
	constexpr Stack() : size_(0) {}

	auto push(value_type &&elem) -> bool {
		while (true) {
			if (try_push(std::move(elem))) {
				bk_.reset();
				return true;
			}
			if (!bk_.backoff()) {
				return false;
			}
		}
	}

	auto push(const value_type &elem) -> bool {
		while (true) {
			if (try_push(elem)) {
				bk_.reset();
				return true;
			}
			if (!bk_.backoff()) {
				return false;
			}
		}
	}

	auto pop() -> std::optional<value_type> {
		value_type res;
		while (true) {
			if (try_pop(res)) {
				bk_.reset();
				return res;
			}
			if (!bk_.backoff()) {
				return std::nullopt;
			}
		}
	}

	auto size() const -> size_type {
		scoped_lock sl{current_.cur_mt_};
		return size_;
	}

	auto empty() -> bool {
		scoped_lock sl{current_.cur_mt_};
		return current_.idx == bottom_;
	}
	constexpr auto capacity() const -> size_t { return capacity_; }

	constexpr auto data() -> Container & { return elems_; }

	constexpr auto data() const -> const Container & { return elems_; }

	auto print() -> void {
		std::scoped_lock lk{current_.cur_mt_};
		for (int i = 0; i <= current_.idx; i++) {
			std::cout << std::format("{}\n", elems_[i]);
		}
		std::cout << std::format("Stack's size: {}\n", size_.load());
	}

private:
	auto try_pop(value_type &res) -> bool {
		{
			scoped_lock lk{current_.cur_mt_};
			if (current_.idx == bottom_) {
				return false;
			}
			res = elems_[current_.idx--];
			elems_.pop_back();
		}
		--size_;
		return true;
	}

	auto try_push(value_type &&elem) -> bool {
		{
			std::scoped_lock lk{current_.cur_mt_};
			if (current_.idx == top_) {
				return false;
			}
			elems_.push_back(std::move(elem));
			++current_.idx;
		}
		++size_;
		return true;
	}

	auto try_push(const value_type &elem) -> bool {
		{
			std::scoped_lock lk{current_.cur_mt_};
			if (current_.idx == top_) {
				return false;
			}
			elems_.push_back(elem);
			++current_.idx;
		}
		++size_;
		return true;
	}

private:
	static constexpr size_t capacity_ = N;

	/// @note "-1" repersent bottom of stack, and "0" repersent the first
	/// element of stack
	int top_ = N - 1;
	Current current_;
	int bottom_ = -1;

	atomic::size_t size_;
	Container elems_;

	details::Backoff bk_{1000, 5, 5};
};

template <class T, size_t N> class LockFreeStack {
public:
	using value_type = T;
	using pointer = T *;
	using size_type = size_t;
	using Container = concur::inplace_vector<value_type, N>;

	LockFreeStack() noexcept {
		for (u32 i = 0; i + 1 < capacity_; ++i)
			next_[i].value.store(i + 1, memorder::relaxed);
		next_[capacity_ - 1].value.store(NULL_IDX, memorder::relaxed);

		free_top_.store(details::pack(0, 0), memorder::relaxed);
		data_top_.store(details::pack(NULL_IDX, 0), memorder::relaxed);
		size_.store(0, memorder::relaxed);
	}

	LockFreeStack(const std::initializer_list<value_type> &il)
		: size_(il.size()) {
		static_assert(std::atomic<u64>::is_always_lock_free,
					  "free_top_/data_top_ must be lock-free for this design");
		if (il.size() > capacity_) {
			throw std::length_error{"Too many elems to initialize stack"};
		}
		const u32 n = static_cast<u32>(il.size());
		for (u32 i = 0; const auto &value : il) {
			std::construct_at(slot_ptr(i++), value);
		}
		if (n > 0) {
			next_[0].value.store(NULL_IDX, memorder::relaxed);
			for (u32 i = 1; i < n; i++) {
				next_[i].value.store(i - 1, memorder::relaxed);
			}
		}

		for (u32 i = n; i + 1 < capacity_; i++) {
			next_[i].value.store(i + 1, memorder::relaxed);
		}
		if (n < capacity_) {
			next_[capacity_ - 1].value.store(NULL_IDX, memorder::relaxed);
		}

		free_top_.store(details::pack(n < capacity_ ? n : NULL_IDX, 0),
						memorder::relaxed);
		data_top_.store(details::pack(n > 0 ? n - 1 : NULL_IDX, 0),
						memorder::relaxed);
	}

	LockFreeStack(const size_type n, const value_type &value) : size_(n) {
		if (n > capacity_) {
			throw std::length_error{"Too many elems to initialize stack"};
		}
		const u32 n_ = static_cast<u32>(n);
		for (u32 i = 0; i < n_; i++) {
			std::construct_at(slot_ptr(i), value);
		}
		if (n_ > 0) {
			next_[0].value.store(NULL_IDX, memorder::relaxed);
			for (u32 i = 1; i < n_; i++) {
				next_[i].value.store(i - 1, memorder::relaxed);
			}
		}

		for (u32 i = n_; i + 1 < capacity_; i++) {
			next_[i].value.store(i + 1, memorder::relaxed);
		}

		if (n_ < capacity_) {
			next_[capacity_ - 1].value.store(NULL_IDX, memorder::relaxed);
		}
		free_top_.store(details::pack(n_ < capacity_ ? n_ : NULL_IDX, 0),
						memorder::relaxed);
		data_top_.store(details::pack(n_ > 0 ? n_ - 1 : NULL_IDX, 0),
						memorder::relaxed);
	}

	LockFreeStack(const LockFreeStack &) = delete;
	auto operator=(const LockFreeStack &) -> LockFreeStack & = delete;
	LockFreeStack(LockFreeStack &&) = delete;
	auto operator=(LockFreeStack &&) -> LockFreeStack & = delete;

	~LockFreeStack() noexcept {
		u32 idx = details::idx_of(data_top_.load(memorder::relaxed));
		while (idx != NULL_IDX) {
			u32 next = next_[idx].value.load(memorder::relaxed);
			slot_ptr(idx)->~T();
			idx = next;
		}
	}

	/// @brief
	/// A failed CAS is contention, not a full stack: only report failure when
	/// the free list really is empty.
	auto push(const value_type &value) -> bool {
		while (true) {
			if (try_push(value)) {
				return true;
			}
			std::this_thread::yield();
		}
	}

	/// @brief Try poping a value continuously until reaching backoff limit.
	/// @return Return a value while try poping successfully, or return a
	/// std::nullopt.
	///
	/// As with push(): a failed CAS means contention, not an empty stack.
	[[nodiscard]] virtual auto pop() -> std::optional<value_type> {
		value_type res;
		while (true) {
			if (try_pop(res)) {
				return res;
			}
			std::this_thread::yield();
		}
	}

	/// @attention This function is for debugging
	auto print() -> void {
		const size_type n = size();
		std::cout << std::format("Stack's Size: {}\n", n);
		u32 idx = details::idx_of(data_top_.load(memorder::acquire));
		while (idx != NULL_IDX) {
			u32 next = next_[idx].value.load(memorder::relaxed);
			std::cout << std::format("<{}> - [{}]\n", idx, *slot_ptr(idx));
			idx = next;
		}
	}

	constexpr auto capacity() const -> size_type { return capacity_; }

	auto size() const -> size_type { return size_.load(memorder::acquire); }

private:
	/// @attention Using reinterpret_cast convert byte * to T * which is
	/// langer need std::launder
	auto slot_ptr(u32 idx) noexcept -> pointer {
		return std::launder(
			reinterpret_cast<pointer>(storage_.data() + (idx * sizeof(T))));
	}
protected:
	/// @brief try_push implement functionality through three steps:
	/// 1. Pick a exclusive slot from free chain
	/// 2. Construct value in the exclusive slot
	/// 3. Release slot to top of data stack
	/// @details In greneral, data_top_ and free_top_ share next_, where free
	/// slot and data slot appear in staggered, next_[free_top_] record the next
	/// free slot, and next_[data_top_] record the next data slot below
	/// top.While excuting from step1 to step3. try_push update the free_top_ to
	/// next free slot, and data_top_ to next data slot, changing the free slot
	/// into data slot through next_[slot] = old_data.idx
	auto try_push(const value_type &value) -> bool {
		u64 old_free = free_top_.load(memorder::acquire);
		u64 new_free = 0;
		u32 slot = 0;
		do {
			slot = details::idx_of(old_free);
			if (slot == NULL_IDX) {
				return false;
			}
			u32 below = next_[slot].value.load(memorder::acquire);
			new_free = details::pack(below, details::tag_of(old_free) + 1);
		} while (!free_top_.compare_exchange_strong(
			old_free, new_free, memorder::acq_rel, memorder::acquire));

		std::construct_at(slot_ptr(slot), value);

		u64 old_data = data_top_.load(memorder::acquire);
		u64 new_data = 0;

		do {
			next_[slot].value.store(details::idx_of(old_data),
									memorder::relaxed);
			new_data = details::pack(slot, details::tag_of(old_data) + 1);
		} while (!data_top_.compare_exchange_strong(
			old_data, new_data, memorder::acq_rel, memorder::relaxed));
		size_.fetch_add(1, memorder::relaxed);
		return true;
	}

	/// @brief try_pop is symmetrical with try_push
	[[nodiscard]] auto try_pop(value_type &res) -> bool {
		u64 old_data = data_top_.load(memorder::acquire);
		u64 new_data = 0;
		u32 slot = 0;

		do {
			slot = details::idx_of(old_data);
			if (slot == NULL_IDX) {
				return false;
			}
			u32 below = next_[slot].value.load(memorder::relaxed);
			new_data = details::pack(below, details::tag_of(old_data) + 1);
		} while (!data_top_.compare_exchange_strong(
			old_data, new_data, memorder::acq_rel, memorder::acquire));

		auto slot_address = slot_ptr(slot);
		res = std::move(*slot_address);
		std::destroy_at(slot_address);

		u64 old_free = free_top_.load(memorder::acquire);
		u64 new_free = 0;

		do {
			next_[slot].value.store(details::idx_of(old_free),
									memorder::relaxed);
			new_free = details::pack(slot, details::tag_of(old_free) + 1);
		} while (!free_top_.compare_exchange_strong(
			old_free, new_free, memorder::acq_rel, memorder::relaxed));
		size_.fetch_sub(1, memorder::relaxed);
		return true;
	}

private:
	static constexpr u32 NULL_IDX = std::numeric_limits<u32>::max();

	struct alignas(std::hardware_destructive_interference_size) PaddedIndex {
		std::atomic<u32> value{NULL_IDX};
	};

	constexpr static size_type capacity_ = N;

	std::array<PaddedIndex, N> next_;
	alignas(
		std::hardware_destructive_interference_size) std::atomic<u64> free_top_;
	alignas(
		std::hardware_destructive_interference_size) std::atomic<u64> data_top_;
	alignas(std::hardware_destructive_interference_size)
		std::atomic<size_type> size_{0};
	alignas(T) std::array<std::byte, capacity_ * sizeof(T)> storage_;
};

/// @todo The CAS in LockFreeExchanger< T > isn't setting carefully
template <class T> class LockFreeExchanger {
public:
	using value_type = T;
	using pointer = T *;

	using time_point =
		std::chrono::time_point<std::chrono::steady_clock, std::nano>;
	using clock = std::chrono::steady_clock;
	using duration =
		std::chrono::duration<std::chrono::steady_clock, std::nano>;
	using nano = std::chrono::nanoseconds;

	auto exchange(pointer my_item, int nano_timeout)
		-> std::optional<value_type> {
		auto time_bound = clock::now() + nano{nano_timeout};
		while (true) {
			if (clock::now() > time_bound) {
				return std::nullopt;
			}
			auto [yr_item, tag] = slot_.get_snapshot();
			switch (tag) {
			case 0: {
				if (slot_.compare_exchange_strong(yr_item, S_EMPTY, my_item,
												  S_WATING, memorder::seq_cst,
												  memorder::seq_cst)) {
					while (clock::now() < time_bound) {
						auto [yr_item, tag] = slot_.get_snapshot();
						if (tag == S_BUSY) {
							slot_.store(nullptr, S_EMPTY);
						}
					}
				}
				if (slot_.compare_exchange_strong(my_item, S_WATING, nullptr,
												  S_EMPTY, memorder::seq_cst,
												  memorder::seq_cst)) {
					return std::nullopt;
				} else {
					auto [yr_item, tag] = slot_.get_snapshot();
					slot_.store(nullptr, S_EMPTY);
					return *yr_item;
				}
			} break;
			case 1: {
				if (slot_.compare_exchange_strong(yr_item, S_WATING, my_item,
												  S_BUSY, memorder::seq_cst,
												  memorder::seq_cst)) {
					return *yr_item;
				}
			} break;
			case 2: {
				break;
			}
			default:
				break;
			}
		}
	}

private:
private:
	uintptr S_EMPTY = 0;
	uintptr S_WATING = 1;
	uintptr S_BUSY = 2;
	details::AtomicTaggedPtr<T> slot_{nullptr, S_EMPTY};
};

template <class T, size_t N> class ElimiArray {
public:
	using value_type = T;
	using pointer = T *;

	[[nodiscard]] auto visit(pointer value, const int rng_begin,
							 const int rng_end) -> std::optional<value_type> {
		const auto res =
			exchangers_[rng::random_int(rng_begin, rng_end)].exchange(value,
																	  duration);
		if (res.has_value()) {
			return res;
		}
		return std::nullopt;
	}

private:
	static constexpr int duration = 500;
	std::array<LockFreeExchanger<T>, N> exchangers_;
};

template <class T, size_t N>
class ElimiBackoffStack : private LockFreeStack<T, N> {
public:
	using value_type = T;
	using size_type = size_t;

	ElimiBackoffStack() : concur::LockFreeStack<T, N>() {}
	ElimiBackoffStack(const std::initializer_list<value_type> &il)
		: concur::LockFreeStack<T, N>(il) {}

	ElimiBackoffStack(const size_type n, const value_type &value)
		: concur::LockFreeStack<T, N>(n, value) {}

	auto push(value_type &value) -> bool {
		while (true) {
			if (LockFreeStack<T, N>::try_push(value)) {
				return true;
			} else {
				const auto &[bg, ed] = get_subrange();
				auto other_v = elimi_.visit(&value, bg, ed);
				if (other_v.has_value()) {
					return true;
				}
			}
		}
	}

	[[nodiscard]] auto pop() -> std::optional<value_type> override {
		value_type res;
		while (true) {
			if (LockFreeStack<T, N>::try_pop(res)) {
				return res;
			} else {
				const auto &[bg, ed] = get_subrange();
				auto other_v = elimi_.visit(nullptr, bg, ed);
				if (other_v.has_value()) {
					return other_v;
				}
			}
		}
	}

	using LockFreeStack<T, N>::print;

private:
	struct Subrange {
		int bg;
		int ed;
	};

	/// @todo The selected policy in ElimiBackoofStack is rough
	auto get_subrange() const -> Subrange {
		auto rd = rng::make_stream();
		return {
			rd.uniform(0, subrng_sp),
			rd.uniform(subrng_sp, static_cast<int>(capacity_)),
		};
	}

private:
	static constexpr size_t capacity_ = N;

	static constexpr int subrng_sp = capacity_ / 3;

	ElimiArray<T, N> elimi_;
};

} // namespace concur
