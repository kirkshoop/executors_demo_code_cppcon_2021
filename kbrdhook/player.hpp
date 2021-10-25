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

#include <unifex/async_manual_reset_event.hpp>
#include <unifex/async_scope.hpp>
#include <unifex/just_from.hpp>
#include <unifex/manual_event_loop.hpp>
#include <unifex/scheduler_concepts.hpp>
#include <unifex/scope_guard.hpp>
#include <unifex/sender_concepts.hpp>
#include <unifex/sequence.hpp>

#include "kbdhook/com_thread.hpp"

#include <mfplay.h>
#include <windows.h>
#include <windowsx.h>
#include <winuser.h>
#pragma comment(lib, "mfplay.lib")
#include <Shlwapi.h>
#include <mferror.h>
#include <shobjidl.h>  // defines IFileOpenDialog
#pragma comment(lib, "shlwapi.lib")
#include <strsafe.h>

struct Player {
  class MediaPlayerCallback : public IMFPMediaPlayerCallback {
    size_t id_;
    Player* player_;
    long m_cRef;  // Reference count

  public:
    explicit MediaPlayerCallback(Player* player, size_t id)
      : id_(id)
      , player_(player)
      , m_cRef(1) {}

    STDMETHODIMP QueryInterface(REFIID riid, void** ppv) {
      static const QITAB qit[] = {
          QITABENT(MediaPlayerCallback, IMFPMediaPlayerCallback),
          {0},
      };
      return QISearch(this, qit, riid, ppv);
    }
    STDMETHODIMP_(ULONG) AddRef() { return InterlockedIncrement(&m_cRef); }
    STDMETHODIMP_(ULONG) Release() {
      ULONG count = InterlockedDecrement(&m_cRef);
      if (count == 0) {
        delete this;
        return 0;
      }
      return count;
    }

    // IMFPMediaPlayerCallback methods
    void STDMETHODCALLTYPE OnMediaPlayerEvent(MFP_EVENT_HEADER* pEventHeader) {
      if (FAILED(pEventHeader->hrEvent)) {
        player_->ShowErrorMessage(L"Playback error", pEventHeader->hrEvent);
        return;
      }

      switch (pEventHeader->eEventType) {
        case MFP_EVENT_TYPE_MEDIAITEM_CREATED: {
          auto* pEvent = MFP_GET_MEDIAITEM_CREATED_EVENT(pEventHeader);
          auto pMediaItem = pEvent->pMediaItem;
          // The media item was created successfully.
          player_->players_[id_].ItemCreated(player_, pMediaItem);
        } break;

        case MFP_EVENT_TYPE_MEDIAITEM_SET:
          // set completed
          player_->players_[id_].ItemSet(player_);
          break;
      }
    }
  };

  struct player {
    ~player() { destroy(); }
    player() : id_(-1), pCallback_(nullptr), pPlayer_(nullptr) {}
    void start(Player* player, size_t id) {
      HRESULT hr = S_OK;

      id_ = id;
      pCallback_ = new (std::nothrow) MediaPlayerCallback{player, id_};

      hr = MFPCreateMediaPlayer(
          NULL,
          FALSE,       // Start playback automatically?
          0,           // Flags
          pCallback_,  // Callback pointer
          NULL,        // Video window
          &pPlayer_);
      if (FAILED(hr)) {
        std::terminate();
      }

      // Create a new media item for this URL.
      hr = pPlayer_->CreateMediaItemFromURL(
          L"https://webwit.nl/input/kbsim/mp3/1_.mp3", FALSE, 0, NULL);
      if (FAILED(hr)) {
        std::terminate();
      }
      printf("player started\n");
      fflush(stdout);
    }

    void destroy() {
      if (!!pPlayer_) {
        std::exchange(pPlayer_, nullptr)->Release();
      }
      if (!!pCallback_) {
        std::exchange(pCallback_, nullptr)->Release();
        printf("player exit\n");
        fflush(stdout);
      }
    }

    void Click() {
      HRESULT hr = pPlayer_->Stop();
      if (FAILED(hr)) {
        std::terminate();
      }
      hr = pPlayer_->Play();
      if (FAILED(hr)) {
        std::terminate();
      }
    }

    void ItemCreated(Player* player, IMFPMediaItem* pMediaItem) {
      HRESULT hr = S_OK;

      // The media item was created successfully.

      if (pPlayer_) {
        // Set the media item on the player. This method completes
        // asynchronously.
        hr = pPlayer_->SetMediaItem(pMediaItem);
      }

      if (FAILED(hr)) {
        player->ShowErrorMessage(L"Error playing this file.", hr);
      }
    }

    void ItemSet(Player* player) {
      if (++player->ready_ == player->players_.size()) {
        player->playersReady_.set();
      }
    }

    size_t id_;
    IMFPMediaPlayerCallback* pCallback_;  // Application callback object.
    IMFPMediaPlayer* pPlayer_;            // The MFPlay player object.
  };
  using scheduler_t = decltype(std::declval<com_thread>().get_scheduler());

  scheduler_t uiLoop_;
  std::array<player, 1> players_;
  size_t current_;
  unifex::async_scope scope_;
  size_t ready_;
  unifex::async_manual_reset_event playersReady_;

  explicit Player(scheduler_t uiLoop)
    : uiLoop_(uiLoop)
    , current_(0)
    , ready_(0) {}

  auto start() {
    return unifex::sequence(
        unifex::schedule(uiLoop_),
        unifex::just_from([this]() {
          for (auto& p : players_) {
            p.start(this, current_++ % players_.size());
          }
        }),
        playersReady_.async_wait());
  }

  [[nodiscard]] auto destroy() {
    return unifex::sequence(
        unifex::schedule(uiLoop_),
        unifex::just_from([this]() {
          for (auto& p : players_) {
            p.destroy();
          }
        }),
        scope_.complete());
  }

  void Click() {
    scope_.spawn_call_on(uiLoop_, [this]() noexcept {
      players_[++current_ % players_.size()].Click();
    });
  }

  void ShowErrorMessage(PCWSTR format, HRESULT hrErr) {
    scope_.spawn_call_on(uiLoop_, [this, format, hrErr]() noexcept {
      HRESULT hr = S_OK;
      WCHAR str[MAX_PATH];

      hr = StringCbPrintfW(str, sizeof(str), L"%s (hr=0x%X)", format, hrErr);

      if (SUCCEEDED(hr)) {
        MessageBoxW(NULL, str, L"Error", MB_ICONERROR);
      }
    });
  }
};