// xoshiro256++ 1.0 — a formal, header-only C++20 implementation
//
// Algorithm: David Blackman & Sebastiano Vigna, 2019.
// Reference: https://prng.di.unimi.it/xoshiro256plusplus.c
// The reference C code is released to the public domain (CC0-like statement).
//
// This project is an independent C++20 port of next(), jump() and long_jump().
// It deliberately does NOT port the newer jump_ce()/jump_n() arbitrary-jump
// helpers from the reference file, because those depend on the separate f2x.c
// polynomial library.  Use jump()/long_jump() for the standard 2^128/2^192
// parallelisation points, or discard(n) for small linear skips.
//
// Configuration macros (define before including this header):
//
//   XOSHIRO256PP_DEBUG
//       0 / 1.  Defaults to 0 when NDEBUG is defined, otherwise 1.
//       This only selects the default for XOSHIRO256PP_ASSERT_LEVEL and
//       XOSHIRO256PP_ENABLE_EXTENDED_STATIC_ASSERTS; it does not itself
//       emit any code.
//
//   XOSHIRO256PP_ASSERT_LEVEL
//       0 = no runtime checks (release default)
//       1 = invariant checks at API boundaries (debug default)
//       2 = level 1 + per-call paranoid state checks
//       Out-of-range values are a hard compile-time error.
//
//   XOSHIRO256PP_ENABLE_CORE_STATIC_ASSERTS
//       0 / 1, default 1.  Core ABI/algorithm invariants.  Do not disable
//       unless you are deliberately testing a broken configuration.
//
//   XOSHIRO256PP_ENABLE_EXTENDED_STATIC_ASSERTS
//       0 / 1, default = XOSHIRO256PP_DEBUG.  Type-trait expectations,
//       noexcept guarantees and concept-model checks.  Turning this off in
//       release only removes compile-time assertions; it does not change ABI.
//
//   XOSHIRO256PP_ASSERT(expr)
//       Optional user override for the runtime-check handler.  When not
//       defined, level >= 1 uses a small abort-with-diagnostic handler.
//
// SPDX-License-Identifier: MIT-0

#ifndef PRNG_XOSHIRO256PP_HPP
#define PRNG_XOSHIRO256PP_HPP

// ---------------------------------------------------------------------------
// Version
// ---------------------------------------------------------------------------

#define XOSHIRO256PP_VERSION_MAJOR 1
#define XOSHIRO256PP_VERSION_MINOR 0
#define XOSHIRO256PP_VERSION_PATCH 0
#define XOSHIRO256PP_VERSION_STRING "1.0.0"

// ---------------------------------------------------------------------------
// Build-mode detection and configuration validation
// ---------------------------------------------------------------------------

#ifndef XOSHIRO256PP_DEBUG
#  if defined(NDEBUG)
#    define XOSHIRO256PP_DEBUG 0
#  else
#    define XOSHIRO256PP_DEBUG 1
#  endif
#endif

#if XOSHIRO256PP_DEBUG != 0 && XOSHIRO256PP_DEBUG != 1
#  error "XOSHIRO256PP_DEBUG must be 0 or 1"
#endif

#ifndef XOSHIRO256PP_ASSERT_LEVEL
#  if XOSHIRO256PP_DEBUG
#    define XOSHIRO256PP_ASSERT_LEVEL 1
#  else
#    define XOSHIRO256PP_ASSERT_LEVEL 0
#  endif
#endif

#if XOSHIRO256PP_ASSERT_LEVEL < 0 || XOSHIRO256PP_ASSERT_LEVEL > 2
#  error "XOSHIRO256PP_ASSERT_LEVEL must be 0, 1 or 2"
#endif

#ifndef XOSHIRO256PP_ENABLE_CORE_STATIC_ASSERTS
#  define XOSHIRO256PP_ENABLE_CORE_STATIC_ASSERTS 1
#endif

#if XOSHIRO256PP_ENABLE_CORE_STATIC_ASSERTS != 0 && \
    XOSHIRO256PP_ENABLE_CORE_STATIC_ASSERTS != 1
#  error "XOSHIRO256PP_ENABLE_CORE_STATIC_ASSERTS must be 0 or 1"
#endif

#ifndef XOSHIRO256PP_ENABLE_EXTENDED_STATIC_ASSERTS
#  define XOSHIRO256PP_ENABLE_EXTENDED_STATIC_ASSERTS XOSHIRO256PP_DEBUG
#endif

#if XOSHIRO256PP_ENABLE_EXTENDED_STATIC_ASSERTS != 0 && \
    XOSHIRO256PP_ENABLE_EXTENDED_STATIC_ASSERTS != 1
#  error "XOSHIRO256PP_ENABLE_EXTENDED_STATIC_ASSERTS must be 0 or 1"
#endif

// ---------------------------------------------------------------------------
// Static-assert switches
// ---------------------------------------------------------------------------

#if XOSHIRO256PP_ENABLE_CORE_STATIC_ASSERTS
#  define XOSHIRO256PP_STATIC_ASSERT(...) static_assert(__VA_ARGS__)
#else
#  define XOSHIRO256PP_STATIC_ASSERT(...) static_assert(true)
#endif

#if XOSHIRO256PP_ENABLE_EXTENDED_STATIC_ASSERTS
#  define XOSHIRO256PP_EXTENDED_STATIC_ASSERT(...) static_assert(__VA_ARGS__)
#else
#  define XOSHIRO256PP_EXTENDED_STATIC_ASSERT(...) static_assert(true)
#endif

// ---------------------------------------------------------------------------
// Runtime assertion switches
// ---------------------------------------------------------------------------

#if XOSHIRO256PP_ASSERT_LEVEL >= 1
#  include <cstdio>
#  include <cstdlib>

namespace prng::detail {

[[noreturn]] inline void assertion_failed(
    const char* expression, const char* file, int line) noexcept {
    std::fprintf(stderr,
                 "xoshiro256pp assertion failed: %s\n  at %s:%d\n",
                 expression, file, line);
    std::abort();
}

} // namespace prng::detail

#  ifndef XOSHIRO256PP_ASSERT
#    define XOSHIRO256PP_ASSERT(expr)                                     \
        ((expr) ? (void)0                                                  \
                : ::prng::detail::assertion_failed(#expr, __FILE__, __LINE__))
#  endif
#else
#  ifndef XOSHIRO256PP_ASSERT
#    define XOSHIRO256PP_ASSERT(expr) ((void)0)
#  endif
#endif

#if XOSHIRO256PP_ASSERT_LEVEL >= 2
#  define XOSHIRO256PP_ASSERT_PARANOID(expr) XOSHIRO256PP_ASSERT(expr)
#else
#  define XOSHIRO256PP_ASSERT_PARANOID(expr) ((void)0)
#endif

// ---------------------------------------------------------------------------
// Dependencies
// ---------------------------------------------------------------------------

#include <array>
#include <bit>
#include <concepts>
#include <cstddef>
#include <cstdint>
#include <ios>
#include <istream>
#include <limits>
#include <ostream>
#include <random>
#include <type_traits>
#include <utility>

namespace prng {

// ---------------------------------------------------------------------------
// Layered concepts
//
// Level 0: word_type / xoshiro_word        — the arithmetic atom
// Level 1: xoshiro_state / seed_sequence_like — building blocks
// Level 2: random_engine / xoshiro_engine  — engine contract
// Level 3: stream_serializable             — text I/O contract
// ---------------------------------------------------------------------------

namespace concepts {

// Level 0 — a 64-bit unsigned word.  The algorithm's constants are 64-bit;
// accepting narrower words would silently produce a different generator.
template <class T>
concept xoshiro_word =
    std::unsigned_integral<T> &&
    sizeof(T) == sizeof(std::uint64_t) &&
    std::numeric_limits<T>::digits == 64;

// Level 1 — a fixed four-word state container.
template <class S>
concept xoshiro_state = requires(S s) {
    typename S::value_type;
    { s.size() } -> std::convertible_to<std::size_t>;
    { s[0] } -> std::convertible_to<typename S::value_type&>;
    { s[1] } -> std::convertible_to<typename S::value_type&>;
    { s[2] } -> std::convertible_to<typename S::value_type&>;
    { s[3] } -> std::convertible_to<typename S::value_type&>;
};

// Level 1 — a SeedSequence-compatible source.  We only require the 32-bit
// generate() interface actually used by the seeding algorithm.
template <class S>
concept seed_sequence_like = requires(S& seq, std::uint32_t* p) {
    { seq.generate(p, p + 8) } -> std::same_as<void>;
};

// Level 2 — the full UniformRandomBitGenerator + engine surface.
template <class G>
concept random_engine =
    std::uniform_random_bit_generator<G> &&
    requires(G g, const G cg, unsigned long long n) {
        typename G::result_type;
        typename G::state_type;
        { G::min() } -> std::same_as<typename G::result_type>;
        { G::max() } -> std::same_as<typename G::result_type>;
        { g() } -> std::same_as<typename G::result_type>;
        { g.discard(n) } -> std::same_as<void>;
        { cg.state() } -> std::same_as<const typename G::state_type&>;
    };

// Level 2a — individual engine capabilities.  Keeping these separate makes
// diagnostics precise and lets users constrain adapters by the minimum they
// actually need.
template <class G>
concept seedable_engine =
    std::uniform_random_bit_generator<G> && requires(
    G g,  G::result_type s, std::seed_seq& seq) {
    { g.seed(s) } -> std::same_as<void>;
    { g.seed(seq) } -> std::same_as<void>;
};

template <class G>
concept discardable_engine =
    std::uniform_random_bit_generator<G> && requires(G g, unsigned long long n) {
    { g.discard(n) } -> std::same_as<void>;
};

template <class G>
concept jumpable_engine =
    std::uniform_random_bit_generator<G> && requires(G g) {
    { g.jump() } -> std::same_as<void>;
    { g.long_jump() } -> std::same_as<void>;
};

template <class G>
concept checkpointable_engine =
    std::uniform_random_bit_generator<G> && requires(
    G g, const G cg,  G::state_type s) {
    { cg.state() } -> std::same_as<const typename G::state_type&>;
    { g.set_state(s) } -> std::same_as<bool>;
    { cg.is_valid() } -> std::same_as<bool>;
};

template <class G>
concept comparable_engine =
    std::uniform_random_bit_generator<G> && requires(const G a, const G b) {
    { a == b } -> std::convertible_to<bool>;
    { a != b } -> std::convertible_to<bool>;
};

template <class G>
concept nothrow_engine =
    std::uniform_random_bit_generator<G> && requires(G g, unsigned long long n) {
    { g() } noexcept -> std::same_as<typename G::result_type>;
    { g.discard(n) } noexcept -> std::same_as<void>;
};

// Level 2b — xoshiro-family extensions composed from the capabilities above.
template <class G>
concept xoshiro_engine =
    random_engine<G> && seedable_engine<G> && discardable_engine<G> &&
    jumpable_engine<G> && checkpointable_engine<G> && comparable_engine<G> &&
    nothrow_engine<G> &&
    requires {
        { G::word_size } -> std::convertible_to<std::size_t>;
        { G::state_word_count } -> std::convertible_to<std::size_t>;
        { G::state_size } -> std::convertible_to<std::size_t>;
    };

// Level 3 — text serialization contract.
template <class G>
concept stream_serializable =
    std::uniform_random_bit_generator<G> && requires(
    std::ostream& os, std::istream& is, G& g, const G& cg) {
    { os << cg } -> std::same_as<std::ostream&>;
    { is >> g } -> std::same_as<std::istream&>;
};

} // namespace concepts

// ---------------------------------------------------------------------------
// basic_xoshiro256pp
// ---------------------------------------------------------------------------

template <class Word = std::uint64_t>
    requires concepts::xoshiro_word<Word>
class basic_xoshiro256pp {
public:
    using result_type = Word;
    using state_type = std::array<Word, 4>;

    static constexpr std::size_t word_size =
        static_cast<std::size_t>(std::numeric_limits<result_type>::digits);
    static constexpr std::size_t state_word_count = 4;
    static constexpr std::size_t state_size = word_size * state_word_count;

    static constexpr result_type default_seed =
        static_cast<result_type>(0x9e3779b97f4a7c15ULL);

    // Core invariants: always checked unless the user explicitly disables
    // XOSHIRO256PP_ENABLE_CORE_STATIC_ASSERTS.
    XOSHIRO256PP_STATIC_ASSERT(concepts::xoshiro_word<result_type>);
    XOSHIRO256PP_STATIC_ASSERT(std::is_unsigned_v<result_type>);
    XOSHIRO256PP_STATIC_ASSERT(std::numeric_limits<result_type>::digits == 64);
    XOSHIRO256PP_STATIC_ASSERT(sizeof(result_type) == sizeof(std::uint64_t));
    XOSHIRO256PP_STATIC_ASSERT(word_size == 64);
    XOSHIRO256PP_STATIC_ASSERT(state_word_count == 4);
    XOSHIRO256PP_STATIC_ASSERT(state_size == 256);
    XOSHIRO256PP_STATIC_ASSERT(sizeof(state_type) == 32);
    XOSHIRO256PP_STATIC_ASSERT(concepts::xoshiro_state<state_type>);

    static constexpr result_type min() noexcept { return result_type{0}; }

    static constexpr result_type max() noexcept {
        return std::numeric_limits<result_type>::max();
    }

    constexpr basic_xoshiro256pp() noexcept : basic_xoshiro256pp(default_seed) {}

    constexpr explicit basic_xoshiro256pp(result_type seed_value) noexcept {
        seed(seed_value);
    }

    template <class SeedSequence>
        requires concepts::seed_sequence_like<SeedSequence>
    explicit basic_xoshiro256pp(SeedSequence& seq) {
        seed(seq);
    }

    constexpr void seed(result_type seed_value) noexcept {
        std::uint64_t x = static_cast<std::uint64_t>(seed_value);
        for (auto& word : state_) {
            word = static_cast<result_type>(splitmix64(x));
        }
        ensure_nonzero();
        XOSHIRO256PP_ASSERT(is_valid());
    }

    template <class SeedSequence>
        requires concepts::seed_sequence_like<SeedSequence>
    void seed(SeedSequence& seq) {
        std::uint32_t words[8]{};
        seq.generate(words, words + 8);

        for (std::size_t i = 0; i < state_word_count; ++i) {
            state_[i] = (static_cast<result_type>(words[2 * i]) << 32U) |
                        static_cast<result_type>(words[(2 * i) + 1]);
        }

        ensure_nonzero();
        XOSHIRO256PP_ASSERT(is_valid());
    }

    // Reference transition/output function.
    constexpr result_type operator()() noexcept {
        XOSHIRO256PP_ASSERT_PARANOID(is_valid());

        const result_type result =
            std::rotl(state_[0] + state_[3], 23) + state_[0];

        const result_type t = state_[1] << 17U;

        state_[2] ^= state_[0];
        state_[3] ^= state_[1];
        state_[1] ^= state_[2];
        state_[0] ^= state_[3];

        state_[2] ^= t;

        state_[3] = std::rotl(state_[3], 45);

        XOSHIRO256PP_ASSERT_PARANOID(is_valid());
        return result;
    }

    constexpr result_type next() noexcept { return (*this)(); }

    constexpr void discard(unsigned long long n) noexcept {
        XOSHIRO256PP_ASSERT_PARANOID(is_valid());
        while (n-- != 0) {
            (void)(*this)();
        }
    }

    constexpr void jump() noexcept { jump_impl(jump_poly); }

    constexpr void long_jump() noexcept { jump_impl(long_jump_poly); }

    [[nodiscard]] constexpr const state_type& state() const noexcept {
        return state_;
    }

    [[nodiscard]] constexpr bool is_valid() const noexcept {
        return is_valid_state(state_);
    }

    // Restores a checkpoint.  Returns false (and leaves the object unchanged)
    // when the supplied state is all-zero, which xoshiro forbids.  Direct
    // calls with an all-zero state also trigger the debug assertion.
    constexpr bool set_state(const state_type& s) noexcept {
        if (!is_valid_state(s)) {
            XOSHIRO256PP_ASSERT(false && "xoshiro256pp: all-zero state is invalid");
            return false;
        }
        state_ = s;
        return true;
    }

    constexpr bool operator==(const basic_xoshiro256pp&) const noexcept = default;

private:
     static constexpr state_type jump_poly{
        static_cast<result_type>(0x180ec6d33cfd0abaULL),
        static_cast<result_type>(0xd5a61266f0c9392cULL),
        static_cast<result_type>(0xa9582618e03fc9aaULL),
        static_cast<result_type>(0x39abdc4529b1661cULL),
    };

     static constexpr state_type long_jump_poly{
        static_cast<result_type>(0x76e15d3efefdcbbfULL),
        static_cast<result_type>(0xc5004e441c522fb3ULL),
        static_cast<result_type>(0x77710069854ee241ULL),
        static_cast<result_type>(0x39109bb02acbe635ULL),
    };

    // The jump polynomials must never be all-zero.
    XOSHIRO256PP_STATIC_ASSERT(jump_poly[0] != result_type{0});
    XOSHIRO256PP_STATIC_ASSERT(jump_poly[1] != result_type{0});
    XOSHIRO256PP_STATIC_ASSERT(jump_poly[2] != result_type{0});
    XOSHIRO256PP_STATIC_ASSERT(jump_poly[3] != result_type{0});
    XOSHIRO256PP_STATIC_ASSERT(long_jump_poly[0] != result_type{0});
    XOSHIRO256PP_STATIC_ASSERT(long_jump_poly[1] != result_type{0});
    XOSHIRO256PP_STATIC_ASSERT(long_jump_poly[2] != result_type{0});
    XOSHIRO256PP_STATIC_ASSERT(long_jump_poly[3] != result_type{0});

    static constexpr bool is_valid_state(const state_type& s) noexcept {
        return s[0] != result_type{0} || s[1] != result_type{0} ||
               s[2] != result_type{0} || s[3] != result_type{0};
    }

    static constexpr std::uint64_t splitmix64(std::uint64_t& x) noexcept {
        std::uint64_t z = (x += 0x9e3779b97f4a7c15ULL);
        z = (z ^ (z >> 30U)) * 0xbf58476d1ce4e5b9ULL;
        z = (z ^ (z >> 27U)) * 0x94d049bb133111ebULL;
        return z ^ (z >> 31U);
    }

    constexpr void ensure_nonzero() noexcept {
        if (!is_valid_state(state_)) {
            state_[0] = default_seed;
        }
    }

    constexpr void jump_impl(const state_type& poly) noexcept {
        XOSHIRO256PP_ASSERT(is_valid());

        result_type s0{0};
        result_type s1{0};
        result_type s2{0};
        result_type s3{0};

        for (const result_type word : poly) {
            for (unsigned b = 0; b < 64U; ++b) {
                if ((word >> b) & result_type{1}) {
                    s0 ^= state_[0];
                    s1 ^= state_[1];
                    s2 ^= state_[2];
                    s3 ^= state_[3];
                }
                (void)(*this)();
            }
        }

        state_ = {s0, s1, s2, s3};
        XOSHIRO256PP_ASSERT(is_valid());
    }

    state_type state_{};
};

// The canonical spelling.
using xoshiro256pp = basic_xoshiro256pp<>;

// ---------------------------------------------------------------------------
// Text serialization
// ---------------------------------------------------------------------------

template <class Word>
    requires concepts::xoshiro_word<Word>
inline std::ostream& operator<<(
    std::ostream& os, const basic_xoshiro256pp<Word>& g) {
    const auto& s = g.state();
    return os << s[0] << ' ' << s[1] << ' ' << s[2] << ' ' << s[3];
}

template <class Word>
    requires concepts::xoshiro_word<Word>
inline std::istream& operator>>(
    std::istream& is, basic_xoshiro256pp<Word>& g) {
    typename basic_xoshiro256pp<Word>::state_type tmp{};

    if (!(is >> tmp[0] >> tmp[1] >> tmp[2] >> tmp[3])) {
        return is;
    }

    // Keep invalid input a stream error, not a debug abort: validate before
    // calling set_state().
    if (tmp[0] == Word{0} && tmp[1] == Word{0} &&
        tmp[2] == Word{0} && tmp[3] == Word{0}) {
        is.setstate(std::ios_base::failbit);
        return is;
    }

    g.set_state(tmp);
    return is;
}

// ---------------------------------------------------------------------------
// Post-definition contract checks
// ---------------------------------------------------------------------------

XOSHIRO256PP_STATIC_ASSERT(std::uniform_random_bit_generator<xoshiro256pp>);
XOSHIRO256PP_STATIC_ASSERT(concepts::random_engine<xoshiro256pp>);
XOSHIRO256PP_STATIC_ASSERT(concepts::xoshiro_engine<xoshiro256pp>);
XOSHIRO256PP_STATIC_ASSERT(concepts::stream_serializable<xoshiro256pp>);

XOSHIRO256PP_EXTENDED_STATIC_ASSERT(concepts::seedable_engine<xoshiro256pp>);
XOSHIRO256PP_EXTENDED_STATIC_ASSERT(concepts::discardable_engine<xoshiro256pp>);
XOSHIRO256PP_EXTENDED_STATIC_ASSERT(concepts::jumpable_engine<xoshiro256pp>);
XOSHIRO256PP_EXTENDED_STATIC_ASSERT(
    concepts::checkpointable_engine<xoshiro256pp>);
XOSHIRO256PP_EXTENDED_STATIC_ASSERT(concepts::comparable_engine<xoshiro256pp>);
XOSHIRO256PP_EXTENDED_STATIC_ASSERT(concepts::nothrow_engine<xoshiro256pp>);

XOSHIRO256PP_EXTENDED_STATIC_ASSERT(std::is_standard_layout_v<xoshiro256pp>);
XOSHIRO256PP_EXTENDED_STATIC_ASSERT(std::is_trivially_copyable_v<xoshiro256pp>);
XOSHIRO256PP_EXTENDED_STATIC_ASSERT(
    std::is_nothrow_default_constructible_v<xoshiro256pp>);
XOSHIRO256PP_EXTENDED_STATIC_ASSERT(
    std::is_nothrow_copy_constructible_v<xoshiro256pp>);
XOSHIRO256PP_EXTENDED_STATIC_ASSERT(
    std::is_nothrow_move_constructible_v<xoshiro256pp>);
XOSHIRO256PP_EXTENDED_STATIC_ASSERT(
    std::is_nothrow_copy_assignable_v<xoshiro256pp>);
XOSHIRO256PP_EXTENDED_STATIC_ASSERT(
    std::is_nothrow_move_assignable_v<xoshiro256pp>);
XOSHIRO256PP_EXTENDED_STATIC_ASSERT(
    std::is_nothrow_destructible_v<xoshiro256pp>);
XOSHIRO256PP_EXTENDED_STATIC_ASSERT(
    std::is_nothrow_invocable_v<xoshiro256pp&>);
XOSHIRO256PP_EXTENDED_STATIC_ASSERT(
    std::is_same_v<decltype(std::declval<xoshiro256pp&>()()),
                   xoshiro256pp::result_type>);
XOSHIRO256PP_EXTENDED_STATIC_ASSERT(sizeof(xoshiro256pp) == 32);

} // namespace prng

#endif // PRNG_XOSHIRO256PP_HPP
