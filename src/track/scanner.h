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

#pragma once

#include <optional>
#include <string>
#include <vector>

#include "base/file_search.h"
#include "track/episode.h"

namespace track {

class Scanner : public base::FileSearch {
public:
  struct EpisodeResult {
    int anime_id;
    int episode_number;
    std::wstring path;
  };
  struct FolderResult {
    int anime_id;
    std::wstring folder;
  };

  bool Search(const std::wstring& root);

  const std::wstring& path_found() const;

  void set_anime_id(int anime_id);
  void set_episode_number(int episode_number);
  void set_path_found(const std::wstring& path_found);

  void set_collect_results(bool collect);
  std::vector<EpisodeResult> take_episode_results();
  std::vector<FolderResult> take_folder_results();

private:
  bool OnDirectory(const base::FileSearchResult& result);
  bool OnFile(const base::FileSearchResult& result);

  std::optional<int> anime_id_;
  anime::Episode episode_;
  int episode_number_ = 0;
  std::wstring path_found_;
  bool collect_results_ = false;
  std::vector<EpisodeResult> episode_results_;
  std::vector<FolderResult> folder_results_;
};

inline Scanner scanner;

}  // namespace track

void ScanAvailableEpisodes(bool silent);
void ScanAvailableEpisodes(bool silent, int anime_id, int episode_number);
void ScanAvailableEpisodesQuick();
void ScanAvailableEpisodesQuick(int anime_id);
void ScanAvailableEpisodesAsync();
void ApplyAsyncScanResults();
void ShutdownAsyncScan();
bool IsScanningAsync();
