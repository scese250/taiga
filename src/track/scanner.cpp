/*
** Taiga
** Copyright (C) 2010-2021, Eren Okka
**
** This program is free software: you can redistribute it and/or modify
** it under the terms of the GNU General Public License as published by
** the Free Software Foundation, either version 3 of the License, or
** (at your option) any later version.
**
** This program is distributed in the hope that it will be useful,
** but WITHOUT ANY WARRANTY; without even the implied warranty of
** MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
** GNU General Public License for more details.
**
** You should have received a copy of the GNU General Public License
** along with this program.  If not, see <http://www.gnu.org/licenses/>.
*/

#include "track/scanner.h"

#include <atomic>
#include <mutex>
#include <thread>

#include "base/file.h"
#include "base/log.h"
#include "base/string.h"
#include "media/anime_db.h"
#include "media/anime_util.h"
#include "taiga/settings.h"
#include "track/episode_util.h"
#include "track/recognition.h"
#include "ui/dlg/dlg_main.h"
#include "ui/ui.h"

namespace track {

bool Scanner::OnDirectory(const base::FileSearchResult& result) {
  static track::recognition::ParseOptions parse_options;
  parse_options.parse_path = false;
  parse_options.streaming_media = false;

  if (!Meow.Parse(result.name, parse_options, episode_)) {
    LOGD(L"Could not parse directory: {}", result.name);
    return false;
  }

  static track::recognition::MatchOptions match_options;
  match_options.allow_sequels = false;
  match_options.check_airing_date = false;
  match_options.check_anime_type = false;
  match_options.check_episode_number = false;
  match_options.streaming_media = false;

  Meow.Identify(episode_, false, match_options);

  const auto anime_item = anime::db.Find(episode_.anime_id);

  if (anime_item && Meow.IsValidAnimeType(episode_)) {
    if (anime_item->GetFolder().empty()) {
      const auto folder = AddTrailingSlash(result.root) + result.name;
      if (collect_results_) {
        folder_results_.push_back({anime_item->GetId(), folder});
      } else {
        anime_item->SetFolder(folder);
      }
    }

    if (anime_id_ && anime_id_.value() == anime_item->GetId()) {
      path_found_ = AddTrailingSlash(result.root) + result.name;
      if (options.skip_files)
        return true;
    }
  }

  return false;
}

bool Scanner::OnFile(const base::FileSearchResult& result) {
  const auto path = AddTrailingSlash(result.root) + result.name;

  static track::recognition::ParseOptions parse_options;
  parse_options.parse_path = true;
  parse_options.streaming_media = false;

  if (!Meow.Parse(path, parse_options, episode_)) {
    LOGD(L"Could not parse filename: {}", result.name);
    return false;
  }

  static track::recognition::MatchOptions match_options;
  match_options.allow_sequels = true;
  match_options.check_airing_date = true;
  match_options.check_anime_type = true;
  match_options.check_episode_number = true;
  match_options.streaming_media = false;

  Meow.Identify(episode_, false, match_options);

  const auto anime_item = anime::db.Find(episode_.anime_id);

  if (anime_item && Meow.IsValidAnimeType(episode_) &&
      Meow.IsValidFileExtension(episode_)) {
    const int upper_bound = anime::GetEpisodeHigh(episode_);
    const int lower_bound = anime::GetEpisodeLow(episode_);

    if (!anime::IsValidEpisodeNumber(upper_bound,
                                     anime_item->GetEpisodeCount()) ||
        !anime::IsValidEpisodeNumber(lower_bound,
                                     anime_item->GetEpisodeCount())) {
      const auto episode_number = anime::GetEpisodeRange(episode_);
      LOGD(L"Invalid episode number: {}\nFile: {}", episode_number, path);
      return false;
    }

    for (int i = lower_bound; i <= upper_bound; ++i) {
      if (collect_results_) {
        episode_results_.push_back({anime_item->GetId(), i, path});
      } else {
        anime_item->SetEpisodeAvailability(i, true, path);
      }
    }

    if (anime_id_ && anime_id_.value() == anime_item->GetId()) {
      // Check if we've found the episode we were looking for
      if (episode_number_ > 0 && episode_number_ >= lower_bound &&
          episode_number_ <= upper_bound) {
        path_found_ = path;
        return true;
      }
      // Check if all episodes are available
      if (episode_number_ == 0 && IsAllEpisodesAvailable(*anime_item)) {
        return true;
      }
    }
  }

  return false;
}

bool Scanner::Search(const std::wstring& root) {
  return base::FileSearch::Search(root,
      [this](const base::FileSearchResult& result) {
        return OnDirectory(result);
      },
      [this](const base::FileSearchResult& result) {
        return OnFile(result);
      }
  );
}

const std::wstring& Scanner::path_found() const {
  return path_found_;
}

void Scanner::set_anime_id(int anime_id) {
  if (anime::IsValidId(anime_id)) {
    anime_id_ = anime_id;
  } else {
    anime_id_.reset();
  }
}

void Scanner::set_episode_number(int episode_number) {
  episode_number_ = episode_number;
}

void Scanner::set_path_found(const std::wstring& path_found) {
  path_found_ = path_found;
}

void Scanner::set_collect_results(bool collect) {
  collect_results_ = collect;
}

std::vector<Scanner::EpisodeResult> Scanner::take_episode_results() {
  return std::move(episode_results_);
}

std::vector<Scanner::FolderResult> Scanner::take_folder_results() {
  return std::move(folder_results_);
}

}  // namespace track

////////////////////////////////////////////////////////////////////////////////

void ScanAvailableEpisodes(bool silent) {
  for (auto& [id, item] : anime::db.items) {
    anime::ValidateFolder(item);
  }

  ScanAvailableEpisodes(silent, anime::ID_UNKNOWN, 0);
}

void ScanAvailableEpisodes(bool silent, int anime_id, int episode_number) {
  using track::scanner;

  const auto library_folders = taiga::settings.GetLibraryFolders();

  // Check if any library folder is available
  if (!silent && library_folders.empty()) {
    ui::OnSettingsLibraryFoldersEmpty();
    return;
  }

  if (!silent) {
    ui::taskbar_list.SetProgressState(TBPF_INDETERMINATE);
    ui::SetSharedCursor(IDC_WAIT);
    ui::ChangeStatusText(L"Scanning available episodes...");
  }

  scanner.set_anime_id(anime_id);
  scanner.set_episode_number(episode_number);
  // Casting file size threshold to int shouldn't be a problem, as the value
  // is never supposed to exceed INT_MAX (i.e. ~2GB).
  scanner.options.min_file_size =
      taiga::settings.GetLibraryFileSizeThreshold();
  scanner.set_path_found(L"");

  auto anime_item = anime::db.Find(anime_id);
  bool found = false;

  if (anime_item) {
    // Check if the anime folder still exists
    anime::ValidateFolder(*anime_item);

    // Search the anime folder for available episodes
    if (!anime_item->GetFolder().empty()) {
      scanner.options.skip_directories = true;
      scanner.options.skip_files = false;
      scanner.options.skip_subdirectories = false;
      found = scanner.Search(anime_item->GetFolder());
    }

    // Search the cached episode path
    if (!found && !anime_item->GetNextEpisodePath().empty()) {
      const auto next_episode_path =
          GetPathOnly(anime_item->GetNextEpisodePath());
      if (!IsEqual(next_episode_path, anime_item->GetFolder())) {
        scanner.options.skip_directories = true;
        scanner.options.skip_files = false;
        scanner.options.skip_subdirectories = true;
        found = scanner.Search(next_episode_path);
      }
    }
  }

  if (!found) {
    // Search library folders for available episodes
    for (const auto& folder : library_folders) {
      if (!FolderExists(folder))
        continue;  // Might be a disconnected external drive
      bool skip_directories = false;
      if (anime_item && !anime_item->GetFolder().empty())
        skip_directories = true;
      scanner.options.skip_directories = skip_directories;
      scanner.options.skip_files = false;
      scanner.options.skip_subdirectories = false;
      if (scanner.Search(folder)) {
        found = true;
        break;
      }
    }
  }

  if (!silent) {
    ui::taskbar_list.SetProgressState(TBPF_NOPROGRESS);
    ui::SetSharedCursor(IDC_ARROW);
    ui::ClearStatusText();
  }

  ui::OnScanAvailableEpisodesFinished();
}

void ScanAvailableEpisodesQuick() {
  ScanAvailableEpisodesQuick(anime::ID_UNKNOWN);
}

void ScanAvailableEpisodesQuick(int anime_id) {
  using track::scanner;

  for (auto it = anime::db.items.rbegin();
       it != anime::db.items.rend(); ++it) {
    anime::Item& anime_item = it->second;

    if (anime_id != anime::ID_UNKNOWN && anime_item.GetId() != anime_id)
      continue;

    const auto folder = anime_item.GetFolder();

    if (folder.empty() || !FolderExists(folder))
      continue;

    scanner.set_anime_id(anime_item.GetId());
    scanner.set_episode_number(0);
    scanner.options.min_file_size =
        taiga::settings.GetLibraryFileSizeThreshold();
    scanner.options.skip_directories = true;
    scanner.options.skip_files = false;
    scanner.options.skip_subdirectories = false;

    scanner.Search(folder);
  }

  ui::OnScanAvailableEpisodesFinished();
}

////////////////////////////////////////////////////////////////////////////////
// Async scan implementation

namespace {
std::thread async_scan_thread;
std::atomic<bool> async_scan_active{false};
std::mutex async_results_mutex;
std::vector<track::Scanner::EpisodeResult> pending_episode_results;
std::vector<track::Scanner::FolderResult> pending_folder_results;
}  // namespace

bool IsScanningAsync() {
  return async_scan_active.load();
}

void ScanAvailableEpisodesAsync() {
  if (async_scan_active.exchange(true)) {
    return;  // Already scanning in background
  }

  if (async_scan_thread.joinable()) {
    async_scan_thread.join();
  }

  // Capture settings on the calling (UI) thread before launching
  const auto library_folders = taiga::settings.GetLibraryFolders();
  const auto min_file_size = taiga::settings.GetLibraryFileSizeThreshold();
  const HWND hwnd = ui::DlgMain.GetWindowHandle();

  async_scan_thread = std::thread([library_folders, min_file_size, hwnd]() {
    track::Scanner local_scanner;
    local_scanner.set_collect_results(true);
    local_scanner.options.min_file_size = min_file_size;

    for (const auto& folder : library_folders) {
      if (!FolderExists(folder))
        continue;
      local_scanner.options.skip_directories = false;
      local_scanner.options.skip_files = false;
      local_scanner.options.skip_subdirectories = false;
      local_scanner.Search(folder);
    }

    {
      std::lock_guard<std::mutex> lock(async_results_mutex);
      pending_episode_results = local_scanner.take_episode_results();
      pending_folder_results = local_scanner.take_folder_results();
    }

    ::PostMessage(hwnd, WM_TAIGA_SCAN_FINISHED, 0, 0);
  });
}

void ApplyAsyncScanResults() {
  std::vector<track::Scanner::EpisodeResult> episode_results;
  std::vector<track::Scanner::FolderResult> folder_results;

  {
    std::lock_guard<std::mutex> lock(async_results_mutex);
    episode_results = std::move(pending_episode_results);
    folder_results = std::move(pending_folder_results);
  }

  // Apply folder assignments
  for (const auto& result : folder_results) {
    auto anime_item = anime::db.Find(result.anime_id);
    if (anime_item && anime_item->GetFolder().empty()) {
      anime_item->SetFolder(result.folder);
    }
  }

  // Apply episode availability
  for (const auto& result : episode_results) {
    auto anime_item = anime::db.Find(result.anime_id);
    if (anime_item) {
      anime_item->SetEpisodeAvailability(
          result.episode_number, true, result.path);
    }
  }

  async_scan_active.store(false);

  ui::OnScanAvailableEpisodesFinished();
}

void ShutdownAsyncScan() {
  if (async_scan_thread.joinable()) {
    async_scan_thread.join();
  }
}
