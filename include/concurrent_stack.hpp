/**
 * @file include/concurrent_stack.hpp
 * @author modelTR (mxtian_ran@163.com)
 * @brief Serval implemention of concurrent stack (descriptions need to be
 * enriched)
 * @version 0.1
 * @date 2026-11-02
 * @copyright MIT License (c) 2026
 *
 */

#pragma once

#include "alias.hpp"
#include "backoff.hpp"
#include "inplace_vector.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <cassert>
#include <cstdarg>
#include <cstddef>
#include <format>
#include <initializer_list>
#include <iostream>
#include <limits>
#include <memory>
#include <mutex>
#include <new>
#include <optional>
#include <stdexcept>
#include <thread>
#include <utility>

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

		free_top_.store(pack(0, 0));
		data_top_.store(pack(NULL_IDX, 0));
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

		free_top_.store(pack(n < capacity_ ? n : NULL_IDX, 0));
		data_top_.store(pack(n > 0 ? n - 1 : NULL_IDX, 0));
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
		free_top_.store(pack(n_ < capacity_ ? n_ : NULL_IDX, 0));
		data_top_.store(pack(n_ > 0 ? n_ - 1 : NULL_IDX, 0));
	}

	LockFreeStack(const LockFreeStack &) = delete;
	auto operator=(const LockFreeStack &) -> LockFreeStack & = delete;
	LockFreeStack(LockFreeStack &&) = delete;
	auto operator=(LockFreeStack &&) -> LockFreeStack & = delete;

	~LockFreeStack() noexcept {
		u32 idx = idx_of(data_top_.load(memorder::relaxed));
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
			if (idx_of(free_top_.load(memorder::acquire)) == NULL_IDX) {
				return false;
			}
			std::this_thread::yield();
		}
	}

	/// @brief Try poping a value continuously until reaching backoff limit.
	/// @return Return a value while try poping successfully, or return a
	/// std::nullopt.
	///
	/// As with push(): a failed CAS means contention, not an empty stack.
	[[nodiscard]] auto pop() -> std::optional<value_type> {
		value_type res;
		while (true) {
			if (try_pop(res)) {
				return res;
			}
			if (idx_of(data_top_.load(memorder::acquire)) == NULL_IDX) {
				return std::nullopt;
			}
			std::this_thread::yield();
		}
	}

	/// @attention This function is for debugging
	auto print() -> void {
		const size_type n = size();
		std::cout << std::format("Stack's Size: {}\n", n);
		u32 idx = idx_of(data_top_.load(memorder::acquire));
		while (idx != NULL_IDX) {
			u32 next = next_[idx].value.load(memorder::relaxed);
			std::cout << std::format("<{}> - [{}]\n", idx, *slot_ptr(idx));
			idx = next;
		}
	}

	constexpr auto capacity() const -> size_type { return capacity_; }

	auto size() const -> size_type { return size_.load(memorder::acquire); }

private:
	static constexpr u64 pack(u32 idx, u32 tag) noexcept {
		return (static_cast<u64>(tag) << 32) | static_cast<u64>(idx);
	}
	static constexpr u32 idx_of(u64 v) noexcept {
		return static_cast<u32>(v & 0xFFFFFFFFULL);
	}
	static constexpr u32 tag_of(u64 v) noexcept {
		return static_cast<u32>(v >> 32);
	}

	/// @attention Using reinterpret_cast convert byte * to T * which is
	/// langer need std::launder
	auto slot_ptr(u32 idx) noexcept -> pointer {
		return std::launder(
			reinterpret_cast<pointer>(storage_.data() + (idx * sizeof(T))));
	}
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

	/// @brief try_pop is symmetrical with try_push
	[[nodiscard]] auto try_pop(value_type &res) -> bool {
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

} // namespace concur
