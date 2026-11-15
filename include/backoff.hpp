#pragma once

/**
 * @file backoff.hpp
 * @author modelTR (mxtian_ran@163.com)
 * @brief A simple implenment of mutex backoff
 * @version 0.1
 * @date 2026-11-02
 * @copyright MIT License (c) 2026
 *
 */

#include <algorithm>
#include <atomic>
#include <chrono>
#include <thread>

#include "random.hpp"

namespace concur::details {

class Backoff {
public:
	Backoff(const int max_delay, const int min_delay, const int retry_time)
		: rd_nums_(rng::make_stream()), max_delay_(max_delay),
		  min_delay_(min_delay), retry_time_(retry_time), limit_(min_delay) {}

	auto backoff() -> bool {
		if (cur_count >= retry_time_) {
			return false;
		}
		const int delay = rd_nums_.uniform(0, limit_);
		limit_ = std::min(max_delay_, 2 * delay);
		std::this_thread::sleep_for(std::chrono::milliseconds{limit_});
		cur_count++;
		return true;
	}

private:
	rng::stream<> rd_nums_;
	const int max_delay_;
	const int min_delay_;
	const int retry_time_;
	int cur_count = 0;
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
	std::atomic_flag state_ = false;
	static const int MAX_DELAY = 100;
	static const int MIN_DELAY = 5;
	static const int RETRY_TIME = 5;
};

} // namespace concur::details