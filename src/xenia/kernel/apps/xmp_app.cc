/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2014 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include "xenia/kernel/apps/xmp_app.h"

#include "poly/threading.h"

namespace xe {
namespace kernel {
namespace apps {

XXMPApp::XXMPApp(KernelState* kernel_state)
    : XApp(kernel_state, 0xFA),
      state_(State::kIdle),
      disabled_(0),
      playback_mode_(PlaybackMode::kUnknown),
      repeat_mode_(RepeatMode::kUnknown),
      unknown_flags_(0),
      volume_(0.0f),
      active_playlist_(nullptr),
      active_song_index_(0),
      next_playlist_handle_(1),
      next_song_handle_(1) {}

X_RESULT XXMPApp::XMPGetStatus(uint32_t state_ptr) {
  // Some stupid games will hammer this on a thread - induce a delay
  // here to keep from starving real threads.
  Sleep(1);

  XELOGD("XMPGetStatus(%.8X)", state_ptr);
  poly::store_and_swap<uint32_t>(membase_ + state_ptr,
                                 static_cast<uint32_t>(state_));
  return X_ERROR_SUCCESS;
}

X_RESULT XXMPApp::XMPCreateTitlePlaylist(
    uint32_t songs_ptr, uint32_t song_count, uint32_t playlist_name_ptr,
    std::wstring playlist_name, uint32_t flags, uint32_t out_song_handles,
    uint32_t out_playlist_handle) {
  XELOGD("XMPCreateTitlePlaylist(%.8X, %.8X, %.8X(%s), %.8X, %.8X, %.8X)",
         songs_ptr, song_count, playlist_name_ptr,
         poly::to_string(playlist_name).c_str(), flags, out_song_handles,
         out_playlist_handle);
  auto playlist = std::make_unique<Playlist>();
  playlist->handle = ++next_playlist_handle_;
  playlist->name = std::move(playlist_name);
  playlist->flags = flags;
  for (uint32_t i = 0; i < song_count; ++i) {
    auto song = std::make_unique<Song>();
    song->handle = ++next_song_handle_;
    uint8_t* song_base = membase_ + songs_ptr + (i * 36);
    song->file_path = poly::load_and_swap<std::wstring>(
        membase_ + poly::load_and_swap<uint32_t>(song_base + 0));
    song->name = poly::load_and_swap<std::wstring>(
        membase_ + poly::load_and_swap<uint32_t>(song_base + 4));
    song->artist = poly::load_and_swap<std::wstring>(
        membase_ + poly::load_and_swap<uint32_t>(song_base + 8));
    song->album = poly::load_and_swap<std::wstring>(
        membase_ + poly::load_and_swap<uint32_t>(song_base + 12));
    song->album_artist = poly::load_and_swap<std::wstring>(
        membase_ + poly::load_and_swap<uint32_t>(song_base + 16));
    song->genre = poly::load_and_swap<std::wstring>(
        membase_ + poly::load_and_swap<uint32_t>(song_base + 20));
    song->track_number = poly::load_and_swap<uint32_t>(song_base + 24);
    song->duration_ms = poly::load_and_swap<uint32_t>(song_base + 28);
    song->format = static_cast<Song::Format>(
        poly::load_and_swap<uint32_t>(song_base + 32));
    if (out_song_handles) {
      poly::store_and_swap<uint32_t>(membase_ + out_song_handles + (i * 4),
                                     song->handle);
    }
    playlist->songs.emplace_back(std::move(song));
  }
  poly::store_and_swap<uint32_t>(membase_ + out_playlist_handle,
                                 playlist->handle);

  std::lock_guard<std::mutex> lock(mutex_);
  playlists_.insert({playlist->handle, playlist.get()});
  playlist.release();
  return X_ERROR_SUCCESS;
}

X_RESULT XXMPApp::XMPDeleteTitlePlaylist(uint32_t playlist_handle) {
  XELOGD("XMPDeleteTitlePlaylist(%.8X)", playlist_handle);
  std::lock_guard<std::mutex> lock(mutex_);
  auto it = playlists_.find(playlist_handle);
  if (it == playlists_.end()) {
    XELOGE("Playlist %.8X not found", playlist_handle);
    return X_ERROR_NOT_FOUND;
  }
  auto playlist = it->second;
  if (playlist == active_playlist_) {
    XMPStop(0);
  }
  playlists_.erase(it);
  delete playlist;
  return X_ERROR_SUCCESS;
}

X_RESULT XXMPApp::XMPPlayTitlePlaylist(uint32_t playlist_handle,
                                       uint32_t song_handle) {
  XELOGD("XMPPlayTitlePlaylist(%.8X, %.8X)", playlist_handle, song_handle);
  Playlist* playlist = nullptr;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = playlists_.find(playlist_handle);
    if (it == playlists_.end()) {
      XELOGE("Playlist %.8X not found", playlist_handle);
      return X_ERROR_NOT_FOUND;
    }
    playlist = it->second;
  }

  if (disabled_) {
    // Ignored because we aren't enabled?
    XELOGW("Ignoring XMPPlayTitlePlaylist because disabled");
    return X_ERROR_SUCCESS;
  }

  // Start playlist?
  XELOGW("Playlist playback not supported");
  active_playlist_ = playlist;
  active_song_index_ = 0;
  state_ = State::kPlaying;
  OnStateChanged();
  return X_ERROR_SUCCESS;
}

X_RESULT XXMPApp::XMPContinue() {
  XELOGD("XMPContinue()");
  if (state_ == State::kPaused) {
    state_ = State::kPlaying;
  }
  OnStateChanged();
  return X_ERROR_SUCCESS;
}

X_RESULT XXMPApp::XMPStop(uint32_t unk) {
  assert_zero(unk);
  XELOGD("XMPStop(%.8X)", unk);
  active_playlist_ = nullptr;  // ?
  active_song_index_ = 0;
  state_ = State::kIdle;
  OnStateChanged();
  return X_ERROR_SUCCESS;
}

X_RESULT XXMPApp::XMPPause() {
  XELOGD("XMPPause()");
  if (state_ == State::kPlaying) {
    state_ = State::kPaused;
  }
  OnStateChanged();
  return X_ERROR_SUCCESS;
}

X_RESULT XXMPApp::XMPNext() {
  XELOGD("XMPNext()");
  if (!active_playlist_) {
    return X_ERROR_NOT_FOUND;
  }
  state_ = State::kPlaying;
  active_song_index_ =
      (active_song_index_ + 1) % active_playlist_->songs.size();
  OnStateChanged();
  return X_ERROR_SUCCESS;
}

X_RESULT XXMPApp::XMPPrevious() {
  XELOGD("XMPPrevious()");
  if (!active_playlist_) {
    return X_ERROR_NOT_FOUND;
  }
  state_ = State::kPlaying;
  if (!active_song_index_) {
    active_song_index_ = static_cast<int>(active_playlist_->songs.size()) - 1;
  } else {
    --active_song_index_;
  }
  OnStateChanged();
  return X_ERROR_SUCCESS;
}

void XXMPApp::OnStateChanged() {
  kernel_state_->BroadcastNotification(kMsgStateChanged,
                                       static_cast<uint32_t>(state_));
}

X_RESULT XXMPApp::DispatchMessageSync(uint32_t message, uint32_t buffer_ptr,
                                      uint32_t buffer_length) {
  // NOTE: buffer_length may be zero or valid.
  switch (message) {
    case 0x00070002: {
      assert_true(!buffer_length || buffer_length == 12);
      uint32_t xmp_client =
          poly::load_and_swap<uint32_t>(membase_ + buffer_ptr + 0);
      uint32_t playlist_handle =
          poly::load_and_swap<uint32_t>(membase_ + buffer_ptr + 4);
      uint32_t song_handle =
          poly::load_and_swap<uint32_t>(membase_ + buffer_ptr + 8);  // 0?
      assert_true(xmp_client == 0x00000002);
      return XMPPlayTitlePlaylist(playlist_handle, song_handle);
    }
    case 0x00070003: {
      assert_true(!buffer_length || buffer_length == 4);
      uint32_t xmp_client =
          poly::load_and_swap<uint32_t>(membase_ + buffer_ptr + 0);
      assert_true(xmp_client == 0x00000002);
      return XMPContinue();
    }
    case 0x00070004: {
      assert_true(!buffer_length || buffer_length == 8);
      uint32_t xmp_client =
          poly::load_and_swap<uint32_t>(membase_ + buffer_ptr + 0);
      uint32_t unk = poly::load_and_swap<uint32_t>(membase_ + buffer_ptr + 4);
      assert_true(xmp_client == 0x00000002);
      return XMPStop(unk);
    }
    case 0x00070005: {
      assert_true(!buffer_length || buffer_length == 4);
      uint32_t xmp_client =
          poly::load_and_swap<uint32_t>(membase_ + buffer_ptr + 0);
      assert_true(xmp_client == 0x00000002);
      return XMPPause();
    }
    case 0x00070006: {
      assert_true(!buffer_length || buffer_length == 4);
      uint32_t xmp_client =
          poly::load_and_swap<uint32_t>(membase_ + buffer_ptr + 0);
      assert_true(xmp_client == 0x00000002);
      return XMPNext();
    }
    case 0x00070007: {
      assert_true(!buffer_length || buffer_length == 4);
      uint32_t xmp_client =
          poly::load_and_swap<uint32_t>(membase_ + buffer_ptr + 0);
      assert_true(xmp_client == 0x00000002);
      return XMPPrevious();
    }
    case 0x00070008: {
      assert_true(!buffer_length || buffer_length == 16);
      uint32_t xmp_client =
          poly::load_and_swap<uint32_t>(membase_ + buffer_ptr + 0);
      uint32_t playback_mode =
          poly::load_and_swap<uint32_t>(membase_ + buffer_ptr + 4);
      uint32_t repeat_mode =
          poly::load_and_swap<uint32_t>(membase_ + buffer_ptr + 8);
      uint32_t flags =
          poly::load_and_swap<uint32_t>(membase_ + buffer_ptr + 12);
      assert_true(xmp_client == 0x00000002);
      XELOGD("XMPSetPlaybackBehavior(%.8X, %.8X, %.8X)", playback_mode,
             repeat_mode, flags);
      playback_mode_ = static_cast<PlaybackMode>(playback_mode);
      repeat_mode_ = static_cast<RepeatMode>(repeat_mode);
      unknown_flags_ = flags;
      kernel_state_->BroadcastNotification(kMsgPlaybackBehaviorChanged, 0);
      return X_ERROR_SUCCESS;
    }
    case 0x00070009: {
      assert_true(!buffer_length || buffer_length == 8);
      uint32_t xmp_client =
          poly::load_and_swap<uint32_t>(membase_ + buffer_ptr + 0);
      uint32_t state_ptr = poly::load_and_swap<uint32_t>(
          membase_ + buffer_ptr + 4);  // out ptr to 4b - expect 0
      assert_true(xmp_client == 0x00000002);
      return XMPGetStatus(state_ptr);
    }
    case 0x0007000B: {
      assert_true(!buffer_length || buffer_length == 8);
      uint32_t xmp_client =
          poly::load_and_swap<uint32_t>(membase_ + buffer_ptr + 0);
      uint32_t float_ptr = poly::load_and_swap<uint32_t>(
          membase_ + buffer_ptr + 4);  // out ptr to 4b - floating point
      assert_true(xmp_client == 0x00000002);
      XELOGD("XMPGetVolume(%.8X)", float_ptr);
      poly::store_and_swap<float>(membase_ + float_ptr, volume_);
      return X_ERROR_SUCCESS;
    }
    case 0x0007000C: {
      assert_true(!buffer_length || buffer_length == 8);
      uint32_t xmp_client =
          poly::load_and_swap<uint32_t>(membase_ + buffer_ptr + 0);
      float float_value = poly::load_and_swap<float>(membase_ + buffer_ptr + 4);
      assert_true(xmp_client == 0x00000002);
      XELOGD("XMPSetVolume(%g)", float_value);
      volume_ = float_value;
      return X_ERROR_SUCCESS;
    }
    case 0x0007000D: {
      assert_true(!buffer_length || buffer_length == 36);
      uint32_t xmp_client =
          poly::load_and_swap<uint32_t>(membase_ + buffer_ptr + 0);
      uint32_t dummy_alloc_ptr =
          poly::load_and_swap<uint32_t>(membase_ + buffer_ptr + 4);
      uint32_t dummy_alloc_size =
          poly::load_and_swap<uint32_t>(membase_ + buffer_ptr + 8);
      uint32_t songs_ptr =
          poly::load_and_swap<uint32_t>(membase_ + buffer_ptr + 12);
      uint32_t song_count =
          poly::load_and_swap<uint32_t>(membase_ + buffer_ptr + 16);
      uint32_t playlist_name_ptr =
          poly::load_and_swap<uint32_t>(membase_ + buffer_ptr + 20);
      uint32_t flags =
          poly::load_and_swap<uint32_t>(membase_ + buffer_ptr + 24);
      uint32_t song_handles_ptr =
          poly::load_and_swap<uint32_t>(membase_ + buffer_ptr + 28);  // 0?
      uint32_t playlist_handle_ptr =
          poly::load_and_swap<uint32_t>(membase_ + buffer_ptr + 32);
      assert_true(xmp_client == 0x00000002);
      auto playlist_name =
          poly::load_and_swap<std::wstring>(membase_ + playlist_name_ptr);
      // dummy_alloc_ptr is the result of a XamAlloc of dummy_alloc_size.
      assert_true(dummy_alloc_size == song_count * 128);
      return XMPCreateTitlePlaylist(songs_ptr, song_count, playlist_name_ptr,
                                    playlist_name, flags, song_handles_ptr,
                                    playlist_handle_ptr);
    }
    case 0x0007000E: {
      assert_true(!buffer_length || buffer_length == 12);
      uint32_t xmp_client =
          poly::load_and_swap<uint32_t>(membase_ + buffer_ptr + 0);
      uint32_t unk_ptr =
          poly::load_and_swap<uint32_t>(membase_ + buffer_ptr + 4);  // 0
      uint32_t info_ptr =
          poly::load_and_swap<uint32_t>(membase_ + buffer_ptr + 8);
      assert_true(xmp_client == 0x00000002);
      assert_zero(unk_ptr);
      XELOGE("XMPGetInfo?(%.8X, %.8X)", unk_ptr, info_ptr);
      if (!active_playlist_) {
        return X_ERROR_NOT_FOUND;
      }
      auto& song = active_playlist_->songs[active_song_index_];
      poly::store_and_swap<uint32_t>(membase_ + info_ptr + 0, song->handle);
      poly::store_and_swap<std::wstring>(membase_ + info_ptr + 4 + 572 + 0,
                                         song->name);
      poly::store_and_swap<std::wstring>(membase_ + info_ptr + 4 + 572 + 40,
                                         song->artist);
      poly::store_and_swap<std::wstring>(membase_ + info_ptr + 4 + 572 + 80,
                                         song->album);
      poly::store_and_swap<std::wstring>(membase_ + info_ptr + 4 + 572 + 120,
                                         song->album_artist);
      poly::store_and_swap<std::wstring>(membase_ + info_ptr + 4 + 572 + 160,
                                         song->genre);
      poly::store_and_swap<uint32_t>(membase_ + info_ptr + 4 + 572 + 200,
                                     song->track_number);
      poly::store_and_swap<uint32_t>(membase_ + info_ptr + 4 + 572 + 204,
                                     song->duration_ms);
      poly::store_and_swap<uint32_t>(membase_ + info_ptr + 4 + 572 + 208,
                                     static_cast<uint32_t>(song->format));
      return X_ERROR_SUCCESS;
    }
    case 0x00070013: {
      assert_true(!buffer_length || buffer_length == 8);
      uint32_t xmp_client =
          poly::load_and_swap<uint32_t>(membase_ + buffer_ptr + 0);
      uint32_t playlist_handle =
          poly::load_and_swap<uint32_t>(membase_ + buffer_ptr + 4);
      assert_true(xmp_client == 0x00000002);
      return XMPDeleteTitlePlaylist(playlist_handle);
    }
    case 0x0007001A: {
      assert_true(!buffer_length || buffer_length == 12);
      uint32_t xmp_client =
          poly::load_and_swap<uint32_t>(membase_ + buffer_ptr + 0);
      uint32_t unk1 = poly::load_and_swap<uint32_t>(membase_ + buffer_ptr + 4);
      uint32_t enabled =
          poly::load_and_swap<uint32_t>(membase_ + buffer_ptr + 8);
      assert_true(xmp_client == 0x00000002);
      assert_zero(unk1);
      XELOGD("XMPSetEnabled(%.8X, %.8X)", unk1, enabled);
      disabled_ = enabled;
      if (disabled_) {
        XMPStop(0);
      }
      kernel_state_->BroadcastNotification(kMsgDisableChanged, disabled_);
      return X_ERROR_SUCCESS;
    }
    case 0x0007001B: {
      assert_true(!buffer_length || buffer_length == 12);
      uint32_t xmp_client =
          poly::load_and_swap<uint32_t>(membase_ + buffer_ptr + 0);
      uint32_t unk_ptr = poly::load_and_swap<uint32_t>(
          membase_ + buffer_ptr + 4);  // out ptr to 4b - expect 0
      uint32_t disabled_ptr = poly::load_and_swap<uint32_t>(
          membase_ + buffer_ptr + 8);  // out ptr to 4b - expect 1 (to skip)
      assert_true(xmp_client == 0x00000002);
      XELOGD("XMPGetEnabled(%.8X, %.8X)", unk_ptr, disabled_ptr);
      poly::store_and_swap<uint32_t>(membase_ + unk_ptr, 0);
      poly::store_and_swap<uint32_t>(membase_ + disabled_ptr, disabled_);
      // Atrain spawns a thread 82437FD0 to call this in a tight loop forever.
      poly::threading::Sleep(std::chrono::milliseconds(10));
      return X_ERROR_SUCCESS;
    }
    case 0x00070029: {
      assert_true(!buffer_length || buffer_length == 16);
      uint32_t xmp_client =
          poly::load_and_swap<uint32_t>(membase_ + buffer_ptr + 0);
      uint32_t playback_mode_ptr =
          poly::load_and_swap<uint32_t>(membase_ + buffer_ptr + 4);
      uint32_t repeat_mode_ptr =
          poly::load_and_swap<uint32_t>(membase_ + buffer_ptr + 8);
      uint32_t unk3_ptr =
          poly::load_and_swap<uint32_t>(membase_ + buffer_ptr + 12);
      assert_true(xmp_client == 0x00000002);
      XELOGD("XMPGetPlaybackBehavior(%.8X, %.8X, %.8X)", playback_mode_ptr,
             repeat_mode_ptr, unk3_ptr);
      poly::store_and_swap<uint32_t>(membase_ + playback_mode_ptr,
                                     static_cast<uint32_t>(playback_mode_));
      poly::store_and_swap<uint32_t>(membase_ + repeat_mode_ptr,
                                     static_cast<uint32_t>(repeat_mode_));
      poly::store_and_swap<uint32_t>(membase_ + unk3_ptr, unknown_flags_);
      return X_ERROR_SUCCESS;
    }
    case 0x0007002E: {
      assert_true(!buffer_length || buffer_length == 12);
      // Query of size for XamAlloc - the result of the alloc is passed to
      // 0x0007000D.
      uint32_t xmp_client =
          poly::load_and_swap<uint32_t>(membase_ + buffer_ptr + 0);
      uint32_t song_count =
          poly::load_and_swap<uint32_t>(membase_ + buffer_ptr + 4);
      uint32_t size_ptr =
          poly::load_and_swap<uint32_t>(membase_ + buffer_ptr + 8);
      assert_true(xmp_client == 0x00000002);
      // We don't use the storage, so just fudge the number.
      poly::store_and_swap<uint32_t>(membase_ + size_ptr, song_count * 128);
      return X_ERROR_SUCCESS;
    }
    case 0x0007003D: {
      // XMPCaptureOutput - not sure how this works :/
      XELOGD("XMPCaptureOutput(...)");
      assert_always("XMP output not unimplemented");
      return X_ERROR_INVALID_PARAMETER;
    }
  }
  XELOGE("Unimplemented XMsg message app=%.8X, msg=%.8X, arg1=%.8X, arg2=%.8X",
         app_id(), message, buffer_ptr, buffer_length);
  return X_ERROR_NOT_FOUND;
}

}  // namespace apps
}  // namespace kernel
}  // namespace xe
