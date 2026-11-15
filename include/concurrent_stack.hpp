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
		std::mutex cur_mt_;
		int idx = -1;
	};

	constexpr Stack(std::initializer_list<value_type> elems)
		: current_({}, elems.size() - 1), size_(elems.size()), elems_(elems) {}
	constexpr Stack(const size_type n, const value_type &elem) : size_(n) {
		static_assert(n <= capacity_, "Too many elems to initialize stack");
		if (n != 0) {
			elems_.fill(n, elem);
			current_.idx = n - 1;
		}
	}
	constexpr Stack() : size_(N) { elems_.fill(N); }

	auto push(value_type &&elem) -> bool {
		while (true) {
			if (try_push(std::move(elem))) {
				return true;
			} else {
				bk_.backoff();
			}
		}
	}

	auto push(const value_type &elem) -> bool {
		while (true) {
			if (try_push(elem)) {
				return true;
			} else {
				if (!bk_.backoff()) {
					return false;
				}
			}
		}
	}

	auto pop() -> std::optional<value_type> {
		value_type res;
		while (true) {
			if (try_pop(res)) {
				return res;
			} else {
				if (!bk_.backoff()) {
					return std::nullopt;
				}
			}
		}
	}

	auto size() const -> size_type {
		scoped_lock sl{current_.cur_mt_};
		return size_;
	}

	auto empty() -> bool {
		scoped_lock sl{current_.cur_mt_};
		return current_ == bottom_;
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
			next_[i].store(i + 1);
		next_[capacity_ - 1].store(NULL_IDX);

		free_top_.store({.idx = 0, .tag = 0});
		data_top_.store({.idx = NULL_IDX, .tag = 0});
	}

	LockFreeStack(const std::initializer_list<value_type> &il) noexcept {
		const u32 n = static_cast<u32>(il.size());
		for (u32 i = 0; const auto &value : il) {
			std::construct_at(slot_ptr(i++), value);
		}
		if (n > 0) {
			next_[0].store(NULL_IDX, memorder::relaxed);
			for (u32 i = 1; i < n; i++) {
				next_[i].store(i - 1, memorder::relaxed);
			}
		}

		for (u32 i = n; i + 1 < capacity_; i++) {
			next_[i].store(i + 1, memorder::relaxed);
		}
		if (n < capacity_) {
			next_[capacity_ - 1].store(NULL_IDX, memorder::relaxed);
		}

		free_top_.store({.idx = n < capacity_ ? n : NULL_IDX, .tag = 0},
						memorder::relaxed);
		data_top_.store({.idx = n > 0 ? n - 1 : NULL_IDX, .tag = 0},
						memorder::relaxed);
	}

	LockFreeStack(const size_type n, const value_type &value) {
		const u32 n_ = static_cast<u32>(n);
		for (u32 i = 0; i < n; i++) {
			std::construct_at(slot_ptr(i), value);
		}
		if (n_ > 0) {
			next_[0].store(NULL_IDX, memorder::relaxed);
			for (u32 i = 1; i < n_; i++) {
				next_[i].store(i - 1, memorder::relaxed);
			}
		}

		for (u32 i = n_; i + 1 < capacity_; i++) {
			next_[i].store(i - 1, memorder::relaxed);
		}

		if (n_ < capacity_) {
			next_[capacity_ - 1].store(NULL_IDX, memorder::relaxed);
		}
		free_top_.store({.idx = n_ < capacity_ ? n : NULL_IDX, .tag = 0},
						memorder::relaxed);
		data_top_.store({.idx = n_ > 0 ? n - 1 : NULL_IDX, .tag = 0},
						memorder::relaxed);
	}

	LockFreeStack(const LockFreeStack &) = delete;
	auto operator=(const LockFreeStack &) -> LockFreeStack & = delete;
	LockFreeStack(LockFreeStack &&) = delete;
	auto operator=(LockFreeStack &&) -> LockFreeStack & = delete;

	~LockFreeStack() noexcept {
		u32 idx = data_top_.load(memorder::relaxed).idx;
		while (idx != NULL_IDX) {
			u32 next = next_[idx].load(memorder::relaxed);
			slot_ptr(idx)->~T();
			idx = next;
		}
	}

	/// @brief Try pushing a value continuously until reaching backoff limit.
	auto push(const value_type &value) -> bool {
		while (true) {
			if (try_push(value)) {
				return true;
			} else {
				if (!bk_.backoff()) {
					return false;
				}
			}
		}
	}

	/// @brief Try poping a value continuously until reaching backoff limit.
	/// @return Return a value while try poping successfully, or return a
	/// std::nullopt.
	[[nodiscard]] auto pop() -> std::optional<value_type> {
		value_type res;
		while (true) {
			if (try_pop(res)) {
				return res;
			} else {
				if (!bk_.backoff()) {
					return std::nullopt;
				}
			}
		}
	}

	/// @attention This function is for debugging
	auto print() -> void {
		u32 idx = data_top_.load().idx;
		std::cout << std::format("Stack's Size: {}\n", idx + 1);
		while (idx != NULL_IDX) {
			u32 next = next_[idx].load();
			std::cout << std::format("<{}> - [{}]\n", idx, *slot_ptr(idx));
			idx = next;
		}
	}

	constexpr auto capacity() const -> size_type { return capacity_; }

	auto size() const -> size_type {
		return static_cast<size_type>(data_top_.load().idx - 1);
	}

private:
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
	/// into data slot through next_[slot] = old_data.idx;
	auto try_push(const value_type &value) -> bool {
		TaggedIdx old_free = free_top_.load(memorder::acquire);
		TaggedIdx new_free;
		u32 slot = 0;

		do {
			slot = old_free.idx;
			if (slot == NULL_IDX) {
				return false;
			}
			u32 below = next_[slot].load(memorder::acquire);
			new_free = {.idx = below, .tag = old_free.tag + 1};
		} while (!free_top_.compare_exchange_strong(
			old_free, new_free, memorder::acq_rel, memorder::acquire));

		std::construct_at(slot_ptr(slot), value);

		TaggedIdx old_data = data_top_.load(memorder::acquire);
		TaggedIdx new_data;

		do {
			next_[slot].store(old_data.idx, memorder::relaxed);
			new_data = {.idx = slot, .tag = old_data.tag + 1};
		} while (!data_top_.compare_exchange_strong(
			old_data, new_data, memorder::acq_rel, memorder::relaxed));
		return true;
	}

	/// @brief try_pop is symmetrical with try_push
	[[nodiscard]] auto try_pop(value_type &res) -> bool {
		TaggedIdx old_data = data_top_.load(memorder::acquire);
		TaggedIdx new_data;
		u32 slot = 0;

		do {
			slot = old_data.idx;
			if (slot == NULL_IDX) {
				return false;
			}
			u32 below = next_[slot].load(memorder::relaxed);
			new_data = {.idx = below, .tag = old_data.tag + 1};
		} while (!data_top_.compare_exchange_strong(
			old_data, new_data, memorder::acq_rel, memorder::acquire));

		auto slot_address = slot_ptr(slot);
		res = std::move(*slot_address);
		std::destroy_at(slot_address);

		TaggedIdx old_free = free_top_.load(memorder::acquire);
		TaggedIdx new_free;

		do {
			next_[slot].store(old_free.idx, memorder::relaxed);
			new_free = {.idx = slot, .tag = old_free.tag + 1};
		} while (!free_top_.compare_exchange_strong(
			old_free, new_free, memorder::acq_rel, memorder::relaxed));
		return true;
	}

private:
	static constexpr u32 NULL_IDX = std::numeric_limits<u32>::max();

	struct TaggedIdx {
		u32 idx = NULL_IDX;
		u32 tag{0};

		friend auto operator==(const TaggedIdx &lv,
							   const TaggedIdx &rv) noexcept -> bool {
			return (lv.idx == rv.idx) && (lv.tag == rv.tag);
		}
	};

	constexpr static size_type capacity_ = N;

	std::array<std::atomic<u32>, N> next_;
	alignas(64) std::atomic<TaggedIdx> free_top_;
	alignas(64) std::atomic<TaggedIdx> data_top_;
	alignas(T) std::array<std::byte, capacity_ * sizeof(T)> storage_;

	std::mutex mt_;
	details::Backoff bk_{1000, 5, 5};
};

} // namespace concur
