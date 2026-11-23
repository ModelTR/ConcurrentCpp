/**
 * @file smoke_stacks_test.cpp
 * @brief Smoke tests for the three stack implementations.
 *
 * These only exercise the documented happy paths plus the bounded push/pop
 * failure paths that are guaranteed to return.  Operations that block until
 * another thread makes progress (an empty LockFreeStack::pop(), for example)
 * are intentionally left to dedicated unit tests.
 */

#include <gtest/gtest.h>

#include <cstddef>
#include <optional>

#include "stacks/elimi_backoff_stack.hpp"
#include "stacks/lockfree_stack.hpp"
#include "stacks/mutex_stack.hpp"

namespace {

TEST(MutexStackSmoke, DefaultConstructedIsEmpty) {
	concur::Stack<int, 4> stack;

	EXPECT_TRUE(stack.empty());
	EXPECT_EQ(stack.size(), std::size_t{0});
	EXPECT_EQ(stack.capacity(), std::size_t{4});
}

TEST(MutexStackSmoke, PushPopIsLifo) {
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

TEST(MutexStackSmoke, FullAndEmptyBoundaries) {
	concur::Stack<int, 2> stack{1, 2};

	EXPECT_EQ(stack.size(), std::size_t{2});
	EXPECT_FALSE(stack.push(3));

	EXPECT_TRUE(stack.pop().has_value());
	EXPECT_TRUE(stack.pop().has_value());
	EXPECT_FALSE(stack.pop().has_value());
}

TEST(MutexStackSmoke, ValueConstructorFillsStack) {
	concur::Stack<int, 3> stack(2, 7);

	ASSERT_EQ(stack.size(), std::size_t{2});
	EXPECT_EQ(stack.pop(), std::optional<int>{7});
	EXPECT_EQ(stack.pop(), std::optional<int>{7});
}

TEST(LockFreeStackSmoke, DefaultConstructedIsEmpty) {
	concur::LockFreeStack<int, 4> stack;

	EXPECT_TRUE(stack.empty());
	EXPECT_FALSE(stack.full());
	EXPECT_EQ(stack.size(), std::size_t{0});
	EXPECT_EQ(stack.capacity(), std::size_t{4});
}

TEST(LockFreeStackSmoke, PushPopIsLifo) {
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

TEST(LockFreeStackSmoke, InitializerListConstructionPopsInReverse) {
	concur::LockFreeStack<int, 4> stack{1, 2, 3};

	ASSERT_EQ(stack.size(), std::size_t{3});
	EXPECT_EQ(stack.pop(), std::optional<int>{3});
	EXPECT_EQ(stack.pop(), std::optional<int>{2});
	EXPECT_EQ(stack.pop(), std::optional<int>{1});
}

TEST(LockFreeStackSmoke, PushAfterInitialization) {
	concur::LockFreeStack<int, 4> stack{1, 2, 3};

	EXPECT_TRUE(stack.push(4));
	EXPECT_EQ(stack.size(), std::size_t{4});
	EXPECT_TRUE(stack.full());

	EXPECT_EQ(stack.pop(), std::optional<int>{4});
	EXPECT_FALSE(stack.full());
}

TEST(ElimiBackoffStackSmoke, DefaultConstructedIsEmpty) {
	concur::ElimiBackoffStack<int, 4> stack;

	EXPECT_TRUE(stack.empty());
	EXPECT_EQ(stack.size(), std::size_t{0});
	EXPECT_EQ(stack.capacity(), std::size_t{4});
}

TEST(ElimiBackoffStackSmoke, PushPopIsLifo) {
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

TEST(ElimiBackoffStackSmoke, InitializerListConstruction) {
	concur::ElimiBackoffStack<int, 4> stack{1, 2, 3};

	ASSERT_EQ(stack.size(), std::size_t{3});
	EXPECT_EQ(stack.pop(), std::optional<int>{3});
	EXPECT_EQ(stack.pop(), std::optional<int>{2});
	EXPECT_EQ(stack.pop(), std::optional<int>{1});
}

} // namespace
