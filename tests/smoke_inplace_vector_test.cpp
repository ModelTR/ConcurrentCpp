/**
 * @file smoke_inplace_vector_test.cpp
 * @brief Minimal smoke coverage for concur::inplace_vector.
 *
 * The full test suite for this container lives in the upstream
 * inplace-vector project.  Here we only check that it compiles and that the
 * basic fixed-capacity contract holds inside ConcurrentCpp.
 */

#include <gtest/gtest.h>

#include <cstddef>
#include <new>

#include "common/inplace_vector.hpp"

namespace {

TEST(InplaceVectorSmoke, DefaultConstructedIsEmpty) {
	concur::inplace_vector<int, 4> values;

	EXPECT_TRUE(values.empty());
	EXPECT_EQ(values.size(), std::size_t{0});
	EXPECT_EQ(values.capacity(), std::size_t{4});
}

TEST(InplaceVectorSmoke, PushPopAndCapacityLimit) {
	concur::inplace_vector<int, 2> values{1};

	values.push_back(2);
	ASSERT_EQ(values.size(), std::size_t{2});
	EXPECT_EQ(values.front(), 1);
	EXPECT_EQ(values.back(), 2);

	EXPECT_EQ(values.try_push_back(3), nullptr);
	EXPECT_THROW(values.push_back(3), std::bad_alloc);

	values.pop_back();
	EXPECT_EQ(values.back(), 1);

	values.clear();
	EXPECT_TRUE(values.empty());
}

} // namespace
