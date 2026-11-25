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
 *  - `concur::Stack::push`/`pop` retry a bounded number of times and then
 *    return `false`/`std::nullopt`; the benchmark loops retry them and count
 *    those benchmark-level retries.  `LockFreeStack` and `ElimiBackoffStack`
 *    retry internally until the operation succeeds, so their retry counters
 *    stay at zero: the retries happen inside the stack, not in the benchmark.
 *    Do not read `fail_rate`/`waits_per_op` as a cross-implementation
 *    contention metric for those two stacks.
 *  - `SetItemsProcessed` is called once per worker with that worker's own item
 *    count; google/benchmark sums the per-thread values and divides by the
 *    wall-clock time to produce a global items/s rate.
 *  - Extra statistics (`min`/`max`/`range`/`iqr`) are attached to every
 *    benchmark.  p90/p99 are intentionally not registered: with the default
 *    nine repetitions they would all collapse to the maximum.
 */

#include <benchmark/benchmark.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <fstream>
#include <initializer_list>
#include <iomanip>
#include <ios>
#include <iostream>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include "common/alias.hpp"
#include "stacks/elimi_backoff_stack.hpp"
#include "stacks/lockfree_stack.hpp"
#include "stacks/mutex_stack.hpp"

/// @todo [x] Standardize namecase of constant with CONSTANT_CASE

namespace {

namespace rngs = std::ranges;

constexpr size_t CAPACITY = 16384;	// 1 << 14
constexpr size_t OPS_SINGLE = 4096; // 1 << 12
constexpr size_t OPS_MULTI = 2048;	// 1 << 11

using MutexStack = concur::Stack<int, CAPACITY>;
using LFStack = concur::LockFreeStack<int, CAPACITY>;
using EmiStack = concur::ElimiBackoffStack<int, CAPACITY, 64>;

// Small-capacity variants: they force full/empty contention, which is the
// regime where elimination can actually help.
using EmiSmall = concur::ElimiBackoffStack<int, 64, 64>;
using EmiSlot16 = concur::ElimiBackoffStack<int, 64, 16>;
using EmiSlot32 = concur::ElimiBackoffStack<int, 64, 32>;
using EmiSlot64 = concur::ElimiBackoffStack<int, 64, 64>;
using EmiSlot128 = concur::ElimiBackoffStack<int, 64, 128>;
using EmiCapacity256 = concur::ElimiBackoffStack<int, 256, 64>;
using EmiCapacity1024 = concur::ElimiBackoffStack<int, 1024, 64>;

auto stat_min(const vec<double> &v) -> double {
	return v.empty() ? 0.0 : *rngs::min_element(v);
}

auto stat_max(const vec<double> &v) -> double {
	return v.empty() ? 0.0 : *rngs::max_element(v);
}

auto stat_range(const vec<double> &v) -> double {
	if (v.empty()) {
		return 0.0;
	}
	const auto [lo, hi] = rngs::minmax_element(v);
	return *hi - *lo;
}

auto stat_percentile(const vec<double> &v, double p) -> double {
	if (v.empty()) {
		return 0.0;
	}
	auto sorted = v;
	rngs::sort(sorted);
	const auto n = static_cast<double>(sorted.size());
	const auto rank = static_cast<std::size_t>(
		std::ceil(p * n) < 1.0 ? 0 : std::ceil(p * n) - 1);
	return sorted[std::min(rank, sorted.size() - 1)];
}

auto stat_iqr(const vec<double> &v) -> double {
	return stat_percentile(v, 0.75) - stat_percentile(v, 0.25);
}

auto add_extra_stats(::benchmark::Benchmark *b) -> void {
	b->ComputeStatistics("min", &stat_min);
	b->ComputeStatistics("max", &stat_max);
	b->ComputeStatistics("range", &stat_range);
	b->ComputeStatistics("iqr", &stat_iqr);
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
	atomic::i32 done{0};
	// Benchmark-level retry counts.  Retries performed *inside* a stack
	// implementation are not visible here.
	atomic::i64 push_retries{0};
	atomic::i64 pop_retries{0};
	int nthreads = 0;

	auto acquire(int tid, int threads, size_t prefill) -> Stack * {
		if (tid == 0) {
			ptr.store(nullptr, memorder::release);
			done.store(0, memorder::relaxed);
			push_retries.store(0, memorder::relaxed);
			pop_retries.store(0, memorder::relaxed);
			owned = std::make_unique<Stack>(std::initializer_list<int>{});
			const auto clamped = std::min(prefill, owned->capacity());
			for (std::size_t i = 0; i < clamped; ++i) {
				(void)owned->push(static_cast<int>(i));
			}
			nthreads = threads;
			ptr.store(owned.get(), memorder::release);
			return owned.get();
		}

		Stack *p = nullptr;
		while ((p = ptr.load(memorder::acquire)) == nullptr) {
			std::this_thread::yield();
		}
		return p;
	}

	auto release(int tid) -> void {
		if (tid == 0) {
			while (done.load(memorder::acquire) < nthreads - 1) {
				std::this_thread::yield();
			}
			ptr.store(nullptr, memorder::release);
			owned.reset();
		} else {
			done.fetch_add(1, memorder::release);
		}
	}
};

template <typename Stack> auto push_single(benchmark::State &state) -> void {
	const auto ops = static_cast<size_t>(state.range(0));
	i64 failed = 0;
	for (auto _ : state) {
		auto sp = std::make_unique<Stack>(std::initializer_list<int>{});

		const auto t0 = std::chrono::steady_clock::now();
		for (size_t i = 0; i < ops; ++i) {
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
	state.counters["push_retries"] = static_cast<double>(failed);
	state.counters["fail_rate"] =
		total_ops > 0.0 ? static_cast<double>(failed) / total_ops : 0.0;
	state.SetItemsProcessed(state.iterations() * static_cast<i64>(ops));
}

template <typename Stack> auto pop_single(benchmark::State &state) -> void {
	const auto ops = static_cast<size_t>(state.range(0));
	i64 empty = 0;
	for (auto _ : state) {
		auto sp = std::make_unique<Stack>(std::initializer_list<int>{});
		for (size_t i = 0; i < ops; ++i) {
			(void)sp->push(static_cast<int>(i));
		}

		const auto t0 = std::chrono::steady_clock::now();
		for (size_t i = 0; i < ops; ++i) {
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
	state.counters["pop_retries"] = static_cast<double>(empty);
	state.counters["fail_rate"] =
		total_ops > 0.0 ? static_cast<double>(empty) / total_ops : 0.0;
	state.SetItemsProcessed(state.iterations() * static_cast<i64>(ops));
}

template <typename Stack> auto mixed_multi(benchmark::State &state) -> void {
	static Shared<Stack> shared;

	const int tid = state.thread_index();
	const int threads = state.threads();
	const auto ops = static_cast<size_t>(state.range(0));
	const auto half = ops / 2;
	// Keep the stack away from both the empty and the full boundary: one
	// iteration pops `threads * half` items in total, so half the capacity
	// leaves enough headroom for the mixed workload to stay balanced.
	Stack *sp = shared.acquire(tid, threads, CAPACITY / 2);

	for (auto _ : state) {
		i64 pop_retries = 0;
		i64 push_retries = 0;
		for (size_t i = 0; i < half; ++i) {
			std::optional<int> v;
			while (!(v = sp->pop())) {
				++pop_retries;
				std::this_thread::yield();
			}
			benchmark::DoNotOptimize(*v);
		}
		for (size_t i = 0; i < half; ++i) {
			const int value = static_cast<int>(i + (tid * half));
			while (!sp->push(value)) {
				++push_retries;
				std::this_thread::yield();
			}
		}
		shared.pop_retries.fetch_add(pop_retries, memorder::relaxed);
		shared.push_retries.fetch_add(push_retries, memorder::relaxed);
	}

	shared.release(tid);
	if (tid == 0) {
		const auto push_retries_v =
			static_cast<double>(shared.push_retries.load());
		const auto pop_retries_v =
			static_cast<double>(shared.pop_retries.load());
		// Each thread performs `half` pushes and `half` pops per iteration.
		const auto per_side_ops = static_cast<double>(state.iterations()) *
								  static_cast<double>(half) *
								  static_cast<double>(threads);

		state.counters["push_retries"] = push_retries_v;
		state.counters["pop_retries"] = pop_retries_v;
		state.counters["fail_rate"] =
			per_side_ops > 0.0 ? push_retries_v / per_side_ops : 0.0;
		state.counters["waits_per_op"] =
			per_side_ops > 0.0 ? pop_retries_v / per_side_ops : 0.0;
	}
	// Every worker reports only its own work; google/benchmark sums them.
	state.SetItemsProcessed(state.iterations() * static_cast<i64>(ops));
}

template <typename Stack>
auto producer_consumer(benchmark::State &state) -> void {
	static Shared<Stack> shared;

	const int tid = state.thread_index();
	const int threads = state.threads();
	const bool is_producer = (tid % 2 == 0);
	const auto ops = static_cast<size_t>(state.range(0));

	const size_t prefill = (static_cast<size_t>(threads) / 2) * ops / 2;
	Stack *sp = shared.acquire(tid, threads, prefill);

	for (auto _ : state) {
		if (is_producer) {
			i64 retries = 0;
			for (size_t i = 0; i < ops; ++i) {
				const int v = static_cast<int>(i + (tid * ops));
				while (!sp->push(v)) {
					++retries;
					std::this_thread::yield();
				}
			}
			shared.push_retries.fetch_add(retries, memorder::relaxed);
		} else {
			i64 retries = 0;
			for (std::size_t i = 0; i < ops; ++i) {
				std::optional<int> v;
				while (!(v = sp->pop())) {
					++retries;
					std::this_thread::yield();
				}
				benchmark::DoNotOptimize(*v);
			}
			shared.pop_retries.fetch_add(retries, memorder::relaxed);
		}
	}

	shared.release(tid);
	if (tid == 0) {
		const auto push_retries_v =
			static_cast<double>(shared.push_retries.load());
		const auto pop_retries_v =
			static_cast<double>(shared.pop_retries.load());
		// Producers and consumers each perform `ops * threads / 2` logical
		// operations, so the same denominator serves both rates.
		const auto per_side_ops = static_cast<double>(state.iterations()) *
								  static_cast<double>(ops) *
								  static_cast<double>(threads / 2);

		state.counters["push_retries"] = push_retries_v;
		state.counters["pop_retries"] = pop_retries_v;
		state.counters["fail_rate"] =
			per_side_ops > 0.0 ? push_retries_v / per_side_ops : 0.0;
		// `pop_retries` counts benchmark-visible spins per successful pop.
		state.counters["waits_per_op"] =
			per_side_ops > 0.0 ? pop_retries_v / per_side_ops : 0.0;
	}
	// Each worker performs `ops` logical operations; report only our own.
	state.SetItemsProcessed(state.iterations() * static_cast<i64>(ops));
}

/// Producer/consumer with an explicit prefill.  Prefill == capacity starts
/// from a full stack (push elimination); Prefill == 0 starts empty (pop
/// elimination).  Producers and consumers do the same number of operations.
template <typename Stack, std::size_t Prefill>
auto producer_consumer_prefilled(benchmark::State &state) -> void {
	static Shared<Stack> shared;

	const int tid = state.thread_index();
	const int threads = state.threads();
	const bool is_producer = (tid % 2 == 0);
	const auto ops = static_cast<std::size_t>(state.range(0));

	Stack *sp = shared.acquire(tid, threads, Prefill);

	for (auto _ : state) {
		if (is_producer) {
			i64 retries = 0;
			for (std::size_t i = 0; i < ops; ++i) {
				const int v = static_cast<int>(i + (tid * ops));
				while (!sp->push(v)) {
					++retries;
					std::this_thread::yield();
				}
			}
			shared.push_retries.fetch_add(retries, memorder::relaxed);
		} else {
			i64 retries = 0;
			for (std::size_t i = 0; i < ops; ++i) {
				std::optional<int> v;
				while (!(v = sp->pop())) {
					++retries;
					std::this_thread::yield();
				}
				benchmark::DoNotOptimize(*v);
			}
			shared.pop_retries.fetch_add(retries, memorder::relaxed);
		}
	}

	shared.release(tid);
	if (tid == 0) {
		const auto push_retries_v =
			static_cast<double>(shared.push_retries.load());
		const auto pop_retries_v =
			static_cast<double>(shared.pop_retries.load());
		// Producers and consumers each perform `ops * threads / 2` logical
		// operations, so the same denominator serves both rates.
		const auto per_side_ops = static_cast<double>(state.iterations()) *
								  static_cast<double>(ops) *
								  static_cast<double>(threads / 2);
		state.counters["push_retries"] = push_retries_v;
		state.counters["pop_retries"] = pop_retries_v;
		state.counters["fail_rate"] =
			per_side_ops > 0.0 ? push_retries_v / per_side_ops : 0.0;
		state.counters["waits_per_op"] =
			per_side_ops > 0.0 ? pop_retries_v / per_side_ops : 0.0;
	}
	// Each worker performs `ops` logical operations; report only our own.
	state.SetItemsProcessed(state.iterations() * static_cast<i64>(ops));
}

/// Imbalanced producer/consumer split: Producers threads push and threads -
/// Producers threads pop, but both sides perform the same *total* number of
/// operations.  This forces sustained full/empty pressure without deadlock.
template <typename Stack, int Producers>
auto imbalanced(benchmark::State &state) -> void {
	static_assert(Producers > 0);

	static Shared<Stack> shared;

	const int tid = state.thread_index();
	const int threads = state.threads();
	const int consumers = threads - Producers;
	if (consumers <= 0) {
		state.SkipWithError("imbalanced: need at least one consumer");
		return;
	}

	const auto total = static_cast<std::size_t>(state.range(0));
	const auto producer_ops = total / static_cast<std::size_t>(Producers);
	const auto consumer_ops = total / static_cast<std::size_t>(consumers);
	const bool is_producer = (tid < Producers);

	Stack *sp = shared.acquire(tid, threads, 0);
	const auto my_ops = is_producer ? producer_ops : consumer_ops;

	for (auto _ : state) {
		if (is_producer) {
			i64 retries = 0;
			for (std::size_t i = 0; i < producer_ops; ++i) {
				const int v = static_cast<int>(i + (tid * producer_ops));
				while (!sp->push(v)) {
					++retries;
					std::this_thread::yield();
				}
			}
			shared.push_retries.fetch_add(retries, memorder::relaxed);
		} else {
			i64 retries = 0;
			for (std::size_t i = 0; i < consumer_ops; ++i) {
				std::optional<int> v;
				while (!(v = sp->pop())) {
					++retries;
					std::this_thread::yield();
				}
				benchmark::DoNotOptimize(*v);
			}
			shared.pop_retries.fetch_add(retries, memorder::relaxed);
		}
	}

	shared.release(tid);
	if (tid == 0) {
		const auto push_retries_v =
			static_cast<double>(shared.push_retries.load());
		const auto pop_retries_v =
			static_cast<double>(shared.pop_retries.load());
		// Producer and consumer populations can differ; use each side's own
		// logical operation count as the denominator.
		const auto producer_total = static_cast<double>(state.iterations()) *
									static_cast<double>(producer_ops) *
									static_cast<double>(Producers);
		const auto consumer_total = static_cast<double>(state.iterations()) *
									static_cast<double>(consumer_ops) *
									static_cast<double>(consumers);
		state.counters["push_retries"] = push_retries_v;
		state.counters["pop_retries"] = pop_retries_v;
		state.counters["fail_rate"] =
			producer_total > 0.0 ? push_retries_v / producer_total : 0.0;
		state.counters["waits_per_op"] =
			consumer_total > 0.0 ? pop_retries_v / consumer_total : 0.0;
	}
	// Report this thread's own logical operation count, not the global total.
	state.SetItemsProcessed(state.iterations() * static_cast<i64>(my_ops));
}

// NOLINTBEGIN(bugprone-throwing-static-initialization)

BENCHMARK_TEMPLATE(push_single, MutexStack)
	->Name("Mutex/Push_Single")
	->Arg(static_cast<i64>(OPS_SINGLE))
	->Iterations(2000)
	->UseManualTime()
	->Unit(benchmark::kNanosecond)
	->Apply(add_extra_stats);

BENCHMARK_TEMPLATE(push_single, LFStack)
	->Name("LockFree/Push_Single")
	->Arg(static_cast<i64>(OPS_SINGLE))
	->Iterations(2000)
	->UseManualTime()
	->Unit(benchmark::kNanosecond)
	->Apply(add_extra_stats);

BENCHMARK_TEMPLATE(pop_single, MutexStack)
	->Name("Mutex/Pop_Single")
	->Arg(static_cast<i64>(OPS_SINGLE))
	->Iterations(2000)
	->UseManualTime()
	->Unit(benchmark::kNanosecond)
	->Apply(add_extra_stats);

BENCHMARK_TEMPLATE(pop_single, LFStack)
	->Name("LockFree/Pop_Single")
	->Arg(static_cast<i64>(OPS_SINGLE))
	->Iterations(2000)
	->UseManualTime()
	->Unit(benchmark::kNanosecond)
	->Apply(add_extra_stats);

BENCHMARK_TEMPLATE(mixed_multi, MutexStack)
	->Name("Mutex/Mixed_Multi")
	->Arg(static_cast<i64>(OPS_MULTI))
	->Threads(1)
	->Threads(2)
	->Threads(4)
	->Iterations(2000)
	->UseRealTime()
	->Apply(add_extra_stats);

BENCHMARK_TEMPLATE(mixed_multi, LFStack)
	->Name("LockFree/Mixed_Multi")
	->Arg(static_cast<i64>(OPS_MULTI))
	->Threads(1)
	->Threads(2)
	->Threads(4)
	->Iterations(2000)
	->UseRealTime()
	->Apply(add_extra_stats);

BENCHMARK_TEMPLATE(producer_consumer, MutexStack)
	->Name("Mutex/Producer_Consumer")
	->Arg(static_cast<i64>(OPS_MULTI))
	->Threads(2)
	->Threads(4)
	->Threads(8)
	->Iterations(2000)
	->UseRealTime()
	->Apply(add_extra_stats);

BENCHMARK_TEMPLATE(producer_consumer, LFStack)
	->Name("LockFree/Producer_Consumer")
	->Arg(static_cast<i64>(OPS_MULTI))
	->Threads(2)
	->Threads(4)
	->Threads(8)
	->Iterations(2000)
	->UseRealTime()
	->Apply(add_extra_stats);

BENCHMARK_TEMPLATE(push_single, EmiStack)
	->Name("ElimiBackoff/Push_Single")
	->Arg(static_cast<i64>(OPS_SINGLE))
	->Iterations(2000)
	->UseManualTime()
	->Unit(benchmark::kNanosecond)
	->Apply(add_extra_stats);

BENCHMARK_TEMPLATE(pop_single, EmiStack)
	->Name("ElimiBackoff/Pop_Single")
	->Arg(static_cast<i64>(OPS_SINGLE))
	->Iterations(2000)
	->UseManualTime()
	->Unit(benchmark::kNanosecond)
	->Apply(add_extra_stats);

BENCHMARK_TEMPLATE(mixed_multi, EmiStack)
	->Name("ElimiBackoff/Mixed_Multi")
	->Arg(static_cast<i64>(OPS_MULTI))
	->Threads(1)
	->Threads(2)
	->Threads(4)
	->Iterations(2000)
	->UseRealTime()
	->Apply(add_extra_stats);

BENCHMARK_TEMPLATE(producer_consumer, EmiStack)
	->Name("ElimiBackoff/Producer_Consumer")
	->Arg(static_cast<i64>(OPS_MULTI))
	->Threads(2)
	->Threads(4)
	->Threads(8)
	->Iterations(2000)
	->UseRealTime()
	->Apply(add_extra_stats);

// ── Scenarios that specifically exercise elimination ───────────────────────

BENCHMARK_TEMPLATE(producer_consumer_prefilled, EmiSlot16, 64)
	->Name("ElimiBackoff/Slots16/FullContention")
	->Arg(static_cast<i64>(OPS_MULTI))
	->Threads(4)
	->Iterations(2000)
	->UseRealTime()
	->Apply(add_extra_stats);

BENCHMARK_TEMPLATE(producer_consumer_prefilled, EmiSlot32, 64)
	->Name("ElimiBackoff/Slots32/FullContention")
	->Arg(static_cast<i64>(OPS_MULTI))
	->Threads(4)
	->Iterations(2000)
	->UseRealTime()
	->Apply(add_extra_stats);

BENCHMARK_TEMPLATE(producer_consumer_prefilled, EmiSlot64, 64)
	->Name("ElimiBackoff/Slots64/FullContention")
	->Arg(static_cast<i64>(OPS_MULTI))
	->Threads(4)
	->Iterations(2000)
	->UseRealTime()
	->Apply(add_extra_stats);

BENCHMARK_TEMPLATE(producer_consumer_prefilled, EmiSlot128, 64)
	->Name("ElimiBackoff/Slots128/FullContention")
	->Arg(static_cast<i64>(OPS_MULTI))
	->Threads(4)
	->Iterations(2000)
	->UseRealTime()
	->Apply(add_extra_stats);

BENCHMARK_TEMPLATE(producer_consumer_prefilled, EmiCapacity256, 0)
	->Name("ElimiBackoff/Capacity256/EmptyContention")
	->Arg(static_cast<i64>(OPS_MULTI))
	->Threads(4)
	->Iterations(2000)
	->UseRealTime()
	->Apply(add_extra_stats);

BENCHMARK_TEMPLATE(producer_consumer_prefilled, EmiCapacity1024, 0)
	->Name("ElimiBackoff/Capacity1024/EmptyContention")
	->Arg(static_cast<i64>(OPS_MULTI))
	->Threads(4)
	->Iterations(2000)
	->UseRealTime()
	->Apply(add_extra_stats);

BENCHMARK_TEMPLATE(producer_consumer_prefilled, EmiSmall, 64)
	->Name("ElimiBackoff/FullContention")
	->Arg(static_cast<i64>(OPS_MULTI))
	->Threads(2)
	->Threads(4)
	->Threads(8)
	->Iterations(2000)
	->UseRealTime()
	->Apply(add_extra_stats);

BENCHMARK_TEMPLATE(producer_consumer_prefilled, EmiSmall, 0)
	->Name("ElimiBackoff/EmptyContention")
	->Arg(static_cast<i64>(OPS_MULTI))
	->Threads(2)
	->Threads(4)
	->Threads(8)
	->Iterations(2000)
	->UseRealTime()
	->Apply(add_extra_stats);

BENCHMARK_TEMPLATE(imbalanced, EmiSmall, 1)
	->Name("ElimiBackoff/Imbalanced_1P4C")
	->Arg(8192)
	->Threads(5)
	->Iterations(2000)
	->UseRealTime()
	->Apply(add_extra_stats);

BENCHMARK_TEMPLATE(imbalanced, EmiSmall, 4)
	->Name("ElimiBackoff/Imbalanced_4P1C")
	->Arg(8192)
	->Threads(5)
	->Iterations(2000)
	->UseRealTime()
	->Apply(add_extra_stats);

/// Compact terminal reporter: one line per benchmark (aggregate mean + CV +
/// key counters), while the JSONReporter keeps the full repetition/statistics
/// detail in the output file.
class CompactReporter : public benchmark::BenchmarkReporter {
public:
	auto ReportContext(const Context &) -> bool override {
		auto &out = GetOutputStream();
		out << "Benchmark | mean | CV | items/s | push_retries | pop_retries | "
			   "fail_rate | waits/op\n";
		out << "-----------------------------------------------------------\n";
		return true;
	}

	auto ReportRuns(const vec<Run> &reports) -> void override {
		if (reports.empty()) {
			return;
		}
		// BenchmarkReporter::ReportRuns is called once with the individual
		// repetitions and once with the aggregates.  With repetitions > 1,
		// wait for the aggregate call so that only one compact line is printed.
		if (reports.front().run_type != Run::RT_Aggregate &&
			reports.front().repetitions > 1) {
			return;
		}

		const Run *mean = nullptr;
		const Run *cv = nullptr;
		for (const auto &run : reports) {
			if (run.run_type != Run::RT_Aggregate) {
				continue;
			}
			if (run.aggregate_name == "mean") {
				mean = &run;
			} else if (run.aggregate_name == "cv") {
				cv = &run;
			}
		}

		const Run *base = mean != nullptr ? mean : &reports.front();
		if (base == nullptr) {
			return;
		}

		auto counter = [&base](const char *name) -> double {
			const auto it = base->counters.find(name);
			return it == base->counters.end() ? 0.0 : it->second.value;
		};

		auto &out = GetOutputStream();
		out << std::fixed << std::setprecision(2);
		out << base->run_name.str() << " | " << base->GetAdjustedRealTime()
			<< " ns | CV "
			// `cv` stores the coefficient of variation as a fraction
			// (0.05 == 5%), so scale it before printing a percentage.
			<< (cv != nullptr ? cv->real_accumulated_time * 100.0 : 0.0)
			<< "% | " << counter("items_per_second") / 1e6 << " M/s | "
			<< counter("push_retries") << " | " << counter("pop_retries")
			<< " | " << counter("fail_rate") * 100.0 << "% | "
			<< counter("waits_per_op") << '\n';
	}
};

} // namespace
// NOLINTEND(bugprone-throwing-static-initialization)

auto main(int argc, char **argv) -> int {

	bool has_repetitions = false;
	bool has_interleaving = false;
	bool has_min_time = false;
	str out_path = "benchmark_results.json";

	for (int i = 1; i < argc; ++i) {
		str_vw arg = argv[i];
		if (arg.starts_with("--benchmark_repetitions"))
			has_repetitions = true;
		if (arg.starts_with("--benchmark_enable_random_interleaving"))
			has_interleaving = true;
		if (arg.starts_with("--benchmark_min_time"))
			has_min_time = true;
		constexpr str_vw out_prefix = "--benchmark_out=";
		if (arg.starts_with(out_prefix)) {
			out_path = str(arg.substr(out_prefix.size()));
		}
	}

	vec<const char *> new_argv(argv, argv + argc);
	if (!has_repetitions) {
		new_argv.push_back("--benchmark_repetitions=9");
	}
	if (!has_interleaving) {
		new_argv.push_back("--benchmark_enable_random_interleaving=true");
	}
	if (!has_min_time) {
		new_argv.push_back("--benchmark_min_time=0.1s");
	}

	// RunSpecifiedBenchmarks(display, file) requires the library's
	// --benchmark_out flag to be present even though we supply our own
	// JSONReporter writing to the same path.
	str out_flag = "--benchmark_out=" + out_path;
	new_argv.push_back(out_flag.c_str());

	int new_argc = static_cast<int>(new_argv.size());
	char **new_argv_ptr = const_cast<char **>(new_argv.data());

	benchmark::Initialize(&new_argc, new_argv_ptr);

	// Use the argc/argv that Initialize() has already filtered; the original
	// argc/argv contain every user flag and would always be reported as
	// unrecognized.
	if (benchmark::ReportUnrecognizedArguments(new_argc, new_argv_ptr)) {
		return 1;
	}

	std::ofstream json_out(out_path);
	if (!json_out) {
		std::cerr << "failed to open benchmark JSON output: " << out_path
				  << '\n';
		return 1;
	}

	CompactReporter compact;
	benchmark::JSONReporter json;
	json.SetOutputStream(&json_out);

	benchmark::SetDefaultTimeUnit(benchmark::kNanosecond);
	benchmark::RunSpecifiedBenchmarks(&compact, &json);
	json_out.flush();

	std::cout << "JSON written to " << out_path << '\n';
	benchmark::Shutdown();
	return 0;
}
