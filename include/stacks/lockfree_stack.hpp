/**
 * @file lockfree_stack.hpp
 * @author your name (you@domain.com)
 * @brief
 * @version 0.2
 * @date 2026-11-23
 *
 * @copyright MIT License (c) 2026
 *
 */

#pragma once

#include "common/alias.hpp"
#include "common/common.hpp"
#include "common/inplace_vector.hpp"

#include <array>
#include <concepts>
#include <cstddef>
#include <format>
#include <initializer_list>
#include <iostream>
#include <limits>
#include <new>
#include <optional>
#include <stdexcept>
#include <thread>

namespace concur {
template <class T, size_t N>
	requires(std::default_initializable<T> and std::is_object_v<T> and
			 !std::is_const_v<T>)
class LockFreeStack {
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
		static_assert(atomic::i64::is_always_lock_free,
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

	virtual ~LockFreeStack() noexcept {
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
	virtual auto push(const value_type &value) -> bool {
		while (true) {
			if (try_push(value)) {
				return true;
			}
			std::this_thread::yield();
		}
	}

	virtual auto push(value_type &&value) -> bool {
		while (true) {
			if (try_push(std::move(value))) {
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

	auto empty() const -> bool { return size() == 0; }

	auto full() const -> bool { return size() >= capacity_; }

private:
	/// @attention Using reinterpret_cast convert byte * to T * which is
	/// langer need std::launder
	auto slot_ptr(const u32 idx) noexcept -> pointer {
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
		using namespace details;
		u64 old_free = free_top_.load(memorder::acquire);
		u64 new_free = 0;
		u32 slot = 0;
		do {
			slot = idx_of(old_free);
			if (slot == NULL_IDX) {
				return false;
			}
			u32 below = next_[slot].value.load(memorder::acquire);
			new_free = pack(below, tag_of(old_free) + 1);
		} while (!free_top_.compare_exchange_strong(
			old_free, new_free, memorder::acq_rel, memorder::acquire));

		std::construct_at(slot_ptr(slot), value);

		u64 old_data = data_top_.load(memorder::acquire);
		u64 new_data = 0;

		do {
			next_[slot].value.store(idx_of(old_data), memorder::relaxed);
			new_data = pack(slot, tag_of(old_data) + 1);
		} while (!data_top_.compare_exchange_strong(
			old_data, new_data, memorder::acq_rel, memorder::relaxed));
		size_.fetch_add(1, memorder::relaxed);
		return true;
	}

	auto try_push(value_type &&value) -> bool {
		using namespace details;
		u64 old_free = free_top_.load(memorder::acquire);
		u64 new_free = 0;
		u32 slot = 0;
		do {
			slot = idx_of(old_free);
			if (slot == NULL_IDX) {
				return false;
			}
			u32 below = next_[slot].value.load(memorder::acquire);
			new_free = pack(below, tag_of(old_free) + 1);
		} while (!free_top_.compare_exchange_strong(
			old_free, new_free, memorder::acq_rel, memorder::acquire));

		std::construct_at(slot_ptr(slot), std::move(value));

		u64 old_data = data_top_.load(memorder::acquire);
		u64 new_data = 0;

		do {
			next_[slot].value.store(idx_of(old_data), memorder::relaxed);
			new_data = pack(slot, tag_of(old_data) + 1);
		} while (!data_top_.compare_exchange_strong(
			old_data, new_data, memorder::acq_rel, memorder::relaxed));
		size_.fetch_add(1, memorder::relaxed);
		return true;
	}

	/// @brief try_pop is symmetrical with try_push
	[[nodiscard]] auto try_pop(value_type &res) -> bool {
		using namespace details;
		u64 old_data = data_top_.load(memorder::acquire);
		u64 new_data = 0;
		u32 slot = 0;

		do {
			slot = idx_of(old_data);
			if (slot == NULL_IDX) {
				return false;
			}
			u32 below = next_[slot].value.load(memorder::relaxed);
			new_data = pack(below, tag_of(old_data) + 1);
		} while (!data_top_.compare_exchange_strong(
			old_data, new_data, memorder::acq_rel, memorder::acquire));

		auto slot_address = slot_ptr(slot);
		res = std::move(*slot_address);
		std::destroy_at(slot_address);

		u64 old_free = free_top_.load(memorder::acquire);
		u64 new_free = 0;

		do {
			next_[slot].value.store(idx_of(old_free), memorder::relaxed);
			new_free = pack(slot, tag_of(old_free) + 1);
		} while (!free_top_.compare_exchange_strong(
			old_free, new_free, memorder::acq_rel, memorder::relaxed));
		size_.fetch_sub(1, memorder::relaxed);
		return true;
	}

private:
	static constexpr u32 NULL_IDX = std::numeric_limits<u32>::max();

	struct alignas(std::hardware_destructive_interference_size) PaddedIndex {
		atomic::u32 value{NULL_IDX};
	};

	constexpr static size_type capacity_ = N;

	std::array<PaddedIndex, N> next_;
	alignas(std::hardware_destructive_interference_size) atomic::u64 free_top_;
	alignas(std::hardware_destructive_interference_size) atomic::u64 data_top_;
	alignas(std::hardware_destructive_interference_size) atomic::size_t size_{
		0};
	alignas(T) std::array<std::byte, capacity_ * sizeof(T)> storage_;
};
} // namespace concur