/**
 * @file main.cpp
 * @author modelTR (mxtian_ran@163.com)
 * @brief Main file
 * @version 0.1
 * @date 2026-11-02
 * @copyright MIT License (c) 2026
 *
 */

/// @todo Sorting the order of output stream message for debugging

#include <chrono>
#include <cstddef>
#include <cstdlib>
#include <exception>
#include <format>
#include <functional>
#include <iostream>
#include <thread>

#include "concurrent_stack.hpp"
#include "include/random.hpp"
#include "inplace_vector.hpp"

constexpr size_t CAPACITY = 10;
constexpr size_t THREAD_NUMS = 3;

using std::chrono::steady_clock;
using steady_time_point = std::chrono::time_point<steady_clock>;

static auto
single_thread(const concur::inplace_vector<std::function<void(int)>, 2> &fns)
	-> void {
	auto rd_nums = rng::make_stream();
	for (int i = 0; i < rd_nums.uniform(0, 10); ++i) {
		fns[rd_nums.uniform(0, 1)](rd_nums.uniform(0, 99));
	}
}

auto main() -> int {

	try {
		auto rd = rng::make_stream();

		concur::inplace_vector<std::function<void(int)>, 2> fns;
		concur::inplace_vector<std::jthread, THREAD_NUMS> threads;

		concur::ElimiBackoffStack<int, CAPACITY> stack{1, 2, 3, 4, 5};

		fns.push_back([&stack](int x) {
			if (stack.push(x)) {
				std::cout << std::format("[{}] : push {}\n",
										 std::chrono::system_clock::now(), x);
			}
		});
		fns.push_back([&stack](int) {
			auto res = stack.pop();
			if (res.has_value()) {
				std::cout << std::format("[{}] : pop {}\n",
										 std::chrono::system_clock::now(),
										 res.value());
			} else {
				std::cout << std::format("[{}] : poping falied\n",
										 std::chrono::system_clock::now());
			}
		});

		stack.print();

		for (size_t i = 0; i < THREAD_NUMS; i++) {
			threads.push_back(std::jthread{single_thread, fns});
		}

		for (auto &thread : threads) {
			thread.join();
		}

		stack.print();

		return EXIT_SUCCESS;
	} catch (std::exception &e) {
		std::cout << e.what();
	}
}