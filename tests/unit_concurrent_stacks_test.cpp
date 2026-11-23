/**
 * @file unit_concurrent_stacks_test.cpp
 * @brief Unit tests for concurrent push/pop behaviour.
 *
 * Every worker pushes a disjoint value range, then every worker pops the same
 * number of values.  The tests verify that no value is lost, duplicated, or
 * replaced while the stack is shared by several threads.
 */

#include <gtest/gtest.h>

#include <algorithm>
#include <cstddef>
#include <thread>

#include "common/alias.hpp"
#include "stacks/elimi_backoff_stack.hpp"
#include "stacks/lockfree_stack.hpp"
#include "stacks/mutex_stack.hpp"

namespace {

constexpr size_t CAPACITY = 2048;
constexpr int THREADS = 4;
constexpr int OPS_PER_THREAD = 256;
constexpr int TOTAL_OPS = THREADS * OPS_PER_THREAD;

/// @brief Push a value, retrying while a bounded push reports failure.
template <typename Stack>
auto push_until_accepted(Stack &stack, int value) -> void {
	while (!stack.push(value)) {
		std::this_thread::yield();
	}
}

/// @brief Pop a value, retrying while a bounded pop returns nullopt.
template <typename Stack> auto pop_until_value(Stack &stack) -> int {
	while (true) {
		if (auto value = stack.pop()) {
			return *value;
		}
		std::this_thread::yield();
	}
}

template <typename Stack> auto concurrent_push_then_pop() -> void {
	Stack stack;
	vec<std::thread> workers;
	workers.reserve(THREADS);

	for (int t = 0; t < THREADS; ++t) {
		workers.emplace_back([&stack, t] {
			for (int i = 0; i < OPS_PER_THREAD; ++i) {
				push_until_accepted(stack, (t * OPS_PER_THREAD) + i);
			}
		});
	}
	for (auto &worker : workers) {
		worker.join();
	}
	ASSERT_EQ(stack.size(), static_cast<std::size_t>(TOTAL_OPS));

	vec<vec<int>> popped(THREADS);
	workers.clear();
	for (int t = 0; t < THREADS; ++t) {
		workers.emplace_back([&stack, &popped, t] {
			for (int i = 0; i < OPS_PER_THREAD; ++i) {
				popped[t].push_back(pop_until_value(stack));
			}
		});
	}
	for (auto &worker : workers) {
		worker.join();
	}

	vec<int> all;
	all.reserve(TOTAL_OPS);
	for (const auto &chunk : popped) {
		all.insert(all.end(), chunk.begin(), chunk.end());
	}
	std::ranges::sort(all);

	ASSERT_EQ(all.size(), static_cast<std::size_t>(TOTAL_OPS));
	for (int i = 0; i < TOTAL_OPS; ++i) {
		EXPECT_EQ(all[i], i);
	}
	EXPECT_TRUE(stack.empty());
}

TEST(LOCKFREE_STACK_UNIT, CONCURRENT_PUSH_POP_PRESERVES_EVERY_VALUE) {
	concurrent_push_then_pop<concur::LockFreeStack<int, CAPACITY>>();
}

TEST(MUTEX_STACK_UNIT, CONCURRENT_PUSH_POP_PRESERVES_EVERY_VALUE) {
	concurrent_push_then_pop<concur::Stack<int, CAPACITY>>();
}

TEST(ELIMI_BACKOFF_STACK_UNIT, CONCURRENT_PUSH_POP_PRESERVES_EVERY_VALUE) {
	concurrent_push_then_pop<concur::ElimiBackoffStack<int, CAPACITY>>();
}

} // namespace
