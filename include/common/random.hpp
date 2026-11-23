// random.hpp — ergonomic random-number facilities built on prng::xoshiro256pp
//
// This header is the "batteries included" layer:
//
//   rng::stream<Engine>      one object with uniform/coin/normal/pick/shuffle
//   rng::synchronized_stream mutex-protected, safe to share between threads
//   rng::thread_rng          one entropy-seeded stream per thread
//   rng::random_int          ready-to-use thread-local helpers
//   rng::uniform_int         unbiased inclusive integer ranges
//   rng::uniform_real        half-open floating-point ranges
//   rng::bernoulli           coin flips
//   rng::normal              Box-Muller normal samples
//   rng::exponential         exponential samples
//   rng::pick                uniform element selection
//   rng::shuffle             Fisher-Yates shuffle
//   rng::fill_bytes          random byte generation
//   rng::make_stream         entropy-seeded or explicitly seeded stream
//   rng::split_streams       jump()-derived independent streams
//
// The engine itself lives in <prng/xoshiro256pp.hpp>; this header depends on it
// and deliberately does not duplicate the generator implementation.
//
// SPDX-License-Identifier: MIT-0

#ifndef PRNG_RANDOM_HPP
#define PRNG_RANDOM_HPP

#include "alias.hpp"
#include "xoshiro256pp.hpp"

#include <algorithm>
#include <array>
#include <cassert>
#include <cmath>
#include <concepts>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <iterator>
#include <limits>
#include <mutex>
#include <numbers>
#include <random>
#include <ranges>
#include <span>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

namespace rng {

namespace rngs = std::ranges;

inline constexpr str_vw VERSION = "1.0.0";

// ---------------------------------------------------------------------------
// Concepts
// ---------------------------------------------------------------------------

// Any engine satisfying the standard URBG surface plus the xoshiro toolkit.
template <class G>
concept engine = prng::concepts::random_engine<G>;

// An engine that supports jump()/long_jump().
template <class G>
concept jumpable_engine = engine<G> && prng::concepts::jumpable_engine<G>;

// A non-bool integral type (bool is not a useful uniform_int target).
template <class T>
concept integer = std::integral<T> && !std::same_as<std::remove_cv_t<T>, bool>;

// float/double; long double with 64-bit mantissa is intentionally excluded
// because the 53-bit path below cannot represent it without extra machinery.
template <class T>
concept real = std::floating_point<T> && (std::numeric_limits<T>::digits <= 53);

// ---------------------------------------------------------------------------
// Low-level unbiased sampling
// ---------------------------------------------------------------------------

namespace detail {

template <std::unsigned_integral U, engine G>
[[nodiscard]] constexpr auto next_unsigned(G &g) noexcept -> U {
	using result_type = std::make_unsigned_t<typename G::result_type>;

	if constexpr (sizeof(result_type) >= sizeof(U)) {
		return static_cast<U>(g());
	} else {
		constexpr size_t PARTS =
			(sizeof(U) + sizeof(result_type) - 1) / sizeof(result_type);
		U value{};
		for (size_t i = 0; i < PARTS; ++i) {
			value = static_cast<U>((value << (sizeof(result_type) * 8U)) |
								   static_cast<U>(g()));
		}
		return value;
	}
}

// Uniform value in [0, bound), without modulo bias.  bound == 0 maps to 0;
// callers that require non-empty ranges check that precondition themselves.
template <std::unsigned_integral U, engine G>
[[nodiscard]] constexpr auto bounded(G &g, U bound) noexcept -> U {
	if (bound == 0) {
		return 0;
	}

	if ((bound & static_cast<U>(bound - U{1})) == 0) {
		return static_cast<U>(next_unsigned<U>(g) &
							  static_cast<U>(bound - U{1}));
	}

	const U threshold = static_cast<U>(static_cast<U>(U{0} - bound) % bound);
	for (;;) {
		const U value = next_unsigned<U>(g);
		if (value >= threshold) {
			return static_cast<U>(value % bound);
		}
	}
}

} // namespace detail

// ---------------------------------------------------------------------------
// Distributions
// ---------------------------------------------------------------------------

// Inclusive integer range [lo, hi].  Reversing the arguments swaps them.
template <integer T, engine G>
[[nodiscard]] constexpr auto uniform_int(G &g, T lo, T hi) noexcept -> T {
	if (hi < lo) {
		std::swap(lo, hi);
	}
	if (lo == hi) {
		return lo;
	}

	using U = std::make_unsigned_t<T>;
	const U range = static_cast<U>(hi) - static_cast<U>(lo);

	if (range == std::numeric_limits<U>::max()) {
		return static_cast<T>(detail::next_unsigned<U>(g));
	}

	const U offset = detail::bounded<U>(g, static_cast<U>(range + U{1}));
	return static_cast<T>(static_cast<U>(lo) + offset);
}

// Half-open floating-point range [lo, hi).  Reversing the arguments swaps them.
template <real F, engine G>
[[nodiscard]] constexpr auto uniform_real(G &g, F lo = F{0},
										  F hi = F{1}) noexcept -> F {
	if (hi < lo) {
		std::swap(lo, hi);
	}
	if (lo == hi) {
		return lo;
	}

	constexpr int DIGITS = std::numeric_limits<F>::digits;
	const auto value = detail::next_unsigned<u64>(g);
	const u64 mantissa = value >> (64U - static_cast<unsigned>(DIGITS));

	const F unit =
		static_cast<F>(mantissa) *
		(F{1} / static_cast<F>(u64{1} << static_cast<unsigned>(DIGITS)));

	return lo + ((hi - lo) * unit);
}

// Bernoulli trial with probability p.  p is clamped to [0, 1].
template <engine G>
[[nodiscard]] constexpr auto bernoulli(G &g, double p = 0.5) noexcept -> bool {
	if (p <= 0.0) {
		return false;
	}
	if (p >= 1.0) {
		return true;
	}
	return uniform_real<double>(g, 0.0, 1.0) < p;
}

// Standard normal distribution, one sample per call (Box-Muller).
template <engine G>
[[nodiscard]] auto normal(G &g, double mean = 0.0, double stddev = 1.0)
	-> double {
	const auto u1 = uniform_real<double>(g, 0.0, 1.0);
	const auto u2 = uniform_real<double>(g, 0.0, 1.0);
	const double radius = std::sqrt(-2.0 * std::log1p(-u1));
	const double angle = 2.0 * std::numbers::pi_v<double> * u2;
	return mean + (stddev * radius * std::cos(angle));
}

// Exponential distribution with the given rate (rate > 0).
template <engine G>
[[nodiscard]] auto exponential(G &g, double rate = 1.0) -> double {
	assert(rate > 0.0);
	return -std::log1p(-uniform_real<double>(g, 0.0, 1.0)) / rate;
}

// ---------------------------------------------------------------------------
// Containers
// ---------------------------------------------------------------------------

// Uniform element selection.  The range must be non-empty and borrowed (so the
// returned reference cannot dangle).
template <engine G, rngs::random_access_range R>
	requires rngs::borrowed_range<R>
[[nodiscard]] constexpr auto pick(G &g, R &&range) -> decltype(auto) {
	const auto count = static_cast<size_t>(rngs::size(std::forward<R>(range)));
	assert(count != 0 && "pick() requires a non-empty range");

	const auto offset = static_cast<rngs::range_difference_t<R>>(
		detail::bounded<size_t>(g, count));

	auto first = rngs::begin(std::forward<R>(range));
	return first[offset];
}

// In-place Fisher-Yates shuffle.
template <engine G, rngs::random_access_range R>
	requires std::permutable<rngs::iterator_t<R>>
constexpr auto shuffle(G &g, R &&range) -> void {
	const auto count = static_cast<size_t>(rngs::size(std::forward<R>(range)));
	if (count < 2) {
		return;
	}

	using difference_type = rngs::range_difference_t<R>;
	auto first = rngs::begin(range);

	for (size_t i = count; i-- > 1;) {
		const auto j =
			static_cast<difference_type>(detail::bounded<size_t>(g, i + 1));
		const auto k = static_cast<difference_type>(i);
		if (j != k) {
			rngs::iter_swap(first + k, first + j);
		}
	}
}

// Fills a byte span from consecutive engine outputs.
template <engine G> auto fill_bytes(G &g, std::span<std::byte> out) -> void {
	using word_type = std::make_unsigned_t<typename G::result_type>;
	constexpr size_t WIDTH = sizeof(word_type);

	for (size_t i = 0; i < out.size(); i += WIDTH) {
		const auto value = static_cast<word_type>(g());
		for (size_t j = 0; j < WIDTH && i + j < out.size(); ++j) {
			out[i + j] = static_cast<std::byte>(static_cast<unsigned char>(
				static_cast<word_type>(value >> (j * 8U)) & word_type{0xFF}));
		}
	}
}

// ---------------------------------------------------------------------------
// Entropy
// ---------------------------------------------------------------------------

// Eight 32-bit words from std::random_device.  Not a replacement for a
// cryptographic RNG; it is only seed material.
[[nodiscard]] inline auto entropy_seed() -> std::array<u32, 8> {
	std::random_device device;
	std::array<u32, 8> words{};
	for (auto &word : words) {
		word = device();
	}
	return words;
}

// ---------------------------------------------------------------------------
// stream — the ergonomic front end
// ---------------------------------------------------------------------------

template <class Engine = prng::xoshiro256pp>
	requires engine<Engine> && prng::concepts::seedable_engine<Engine> &&
			 std::default_initializable<Engine> && std::copyable<Engine>
class stream {
public:
	using engine_type = Engine;
	using result_type = Engine::result_type;
	using state_type = Engine::state_type;

	constexpr stream() = default;

	constexpr explicit stream(u64 seed_value) { engine_.seed(seed_value); }

	explicit stream(std::span<const u32> words) {
		std::seed_seq sequence(words.begin(), words.end());
		engine_.seed(sequence);
	}

	template <prng::concepts::seed_sequence_like SeedSequence>
	explicit stream(SeedSequence &sequence) {
		engine_.seed(sequence);
	}

	[[nodiscard]] constexpr auto next() noexcept -> result_type {
		return engine_();
	}

	[[nodiscard]] constexpr auto operator()() noexcept -> result_type {
		return engine_();
	}

	template <integer T>
	[[nodiscard]] constexpr auto uniform(T lo, T hi) noexcept -> T {
		return rng::uniform_int(engine_, lo, hi);
	}

	template <real F>
	[[nodiscard]] constexpr auto uniform(F lo = F{0}, F hi = F{1}) noexcept
		-> F {
		return rng::uniform_real<F>(engine_, lo, hi);
	}

	[[nodiscard]] constexpr auto coin(double p = 0.5) noexcept -> bool {
		return rng::bernoulli(engine_, p);
	}

	[[nodiscard]] auto normal(double mean = 0.0, double stddev = 1.0)
		-> double {
		return rng::normal(engine_, mean, stddev);
	}

	[[nodiscard]] auto exponential(double rate = 1.0) -> double {
		return rng::exponential(engine_, rate);
	}

	template <rngs::random_access_range R>
	constexpr auto shuffle(R &&range) -> void {
		rng::shuffle(engine_, std::forward<R>(range));
	}

	template <rngs::random_access_range R>
		requires rngs::borrowed_range<R>
	[[nodiscard]] constexpr auto pick(R &&range) -> decltype(auto) {
		return rng::pick(engine_, std::forward<R>(range));
	}

	auto fill_bytes(std::span<std::byte> out) -> void {
		rng::fill_bytes(engine_, out);
	}

	constexpr auto discard(u64 n) noexcept -> void { engine_.discard(n); }

	constexpr auto jump() noexcept -> void
		requires prng::concepts::jumpable_engine<Engine>
	{
		engine_.jump();
	}

	constexpr auto long_jump() noexcept -> void
		requires prng::concepts::jumpable_engine<Engine>
	{
		engine_.long_jump();
	}

	[[nodiscard]] constexpr auto state() const noexcept -> decltype(auto)
		requires requires(const Engine &e) { e.state(); }
	{
		return engine_.state();
	}

	[[nodiscard]] constexpr auto is_valid() const noexcept -> bool
		requires prng::concepts::checkpointable_engine<Engine>
	{
		return engine_.is_valid();
	}

	constexpr auto set_state(const state_type &state) noexcept -> bool
		requires prng::concepts::checkpointable_engine<Engine>
	{
		return engine_.set_state(state);
	}

	[[nodiscard]] constexpr auto engine() noexcept -> Engine & {
		return engine_;
	}

	[[nodiscard]] constexpr auto engine() const noexcept -> const Engine & {
		return engine_;
	}

	constexpr auto operator==(const stream &) const noexcept -> bool = default;

private:
	Engine engine_{};
};

// ---------------------------------------------------------------------------
// synchronized_stream — safe to share between threads
// ---------------------------------------------------------------------------

// A mutex-protected stream.  Every public operation locks the mutex, so one
// instance can be shared by any number of threads.  Prefer with_lock() when
// several draws must come from the same critical section.
template <class Engine = prng::xoshiro256pp>
	requires engine<Engine> && prng::concepts::seedable_engine<Engine> &&
			 std::default_initializable<Engine>
class synchronized_stream {
public:
	using engine_type = Engine;
	using result_type = Engine::result_type;
	using state_type = Engine::state_type;

	static constexpr auto min() noexcept -> result_type {
		return Engine::min();
	}

	static constexpr auto max() noexcept -> result_type {
		return Engine::max();
	}

	synchronized_stream() = default;

	explicit synchronized_stream(u64 seed_value) { engine_.seed(seed_value); }

	explicit synchronized_stream(std::span<const u32> words) {
		std::seed_seq sequence(words.begin(), words.end());
		engine_.seed(sequence);
	}

	template <prng::concepts::seed_sequence_like SeedSequence>
	explicit synchronized_stream(SeedSequence &sequence) {
		engine_.seed(sequence);
	}

	synchronized_stream(const synchronized_stream &) = delete;
	auto operator=(const synchronized_stream &)
		-> synchronized_stream & = delete;
	synchronized_stream(synchronized_stream &&) = delete;
	auto operator=(synchronized_stream &&) -> synchronized_stream & = delete;

	[[nodiscard]] auto next() -> result_type {
		std::scoped_lock lock{mutex_};
		return engine_();
	}

	[[nodiscard]] auto operator()() -> result_type { return next(); }

	template <integer T> [[nodiscard]] auto uniform(T lo, T hi) -> T {
		std::scoped_lock lock{mutex_};
		return rng::uniform_int(engine_, lo, hi);
	}

	template <real F>
	[[nodiscard]] auto uniform(F lo = F{0}, F hi = F{1}) -> F {
		std::scoped_lock lock{mutex_};
		return rng::uniform_real<F>(engine_, lo, hi);
	}

	[[nodiscard]] auto coin(double p = 0.5) -> bool {
		std::scoped_lock lock{mutex_};
		return rng::bernoulli(engine_, p);
	}

	[[nodiscard]] auto normal(double mean = 0.0, double stddev = 1.0)
		-> double {
		std::scoped_lock lock{mutex_};
		return rng::normal(engine_, mean, stddev);
	}

	[[nodiscard]] auto exponential(double rate = 1.0) -> double {
		std::scoped_lock lock{mutex_};
		return rng::exponential(engine_, rate);
	}

	template <rngs::random_access_range R>
		requires std::permutable<rngs::iterator_t<R>>
	auto shuffle(R &&range) -> void {
		std::scoped_lock lock{mutex_};
		rng::shuffle(engine_, std::forward<R>(range));
	}

	// Returns by value: returning a reference after unlocking would race with
	// other writers.
	template <rngs::random_access_range R>
		requires rngs::borrowed_range<R> &&
				 std::copy_constructible<rngs::range_value_t<R>>
	[[nodiscard]] auto pick(R &&range) -> rngs::range_value_t<R> {
		std::scoped_lock lock{mutex_};
		return rng::pick(engine_, std::forward<R>(range));
	}

	auto fill_bytes(std::span<std::byte> out) -> void {
		std::scoped_lock lock{mutex_};
		rng::fill_bytes(engine_, out);
	}

	auto discard(u64 n) -> void {
		std::scoped_lock lock{mutex_};
		engine_.discard(n);
	}

	auto jump() -> void
		requires prng::concepts::jumpable_engine<Engine>
	{
		std::scoped_lock lock{mutex_};
		engine_.jump();
	}

	auto long_jump() -> void
		requires prng::concepts::jumpable_engine<Engine>
	{
		std::scoped_lock lock{mutex_};
		engine_.long_jump();
	}

	[[nodiscard]] auto state() const -> state_type
		requires prng::concepts::checkpointable_engine<Engine>
	{
		std::scoped_lock lock{mutex_};
		return engine_.state();
	}

	[[nodiscard]] auto is_valid() const -> bool
		requires prng::concepts::checkpointable_engine<Engine>
	{
		std::scoped_lock lock{mutex_};
		return engine_.is_valid();
	}

	auto set_state(const state_type &state) -> bool
		requires prng::concepts::checkpointable_engine<Engine>
	{
		std::scoped_lock lock{mutex_};
		return engine_.set_state(state);
	}

	// Advanced escape hatch for operations that must be atomic as a batch.
	// The callback must return by value; return a reference would escape the
	// lock and reintroduce a race.
	template <class F> auto with_lock(F &&function) -> decltype(auto) {
		using result = std::invoke_result_t<F, Engine &>;
		static_assert(!std::is_reference_v<result>,
					  "with_lock callback must return by value");

		std::scoped_lock lock{mutex_};
		return std::invoke(std::forward<F>(function), engine_);
	}

private:
	mutable std::mutex mutex_;
	Engine engine_{};
};

// ---------------------------------------------------------------------------
// Factories
// ---------------------------------------------------------------------------

template <class Engine = prng::xoshiro256pp>
	requires engine<Engine> && prng::concepts::seedable_engine<Engine> &&
			 std::default_initializable<Engine> && std::copyable<Engine>
[[nodiscard]] inline auto make_stream() -> stream<Engine> {
	const auto words = entropy_seed();
	return stream<Engine>(std::span<const u32>{words});
}

template <class Engine = prng::xoshiro256pp>
	requires engine<Engine> && prng::concepts::seedable_engine<Engine> &&
			 std::default_initializable<Engine> && std::copyable<Engine>
[[nodiscard]] constexpr auto make_stream(u64 seed_value) -> stream<Engine> {
	return stream<Engine>(seed_value);
}

// Derive count independent streams from one base.  Each stream is base plus
// i calls to jump(); scheduling does not affect the assignment.
template <class Engine = prng::xoshiro256pp>
	requires engine<Engine> && prng::concepts::jumpable_engine<Engine> &&
			 std::default_initializable<Engine> && std::copyable<Engine>
[[nodiscard]] constexpr auto split_streams(const stream<Engine> &base,
										   size_t count)
	-> vec<stream<Engine>> {
	vec<stream<Engine>> streams;
	streams.reserve(count);

	stream<Engine> current = base;
	for (size_t i = 0; i < count; ++i) {
		streams.push_back(current);
		if (i + 1 < count) {
			current.jump();
		}
	}

	return streams;
}

// ---------------------------------------------------------------------------
// Thread-local convenience API
// ---------------------------------------------------------------------------

// One entropy-seeded stream per thread.  Thread-local initialization is
// thread-safe; the returned stream must not be shared with another thread.
[[nodiscard]] inline auto thread_rng() -> stream<> & {
	thread_local stream<> instance = make_stream();
	return instance;
}

// Free functions for the common "just give me a random number" cases.
// They use thread_rng() and are therefore safe to call from any thread.
template <integer T> [[nodiscard]] inline auto random_int(T lo, T hi) -> T {
	return thread_rng().uniform(lo, hi);
}

template <real F>
[[nodiscard]] inline auto random_real(F lo = F{0}, F hi = F{1}) -> F {
	return thread_rng().uniform(lo, hi);
}

[[nodiscard]] inline auto random_bool(double p = 0.5) -> bool {
	return thread_rng().coin(p);
}

[[nodiscard]] inline auto random_normal(double mean = 0.0, double stddev = 1.0)
	-> double {
	return thread_rng().normal(mean, stddev);
}

[[nodiscard]] inline auto random_exponential(double rate = 1.0) -> double {
	return thread_rng().exponential(rate);
}

template <rngs::random_access_range R>
	requires std::permutable<rngs::iterator_t<R>>
inline auto random_shuffle(R &&range) -> void {
	thread_rng().shuffle(std::forward<R>(range));
}

template <rngs::random_access_range R>
	requires rngs::borrowed_range<R> &&
			 std::copy_constructible<rngs::range_value_t<R>>
[[nodiscard]] inline auto random_pick(R &&range) -> rngs::range_value_t<R> {
	return thread_rng().pick(std::forward<R>(range));
}

inline auto random_fill_bytes(std::span<std::byte> out) -> void {
	thread_rng().fill_bytes(out);
}

} // namespace rng

#endif // PRNG_RANDOM_HPP
