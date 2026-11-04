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

#include <algorithm>
#include <array>
#include <cstddef>
#include <initializer_list>
#include <iostream>
#include <mutex>
#include <stdexcept>


namespace concur {
/// stack with fixed capacity, using mutex
template <class T, size_t N> class Stack {
private:
	using value_type = T;
	using pointer = value_type *;
	using Container = std::array<value_type, N>;

	using scoped_lock = std::scoped_lock<std::mutex>;

	/// @todo Moving mutex into Top
	struct Node {};

public:
	Stack(const std::initializer_list<value_type> &elems) {
		std::ranges::copy(elems, elems_.begin());
		current_ = elems.size() - 1;
	}
	Stack(T elem, const size_t n) {
		static_assert(n <= capacity_, "Too many elems to initialize stack");
		elems_.fill(value_type{});
		for (int i = 0; i < n; i++) {
			elems_[i] = elem;
		}
		current_ = n - 1;
	}
	Stack() { elems_.fill(value_type{}); }

	auto push(value_type &&elem) -> bool {
		scoped_lock sl{mt_};
		if (current_ == top_) {
			throw std::runtime_error{"Pushing a full stack"};
		}
		elems_[++current_] = std::move(elem);
		return true;
	}

	/// @todo replace all exception handle with std::expected
	auto pop() -> value_type {
		scoped_lock sl{mt_};
		if (current_ == bottom_) {
			throw std::runtime_error{"Poping a empty stack"};
		}
		auto res = elems_[current_--];
		return res;
	}

	auto empty() -> bool {
		scoped_lock sl{mt_};
		return current_ == bottom_;
	}
	auto capacity() -> size_t { return capacity_; }

	auto print() -> void {
		scoped_lock sl{mt_};
		for (int i = 0; i <= current_; i++) {
			std::cout << elems_[i] << '\n';
		}
	}

private:
	std::mutex mt_;
	size_t capacity_ = N;

	int top_ = N - 1;
	int current_ = -1;
	int bottom_ = -1;

	Container elems_;
};
} // namespace concur
