#pragma once

/**
 * @file backoff.hpp
 * @author modelTR (mxtian_ran@163.com)
 * @brief A simple implenment of mutex backoff
 * @version 0.2
 * @date 2026-11-23
 * @copyright MIT License (c) 2026
 *
 */

#include <algorithm>
#include <atomic>
#include <emmintrin.h>
#include <mutex>

#include "random.hpp"

namespace concur::details {

/// Exponential backoff shared by the stack implementations.
class Backoff {
public:
	Backoff(const int max_delay, const int min_delay, const int retry_time)
		: rd_nums_(rng::make_stream()), max_delay_(std::max(1, max_delay)),
		  min_delay_(std::max(1, min_delay)),
		  retry_time_(std::max(1, retry_time)),
		  limit_(std::max(1, min_delay)) {}

	/// @return true while the caller is still within its retry budget, false
	/// afterwards.  Every call pauses, even after the budget is spent,
	/// so a caller that intentionally keeps retrying (lock-free CAS
	/// contention) still backs off instead of busy-spinning.
	auto backoff() -> bool {
		std::scoped_lock lk{mt_};
		const int delay = std::max(1, rd_nums_.uniform(0, limit_));
		for (int i = 0; i < delay; ++i) {
			_mm_pause();
		}
		limit_ = std::min(max_delay_, std::max(min_delay_, 2 * limit_));
		return ++cur_count_ <= retry_time_;
	}

	/// Called after a successful push/pop so the next contention episode
	/// starts from a short delay again.
	auto reset() -> void {
		std::scoped_lock lk{mt_};
		cur_count_ = 0;
		limit_ = min_delay_;
	}

private:
	std::mutex mt_;
	rng::stream<> rd_nums_;
	const int max_delay_;
	const int min_delay_;
	const int retry_time_;
	int cur_count_ = 0;
	int limit_{};
};

class BackoffMutex {
public:
	auto lock() -> void {
		Backoff bk{MAX_DELAY, MIN_DELAY, RETRY_TIME};
		while (true) {
			while (state_.test())
				;
			if (!state_.test_and_set()) {
				return;
			} else {
				bk.backoff();
			}
		}
	}
	auto unlock() -> void { state_.clear(); }

private:
	std::atomic_flag state_;
	static const int MAX_DELAY = 100;
	static const int MIN_DELAY = 5;
	static const int RETRY_TIME = 5;
};

} // namespace concur::details