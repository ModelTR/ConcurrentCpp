/**
 * @file inplace_vector.hpp
 * @author modelTR (mxtian_ran@163.com)
 * @brief Header-only, fixed-capacity, in-place sequence container (C++20).
 *
 * This is an iterative rewrite of the original `concur::inplace_vector`
 * sketch.  The design goals are, in order:
 *
 *   1. Correctness: value semantics, object lifetime, exception safety and
 *      iterator invalidation rules are spelled out and tested.
 *   2. Precise constraints: every member is constrained with the smallest
 *      concept that makes it well-formed; the concepts themselves are layered
 *      (element -> insertion -> iterator/range).
 *   3. Diagnosability: compile-time contract checks and runtime checks are
 *      independently switchable (see the configuration macros below).
 *   4. Standard-library citizenship: contiguous/random-access/sized range,
 *      `std::span` construction, C++20 algorithms/ranges, ADL `swap` and
 *      `erase`/`erase_if`, `std::ranges::to` (C++23) and `std::from_range_t`.
 *
 * Constant evaluation: storage is a union plus placement new, the portable way
 * to hold non-default-constructible objects without allocating.  C++20 forbids
 * placement-new into a union array member during constant evaluation, so
 * mutating members are not constant-evaluable (default construction and the
 * query interface are).  This matches the practical restriction of the C++26
 * `std::inplace_vector`, whose constexpr support is limited to trivial types.
 *
 * Configuration macros (must be identical in every TU of one program):
 *
 *   INPLACE_VECTOR_DEBUG
 *       0/1.  Defaults to 0 when NDEBUG is defined, 1 otherwise.
 *   INPLACE_VECTOR_ASSERT_LEVEL
 *       0 = no runtime checks (release semantics).
 *       1 = preconditions of front/back/pop_back/erase/... are checked with a
 *           fatal diagnostic (std::abort).  This is the default in a Debug
 *           build (INPLACE_VECTOR_DEBUG == 1).
 *       2 = additionally re-validates internal invariants and the unchecked_*
 *           entry points.  This is the default when INPLACE_VECTOR_DEBUG is 1.
 *       An explicit value always wins over the Debug/Release default.
 *   INPLACE_VECTOR_STATIC_ASSERT_LEVEL
 *       0 = no extra compile-time checks.
 *       1 = iterator / container concept contracts.
 *       2 = additionally representation and noexcept contracts (default).
 *
 * @version 1.0.0
 * @date 2026-11-13
 * @copyright MIT License (c) 2026
 */

#ifndef CONCUR_INPLACE_VECTOR_HPP
#define CONCUR_INPLACE_VECTOR_HPP

#define INPLACE_VECTOR_VERSION_MAJOR 1
#define INPLACE_VECTOR_VERSION_MINOR 0
#define INPLACE_VECTOR_VERSION_PATCH 0
#define INPLACE_VECTOR_VERSION_STRING "1.0.0"

#include <algorithm>
#include <compare>
#include <concepts>
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <initializer_list>
#include <iterator>
#include <memory>
#include <new>
#include <ranges>
#include <stdexcept>
#include <type_traits>
#include <utility>

// ---------------------------------------------------------------------------
// Configuration
// ---------------------------------------------------------------------------

#ifndef INPLACE_VECTOR_DEBUG
#if defined(NDEBUG)
#define INPLACE_VECTOR_DEBUG 0
#else
#define INPLACE_VECTOR_DEBUG 1
#endif
#endif

#ifndef INPLACE_VECTOR_ASSERT_LEVEL
#if INPLACE_VECTOR_DEBUG
#define INPLACE_VECTOR_ASSERT_LEVEL 2
#else
#define INPLACE_VECTOR_ASSERT_LEVEL 0
#endif
#endif

#if INPLACE_VECTOR_ASSERT_LEVEL < 0 || INPLACE_VECTOR_ASSERT_LEVEL > 2
#error "INPLACE_VECTOR_ASSERT_LEVEL must be 0, 1 or 2"
#endif

#ifndef INPLACE_VECTOR_STATIC_ASSERT_LEVEL
#define INPLACE_VECTOR_STATIC_ASSERT_LEVEL 2
#endif

#if INPLACE_VECTOR_STATIC_ASSERT_LEVEL < 0 ||                                  \
	INPLACE_VECTOR_STATIC_ASSERT_LEVEL > 2
#error "INPLACE_VECTOR_STATIC_ASSERT_LEVEL must be 0, 1 or 2"
#endif

namespace concur {

// ---------------------------------------------------------------------------
// from_range
// ---------------------------------------------------------------------------

#if defined(__cpp_lib_containers_ranges) &&                                    \
	__cpp_lib_containers_ranges >= 202202L
using from_range_t = std::from_range_t;
inline constexpr from_range_t from_range{};
#else
struct from_range_t {
	explicit constexpr from_range_t() = default;
};
inline constexpr from_range_t from_range{};
#endif

namespace detail {

[[noreturn]] inline void assert_fail(const char *what, const char *file,
									 int line) noexcept {
	std::fprintf(stderr, "inplace_vector: assertion failed: %s at %s:%d\n",
				 what, file, line);
	std::abort();
}

} // namespace detail

#if INPLACE_VECTOR_ASSERT_LEVEL >= 1
#define INPLACE_VECTOR_ASSERT(expr)                                            \
	((expr) ? static_cast<void>(0)                                             \
			: ::concur::detail::assert_fail(#expr, __FILE__, __LINE__))
#else
#define INPLACE_VECTOR_ASSERT(expr) (static_cast<void>(0))
#endif

namespace detail {

// ---------------------------------------------------------------------------
// Concept layer 1: element / lifetime requirements
// ---------------------------------------------------------------------------

/// The absolute minimum for a type to be stored in an inplace_vector.
template <class T>
concept Element = std::destructible<T> && std::is_object_v<T> &&
				  !std::is_const_v<T> && !std::is_volatile_v<T>;

template <class T>
concept DefaultInitializable = std::default_initializable<T>;

template <class T>
concept MoveInsertable = std::constructible_from<T, T &&>;

template <class T>
concept CopyInsertable = std::constructible_from<T, const T &>;

template <class T>
concept MoveAssignable = std::assignable_from<T &, T &&>;

/// Relocation is used by insert/erase/swap: both operations must not throw,
/// which lets those members provide the strong exception-safety guarantee.
template <class T>
concept NothrowRelocatable = std::is_nothrow_move_constructible_v<T> &&
							 std::is_nothrow_move_assignable_v<T>;

template <class T>
concept Swappable = std::swappable<T>;

template <class T>
concept EqualityComparable = requires(const T &a, const T &b) {
	{ a == b } -> std::convertible_to<bool>;
};

template <class T>
concept ThreeWayComparable = std::three_way_comparable<T>;

template <class T, class U>
concept ComparableWith = requires(const T &a, const U &b) {
	{ a == b } -> std::convertible_to<bool>;
};

// ---------------------------------------------------------------------------
// Concept layer 2: construction with arguments
// ---------------------------------------------------------------------------

template <class T, class... Args>
concept EmplaceConstructible = std::constructible_from<T, Args...>;

template <class T, class... Args>
concept NothrowEmplaceConstructible =
	EmplaceConstructible<T, Args...> &&
	std::is_nothrow_constructible_v<T, Args...>;

// ---------------------------------------------------------------------------
// Concept layer 3: iterators and ranges
// ---------------------------------------------------------------------------

template <class It, class T>
concept InputIteratorOf = std::input_iterator<It> &&
						  std::constructible_from<T, std::iter_reference_t<It>>;

template <class R, class T>
concept InputRangeOf =
	std::ranges::input_range<R> &&
	std::constructible_from<T, std::ranges::range_reference_t<R>>;

template <class It, class Sent>
concept SentinelFor = std::sentinel_for<Sent, It>;

// ---------------------------------------------------------------------------
// Storage
// ---------------------------------------------------------------------------

/// Raw storage for N objects of type T.  Deliberately a union: this keeps the
/// storage usable in constant evaluation (no reinterpret_cast, no allocator)
/// and gives it the alignment of T for free.
template <class T, std::size_t N> union Storage {
	char dummy_;
	T data[N];
	constexpr Storage() noexcept : dummy_() {}
	constexpr ~Storage() {}
};

template <class T> union Storage<T, 0> {
	constexpr Storage() noexcept {}
	constexpr ~Storage() {}
};

// ---------------------------------------------------------------------------
// Iterator
// ---------------------------------------------------------------------------

/// Random-access, contiguous iterator over the in-place storage.
///
/// T is either `U` or `const U`; the conversion from `iterator` to
/// `const_iterator` is implicit, comparisons and differences work across
/// constness, and `operator->` returns a raw pointer so that
/// `std::to_address` (and therefore `std::contiguous_iterator`) works.
template <class T> class InplaceVectorIterator {
public:
	using iterator_concept = std::contiguous_iterator_tag;
	using iterator_category = std::random_access_iterator_tag;
	using value_type = std::remove_cv_t<T>;
	using difference_type = std::ptrdiff_t;
	using pointer = T *;
	using reference = T &;

private:
	pointer ptr_ = nullptr;

public:
	constexpr InplaceVectorIterator() noexcept = default;
	constexpr explicit InplaceVectorIterator(pointer p) noexcept : ptr_(p) {}

	template <class U>
		requires std::convertible_to<U *, T *>
	constexpr InplaceVectorIterator(
		const InplaceVectorIterator<U> &other) noexcept
		: ptr_(other.base()) {}

	[[nodiscard]] constexpr auto base() const noexcept -> pointer {
		return ptr_;
	}

	constexpr auto operator*() const noexcept -> reference { return *ptr_; }
	constexpr auto operator->() const noexcept -> pointer { return ptr_; }
	constexpr auto operator[](difference_type n) const noexcept -> reference {
		return ptr_[n];
	}

	constexpr auto operator++() noexcept -> InplaceVectorIterator & {
		++ptr_;
		return *this;
	}
	constexpr auto operator++(int) noexcept -> InplaceVectorIterator {
		auto tmp = *this;
		++ptr_;
		return tmp;
	}
	constexpr auto operator--() noexcept -> InplaceVectorIterator & {
		--ptr_;
		return *this;
	}
	constexpr auto operator--(int) noexcept -> InplaceVectorIterator {
		auto tmp = *this;
		--ptr_;
		return tmp;
	}

	constexpr auto operator+=(difference_type n) noexcept
		-> InplaceVectorIterator & {
		ptr_ += n;
		return *this;
	}
	constexpr auto operator-=(difference_type n) noexcept
		-> InplaceVectorIterator & {
		ptr_ -= n;
		return *this;
	}

	friend constexpr auto operator+(InplaceVectorIterator it,
									difference_type n) noexcept
		-> InplaceVectorIterator {
		it += n;
		return it;
	}
	friend constexpr auto operator+(difference_type n,
									InplaceVectorIterator it) noexcept
		-> InplaceVectorIterator {
		it += n;
		return it;
	}
	friend constexpr auto operator-(InplaceVectorIterator it,
									difference_type n) noexcept
		-> InplaceVectorIterator {
		it -= n;
		return it;
	}
};

template <class U, class V>
constexpr auto operator==(const InplaceVectorIterator<U> &a,
						  const InplaceVectorIterator<V> &b) noexcept -> bool
	requires std::equality_comparable_with<U *, V *>
{
	return a.base() == b.base();
}

template <class U, class V>
constexpr auto operator<=>(const InplaceVectorIterator<U> &a,
						   const InplaceVectorIterator<V> &b) noexcept
	requires std::three_way_comparable_with<U *, V *>
{
	return a.base() <=> b.base();
}

template <class U, class V>
constexpr auto operator-(const InplaceVectorIterator<U> &a,
						 const InplaceVectorIterator<V> &b) noexcept
	-> std::ptrdiff_t
	requires(std::is_convertible_v<U *, V *> || std::is_convertible_v<V *, U *>)
{
	return a.base() - b.base();
}

} // namespace detail

/// Backwards-compatible namespace alias for the original implementation.

#if INPLACE_VECTOR_STATIC_ASSERT_LEVEL >= 1
static_assert(std::random_access_iterator<detail::InplaceVectorIterator<int>>);
static_assert(std::contiguous_iterator<detail::InplaceVectorIterator<int>>);
static_assert(
	std::contiguous_iterator<detail::InplaceVectorIterator<const int>>);
static_assert(sizeof(detail::InplaceVectorIterator<int>) == sizeof(int *));
#endif

// ---------------------------------------------------------------------------
// inplace_vector
// ---------------------------------------------------------------------------

/**
 * Fixed-capacity sequence container that stores its elements inside the
 * object itself.  Never allocates; overflowing operations throw
 * `std::bad_alloc` (the C++26 `std::inplace_vector` contract).
 *
 * The type models `std::ranges::contiguous_range`, is usable in constant
 * expressions whenever T is, and interoperates with `std::span` and the
 * C++20 algorithms/ranges.
 */
template <detail::Element T, std::size_t N> class inplace_vector {
	using Storage = detail::Storage<T, N>;

	Storage storage_;
	std::size_t size_ = 0;

	[[nodiscard]] constexpr auto data_ptr() noexcept -> T * {
		if constexpr (N == 0)
			return nullptr;
		else
			return storage_.data;
	}
	[[nodiscard]] constexpr auto data_ptr() const noexcept -> const T * {
		if constexpr (N == 0)
			return nullptr;
		else
			return storage_.data;
	}

	/// Index of an iterator that is assumed to belong to this container.
	[[nodiscard]] constexpr auto
	index_of(detail::InplaceVectorIterator<const T> pos) const -> std::size_t {
		INPLACE_VECTOR_ASSERT(pos >= cbegin() && pos <= cend());
		return static_cast<std::size_t>(pos - cbegin());
	}

	/// Re-validate the representation invariant (assert level 2).
	constexpr auto check_invariant() const noexcept -> void {
#if INPLACE_VECTOR_ASSERT_LEVEL >= 2
		INPLACE_VECTOR_ASSERT(size_ <= N);
		if constexpr (N == 0) {
			INPLACE_VECTOR_ASSERT(size_ == 0);
		}
#endif
	}

public:
	using value_type = T;
	using size_type = std::size_t;
	using difference_type = std::ptrdiff_t;
	using reference = T &;
	using const_reference = const T &;
	using pointer = T *;
	using const_pointer = const T *;
	using iterator = detail::InplaceVectorIterator<T>;
	using const_iterator = detail::InplaceVectorIterator<const T>;
	using reverse_iterator = std::reverse_iterator<iterator>;
	using const_reverse_iterator = std::reverse_iterator<const_iterator>;

#if INPLACE_VECTOR_STATIC_ASSERT_LEVEL >= 1
	static_assert(std::is_same_v<value_type, T>);
	static_assert(std::is_same_v<difference_type, std::ptrdiff_t>);
	static_assert(std::is_same_v<std::iter_value_t<iterator>, T>);
	static_assert(std::is_same_v<std::iter_reference_t<iterator>, T &>);
	static_assert(std::random_access_iterator<iterator>);
	static_assert(std::random_access_iterator<const_iterator>);
	static_assert(std::contiguous_iterator<iterator>);
	static_assert(std::contiguous_iterator<const_iterator>);
#endif
#if INPLACE_VECTOR_STATIC_ASSERT_LEVEL >= 2
	static_assert(sizeof(iterator) == sizeof(pointer));
	static_assert(sizeof(const_iterator) == sizeof(const_pointer));
	static_assert(std::is_trivially_copyable_v<iterator>);
	static_assert(std::is_trivially_copyable_v<const_iterator>);
	static_assert(N == 0 || alignof(Storage) >= alignof(T));
	static_assert(sizeof(Storage) >= N * sizeof(T));
#endif

	// -- construction / destruction -----------------------------------------

	constexpr inplace_vector() noexcept = default;
	constexpr ~inplace_vector() { clear(); }

	constexpr inplace_vector(const inplace_vector &other)
		requires detail::CopyInsertable<T>
	{
		try {
			for (; size_ < other.size_; ++size_)
				std::construct_at(data_ptr() + size_, other.data_ptr()[size_]);
		} catch (...) {
			clear();
			throw;
		}
	}

	constexpr inplace_vector(inplace_vector &&other) noexcept(
		std::is_nothrow_move_constructible_v<T>)
		requires detail::MoveInsertable<T>
	{
		for (; size_ < other.size_; ++size_)
			std::construct_at(data_ptr() + size_,
							  std::move(other.data_ptr()[size_]));
		other.clear();
	}

	constexpr explicit inplace_vector(size_type n)
		requires detail::DefaultInitializable<T>
	{
		if (n > N)
			throw std::bad_alloc();
		try {
			for (; size_ < n; ++size_)
				std::construct_at(data_ptr() + size_);
		} catch (...) {
			clear();
			throw;
		}
	}

	constexpr inplace_vector(size_type n, const T &value)
		requires detail::CopyInsertable<T>
	{
		if (n > N)
			throw std::bad_alloc();
		try {
			for (; size_ < n; ++size_)
				std::construct_at(data_ptr() + size_, value);
		} catch (...) {
			clear();
			throw;
		}
	}

	constexpr inplace_vector(std::initializer_list<T> il)
		requires detail::CopyInsertable<T>
	{
		if (il.size() > N)
			throw std::bad_alloc();
		try {
			for (const auto &value : il) {
				std::construct_at(data_ptr() + size_, value);
				++size_;
			}
		} catch (...) {
			clear();
			throw;
		}
	}

	template <std::input_iterator It, std::sentinel_for<It> Sent>
		requires std::constructible_from<T, std::iter_reference_t<It>>
	constexpr inplace_vector(It first, Sent last) {
		try {
			for (; first != last; ++first) {
				if (size_ == N)
					throw std::bad_alloc();
				std::construct_at(data_ptr() + size_, *first);
				++size_;
			}
		} catch (...) {
			clear();
			throw;
		}
	}

	template <detail::InputRangeOf<T> R>
	constexpr explicit inplace_vector(from_range_t, R &&rg) {
		try {
			append_range(std::forward<R>(rg));
		} catch (...) {
			clear();
			throw;
		}
	}

	// -- assignment ----------------------------------------------------------

	constexpr auto operator=(const inplace_vector &other) -> inplace_vector &
		requires detail::CopyInsertable<T>
	{
		if (this == &other)
			return *this;
		clear();
		try {
			for (; size_ < other.size_; ++size_)
				std::construct_at(data_ptr() + size_, other.data_ptr()[size_]);
		} catch (...) {
			clear();
			throw;
		}
		return *this;
	}

	constexpr auto operator=(inplace_vector &&other) noexcept(
		std::is_nothrow_move_constructible_v<T>) -> inplace_vector &
		requires detail::MoveInsertable<T>
	{
		if (this == &other)
			return *this;
		clear();
		for (; size_ < other.size_; ++size_)
			std::construct_at(data_ptr() + size_,
							  std::move(other.data_ptr()[size_]));
		other.clear();
		return *this;
	}

	// -- element access ------------------------------------------------------

	constexpr auto at(size_type pos) -> reference {
		if (pos >= size_)
			throw std::out_of_range("inplace_vector::at");
		return data_ptr()[pos];
	}
	constexpr auto at(size_type pos) const -> const_reference {
		if (pos >= size_)
			throw std::out_of_range("inplace_vector::at");
		return data_ptr()[pos];
	}

	constexpr auto unchecked_at(size_type pos) -> reference {
		return data_ptr()[pos];
	}
	constexpr auto unchecked_at(size_type pos) const -> const_reference {
		return data_ptr()[pos];
	}

	constexpr auto operator[](size_type pos) -> reference {
#if INPLACE_VECTOR_ASSERT_LEVEL >= 2
		INPLACE_VECTOR_ASSERT(pos < size_);
#endif
		return data_ptr()[pos];
	}
	constexpr auto operator[](size_type pos) const -> const_reference {
#if INPLACE_VECTOR_ASSERT_LEVEL >= 2
		INPLACE_VECTOR_ASSERT(pos < size_);
#endif
		return data_ptr()[pos];
	}

	constexpr auto front() -> reference {
		INPLACE_VECTOR_ASSERT(!empty());
		return data_ptr()[0];
	}
	constexpr auto front() const -> const_reference {
		INPLACE_VECTOR_ASSERT(!empty());
		return data_ptr()[0];
	}

	constexpr auto back() -> reference {
		INPLACE_VECTOR_ASSERT(!empty());
		return data_ptr()[size_ - 1];
	}
	constexpr auto back() const -> const_reference {
		INPLACE_VECTOR_ASSERT(!empty());
		return data_ptr()[size_ - 1];
	}

	constexpr auto data() noexcept -> pointer { return data_ptr(); }
	constexpr auto data() const noexcept -> const_pointer { return data_ptr(); }

	// -- iterators -----------------------------------------------------------

	constexpr auto begin() noexcept -> iterator { return iterator(data_ptr()); }
	constexpr auto begin() const noexcept -> const_iterator {
		return const_iterator(data_ptr());
	}
	constexpr auto cbegin() const noexcept -> const_iterator {
		return const_iterator(data_ptr());
	}

	constexpr auto end() noexcept -> iterator {
		return iterator(size_ == 0 ? data_ptr() : data_ptr() + size_);
	}
	constexpr auto end() const noexcept -> const_iterator {
		return const_iterator(size_ == 0 ? data_ptr() : data_ptr() + size_);
	}
	constexpr auto cend() const noexcept -> const_iterator {
		return const_iterator(size_ == 0 ? data_ptr() : data_ptr() + size_);
	}

	constexpr auto rbegin() noexcept -> reverse_iterator {
		return reverse_iterator(end());
	}
	constexpr auto rbegin() const noexcept -> const_reverse_iterator {
		return const_reverse_iterator(end());
	}
	constexpr auto crbegin() const noexcept -> const_reverse_iterator {
		return const_reverse_iterator(end());
	}

	constexpr auto rend() noexcept -> reverse_iterator {
		return reverse_iterator(begin());
	}
	constexpr auto rend() const noexcept -> const_reverse_iterator {
		return const_reverse_iterator(begin());
	}
	constexpr auto crend() const noexcept -> const_reverse_iterator {
		return const_reverse_iterator(begin());
	}

	// -- capacity ------------------------------------------------------------

	[[nodiscard]] constexpr auto empty() const noexcept -> bool {
		return size_ == 0;
	}
	[[nodiscard]] constexpr auto size() const noexcept -> size_type {
		return size_;
	}

	[[nodiscard]] static constexpr auto max_size() noexcept -> size_type {
		return N;
	}
	[[nodiscard]] static constexpr auto capacity() noexcept -> size_type {
		return N;
	}

	static constexpr auto reserve(size_type n) -> void {
		if (n > N)
			throw std::bad_alloc();
	}
	static constexpr auto shrink_to_fit() noexcept -> void {}

	// -- modifiers -----------------------------------------------------------

	constexpr auto clear() noexcept -> void {
		if constexpr (!std::is_trivially_destructible_v<T>) {
			for (size_type i = 0; i < size_; ++i)
				std::destroy_at(data_ptr() + i);
		}
		size_ = 0;
	}

	constexpr auto push_back(const T &value) -> reference
		requires detail::CopyInsertable<T>
	{
		return emplace_back(value);
	}
	constexpr auto push_back(T &&value) -> reference
		requires detail::MoveInsertable<T>
	{
		return emplace_back(std::move(value));
	}

	template <class... Args>
		requires detail::EmplaceConstructible<T, Args...>
	constexpr auto emplace_back(Args &&...args) -> reference {
		if (size_ == N)
			throw std::bad_alloc();
		std::construct_at(data_ptr() + size_, std::forward<Args>(args)...);
		++size_;
		check_invariant();
		return data_ptr()[size_ - 1];
	}

	constexpr auto pop_back() noexcept(std::is_nothrow_destructible_v<T>)
		-> void {
		INPLACE_VECTOR_ASSERT(!empty());
		std::destroy_at(data_ptr() + size_ - 1);
		--size_;
		check_invariant();
	}

	// -- try_* / unchecked_* extensions --------------------------------------

	constexpr auto try_push_back(const T &value) noexcept(
		std::is_nothrow_constructible_v<T, const T &>) -> pointer
		requires detail::CopyInsertable<T>
	{
		if (size_ == N)
			return nullptr;
		std::construct_at(data_ptr() + size_, value);
		++size_;
		check_invariant();
		return data_ptr() + size_ - 1;
	}

	constexpr auto
	try_push_back(T &&value) noexcept(std::is_nothrow_constructible_v<T, T &&>)
		-> pointer
		requires detail::MoveInsertable<T>
	{
		if (size_ == N)
			return nullptr;
		std::construct_at(data_ptr() + size_, std::move(value));
		++size_;
		check_invariant();
		return data_ptr() + size_ - 1;
	}

	template <class... Args>
		requires detail::EmplaceConstructible<T, Args...>
	constexpr auto try_emplace_back(Args &&...args) noexcept(
		std::is_nothrow_constructible_v<T, Args...>) -> pointer {
		if (size_ == N)
			return nullptr;
		std::construct_at(data_ptr() + size_, std::forward<Args>(args)...);
		++size_;
		check_invariant();
		return data_ptr() + size_ - 1;
	}

	constexpr auto unchecked_push_back(const T &value) -> reference
		requires detail::CopyInsertable<T>
	{
#if INPLACE_VECTOR_ASSERT_LEVEL >= 2
		INPLACE_VECTOR_ASSERT(size_ < N);
#endif
		std::construct_at(data_ptr() + size_, value);
		++size_;
		check_invariant();
		return data_ptr()[size_ - 1];
	}

	constexpr auto unchecked_push_back(T &&value) -> reference
		requires detail::MoveInsertable<T>
	{
#if INPLACE_VECTOR_ASSERT_LEVEL >= 2
		INPLACE_VECTOR_ASSERT(size_ < N);
#endif
		std::construct_at(data_ptr() + size_, std::move(value));
		++size_;
		check_invariant();
		return data_ptr()[size_ - 1];
	}

	template <class... Args>
		requires detail::EmplaceConstructible<T, Args...>
	constexpr auto unchecked_emplace_back(Args &&...args) -> reference {
#if INPLACE_VECTOR_ASSERT_LEVEL >= 2
		INPLACE_VECTOR_ASSERT(size_ < N);
#endif
		std::construct_at(data_ptr() + size_, std::forward<Args>(args)...);
		++size_;
		check_invariant();
		return data_ptr()[size_ - 1];
	}

	// -- resize / fill -------------------------------------------------------

	constexpr auto resize(size_type n) -> void
		requires detail::DefaultInitializable<T>
	{
		if (n > N)
			throw std::bad_alloc();
		if (n < size_) {
			for (size_type i = n; i < size_; ++i)
				std::destroy_at(data_ptr() + i);
			size_ = n;
		} else {
			for (; size_ < n; ++size_)
				std::construct_at(data_ptr() + size_);
		}
		check_invariant();
	}

	constexpr auto resize(size_type n, const T &value) -> void
		requires detail::CopyInsertable<T>
	{
		if (n > N)
			throw std::bad_alloc();
		if (n <= size_) {
			for (size_type i = n; i < size_; ++i)
				std::destroy_at(data_ptr() + i);
			size_ = n;
		} else {
			T tmp(value);
			for (; size_ < n; ++size_)
				std::construct_at(data_ptr() + size_, tmp);
		}
		check_invariant();
	}

	/// Extension: clear the container and fill it with `n` copies of `value`.
	constexpr auto fill(size_type n, const T &value) -> void
		requires detail::CopyInsertable<T>
	{
		if (n > N)
			throw std::bad_alloc();
		T tmp(value);
		clear();
		for (; size_ < n; ++size_)
			std::construct_at(data_ptr() + size_, tmp);
		check_invariant();
	}

	// -- insert / emplace ----------------------------------------------------

	template <class... Args>
		requires detail::EmplaceConstructible<T, Args...> &&
				 detail::NothrowRelocatable<T>
	constexpr auto emplace(const_iterator pos, Args &&...args) -> iterator {
		if (size_ == N)
			throw std::bad_alloc();
		const auto idx = index_of(pos);
		INPLACE_VECTOR_ASSERT(idx <= size_);

		// Construct the new value first: if this throws, *this is untouched.
		T tmp(std::forward<Args>(args)...);

		// Relocate [idx, size_) one slot to the right.  The destination of the
		// last element is raw storage; everything else is already constructed.
		for (size_type i = size_; i > idx; --i) {
			const size_type src = i - 1;
			if (i == size_)
				std::construct_at(data_ptr() + i, std::move(data_ptr()[src]));
			else
				data_ptr()[i] = std::move(data_ptr()[src]);
		}

		if (idx == size_)
			std::construct_at(data_ptr() + idx, std::move(tmp));
		else
			data_ptr()[idx] = std::move(tmp);

		++size_;
		check_invariant();
		return begin() + static_cast<difference_type>(idx);
	}

	constexpr auto insert(const_iterator pos, const T &value) -> iterator
		requires detail::CopyInsertable<T> && detail::NothrowRelocatable<T>
	{
		return emplace(pos, value);
	}
	constexpr auto insert(const_iterator pos, T &&value) -> iterator
		requires detail::MoveInsertable<T> && detail::NothrowRelocatable<T>
	{
		return emplace(pos, std::move(value));
	}

	constexpr auto insert(const_iterator pos, size_type n, const T &value)
		-> iterator
		requires detail::CopyInsertable<T> && detail::NothrowRelocatable<T>
	{
		if (n > N - size_)
			throw std::bad_alloc();
		const auto idx = index_of(pos);
		for (size_type k = 0; k < n; ++k)
			emplace(begin() + static_cast<difference_type>(idx + k), value);
		return begin() + static_cast<difference_type>(idx);
	}

	template <std::input_iterator It, std::sentinel_for<It> Sent>
		requires std::constructible_from<T, std::iter_reference_t<It>> &&
				 detail::NothrowRelocatable<T>
	constexpr auto insert(const_iterator pos, It first, Sent last) -> iterator {
		const auto idx = index_of(pos);
		if constexpr (std::forward_iterator<It>) {
			const auto count = std::ranges::distance(first, last);
			if (count > 0 && static_cast<size_type>(count) > N - size_)
				throw std::bad_alloc();
		}
		size_type written = 0;
		for (; first != last; ++first, ++written) {
			if (size_ == N)
				throw std::bad_alloc();
			emplace(begin() + static_cast<difference_type>(idx + written),
					*first);
		}
		return begin() + static_cast<difference_type>(idx);
	}

	template <detail::InputRangeOf<T> R>
		requires detail::NothrowRelocatable<T>
	constexpr auto insert_range(const_iterator pos, R &&rg) -> iterator {
		return insert(pos, std::ranges::begin(std::forward<R>(rg)),
					  std::ranges::end(std::forward<R>(rg)));
	}

	// -- range append / assign ----------------------------------------------

	template <detail::InputRangeOf<T> R>
	constexpr auto append_range(R &&rg) -> void {
		if constexpr (std::ranges::forward_range<R> ||
					  std::ranges::sized_range<R>) {
			const auto count = std::ranges::distance(std::forward<R>(rg));
			if (count > 0 && static_cast<size_type>(count) > N - size_)
				throw std::bad_alloc();
		}
		auto first = std::ranges::begin(std::forward<R>(rg));
		auto last = std::ranges::end(std::forward<R>(rg));
		for (; first != last; ++first) {
			if (size_ == N)
				throw std::bad_alloc();
			std::construct_at(data_ptr() + size_, *first);
			++size_;
		}
		check_invariant();
	}

	template <detail::InputRangeOf<T> R>
	constexpr auto try_append_range(R &&rg)
		-> std::ranges::borrowed_iterator_t<R> {
		auto first = std::ranges::begin(std::forward<R>(rg));
		auto last = std::ranges::end(std::forward<R>(rg));
		for (; first != last; ++first) {
			if (size_ == N) {
				if constexpr (std::ranges::borrowed_range<R>)
					return first;
				else
					return std::ranges::dangling{};
			}
			std::construct_at(data_ptr() + size_, *first);
			++size_;
		}
		check_invariant();
		if constexpr (std::ranges::borrowed_range<R>)
			return last;
		else
			return std::ranges::dangling{};
	}

	template <detail::InputRangeOf<T> R>
	constexpr auto assign_range(R &&rg) -> void {
		clear();
		try {
			append_range(std::forward<R>(rg));
		} catch (...) {
			clear();
			throw;
		}
	}

	// -- erase ---------------------------------------------------------------

	constexpr auto erase(const_iterator pos) -> iterator
		requires detail::NothrowRelocatable<T>
	{
		INPLACE_VECTOR_ASSERT(pos >= cbegin() && pos < cend());
		const auto idx = index_of(pos);
		for (size_type i = idx; i + 1 < size_; ++i)
			data_ptr()[i] = std::move(data_ptr()[i + 1]);
		std::destroy_at(data_ptr() + size_ - 1);
		--size_;
		check_invariant();
		return begin() + static_cast<difference_type>(idx);
	}

	constexpr auto erase(const_iterator first, const_iterator last) -> iterator
		requires detail::NothrowRelocatable<T>
	{
		const auto f = index_of(first);
		const auto l = index_of(last);
		INPLACE_VECTOR_ASSERT(f <= l);
		const auto n = l - f;
		for (size_type i = f; i + n < size_; ++i)
			data_ptr()[i] = std::move(data_ptr()[i + n]);
		for (size_type i = size_ - n; i < size_; ++i)
			std::destroy_at(data_ptr() + i);
		size_ -= n;
		check_invariant();
		return begin() + static_cast<difference_type>(f);
	}

	// -- swap ----------------------------------------------------------------

	constexpr auto swap(inplace_vector &other) noexcept(
		std::is_nothrow_swappable_v<T> &&
		std::is_nothrow_move_constructible_v<T>) -> void
		requires detail::Swappable<T> && detail::NothrowRelocatable<T>
	{
		if (this == &other)
			return;
		const auto common = size_ < other.size_ ? size_ : other.size_;
		for (size_type i = 0; i < common; ++i)
			std::swap(data_ptr()[i], other.data_ptr()[i]);

		if (size_ > other.size_) {
			for (size_type i = common; i < size_; ++i) {
				std::construct_at(other.data_ptr() + i,
								  std::move(data_ptr()[i]));
				std::destroy_at(data_ptr() + i);
			}
		} else {
			for (size_type i = common; i < other.size_; ++i) {
				std::construct_at(data_ptr() + i,
								  std::move(other.data_ptr()[i]));
				std::destroy_at(other.data_ptr() + i);
			}
		}
		std::swap(size_, other.size_);
		check_invariant();
	}

	// -- comparison ----------------------------------------------------------

	friend constexpr auto operator==(const inplace_vector &a,
									 const inplace_vector &b) -> bool
		requires detail::EqualityComparable<T>
	{
		return a.size_ == b.size_ &&
			   std::equal(a.begin(), a.end(), b.begin(), b.end());
	}

	friend constexpr auto operator<=>(const inplace_vector &a,
									  const inplace_vector &b)
		requires detail::ThreeWayComparable<T>
	{
		return std::lexicographical_compare_three_way(a.begin(), a.end(),
													  b.begin(), b.end());
	}
};

// ---------------------------------------------------------------------------
// Non-member functions
// ---------------------------------------------------------------------------

template <detail::Element T, std::size_t N>
constexpr auto swap(inplace_vector<T, N> &a,
					inplace_vector<T, N> &b) noexcept(noexcept(a.swap(b)))
	-> void
	requires detail::Swappable<T> && detail::NothrowRelocatable<T>
{
	a.swap(b);
}

template <detail::Element T, std::size_t N, class U>
	requires detail::ComparableWith<T, U> && detail::NothrowRelocatable<T>
constexpr auto erase(inplace_vector<T, N> &c, const U &value)
	-> inplace_vector<T, N>::size_type {
	if (c.empty())
		return 0;
	auto it = std::remove(c.begin(), c.end(), value);
	const auto removed = c.size() - static_cast<std::size_t>(it - c.begin());
	c.erase(it, c.end());
	return removed;
}

template <detail::Element T, std::size_t N, class Pred>
	requires std::predicate<Pred &, T &> && detail::NothrowRelocatable<T>
constexpr auto erase_if(inplace_vector<T, N> &c, Pred pred)
	-> inplace_vector<T, N>::size_type {
	if (c.empty())
		return 0;
	auto it = std::remove_if(c.begin(), c.end(), pred);
	const auto removed = c.size() - static_cast<std::size_t>(it - c.begin());
	c.erase(it, c.end());
	return removed;
}

} // namespace concur

#endif // CONCUR_INPLACE_VECTOR_HPP
