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

#include <unifex/inplace_stop_token.hpp>
#include <unifex/just_from.hpp>
#include <unifex/manual_event_loop.hpp>
#include <unifex/scheduler_concepts.hpp>
#include <unifex/sender_concepts.hpp>
#include <unifex/sequence.hpp>

#include "kbdhook/com_thread.hpp"

#include <windows.h>
#include <windowsx.h>
#include <winuser.h>

#include <atomic>

struct clean_stop {
  using scheduler_t = decltype(std::declval<com_thread>().get_scheduler());

  static inline std::atomic<unifex::inplace_stop_source*> stop_{nullptr};

  scheduler_t uiLoop_;
  unifex::inplace_stop_source stopSource_;

  [[nodiscard]] auto start() {
    return unifex::sequence(
        unifex::schedule(uiLoop_), unifex::just_from([this]() {
          if (stop_.exchange(&stopSource_) != nullptr) {
            std::terminate();
          }
          if (!SetConsoleCtrlHandler(&consoleHandler, TRUE)) {
            std::terminate();
          }
        }));
  }
  [[nodiscard]] auto destroy() {
    return unifex::sequence(
        unifex::schedule(uiLoop_), unifex::just_from([this]() {
          if (!SetConsoleCtrlHandler(&consoleHandler, FALSE)) {
            std::terminate();
          }
          if (stop_.exchange(nullptr) == nullptr) {
            std::terminate();
          }
        }));
  }
  struct make_event {
    clean_stop* self_;

    template <
        template <typename...>
        class Variant,
        template <typename...>
        class Tuple>
    using value_types = Variant<Tuple<>>;
    template <template <typename...> class Variant>
    using error_types = Variant<>;
    static inline constexpr bool sends_done = false;
    template <typename Receiver>
    auto operator()(Receiver& rec) noexcept {
      auto exit = [&rec]() noexcept {
        unifex::set_value(rec);
      };
      return unifex::inplace_stop_callback<decltype(exit)>{
          self_->stopSource_.get_token(), exit};
    }
  };
  [[nodiscard]] auto event() { return unifex::create(make_event{this}); }

  static BOOL WINAPI consoleHandler(DWORD signal) {
    if (signal == CTRL_C_EVENT) {
      printf("\n");  // end the line of '.'
      stop_.load()->request_stop();
    }
    return TRUE;
  }

  ~clean_stop() {
    if (stop_.load() != nullptr) {
      // destroy() must be called.
      std::terminate();
    }
  }
  explicit clean_stop(scheduler_t uiLoop) : uiLoop_(uiLoop) {}
};
