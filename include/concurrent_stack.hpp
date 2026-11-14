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
#include <atomic>
#include <cassert>
#include <cstddef>
#include <format>
#include <initializer_list>
#include <iostream>
#include <mutex>
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

	auto push(const value_type &elem) -> bool {
		while (true) {
			if (try_push(elem)) {
				return true;
			} else {
				bk_.backoff();
			}
		}
	}

	auto pop() -> value_type {
		while (true) {
			auto [is_success, res] = try_pop();
			if (is_success) {
				return res;
			} else {
				bk_.backoff();
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
	auto try_pop() -> std::pair<bool, value_type> {
		value_type res;
		{
			scoped_lock lk{current_.cur_mt_};
			if (current_.idx == bottom_) {
				return {false, value_type{}};
			}
			res = elems_[current_.idx--];
			elems_.pop_back();
		}
		--size_;
		return {true, res};
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

	details::Backoff bk_{5, 1000};
};



} // namespace concur
