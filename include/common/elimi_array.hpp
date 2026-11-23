/**
 * @file lockfree_exchanger.hpp
 * @author modelTR (mxtian_ran@163.com)
 * @brief A simple impl of ElimiArray using AtomicTaggedPtr. 
 * @version 0.2
 * @date 2026-11-23
 *
 * @copyright MIT License (c) 2026
 *
 */

#pragma once

#include "lockfree_exchanger.hpp"
#include "common/random.hpp"

#include <array>
#include <cassert>
#include <cstddef>

namespace concur {
template <class T, size_t Slots> class ElimiArray {
public:
	static_assert(Slots >= 1, "ElimiArray needs at least one exchange slot");

	using value_type = T;
	using pointer = T *;
	using size_type = size_t;

	using ExchRes = LockFreeExchanger<T>::ExchRes;

	static constexpr size_type CACHE_LINE = 64;
	struct alignas(CACHE_LINE) Slot {
		LockFreeExchanger<T> exchanger;
	};

	static_assert(sizeof(Slot) % CACHE_LINE == 0,
				  "Slot must occupy a whole cache line");

	[[nodiscard]] auto visit(pointer value, const int rng_begin,
							 const int rng_end) -> ExchRes {
		const int idx = rng::random_int(rng_begin, rng_end);
		assert(idx >= 0 && static_cast<size_type>(idx) < Slots);
		return exchanger_at(static_cast<size_type>(idx))
			.exchange(value, duration);
	}

private:
	[[nodiscard]] auto exchanger_at(size_type idx) -> LockFreeExchanger<T> & {
		return exchangers_[idx].exchanger;
	}

	static constexpr int duration = 1000;
	std::array<Slot, Slots> exchangers_;
};
} // namespace concur