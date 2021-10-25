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

#include <unifex/just.hpp>
#include <unifex/let_done.hpp>
#include <unifex/manual_event_loop.hpp>
#include <unifex/repeat_effect_until.hpp>
#include <unifex/scheduler_concepts.hpp>
#include <unifex/sender_concepts.hpp>
#include <unifex/stop_when.hpp>
#include <unifex/sync_wait.hpp>
#include <unifex/timed_single_thread_context.hpp>

#include <chrono>
#include <thread>

#include <windows.h>
#include <windowsx.h>
#include <winuser.h>

struct com_thread {
  using run_scheduler_t =
      decltype(std::declval<unifex::manual_event_loop&>().get_scheduler());
  using time_scheduler_t =
      decltype(std::declval<unifex::timed_single_thread_context>()
                   .get_scheduler());
  using duration_t =
      typename unifex::timed_single_thread_context::clock_t::duration;
  duration_t maxTime_;
  unifex::timed_single_thread_context time_;
  unifex::manual_event_loop run_;
  std::thread comThread_;
  ~com_thread() { join(); }
  com_thread() = delete;
  explicit com_thread(duration_t maxTime)
    : maxTime_(maxTime)
    , comThread_([this]() noexcept {
      {  // create message queue
        MSG msg;
        PeekMessage(&msg, NULL, WM_USER, WM_USER, PM_NOREMOVE);
      }
      printf("com thread start\n");
      fflush(stdout);

      if (FAILED(CoInitializeEx(
              nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE))) {
        std::terminate();
      }

      unifex::scope_guard exit{[this]() noexcept {
        run_.stop();
        run_.run();  // run until empty

        CoUninitialize();

        printf("com thread exit\n");
        fflush(stdout);
      }};

      BOOL pendingMessages = FALSE;
      MSG msg = {};
      while ((pendingMessages = GetMessage(&msg, NULL, 0, 0)) != 0) {
        if (pendingMessages == -1) {
          std::terminate();
        }
        TranslateMessage(&msg);
        DispatchMessage(&msg);
        unifex::sync_wait(
            run_.run_as_sender() |
            unifex::stop_when(
                unifex::schedule_after(time_.get_scheduler(), maxTime_)));
      }
    }) {}
  struct make_sender {
    using sender_t =
        decltype(unifex::schedule(std::declval<run_scheduler_t&>()));
    com_thread* self_;
    explicit make_sender(com_thread* self) : self_(self) {}
    template <
        template <typename...>
        class Variant,
        template <typename...>
        class Tuple>
    using value_types = unifex::sender_value_types_t<sender_t, Variant, Tuple>;
    template <template <typename...> class Variant>
    using error_types = unifex::sender_error_types_t<sender_t, Variant>;
    static inline constexpr bool sends_done = sender_t::sends_done;

    template <typename Receiver>
    auto operator()(Receiver& rec) noexcept {
      struct state {
        using op_t = unifex::connect_result_t<sender_t, Receiver&>;
        com_thread* self_;
        op_t op_;
        state(com_thread* self, sender_t sender, Receiver rec)
          : self_(self)
          , op_(unifex::connect(std::move(sender), rec)) {
          unifex::start(op_);
          // wake up the message loop
          while (self_->comThread_.joinable() &&
                 !PostThreadMessageW(
                     GetThreadId(self_->comThread_.native_handle()),
                     WM_USER,
                     0,
                     0L)) {
          }
        }
        state() = delete;
        state(const state&) = delete;
        state(state&&) = delete;
      };

      return state{self_, unifex::schedule(self_->run_.get_scheduler()), rec};
    }
  };
  struct _scheduler {
    com_thread* self_;
    _scheduler() = delete;
    explicit _scheduler(com_thread* self) : self_(self) {}
    _scheduler(const _scheduler&) = default;
    _scheduler(_scheduler&&) = default;

    auto schedule() {
      // return unifex::schedule(self_->run_.get_scheduler());
      return unifex::create(make_sender{self_});
    }

    friend bool operator==(_scheduler a, _scheduler b) noexcept {
      return a.self_->run_.get_scheduler() == b.self_->run_.get_scheduler();
    }
    friend bool operator!=(_scheduler a, _scheduler b) noexcept {
      return a.self_->run_.get_scheduler() != b.self_->run_.get_scheduler();
    }
  };
  _scheduler get_scheduler() { return _scheduler{this}; }

  void join() {
    if (comThread_.joinable()) {
      if (!PostThreadMessageW(
              GetThreadId(comThread_.native_handle()), WM_QUIT, 0, 0L)) {
        std::terminate();
      }
      try {
        // there is a race in the windows thread implementation :(
        std::this_thread::sleep_for(std::chrono::milliseconds(500));
        if (comThread_.joinable()) {
          comThread_.join();
        }
      } catch (...) {
      }
    }
  }
};