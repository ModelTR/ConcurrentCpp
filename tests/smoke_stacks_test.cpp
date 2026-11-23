/**
 * @file smoke_stacks_test.cpp
 * @brief Smoke tests for the three stack implementations.
 *
 * These only exercise the documented happy paths plus the bounded push/pop
 * failure paths that are guaranteed to return.  Operations that block until
 * another thread makes progress (an empty LockFreeStack::pop(), for example)
 * are left to the unit tests.
 */

#include <gtest/gtest.h>

#include <cstddef>
#include <optional>

#include "stacks/elimi_backoff_stack.hpp"
#include "stacks/lockfree_stack.hpp"
#include "stacks/mutex_stack.hpp"

namespace {

TEST(MUTEX_STACK_SMOKE, DEFAULT_CONSTRUCTED_IS_EMPTY) {
	concur::Stack<int, 4> stack;

	EXPECT_TRUE(stack.empty());
	EXPECT_EQ(stack.size(), std::size_t{0});
	EXPECT_EQ(stack.capacity(), std::size_t{4});
}

TEST(MUTEX_STACK_SMOKE, PUSH_POP_IS_LIFO) {
	concur::Stack<int, 4> stack;

	EXPECT_TRUE(stack.push(1));
	EXPECT_TRUE(stack.push(2));
	EXPECT_EQ(stack.size(), std::size_t{2});

	auto top = stack.pop();
	ASSERT_TRUE(top.has_value());
	EXPECT_EQ(*top, 2);

	auto bottom = stack.pop();
	ASSERT_TRUE(bottom.has_value());
	EXPECT_EQ(*bottom, 1);

	EXPECT_TRUE(stack.empty());
}

TEST(MUTEX_STACK_SMOKE, FULL_AND_EMPTY_BOUNDARIES) {
	concur::Stack<int, 2> stack{1, 2};

	EXPECT_EQ(stack.size(), std::size_t{2});
	EXPECT_FALSE(stack.push(3));

	EXPECT_TRUE(stack.pop().has_value());
	EXPECT_TRUE(stack.pop().has_value());
	EXPECT_FALSE(stack.pop().has_value());
}

TEST(MUTEX_STACK_SMOKE, VALUE_CONSTRUCTOR_FILLS_STACK) {
	concur::Stack<int, 3> stack(2, 7);

	ASSERT_EQ(stack.size(), std::size_t{2});
	EXPECT_EQ(stack.pop(), std::optional<int>{7});
	EXPECT_EQ(stack.pop(), std::optional<int>{7});
}

TEST(LOCKFREE_STACK_SMOKE, DEFAULT_CONSTRUCTED_IS_EMPTY) {
	concur::LockFreeStack<int, 4> stack;

	EXPECT_TRUE(stack.empty());
	EXPECT_FALSE(stack.full());
	EXPECT_EQ(stack.size(), std::size_t{0});
	EXPECT_EQ(stack.capacity(), std::size_t{4});
}

TEST(LOCKFREE_STACK_SMOKE, PUSH_POP_IS_LIFO) {
	concur::LockFreeStack<int, 4> stack;

	EXPECT_TRUE(stack.push(1));
	EXPECT_TRUE(stack.push(2));
	EXPECT_EQ(stack.size(), std::size_t{2});

	auto top = stack.pop();
	ASSERT_TRUE(top.has_value());
	EXPECT_EQ(*top, 2);

	auto bottom = stack.pop();
	ASSERT_TRUE(bottom.has_value());
	EXPECT_EQ(*bottom, 1);

	EXPECT_TRUE(stack.empty());
}

TEST(LOCKFREE_STACK_SMOKE, INITIALIZER_LIST_CONSTRUCTION_POPS_IN_REVERSE) {
	concur::LockFreeStack<int, 4> stack{1, 2, 3};

	ASSERT_EQ(stack.size(), std::size_t{3});
	EXPECT_EQ(stack.pop(), std::optional<int>{3});
	EXPECT_EQ(stack.pop(), std::optional<int>{2});
	EXPECT_EQ(stack.pop(), std::optional<int>{1});
}

TEST(LOCKFREE_STACK_SMOKE, PUSH_AFTER_INITIALIZATION) {
	concur::LockFreeStack<int, 4> stack{1, 2, 3};

	EXPECT_TRUE(stack.push(4));
	EXPECT_EQ(stack.size(), std::size_t{4});
	EXPECT_TRUE(stack.full());

	EXPECT_EQ(stack.pop(), std::optional<int>{4});
	EXPECT_FALSE(stack.full());
}

TEST(ELIMI_BACKOFF_STACK_SMOKE, DEFAULT_CONSTRUCTED_IS_EMPTY) {
	concur::ElimiBackoffStack<int, 4> stack;

	EXPECT_TRUE(stack.empty());
	EXPECT_EQ(stack.size(), std::size_t{0});
	EXPECT_EQ(stack.capacity(), std::size_t{4});
}

TEST(ELIMI_BACKOFF_STACK_SMOKE, PUSH_POP_IS_LIFO) {
	concur::ElimiBackoffStack<int, 4> stack;

	EXPECT_TRUE(stack.push(1));
	EXPECT_TRUE(stack.push(2));
	EXPECT_EQ(stack.size(), std::size_t{2});

	auto top = stack.pop();
	ASSERT_TRUE(top.has_value());
	EXPECT_EQ(*top, 2);

	auto bottom = stack.pop();
	ASSERT_TRUE(bottom.has_value());
	EXPECT_EQ(*bottom, 1);
}

TEST(ELIMI_BACKOFF_STACK_SMOKE, INITIALIZER_LIST_CONSTRUCTION) {
	concur::ElimiBackoffStack<int, 4> stack{1, 2, 3};

	ASSERT_EQ(stack.size(), std::size_t{3});
	EXPECT_EQ(stack.pop(), std::optional<int>{3});
	EXPECT_EQ(stack.pop(), std::optional<int>{2});
	EXPECT_EQ(stack.pop(), std::optional<int>{1});
}

} // namespace
