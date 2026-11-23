/**
 * @file smoke_primitives_test.cpp
 * @brief Smoke tests for the low-level building blocks.
 *
 * Covers the packed-index helpers, AtomicTaggedPtr, LockFreeExchanger,
 * ElimiArray and Backoff.  Only the deterministic paths are exercised:
 * an exchange without a partner returns TIMEOUT instead of waiting forever.
 */

#include <gtest/gtest.h>

#include <cstdint>

#include "common/atomic_tagged_ptr.hpp"
#include "common/backoff.hpp"
#include "common/common.hpp"
#include "common/elimi_array.hpp"
#include "common/lockfree_exchanger.hpp"

namespace {

struct alignas(64) Node {
	int value = 0;
};

TEST(CommonSmoke, PackRoundTripsIndexAndTag) {
	const auto packed = concur::details::pack(0x12345678u, 0xABCDu);

	EXPECT_EQ(concur::details::idx_of(packed), 0x12345678u);
	EXPECT_EQ(concur::details::tag_of(packed), 0xABCDu);
}

TEST(AtomicTaggedPtrSmoke, StoreAndSnapshot) {
	concur::details::AtomicTaggedPtr<Node, 16> ptr{nullptr, 0};
	Node a;
	Node b;

	EXPECT_EQ(ptr.get(), nullptr);
	EXPECT_EQ(ptr.get_tag(), std::uintptr_t{0});

	ptr.store(&a, 7);
	EXPECT_EQ(ptr.get(), &a);
	EXPECT_EQ(ptr.get_tag(), std::uintptr_t{7});

	const auto snapshot = ptr.snapshot();
	EXPECT_EQ(snapshot.ptr, &a);
	EXPECT_EQ(snapshot.tag, std::uintptr_t{7});

	ptr.store(&b, 9);
	EXPECT_EQ(ptr.get(), &b);
	EXPECT_EQ(ptr.get_tag(), std::uintptr_t{9});
}

TEST(AtomicTaggedPtrSmoke, CompareExchangeUpdatesPointerAndTag) {
	concur::details::AtomicTaggedPtr<Node, 16> ptr{nullptr, 0};
	Node a;
	Node b;
	ptr.store(&a, 7);

	Node *expected_ptr = &a;
	std::uintptr_t expected_tag = 7;
	EXPECT_TRUE(ptr.compare_exchange_strong(expected_ptr, expected_tag, &b, 8));
	EXPECT_EQ(ptr.get(), &b);
	EXPECT_EQ(ptr.get_tag(), std::uintptr_t{8});

	// A stale expectation fails and is refreshed with the current value.
	expected_ptr = &a;
	expected_tag = 7;
	EXPECT_FALSE(
		ptr.compare_exchange_strong(expected_ptr, expected_tag, &a, 1));
	EXPECT_EQ(expected_ptr, &b);
	EXPECT_EQ(expected_tag, std::uintptr_t{8});
}

TEST(LockFreeExchangerSmoke, WithoutPartnerTimesOut) {
	concur::LockFreeExchanger<int> exchanger;

	const auto result = exchanger.exchange(nullptr, 0);

	EXPECT_EQ(result.state,
			  concur::LockFreeExchanger<int>::ExchResState::TIMEOUT);
	EXPECT_FALSE(result.value.has_value());
}

TEST(ElimiArraySmoke, VisitWithoutPartnerTimesOut) {
	concur::ElimiArray<int, 1> array;

	const auto result = array.visit(nullptr, 0, 0);

	EXPECT_EQ(result.state,
			  concur::LockFreeExchanger<int>::ExchResState::TIMEOUT);
	EXPECT_FALSE(result.value.has_value());
}

TEST(BackoffSmoke, RetryBudgetIsBounded) {
	concur::details::Backoff backoff{1, 1, 2};

	EXPECT_TRUE(backoff.backoff());
	EXPECT_TRUE(backoff.backoff());
	EXPECT_FALSE(backoff.backoff());

	backoff.reset();
	EXPECT_TRUE(backoff.backoff());
}

} // namespace
