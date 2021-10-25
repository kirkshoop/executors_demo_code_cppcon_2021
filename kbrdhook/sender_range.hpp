/*
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * Licensed under the Apache License Version 2.0 with LLVM Exceptions
 * (the "License"); you may not use this file except in compliance with
 * the License. You may obtain a copy of the License at
 *
 *   https://llvm.org/LICENSE.txt
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#pragma once

#include <unifex/detail/atomic_intrusive_queue.hpp>

#include <unifex/create.hpp>
#include <unifex/scheduler_concepts.hpp>
#include <unifex/sender_concepts.hpp>
#include <unifex/sender_concepts.hpp>
#include <unifex/unstoppable_token.hpp>

#include <optional>
#include <ranges>

namespace detail {
// _conv needed so we can emplace construct non-movable types into
// a std::optional.
template <typename F>
struct _conv {
  F f_;
  explicit _conv(F f) noexcept : f_((F &&) f) {}
  operator std::invoke_result_t<F>() && { return ((F &&) f_)(); }
};
}  // namespace detail

template <
    typename EventType,
    typename RangeStopToken,
    typename RegisterFn,
    typename UnregisterFn>
struct sender_range {
  using complete_function_t = void (*)(void*, EventType*) noexcept;

  struct pending_operation {
    void* pendingOperation_;
    complete_function_t complete_with_event_;

    void operator()(EventType* e) {
      std::exchange(complete_with_event_, nullptr)(
          std::exchange(pendingOperation_, nullptr), e);
    };

    pending_operation* next_{nullptr};
  };

  template <typename State>
  void start(State* state) noexcept {
    if (rangeToken_.stop_requested() ||
        state->eventStopToken_.stop_requested()) {
      unifex::set_done(std::move(state->rec_));
    } else {
      (void)pendingOperations_.enqueue(&state->pending_);
    }
  }

  void dispatch(EventType* event) {
    auto pending = pendingOperations_.dequeue_all();

    if (pending.empty()) {
      // no pending operations to complete, discard this event
      return;
    }

    auto& complete = *pending.pop_front();
    if (!pending.empty()) {
      // more than one pending operation - bug in sender_range usage (do not
      // start a sender from the range until the previous sender has completed.)
      std::terminate();
    }

    complete(event);
  }

  void stop_pending() { dispatch(nullptr); }

  struct event_function {
    sender_range* range_;

    template <typename EventType2>
    void operator()(EventType2&& event) {
      range_->dispatch(&event);
    }
  };

  using registration_t = unifex::callable_result_t<RegisterFn, event_function&>;

  struct create_sender {
    template <
        template <typename...>
        class Variant,
        template <typename...>
        class Tuple>
    using value_types = Variant<Tuple<EventType>>;

    template <template <typename...> class Variant>
    using error_types = Variant<std::exception_ptr>;

    static inline constexpr bool sends_done = true;

    template <typename Receiver, typename EventStopToken>
    struct state {
      static void
      _complete_with_event(void* selfVoid, EventType* event) noexcept {
        auto& self = *reinterpret_cast<state*>(selfVoid);
        if (!!event) {
          unifex::set_value(std::move(self.rec_), std::move(*event));
        } else {
          unifex::set_done(std::move(self.rec_));
        }
      }

      // args
      sender_range* range_;
      Receiver& rec_;
      EventStopToken eventStopToken_;

      // this is stored in an intrusive queue so that the event_function can
      // dequeue and dispatch the next event
      pending_operation pending_;

      // cancellation of the pending sender
      struct stop_callback {
        sender_range* range_;
        void operator()() noexcept { range_->stop_pending(); }
      };
      typename EventStopToken::template callback_type<stop_callback> callback_;

      state(sender_range* scope, Receiver& rec, EventStopToken eventStopToken)
        : range_(scope)
        , rec_(rec)
        , eventStopToken_(eventStopToken)
        , pending_({this, &_complete_with_event})
        , callback_(eventStopToken_, stop_callback{range_}) {
        range_->start(this);
      }
      state(state&&) = delete;
    };

    template <typename Receiver>
    requires unifex::
        is_callable_v<unifex::tag_t<unifex::get_stop_token>, Receiver>
    state<Receiver, unifex::stop_token_type_t<Receiver>>
    operator()(Receiver& rec, sender_range* scope) noexcept {
      return {scope, rec, unifex::get_stop_token(rec)};
    }

    template <typename Receiver>
    requires (!unifex::is_callable_v<unifex::tag_t<unifex::get_stop_token>, Receiver>)
    state<Receiver, unifex::unstoppable_token>
    operator()(Receiver& rec, sender_range* scope) noexcept {
      return {scope, rec, unifex::unstoppable_token{}};
    }
  };

  struct stop_callback {
    sender_range* range_;
    void operator()() noexcept { range_->_unregister(); }
  };

  // this function is used to provide a stable type for the expression inside
  // (that uses a lambda)
  static auto make_range(sender_range* self) {
    return std::views::iota(0) | std::views::transform([self](int) {
             return unifex::create(create_sender{}, self);
           });
  }

  // storage for underlying range that produces senders
  using RangeType = decltype(make_range(nullptr));
  RangeType range_;
  // args
  RangeStopToken rangeToken_;
  RegisterFn registerFn_;
  UnregisterFn unregisterFn_;
  // cancellation
  typename RangeStopToken::template callback_type<stop_callback> callback_;
  // tracking result of registerFn
  std::optional<registration_t> registration_;
  // type-erased registration of a sender waiting for an event
  unifex::atomic_intrusive_queue<pending_operation, &pending_operation::next_>
      pendingOperations_;
  // fixed storage for the fucntion used to emit an event (allows
  // event_function& to have the right lifetime)
  event_function event_function_;

  auto _register() noexcept {
    return detail::_conv{[this]() noexcept {
      return registerFn_(event_function_);
    }};
  }

  void _unregister() {
    if (!!registration_) {
      unregisterFn_(registration_.value());
      registration_.reset();
      stop_pending();
    }
  }

public:
  sender_range(
      RangeStopToken token, RegisterFn registerFn, UnregisterFn unregisterFn)
    : range_(make_range(this))
    , rangeToken_(token)
    , registerFn_(registerFn)
    , unregisterFn_(unregisterFn)
    , callback_(rangeToken_, stop_callback{this})
    , registration_(_register())
    , pendingOperations_()
    , event_function_(this) {}
  sender_range(sender_range&&) = delete;
  ~sender_range() noexcept { _unregister(); }

  auto view() {
    struct sender_view {
      RangeType* range_;

      auto begin() { return range_->begin(); }
      auto end() { return range_->end(); }
    };
    return sender_view{&range_};
  }

  auto& get_registration() { return registration_; }

  auto begin() noexcept { return range_.begin(); }
  auto end() noexcept { return range_.end(); }
};

template <
    typename EventType,
    typename StopToken,
    typename RegisterFn,
    typename UnregisterFn>
sender_range<EventType, StopToken, RegisterFn, UnregisterFn>
create_event_sender_range(
    StopToken token, RegisterFn&& registerFn, UnregisterFn&& unregisterFn) {
  using result_t = sender_range<EventType, StopToken, RegisterFn, UnregisterFn>;
  using registration_t =
      unifex::callable_result_t<RegisterFn, typename result_t::event_function&>;

  static_assert(
      unifex::
          is_nothrow_callable_v<RegisterFn, typename result_t::event_function&>,
      "register function must be noexcept");
  static_assert(
      unifex::is_nothrow_callable_v<UnregisterFn, registration_t&>,
      "unregister function must be noexcept");

  return {token, (RegisterFn &&) registerFn, (UnregisterFn &&) unregisterFn};
}
