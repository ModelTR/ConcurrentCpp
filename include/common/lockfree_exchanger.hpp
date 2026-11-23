/**
 * @file lockfree_exchanger.hpp
 * @author modelTR (mxtian_ran@163.com)
 * @brief A simple impl of lockfree exchanger.
 * @version 0.2
 * @date 2026-11-23
 *
 * @copyright MIT License (c) 2026
 *
 */

#pragma once

#include "alias.hpp"
#include "atomic_tagged_ptr.hpp"

#include <chrono>
#include <optional>

namespace concur {
template <class T>
concept ReadWritePointee = std::is_object_v<T> && !std::is_const_v<T>;

template <ReadWritePointee T> class LockFreeExchanger {
public:
	using value_type = T;
	using pointer = T *;

	using clock = std::chrono::steady_clock;
	using nanos = std::chrono::nanoseconds;
	using tagged_ptr = details::AtomicTaggedPtr<T, 16>;

	using stamp_type = details::StampedTag<tagged_ptr::TAG_BITS, 2>;

	enum class ExchResState { TIMEOUT = 0, MATCHED_EMPTY = 1, MARCHED = 2 };

	struct ExchRes {
		std::optional<value_type> value;
		ExchResState state;
	};

	[[nodiscard]]
	auto exchange(pointer my_item, int nano_timeout) -> ExchRes {
		auto time_bound = clock::now() + nanos{nano_timeout};
		while (true) {
			if (clock::now() > time_bound) {
				return {std::nullopt, ExchResState::TIMEOUT};
			}
			auto [yr_item, tag] = slot_.snapshot();
			const auto state = decode_payload(tag);
			switch (state) {
			case S_EMPTY: {
				auto waiting_tag = next_(tag, S_WAITING);
				const bool claimed = slot_.compare_exchange_strong(
					yr_item, tag, my_item, transition_to(S_WAITING),
					memorder::acq_rel, memorder::acquire);
				if (claimed) {
					while (clock::now() < time_bound) {
						auto [other_item, other_tag] = slot_.snapshot();
						const auto other_state = decode_payload(other_tag);
						if (other_state == S_BUSY) {
							auto tmp = other_item;
							if (slot_.compare_exchange_strong(
									other_item, other_tag, nullptr,
									transition_to(S_EMPTY), memorder::acq_rel,
									memorder::acquire)) {
								return tmp ? ExchRes{*tmp,
													 ExchResState::MARCHED,}
										   : ExchRes{
												 std::nullopt,
												 ExchResState::MATCHED_EMPTY,
											 };
							}
						}
					}
				}
				if (claimed) {
					if (slot_.compare_exchange_strong(
							my_item, waiting_tag, nullptr,
							transition_to(S_EMPTY), memorder::release,
							memorder::relaxed)) {
						return {std::nullopt, ExchResState::TIMEOUT};
					} else {
						auto [other_item, other_tag] =
							slot_.snapshot(memorder::acquire);
						if (decode_payload(other_tag) == S_BUSY) {
							if (slot_.compare_exchange_strong(
									other_item, other_tag, nullptr,
									transition_to(S_EMPTY), memorder::acq_rel,
									memorder::acquire)) {
								return other_item ? ExchRes{*other_item,
														ExchResState::MARCHED,}
											  : ExchRes{
													std::nullopt,
													ExchResState::MATCHED_EMPTY,
												};
							}
						}
						continue;
					}
				}

			} break;
			case S_WAITING: {
				if (slot_.compare_exchange_strong(
						yr_item, tag, my_item, transition_to(S_BUSY),
						memorder::acq_rel, memorder::acquire)) {
					return yr_item ? ExchRes{*yr_item, ExchResState::MARCHED}
								   : ExchRes{
										 std::nullopt,
										 ExchResState::MATCHED_EMPTY,
									 };
				}
			} break;
			case S_BUSY: {
				break;
			}
			default:
				break;
			}
		}
	}

private:
	static auto transition_to(uintptr new_payload) noexcept {
		return details::tag_transition<
			details::AtomicTaggedPtr<T, 16>::TAG_BITS, 2>(new_payload);
	}

	static auto decode_payload(uintptr tag) noexcept -> uintptr {
		return stamp_type::decode(tag).payload;
	}
	static auto next_(uintptr tag, uintptr new_payload) noexcept -> uintptr {
		return stamp_type::decode(tag).next_tag(new_payload);
	}

private:
	enum class State : uintptr { EMPTY = 0, WAITING = 1, BUSY = 2 };
	static constexpr uintptr S_EMPTY = static_cast<uintptr>(State::EMPTY);
	static constexpr uintptr S_WAITING = static_cast<uintptr>(State::WAITING);
	static constexpr uintptr S_BUSY = static_cast<uintptr>(State::BUSY);
	details::AtomicTaggedPtr<T, 16> slot_{nullptr, S_EMPTY};
};
} // namespace concur