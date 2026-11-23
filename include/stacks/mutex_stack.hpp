/**
 * @file mutex_stack.hpp
 * @author modelTR (mxtian_ran@163.com)
 * @brief
 * @version 0.2
 * @date 2026-11-23
 *
 * @copyright MIT License (c) 2026
 *
 */

#pragma once

#include "common/alias.hpp"
#include "common/backoff.hpp"
#include "common/inplace_vector.hpp"

#include <cstddef>
#include <format>
#include <initializer_list>
#include <iostream>
#include <mutex>
#include <optional>
#include <stdexcept>

#include "absl/synchronization/mutex.h"


namespace concur {
/// stack with fixed capacity, using mutex
template <class T, size_t N, class MT = std::mutex> class Stack {
public:
	using value_type = T;
	using pointer = value_type *;
	using Container = inplace_vector<T, N>;
	using size_type = std::size_t;
    using mutex = absl::Mutex;
    using mutex_holder = absl::MutexLock;

	struct Current {
		mutable mutex cur_mt_;
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
		mutex_holder sl{current_.cur_mt_};
		return size_;
	}

	auto empty() -> bool {
		mutex_holder sl{current_.cur_mt_};
		return current_.idx == bottom_;
	}
	constexpr auto capacity() const -> size_t { return capacity_; }

	constexpr auto data() -> Container & { return elems_; }

	constexpr auto data() const -> const Container & { return elems_; }

	auto print() -> void {
		mutex_holder lk{current_.cur_mt_};
		for (int i = 0; i <= current_.idx; i++) {
			std::cout << std::format("{}\n", elems_[i]);
		}
		std::cout << std::format("Stack's size: {}\n", size_.load());
	}

private:
	auto try_pop(value_type &res) -> bool {
		{
			mutex_holder lk{current_.cur_mt_};
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
			mutex_holder lk{current_.cur_mt_};
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
			mutex_holder lk{current_.cur_mt_};
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

} // namespace concur