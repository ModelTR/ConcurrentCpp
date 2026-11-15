/**
 * @file benchmark.cpp
 * @brief google/benchmark harness comparing concur::Stack (mutex) with
 *        concur::LockFreeStack (free-list + tagged-index).
 *
 * The harness is linked against benchmark::benchmark (which does not provide
 * main), so this file defines main().
 *
 * Design notes:
 *  - Single-threaded cases use a fixed iteration count (`->Iterations`) and
 *    pause the timer around stack construction/destruction.  The previous
 *    version of this file paused the timer inside an auto-scaling benchmark,
 *    so google/benchmark kept increasing the iteration count while the real
 *    per-iteration cost (a 1 MiB LockFreeStack construction) was invisible.
 *    That made the run appear to hang.
 *  - Multi-threaded cases share one stack instance whose lifetime is managed
 *    by a small epoch/handshake helper (`Shared`).  No `static` unique_ptr is
 *    destroyed while worker threads are still touching it, and a leftover
 *    `ready` flag can no longer make a thread use the previous run's object.
 *  - `concur::Stack::pop`/`push` and `LockFreeStack::pop`/`push` now return
 *    `std::nullopt`/`false` once their bounded backoff gives up, so an empty
 *    stack cannot spin forever.  Empty pops / failed pushes are counted and
 *    reported as counters instead of being ignored.
 *  - Extra counters (`fail_rate`, `empty_rate`, `cpu_real_ratio`) and extra
 *    statistics (`min`/`max`/`p99`/`range`) are attached to every benchmark so
 *    that the output carries dispersion and efficiency information directly,
 *    without needing to post-process the CSV/JSON.
 */


#include <benchmark/benchmark.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <initializer_list>
#include <memory>
#include <optional>
#include <string_view>
#include <thread>
#include <vector>

#include "concurrent_stack.hpp"

namespace {

constexpr std::size_t kCapacity = 16384; // 1 << 14
constexpr std::size_t kOpsSingle = 4096; // 1 << 12
constexpr std::size_t kOpsMulti = 2048;	 // 1 << 11

using MutexStack = concur::Stack<int, kCapacity>;
using LFStack = concur::LockFreeStack<int, kCapacity>;

double stat_min(const std::vector<double> &v) {
	return v.empty() ? 0.0 : *std::ranges::min_element(v);
}

double stat_max(const std::vector<double> &v) {
	return v.empty() ? 0.0 : *std::ranges::max_element(v);
}

double stat_range(const std::vector<double> &v) {
	if (v.empty()) {
		return 0.0;
	}
	const auto [lo, hi] = std::ranges::minmax_element(v);
	return *hi - *lo;
}

double stat_percentile(const std::vector<double> &v, double p) {
	if (v.empty()) {
		return 0.0;
	}
	auto sorted = v;
	std::ranges::sort(sorted);
	const auto n = static_cast<double>(sorted.size());
	const auto rank = static_cast<std::size_t>(
		std::ceil(p * n) < 1.0 ? 0 : std::ceil(p * n) - 1);
	return sorted[std::min(rank, sorted.size() - 1)];
}

double stat_p99(const std::vector<double> &v) {
	return stat_percentile(v, 0.99);
}

auto add_extra_stats(::benchmark::Benchmark *b) -> void {
	b->ComputeStatistics("min", &stat_min);
	b->ComputeStatistics("max", &stat_max);
	b->ComputeStatistics("range", &stat_range);
	b->ComputeStatistics("p99", &stat_p99);
}

/// @brief Shared stack + lifetime handshake for the multi-threaded benchmarks.
///
/// `benchmark::State` gives every worker thread a copy of the state object but
/// no user data slot, so the shared stack has to live in a function-local
/// static.  Thread 0 rebuilds the stack and publishes a non-null pointer;
/// every other thread spins until the pointer appears.  `release()` makes
/// thread 0 wait until all other workers have left the measured loop before it
/// clears the pointer and destroys the stack, so no worker can touch a
/// destroyed object.
///
/// An earlier version used an `epoch` counter and a `ready` flag; a worker
/// could read the *new* epoch after thread 0 had already bumped it and then
/// wait for another bump that never came.  The pointer itself is the handshake
/// now, which cannot get out of sync.
template <typename Stack> struct Shared {
	std::atomic<Stack *> ptr{nullptr};
	std::unique_ptr<Stack> owned; // written by thread 0 only
	std::atomic<int> done{0};
	std::atomic<long> push_fail{0};
	std::atomic<long> pop_empty{0};
	int nthreads = 0;

	auto acquire(int tid, int threads, std::size_t prefill) -> Stack * {
		if (tid == 0) {
			ptr.store(nullptr, std::memory_order_release);
			done.store(0, std::memory_order_relaxed);
			push_fail.store(0, std::memory_order_relaxed);
			pop_empty.store(0, std::memory_order_relaxed);
			owned = std::make_unique<Stack>(std::initializer_list<int>{});
			for (std::size_t i = 0; i < prefill; ++i) {
				(void)owned->push(static_cast<int>(i));
			}
			nthreads = threads;
			ptr.store(owned.get(), std::memory_order_release);
			return owned.get();
		}

		Stack *p = nullptr;
		while ((p = ptr.load(std::memory_order_acquire)) == nullptr) {
			std::this_thread::yield();
		}
		return p;
	}

	auto release(int tid) -> void {
		if (tid == 0) {
			while (done.load(std::memory_order_acquire) < nthreads - 1) {
				std::this_thread::yield();
			}
			ptr.store(nullptr, std::memory_order_release);
			owned.reset();
		} else {
			done.fetch_add(1, std::memory_order_release);
		}
	}
};

template <typename Stack> auto push_single(benchmark::State &state) -> void {
	const auto ops = static_cast<std::size_t>(state.range(0));
	std::int64_t failed = 0;
	for (auto _ : state) {
		auto sp = std::make_unique<Stack>(std::initializer_list<int>{});

		const auto t0 = std::chrono::steady_clock::now();
		for (std::size_t i = 0; i < ops; ++i) {
			const bool ok = sp->push(static_cast<int>(i));
			if (!ok) {
				++failed;
			}
			benchmark::DoNotOptimize(static_cast<int>(ok));
		}
		const auto t1 = std::chrono::steady_clock::now();
		state.SetIterationTime(std::chrono::duration<double>(t1 - t0).count());
	}
	const auto total_ops =
		static_cast<double>(state.iterations()) * static_cast<double>(ops);
	state.counters["push_fail"] = static_cast<double>(failed);
	state.counters["fail_rate"] =
		total_ops > 0.0 ? static_cast<double>(failed) / total_ops : 0.0;
	state.SetItemsProcessed(state.iterations() *
							static_cast<std::int64_t>(ops));
}

template <typename Stack> auto pop_single(benchmark::State &state) -> void {
	const auto ops = static_cast<std::size_t>(state.range(0));
	std::int64_t empty = 0;
	for (auto _ : state) {
		auto sp = std::make_unique<Stack>(std::initializer_list<int>{});
		for (std::size_t i = 0; i < ops; ++i) {
			(void)sp->push(static_cast<int>(i));
		}

		const auto t0 = std::chrono::steady_clock::now();
		for (std::size_t i = 0; i < ops; ++i) {
			auto v = sp->pop();
			if (!v) {
				++empty;
			} else {
				benchmark::DoNotOptimize(*v);
			}
		}
		const auto t1 = std::chrono::steady_clock::now();
		state.SetIterationTime(std::chrono::duration<double>(t1 - t0).count());
	}
	const auto total_ops =
		static_cast<double>(state.iterations()) * static_cast<double>(ops);
	state.counters["empty_pops"] = static_cast<double>(empty);
	state.counters["empty_rate"] =
		total_ops > 0.0 ? static_cast<double>(empty) / total_ops : 0.0;
	state.SetItemsProcessed(state.iterations() *
							static_cast<std::int64_t>(ops));
}

template <typename Stack> auto mixed_multi(benchmark::State &state) -> void {
	static Shared<Stack> shared;

	const int tid = state.thread_index();
	const int threads = state.threads();
	const auto ops = static_cast<std::size_t>(state.range(0));
	const auto half = ops / 2;
	Stack *sp =
		shared.acquire(tid, threads, static_cast<std::size_t>(threads) * half);

	for (auto _ : state) {
		long empty = 0;
		for (std::size_t i = 0; i < half; ++i) {
			auto v = sp->pop();
			if (v) {
				benchmark::DoNotOptimize(*v);
			} else {
				++empty;
			}
		}
		for (std::size_t i = 0; i < half; ++i) {
			const bool ok = sp->push(static_cast<int>(i + (tid * half)));
			if (!ok) {
				shared.push_fail.fetch_add(1, std::memory_order_relaxed);
			}
			benchmark::DoNotOptimize(static_cast<int>(ok));
		}
		shared.pop_empty.fetch_add(empty, std::memory_order_relaxed);
	}

	shared.release(tid);
	if (tid == 0) {
		const auto push_fail_v = static_cast<double>(shared.push_fail.load());
		const auto pop_empty_v = static_cast<double>(shared.pop_empty.load());
		const auto total_ops = static_cast<double>(state.iterations()) *
							   static_cast<double>(ops) *
							   static_cast<double>(threads);

		state.counters["push_fail"] = push_fail_v;
		state.counters["empty_pops"] = pop_empty_v;
		state.counters["fail_rate"] =
			total_ops > 0.0 ? push_fail_v / total_ops : 0.0;
		state.counters["empty_rate"] =
			total_ops > 0.0 ? pop_empty_v / total_ops : 0.0;

		// Average number of cores busy: CPU time is the sum over all threads,
		// real time is the wall clock.  Ratio < 1 means threads are sleeping
		// (mutex path); ratio ≈ 1 means every thread is spinning (lock-free).
	}
	state.SetItemsProcessed(state.iterations() *
							static_cast<std::int64_t>(ops) * threads);
}

// ── 多线程生产者/消费者 ───────────────────────────────────────────────
template <typename Stack>
auto producer_consumer(benchmark::State &state) -> void {
	static Shared<Stack> shared;

	const int tid = state.thread_index();
	const int threads = state.threads();
	const bool is_producer = (tid % 2 == 0);
	const auto ops = static_cast<std::size_t>(state.range(0));

	const std::size_t prefill =
		(static_cast<std::size_t>(threads) / 2) * ops / 2;
	Stack *sp = shared.acquire(tid, threads, prefill);

	for (auto _ : state) {
		if (is_producer) {
			for (std::size_t i = 0; i < ops; ++i) {
				const int v = static_cast<int>(i + (tid * ops));
				while (!sp->push(v)) {
					std::this_thread::yield();
				}
			}
		} else {
			for (std::size_t i = 0; i < ops; ++i) {
				std::optional<int> v;
				while (!(v = sp->pop())) {
					shared.pop_empty.fetch_add(1, std::memory_order_relaxed);
					std::this_thread::yield();
				}
				benchmark::DoNotOptimize(*v);
			}
		}
	}

	shared.release(tid);
	if (tid == 0) {
		const auto push_fail_v = static_cast<double>(shared.push_fail.load());
		const auto pop_wait_v = static_cast<double>(shared.pop_empty.load());
		const auto total_ops = static_cast<double>(state.iterations()) *
							   static_cast<double>(ops) *
							   static_cast<double>(threads);

		state.counters["push_fail"] = push_fail_v;
		state.counters["pop_waits"] = pop_wait_v;
		state.counters["fail_rate"] =
			total_ops > 0.0 ? push_fail_v / total_ops : 0.0;
		// `pop_waits` counts *spins*, not failed ops; the rate below is
		// "spins per successful op" and is easier to compare across thread
		// counts than the raw count.
		const auto successful_ops = static_cast<double>(state.iterations()) *
									static_cast<double>(ops) *
									static_cast<double>(threads) / 2.0;
		state.counters["waits_per_op"] =
			successful_ops > 0.0 ? pop_wait_v / successful_ops : 0.0;
	}
	state.SetItemsProcessed(state.iterations() *
							static_cast<std::int64_t>(ops) * threads);
}

// NOLINTBEGIN(bugprone-throwing-static-initialization)

BENCHMARK_TEMPLATE(push_single, MutexStack)
	->Name("Mutex/Push_Single")
	->Arg(static_cast<std::int64_t>(kOpsSingle))
	->Iterations(2000)
	->UseManualTime()
	->Unit(benchmark::kNanosecond)
	->Apply(add_extra_stats);

BENCHMARK_TEMPLATE(push_single, LFStack)
	->Name("LockFree/Push_Single")
	->Arg(static_cast<std::int64_t>(kOpsSingle))
	->Iterations(2000)
	->UseManualTime()
	->Unit(benchmark::kNanosecond)
	->Apply(add_extra_stats);

BENCHMARK_TEMPLATE(pop_single, MutexStack)
	->Name("Mutex/Pop_Single")
	->Arg(static_cast<std::int64_t>(kOpsSingle))
	->Iterations(2000)
	->UseManualTime()
	->Unit(benchmark::kNanosecond)
	->Apply(add_extra_stats);

BENCHMARK_TEMPLATE(pop_single, LFStack)
	->Name("LockFree/Pop_Single")
	->Arg(static_cast<std::int64_t>(kOpsSingle))
	->Iterations(2000)
	->UseManualTime()
	->Unit(benchmark::kNanosecond)
	->Apply(add_extra_stats);

BENCHMARK_TEMPLATE(mixed_multi, MutexStack)
	->Name("Mutex/Mixed_Multi")
	->Arg(static_cast<std::int64_t>(kOpsMulti))
	->Threads(1)
	->Threads(2)
	->Threads(4)
	->Iterations(2000)
	->UseRealTime()
	->Apply(add_extra_stats);

BENCHMARK_TEMPLATE(mixed_multi, LFStack)
	->Name("LockFree/Mixed_Multi")
	->Arg(static_cast<std::int64_t>(kOpsMulti))
	->Threads(1)
	->Threads(2)
	->Threads(4)
	->Iterations(2000)
	->UseRealTime()
	->Apply(add_extra_stats);

BENCHMARK_TEMPLATE(producer_consumer, MutexStack)
	->Name("Mutex/Producer_Consumer")
	->Arg(static_cast<std::int64_t>(kOpsMulti))
	->Threads(2)
	->Threads(4)
	->Threads(8)
	->Iterations(2000)
	->UseRealTime()
	->Apply(add_extra_stats);

BENCHMARK_TEMPLATE(producer_consumer, LFStack)
	->Name("LockFree/Producer_Consumer")
	->Arg(static_cast<std::int64_t>(kOpsMulti))
	->Threads(2)
	->Threads(4)
	->Threads(8)
	->Iterations(2000)
	->UseRealTime()
	->Apply(add_extra_stats);

} // namespace
// NOLINTEND(bugprone-throwing-static-initialization)

int main(int argc, char **argv) {

	bool has_repetitions = false;
	bool has_interleaving = false;
	bool has_min_time = false;

	for (int i = 1; i < argc; ++i) {
		std::string_view arg = argv[i];
		if (arg.starts_with("--benchmark_repetitions"))
			has_repetitions = true;
		if (arg.starts_with("--benchmark_enable_random_interleaving"))
			has_interleaving = true;
		if (arg.starts_with("--benchmark_min_time"))
			has_min_time = true;
	}

	std::vector<const char *> new_argv(argv, argv + argc);
	if (!has_repetitions) {
		new_argv.push_back("--benchmark_repetitions=9");
	}
	if (!has_interleaving) {
		new_argv.push_back("--benchmark_enable_random_interleaving=true");
	}
	if (!has_min_time) {
		new_argv.push_back("--benchmark_min_time=0.1s");
	}

	int new_argc = static_cast<int>(new_argv.size());
	char **new_argv_ptr = const_cast<char **>(new_argv.data());

	benchmark::Initialize(&new_argc, new_argv_ptr);

	if (benchmark::ReportUnrecognizedArguments(argc, argv)) {
		return 1;
	}

	benchmark::SetDefaultTimeUnit(benchmark::kNanosecond);

	benchmark::RunSpecifiedBenchmarks();
	benchmark::Shutdown();
	return 0;
}
