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

#include <unifex/just_from.hpp>
#include <unifex/manual_event_loop.hpp>
#include <unifex/scheduler_concepts.hpp>
#include <unifex/sender_concepts.hpp>
#include <unifex/sequence.hpp>

#include "kbdhook/sender_range.hpp"
#include "kbdhook/com_thread.hpp"

#include <windows.h>
#include <windowsx.h>
#include <winuser.h>

#include <atomic>

template <typename Fn>
struct _keyboard_hook {
  using scheduler_t = decltype(std::declval<com_thread>().get_scheduler());

  Fn& fn_;
  scheduler_t uiLoop_;
  unifex::inplace_stop_token token_;
  HHOOK hHook_;

  static inline std::atomic<_keyboard_hook*> self_{nullptr};

  ~_keyboard_hook() {
    if (!!hHook_) {
      // must call destroy()
      std::terminate();
    }
  }
  explicit _keyboard_hook(Fn& fn, scheduler_t uiLoop)
    : fn_(fn)
    , uiLoop_(uiLoop)
    , hHook_(NULL) {}

  [[nodiscard]] auto start() {
    return unifex::sequence(
        unifex::schedule(uiLoop_), unifex::just_from([this]() noexcept {
          _keyboard_hook* empty = nullptr;
          if (!self_.compare_exchange_strong(empty, this)) {
            std::terminate();
          }

          hHook_ = SetWindowsHookExW(WH_KEYBOARD_LL, &KbdHookProc, NULL, NULL);
          if (!hHook_) {
            LPCWSTR message = nullptr;
            FormatMessageW(
                FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM |
                    FORMAT_MESSAGE_IGNORE_INSERTS,
                NULL,
                GetLastError(),
                0,
                (LPWSTR)&message,
                128,
                nullptr);

            printf("failed to set keyboard hook\n");
            printf("Error: %S\n", message);
            LocalFree((HLOCAL)message);
            std::terminate();
          }
          printf("keyboard hook set\n");
        }));
  }

  [[nodiscard]] auto destroy() {
    return unifex::sequence(
        unifex::schedule(uiLoop_), unifex::just_from([this]() noexcept {
          bool result = UnhookWindowsHookEx(std::exchange(hHook_, (HHOOK)NULL));
          if (!result) {
            std::terminate();
          }

          _keyboard_hook* expired = this;
          if (!self_.compare_exchange_strong(expired, nullptr)) {
            std::terminate();
          }

          printf("keyboard hook removed\n");
        }));
  }

  static LRESULT CALLBACK
  KbdHookProc(_In_ int nCode, _In_ WPARAM wParam, _In_ LPARAM lParam) {
    _keyboard_hook* self = self_.load();
    if (!!self && nCode >= 0 &&
        (wParam == WM_KEYDOWN || wParam == WM_SYSKEYDOWN)) {
      self->fn_(wParam);
      return CallNextHookEx(self->hHook_, nCode, wParam, lParam);
    }
    return CallNextHookEx(NULL, nCode, wParam, lParam);
  }
};

namespace detail {
// create a range of senders where each sender completes on the next
// keyboard press
template <typename Scheduler>
auto keyboard_events(Scheduler uiLoop) {
  static auto register_ = [uiLoop](auto& fn) noexcept {
    return _keyboard_hook<decltype(fn)>{fn, uiLoop};
  };
  static auto unregister_ = [](auto& r) noexcept {
    // caller is responsible for destroy()
  };
  return std::make_pair(register_, unregister_);
}
}  // namespace detail
class keyboard_hook {
  using scheduler_t =
      decltype(std::declval<com_thread>().get_scheduler());
  using fns = decltype(detail::keyboard_events(std::declval<scheduler_t&>()));
  using RangeType = sender_range<
      WPARAM,
      unifex::inplace_stop_token,
      typename fns::first_type,
      typename fns::second_type>;

  unifex::inplace_stop_source stopSource_;
  RangeType range_;

public:
  explicit keyboard_hook(scheduler_t uiLoop)
    : range_(
          stopSource_.get_token(),
          detail::keyboard_events(uiLoop).first,
          detail::keyboard_events(uiLoop).second) {}

  unifex::inplace_stop_source& get_stop_source() { return stopSource_; }
  void request_stop() { stopSource_.request_stop(); }

  [[nodiscard]] auto start() { return range_.get_registration()->start(); }
  [[nodiscard]] auto destroy() { return range_.get_registration()->destroy(); }

  auto events() { return range_.view(); }
};
