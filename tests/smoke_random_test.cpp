/**
 * @file smoke_random_test.cpp
 * @brief Minimal smoke coverage for the rng facilities.
 *
 * xoshiro256pp and these random helpers have their own full test suite in the
 * upstream xoshiro256pp project.  This file only verifies that the facilities
 * compile and behave sanely when used from ConcurrentCpp.
 */

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <vector>

#include "common/random.hpp"

namespace {

TEST(RandomSmoke, UniformDrawsStayInRange) {
	auto stream = rng::make_stream(42ull);

	for (int i = 0; i < 128; ++i) {
		const int integer = stream.uniform(1, 6);
		EXPECT_GE(integer, 1);
		EXPECT_LE(integer, 6);

		const double real = stream.uniform(0.0, 1.0);
		EXPECT_GE(real, 0.0);
		EXPECT_LT(real, 1.0);
	}

	EXPECT_FALSE(stream.coin(0.0));
	EXPECT_TRUE(stream.coin(1.0));
	EXPECT_FALSE(rng::random_bool(0.0));
	EXPECT_TRUE(rng::random_bool(1.0));
}

TEST(RandomSmoke, ShufflePickFillAndSynchronizedStream) {
	auto stream = rng::make_stream(7ull);
	std::vector<int> values{1, 2, 3, 4, 5};

	stream.shuffle(values);
	std::sort(values.begin(), values.end());
	EXPECT_EQ(values, (std::vector<int>{1, 2, 3, 4, 5}));

	const int picked = stream.pick(values);
	EXPECT_TRUE(picked == 10 || (picked >= 1 && picked <= 5));

	std::array<std::byte, 32> buffer{};
	stream.fill_bytes(buffer);
	EXPECT_FALSE(std::all_of(buffer.begin(), buffer.end(), [](std::byte byte) {
		return byte == std::byte{0};
	}));

	rng::synchronized_stream<> synchronized{2024ull};
	const int value = synchronized.uniform(0, 9);
	EXPECT_GE(value, 0);
	EXPECT_LE(value, 9);
}

} // namespace
