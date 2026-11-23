/**
 * @file elimi_backoff_stack.hpp
 * @author your name (you@domain.com)
 * @brief
 * @version 0.2
 * @date 2026-11-23
 *
 * @copyright MIT License (c) 2026
 *
 */

#pragma once

#include "common/elimi_array.hpp"
#include "common/lockfree_exchanger.hpp"
#include "common/random.hpp"
#include "lockfree_stack.hpp"


#include <algorithm>
#include <concepts>
#include <cstddef>
#include <initializer_list>
#include <optional>
#include <thread>

namespace concur {
template <class T, size_t N, size_t ElimiSlot = std::min(N, size_t{64})>
	requires(std::default_initializable<T> && std::copy_constructible<T>)
class ElimiBackoffStack : private LockFreeStack<T, N> {
public:
	using value_type = T;
	using size_type = size_t;

	ElimiBackoffStack() : concur::LockFreeStack<T, N>() {}
	ElimiBackoffStack(const std::initializer_list<value_type> &il)
		: concur::LockFreeStack<T, N>(il) {}

	ElimiBackoffStack(const size_type n, const value_type &value)
		: concur::LockFreeStack<T, N>(n, value) {}
	
	~ElimiBackoffStack() noexcept = default;

	auto push(const value_type &value) -> bool override {
		while (true) {
			if (try_push(value)) {
				return true;
			} else if (full()) {
				value_type local = value;
				const auto &[bg, ed] = get_subrange();
				auto other_v = elimi_.visit(&local, bg, ed);
				if (other_v.state == ExchResState::MATCHED_EMPTY) {
					return true;
				}
			}
			std::this_thread::yield();
		}
	}

	auto push(value_type &&value) -> bool override {
		while (true) {
			if (try_push(std::move(value))) {
				return true;
			} else if (full()) {
				const auto &[bg, ed] = get_subrange();
				auto other_v = elimi_.visit(&value, bg, ed);
				if (other_v.state == ExchResState::MATCHED_EMPTY) {
					return true;
				}
			}
			std::this_thread::yield();
		}
	}

	[[nodiscard]] auto pop() -> std::optional<value_type> override {
		value_type res;
		while (true) {
			if (try_pop(res)) {
				return res;
			} else if (empty()) {
				const auto &[bg, ed] = get_subrange();
				auto other_v = elimi_.visit(nullptr, bg, ed);
				if (other_v.state == ExchResState::MARCHED) {
					return other_v.value.value();
				}
			}
			std::this_thread::yield();
		}
	}

	using LockFreeStack<T, N>::print;
	using LockFreeStack<T, N>::size;
	using LockFreeStack<T, N>::empty;
	using LockFreeStack<T, N>::capacity;
	using LockFreeStack<T, N>::full;

private:
	using LockFreeStack<T, N>::try_pop;
	using LockFreeStack<T, N>::try_push;
	using ExchResState = LockFreeExchanger<T>::ExchResState;

	struct Subrange {
		int bg;
		int ed;
	};

	/// @todo The selected policy in ElimiBackoofStack is rough
	auto get_subrange() const -> Subrange {
		if (ELIMI_CAPACITY != 1) {
			thread_local auto rd = rng::make_stream();
			int break_p =
				ELIMI_CAPACITY /
				rd.uniform(2, std::min(8, static_cast<int>(ELIMI_CAPACITY)));
			return {
				rd.uniform(0, break_p),
				rd.uniform(break_p, static_cast<int>(ELIMI_CAPACITY - 1)),
			};
		}
		return {0, 0};
	}

private:
	static constexpr size_type CAPACITY = N;
	static constexpr size_type ELIMI_CAPACITY = ElimiSlot;

	ElimiArray<T, ElimiSlot> elimi_{};
};
} // namespace concur