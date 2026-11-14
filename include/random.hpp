// random.hpp — ergonomic random-number facilities built on prng::xoshiro256pp
//
// This header is the "batteries included" layer:
//
//   rng::stream<Engine>      one object with uniform/coin/normal/pick/shuffle
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

#include <xoshiro256pp.hpp>

#include <algorithm>
#include <array>
#include <cassert>
#include <cmath>
#include <concepts>
#include <cstddef>
#include <cstdint>
#include <iterator>
#include <limits>
#include <numbers>
#include <random>
#include <ranges>
#include <span>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

namespace rng {

inline constexpr std::string_view version = "1.0.0";

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
[[nodiscard]] constexpr U next_unsigned(G &g) noexcept {
	using result_type = std::make_unsigned_t<typename G::result_type>;

	if constexpr (sizeof(result_type) >= sizeof(U)) {
		return static_cast<U>(g());
	} else {
		constexpr std::size_t parts =
			(sizeof(U) + sizeof(result_type) - 1) / sizeof(result_type);
		U value{};
		for (std::size_t i = 0; i < parts; ++i) {
			value = static_cast<U>((value << (sizeof(result_type) * 8U)) |
								   static_cast<U>(g()));
		}
		return value;
	}
}

// Uniform value in [0, bound), without modulo bias.  bound == 0 maps to 0;
// callers that require non-empty ranges check that precondition themselves.
template <std::unsigned_integral U, engine G>
[[nodiscard]] constexpr U bounded(G &g, U bound) noexcept {
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
[[nodiscard]] constexpr T uniform_int(G &g, T lo, T hi) noexcept {
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
[[nodiscard]] constexpr F uniform_real(G &g, F lo = F{0},
									   F hi = F{1}) noexcept {
	if (hi < lo) {
		std::swap(lo, hi);
	}
	if (lo == hi) {
		return lo;
	}

	constexpr int digits = std::numeric_limits<F>::digits;
	const std::uint64_t value = detail::next_unsigned<std::uint64_t>(g);
	const std::uint64_t mantissa =
		value >> (64U - static_cast<unsigned>(digits));

	const F unit = static_cast<F>(mantissa) *
				   (F{1} / static_cast<F>(std::uint64_t{1}
										  << static_cast<unsigned>(digits)));

	return lo + ((hi - lo) * unit);
}

// Bernoulli trial with probability p.  p is clamped to [0, 1].
template <engine G>
[[nodiscard]] constexpr bool bernoulli(G &g, double p = 0.5) noexcept {
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
[[nodiscard]] double normal(G &g, double mean = 0.0, double stddev = 1.0) {
	const double u1 = uniform_real<double>(g, 0.0, 1.0);
	const double u2 = uniform_real<double>(g, 0.0, 1.0);
	const double radius = std::sqrt(-2.0 * std::log1p(-u1));
	const double angle = 2.0 * std::numbers::pi_v<double> * u2;
	return mean + (stddev * radius * std::cos(angle));
}

// Exponential distribution with the given rate (rate > 0).
template <engine G> [[nodiscard]] double exponential(G &g, double rate = 1.0) {
	assert(rate > 0.0);
	return -std::log1p(-uniform_real<double>(g, 0.0, 1.0)) / rate;
}

// ---------------------------------------------------------------------------
// Containers
// ---------------------------------------------------------------------------

// Uniform element selection.  The range must be non-empty and borrowed (so the
// returned reference cannot dangle).
template <engine G, std::ranges::random_access_range R>
	requires std::ranges::borrowed_range<R>
[[nodiscard]] constexpr decltype(auto) pick(G &g, R &&range) {
	const auto count =
		static_cast<std::size_t>(std::ranges::size(std::forward<R>(range)));
	assert(count != 0 && "pick() requires a non-empty range");

	const auto offset = static_cast<std::ranges::range_difference_t<R>>(
		detail::bounded<std::size_t>(g, count));

	auto first = std::ranges::begin(range);
	return first[offset];
}

// In-place Fisher-Yates shuffle.
template <engine G, std::ranges::random_access_range R>
	requires std::permutable<std::ranges::iterator_t<R>>
constexpr void shuffle(G &g, R &&range) {
	const auto count =
		static_cast<std::size_t>(std::ranges::size(std::forward<R>(range)));
	if (count < 2) {
		return;
	}

	using difference_type = std::ranges::range_difference_t<R>;
	auto first = std::ranges::begin(range);

	for (std::size_t i = count; i-- > 1;) {
		const auto j = static_cast<difference_type>(
			detail::bounded<std::size_t>(g, i + 1));
		const auto k = static_cast<difference_type>(i);
		if (j != k) {
			std::ranges::iter_swap(first + k, first + j);
		}
	}
}

// Fills a byte span from consecutive engine outputs.
template <engine G> void fill_bytes(G &g, std::span<std::byte> out) {
	using word_type = std::make_unsigned_t<typename G::result_type>;
	constexpr std::size_t width = sizeof(word_type);

	for (std::size_t i = 0; i < out.size(); i += width) {
		const word_type value = static_cast<word_type>(g());
		for (std::size_t j = 0; j < width && i + j < out.size(); ++j) {
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
[[nodiscard]] inline std::array<std::uint32_t, 8> entropy_seed() {
	std::random_device device;
	std::array<std::uint32_t, 8> words{};
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

	constexpr explicit stream(std::uint64_t seed_value) {
		engine_.seed(seed_value);
	}

	explicit stream(std::span<const std::uint32_t> words) {
		std::seed_seq sequence(words.begin(), words.end());
		engine_.seed(sequence);
	}

	template <prng::concepts::seed_sequence_like SeedSequence>
	explicit stream(SeedSequence &sequence) {
		engine_.seed(sequence);
	}

	[[nodiscard]] constexpr result_type next() noexcept { return engine_(); }

	[[nodiscard]] constexpr result_type operator()() noexcept {
		return engine_();
	}

	template <integer T>
	[[nodiscard]] constexpr T uniform(T lo, T hi) noexcept {
		return rng::uniform_int(engine_, lo, hi);
	}

	template <real F>
	[[nodiscard]] constexpr F uniform(F lo = F{0}, F hi = F{1}) noexcept {
		return rng::uniform_real<F>(engine_, lo, hi);
	}

	[[nodiscard]] constexpr bool coin(double p = 0.5) noexcept {
		return rng::bernoulli(engine_, p);
	}

	[[nodiscard]] double normal(double mean = 0.0, double stddev = 1.0) {
		return rng::normal(engine_, mean, stddev);
	}

	[[nodiscard]] double exponential(double rate = 1.0) {
		return rng::exponential(engine_, rate);
	}

	template <std::ranges::random_access_range R>
	constexpr void shuffle(R &&range) {
		rng::shuffle(engine_, std::forward<R>(range));
	}

	template <std::ranges::random_access_range R>
		requires std::ranges::borrowed_range<R>
	[[nodiscard]] constexpr decltype(auto) pick(R &&range) {
		return rng::pick(engine_, std::forward<R>(range));
	}

	void fill_bytes(std::span<std::byte> out) { rng::fill_bytes(engine_, out); }

	constexpr void discard(unsigned long long n) noexcept {
		engine_.discard(n);
	}

	constexpr void jump() noexcept
		requires prng::concepts::jumpable_engine<Engine>
	{
		engine_.jump();
	}

	constexpr void long_jump() noexcept
		requires prng::concepts::jumpable_engine<Engine>
	{
		engine_.long_jump();
	}

	[[nodiscard]] constexpr decltype(auto) state() const noexcept
		requires requires(const Engine &e) { e.state(); }
	{
		return engine_.state();
	}

	[[nodiscard]] constexpr bool is_valid() const noexcept
		requires prng::concepts::checkpointable_engine<Engine>
	{
		return engine_.is_valid();
	}

	constexpr bool set_state(const state_type &state) noexcept
		requires prng::concepts::checkpointable_engine<Engine>
	{
		return engine_.set_state(state);
	}

	[[nodiscard]] constexpr Engine &engine() noexcept { return engine_; }

	[[nodiscard]] constexpr const Engine &engine() const noexcept {
		return engine_;
	}

	constexpr bool operator==(const stream &) const noexcept = default;

private:
	Engine engine_{};
};

// ---------------------------------------------------------------------------
// Factories
// ---------------------------------------------------------------------------

template <class Engine = prng::xoshiro256pp>
	requires engine<Engine> && prng::concepts::seedable_engine<Engine> &&
			 std::default_initializable<Engine> && std::copyable<Engine>
[[nodiscard]] inline stream<Engine> make_stream() {
	const auto words = entropy_seed();
	return stream<Engine>(std::span<const std::uint32_t>{words});
}

template <class Engine = prng::xoshiro256pp>
	requires engine<Engine> && prng::concepts::seedable_engine<Engine> &&
			 std::default_initializable<Engine> && std::copyable<Engine>
[[nodiscard]] constexpr stream<Engine> make_stream(std::uint64_t seed_value) {
	return stream<Engine>(seed_value);
}

// Derive count independent streams from one base.  Each stream is base plus
// i calls to jump(); scheduling does not affect the assignment.
template <class Engine = prng::xoshiro256pp>
	requires engine<Engine> && prng::concepts::jumpable_engine<Engine> &&
			 std::default_initializable<Engine> && std::copyable<Engine>
[[nodiscard]] constexpr std::vector<stream<Engine>>
split_streams(const stream<Engine> &base, std::size_t count) {
	std::vector<stream<Engine>> streams;
	streams.reserve(count);

	stream<Engine> current = base;
	for (std::size_t i = 0; i < count; ++i) {
		streams.push_back(current);
		if (i + 1 < count) {
			current.jump();
		}
	}

	return streams;
}

} // namespace rng

#endif // PRNG_RANDOM_HPP
