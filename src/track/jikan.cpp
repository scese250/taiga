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

#include "track/jikan.h"

#include <chrono>
#include <climits>
#include <memory>
#include <set>
#include <thread>
#include <vector>

#include "base/file.h"
#include "base/format.h"
#include "base/json.h"
#include "base/log.h"
#include "base/string.h"
#include "media/anime.h"
#include "media/anime_db.h"
#include "media/anime_util.h"
#include "sync/service.h"
#include "taiga/http.h"
#include "taiga/path.h"
#include "taiga/settings.h"
#include "track/recognition.h"

namespace track::jikan {

// Sends a Jikan API request with rate-limit delay and automatic retry.
// Retries on HTTP 429 (Too Many Requests) and 504 (Gateway Timeout).
static void SendJikanRequest(taiga::http::Request request,
                             taiga::http::ResponseCallback on_response,
                             int retries_left = 3) {
  std::this_thread::sleep_for(std::chrono::milliseconds(500));

  auto wrapper = [request, on_response, retries_left](
      const taiga::http::Response& response) mutable {
    const int status = response.status_code();
    if ((status == 429 || status == 504) && retries_left > 0) {
      const int wait_ms = (status == 429) ? 3000 : 2000;
      LOGW(L"Jikan: HTTP {} - retrying in {}ms ({} attempts left).",
           status, wait_ms, retries_left);
      std::this_thread::sleep_for(std::chrono::milliseconds(wait_ms));
      SendJikanRequest(request, on_response, retries_left - 1);
    } else {
      on_response(response);
    }
  };

  taiga::http::Send(request, nullptr, wrapper);
}

////////////////////////////////////////////////////////////////////////////////
// Data structures

struct SeasonEntry {
  int mal_id = 0;
  int episodes = 0;
};

struct SequelChain {
  int root_id = 0;
  std::vector<SeasonEntry> seasons;
};

////////////////////////////////////////////////////////////////////////////////
// Rule injection from chain data

static void InjectRulesFromChains(const std::vector<SequelChain>& chains) {
  for (const auto& chain : chains) {
    if (chain.seasons.size() < 2)
      continue;

    int cumulative_offset = 0;

    for (size_t i = 0; i < chain.seasons.size(); ++i) {
      const auto& season = chain.seasons[i];

      if (i > 0) {
        // Generate a rule: root_id:(offset+1)-(offset+N) -> season_id:1-N
        const int range_start = cumulative_offset + 1;
        const int dest_episodes = season.episodes > 0 ? season.episodes : INT_MAX;
        const int range_end = season.episodes > 0
            ? cumulative_offset + season.episodes
            : INT_MAX;

        Meow.InjectRelation(
            chain.root_id, season.mal_id,
            {range_start, range_end},
            {1, dest_episodes});

        LOGD(L"Jikan: injected rule {}:{}-{} -> {}:1-{}",
             chain.root_id, range_start,
             range_end == INT_MAX ? L"?" : ToWstr(range_end),
             season.mal_id,
             dest_episodes == INT_MAX ? L"?" : ToWstr(dest_episodes));
      }

      // Accumulate offset for next season
      if (season.episodes > 0) {
        cumulative_offset += season.episodes;
      } else {
        // Unknown episode count: cannot compute offset for subsequent seasons
        break;
      }
    }
  }
}

////////////////////////////////////////////////////////////////////////////////
// Cache persistence

static bool LoadCacheFromDisk(std::vector<SequelChain>& chains) {
  const std::wstring path =
      taiga::GetPath(taiga::Path::DatabaseJikanRelationsCache);
  std::string content;

  if (!ReadFromFile(path, content))
    return false;

  Json root;
  if (!JsonParseString(content, root))
    return false;

  if (!root.contains("chains") || !root["chains"].is_array())
    return false;

  for (const auto& chain_json : root["chains"]) {
    SequelChain chain;
    chain.root_id = JsonReadInt(chain_json, "root_id");

    if (chain_json.contains("seasons") && chain_json["seasons"].is_array()) {
      for (const auto& season_json : chain_json["seasons"]) {
        SeasonEntry entry;
        entry.mal_id = JsonReadInt(season_json, "mal_id");
        entry.episodes = JsonReadInt(season_json, "episodes");
        chain.seasons.push_back(entry);
      }
    }

    if (chain.root_id > 0 && !chain.seasons.empty())
      chains.push_back(std::move(chain));
  }

  return !chains.empty();
}

static bool SaveCacheToDisk(const std::vector<SequelChain>& chains) {
  Json root;
  root["chains"] = Json::array();

  for (const auto& chain : chains) {
    Json chain_json;
    chain_json["root_id"] = chain.root_id;
    chain_json["seasons"] = Json::array();

    for (const auto& season : chain.seasons) {
      Json season_json;
      season_json["mal_id"] = season.mal_id;
      season_json["episodes"] = season.episodes;
      chain_json["seasons"].push_back(season_json);
    }

    root["chains"].push_back(chain_json);
  }

  const std::wstring path =
      taiga::GetPath(taiga::Path::DatabaseJikanRelationsCache);
  return SaveToFile(root.dump(2), path);
}

////////////////////////////////////////////////////////////////////////////////
// Async fetch state machine

struct FetchState {
  std::vector<int> watching_ids;
  size_t current_index = 0;
  std::vector<SequelChain> chains;
  std::set<int> processed_roots;
  std::function<void()> on_complete;

  // State for walking a single sequel chain
  SequelChain current_chain;
  int current_sequel_id = 0;
};

static void ProcessNextAnime(std::shared_ptr<FetchState> state);
static void WalkSequelChain(std::shared_ptr<FetchState> state, int anime_id);
static void FetchEpisodeCount(std::shared_ptr<FetchState> state, int anime_id,
                              std::function<void(int)> on_result);

static int GetLocalEpisodeCount(int mal_id) {
  const auto* item = anime::db.Find(mal_id, false);
  if (item) {
    return item->GetEpisodeCount();
  }
  return 0;
}

static void FinishFetch(std::shared_ptr<FetchState> state) {
  if (!state->chains.empty()) {
    InjectRulesFromChains(state->chains);
    SaveCacheToDisk(state->chains);
    LOGD(L"Jikan: saved {} sequel chains to cache.", state->chains.size());
  }

  if (state->on_complete)
    state->on_complete();
}

static void FinalizeChain(std::shared_ptr<FetchState> state) {
  if (state->current_chain.seasons.size() >= 2) {
    state->chains.push_back(std::move(state->current_chain));
  }
  state->current_chain = {};

  // Move to next anime in the watching list
  state->current_index++;
  ProcessNextAnime(state);
}

static void WalkSequelChain(std::shared_ptr<FetchState> state, int anime_id) {
  // Get episode count for this anime
  int local_eps = GetLocalEpisodeCount(anime_id);

  if (local_eps > 0) {
    state->current_chain.seasons.push_back({anime_id, local_eps});

    // Query Jikan for this anime's data (includes relations) to find sequel
    taiga::http::Request request;
    request.set_target(
        "https://api.jikan.moe/v4/anime/" + std::to_string(anime_id) + "/full");

    const auto on_response =
        [state, anime_id](const taiga::http::Response& response) {
      if (response.error()) {
        LOGW(L"Jikan: network error getting relations for MAL ID {}: {}",
             anime_id, StrToWstr(response.error().str()));
        FinalizeChain(state);
        return;
      }
      if (response.status_class() != 200) {
        LOGW(L"Jikan: HTTP {} getting relations for MAL ID {}.",
             response.status_code(), anime_id);
        FinalizeChain(state);
        return;
      }

      Json root;
      if (!JsonParseString(response.body(), root) ||
          !root.contains("data") || !root["data"].is_object() ||
          !root["data"].contains("relations") || !root["data"]["relations"].is_array()) {
        FinalizeChain(state);
        return;
      }

      // Find sequel relation
      int sequel_id = 0;
      for (const auto& relation : root["data"]["relations"]) {
        if (JsonReadStr(relation, "relation") != "Sequel")
          continue;
        if (!relation.contains("entry") || !relation["entry"].is_array())
          continue;
        for (const auto& entry : relation["entry"]) {
          if (JsonReadStr(entry, "type") == "anime") {
            sequel_id = JsonReadInt(entry, "mal_id");
            break;
          }
        }
        if (sequel_id > 0)
          break;
      }

      if (sequel_id > 0) {
        state->processed_roots.insert(sequel_id);
        // Continue walking the chain
        WalkSequelChain(state, sequel_id);
      } else {
        // No more sequels, chain is complete
        FinalizeChain(state);
      }
    };

    SendJikanRequest(request, on_response);
  } else {
    // No local episode count; try Jikan API to get it
    FetchEpisodeCount(state, anime_id, [state, anime_id](int eps) {
      state->current_chain.seasons.push_back({anime_id, eps});

      if (eps == 0) {
        // Still unknown, end the chain here (can't compute offsets)
        // but keep the entry with 0 so it gets the ? range
        FinalizeChain(state);
        return;
      }

      // Continue finding sequels via /full endpoint
      taiga::http::Request request;
      request.set_target(
          "https://api.jikan.moe/v4/anime/" + std::to_string(anime_id) + "/full");

      const auto on_response =
          [state, anime_id](const taiga::http::Response& response) {
        if (response.error()) {
          LOGW(L"Jikan: network error getting relations (2nd path) for MAL ID {}: {}",
               anime_id, StrToWstr(response.error().str()));
          FinalizeChain(state);
          return;
        }
        if (response.status_class() != 200) {
          LOGW(L"Jikan: HTTP {} getting relations (2nd path) for MAL ID {}.",
               response.status_code(), anime_id);
          FinalizeChain(state);
          return;
        }

        Json root;
        if (!JsonParseString(response.body(), root) ||
            !root.contains("data") || !root["data"].is_object() ||
            !root["data"].contains("relations") || !root["data"]["relations"].is_array()) {
          FinalizeChain(state);
          return;
        }

        int sequel_id = 0;
        for (const auto& relation : root["data"]["relations"]) {
          if (JsonReadStr(relation, "relation") != "Sequel")
            continue;
          if (!relation.contains("entry") || !relation["entry"].is_array())
            continue;
          for (const auto& entry : relation["entry"]) {
            if (JsonReadStr(entry, "type") == "anime") {
              sequel_id = JsonReadInt(entry, "mal_id");
              break;
            }
          }
          if (sequel_id > 0)
            break;
        }

        if (sequel_id > 0) {
          state->processed_roots.insert(sequel_id);
          WalkSequelChain(state, sequel_id);
        } else {
          FinalizeChain(state);
        }
      };

      SendJikanRequest(request, on_response);
    });
  }
}

static void FetchEpisodeCount(std::shared_ptr<FetchState> state, int anime_id,
                              std::function<void(int)> on_result) {
  taiga::http::Request request;
  request.set_target(
      "https://api.jikan.moe/v4/anime/" + std::to_string(anime_id));

  const auto on_response =
      [on_result](const taiga::http::Response& response) {
    if (response.error()) {
      LOGW(L"Jikan: network error fetching episode count for anime: {}",
           StrToWstr(response.error().str()));
      on_result(0);
      return;
    }
    if (response.status_class() != 200) {
      LOGW(L"Jikan: HTTP {} fetching episode count.", response.status_code());
      on_result(0);
      return;
    }

    Json root;
    if (!JsonParseString(response.body(), root) ||
        !root.contains("data")) {
      on_result(0);
      return;
    }

    const int episodes = JsonReadInt(root["data"], "episodes");
    on_result(episodes);
  };

  SendJikanRequest(request, on_response);
}

static void ProcessNextAnime(std::shared_ptr<FetchState> state) {
  // Skip anime that were already processed as part of another chain
  while (state->current_index < state->watching_ids.size()) {
    const int id = state->watching_ids[state->current_index];
    if (state->processed_roots.count(id) > 0) {
      state->current_index++;
      continue;
    }
    break;
  }

  if (state->current_index >= state->watching_ids.size()) {
    FinishFetch(state);
    return;
  }

  const int anime_id = state->watching_ids[state->current_index];
  state->processed_roots.insert(anime_id);

  LOGD(L"Jikan: processing sequel chain for MAL ID {}.", anime_id);

  // Start a new chain from this anime
  state->current_chain = {};
  state->current_chain.root_id = anime_id;

  WalkSequelChain(state, anime_id);
}

////////////////////////////////////////////////////////////////////////////////
// Public API

bool LoadCache() {
  if (!taiga::settings.GetRecognitionAutoResolveSequels())
    return false;

  if (sync::GetCurrentServiceId() != sync::ServiceId::MyAnimeList)
    return false;

  std::vector<SequelChain> chains;
  if (!LoadCacheFromDisk(chains)) {
    LOGD(L"Jikan: no cached sequel relation data found.");
    return false;
  }

  InjectRulesFromChains(chains);
  LOGD(L"Jikan: loaded {} sequel chains from cache.", chains.size());
  return true;
}

void FetchSequelRelations(std::function<void()> on_complete) {
  if (!taiga::settings.GetRecognitionAutoResolveSequels()) {
    if (on_complete) on_complete();
    return;
  }

  if (sync::GetCurrentServiceId() != sync::ServiceId::MyAnimeList) {
    if (on_complete) on_complete();
    return;
  }

  auto state = std::make_shared<FetchState>();
  state->on_complete = std::move(on_complete);

  // Collect all anime with "Watching" status
  for (const auto& [id, item] : anime::db.items) {
    if (item.GetMyStatus(false) == anime::MyStatus::Watching) {
      state->watching_ids.push_back(id);
    }
  }

  if (state->watching_ids.empty()) {
    LOGD(L"Jikan: no anime in Watching status, skipping.");
    if (state->on_complete) state->on_complete();
    return;
  }

  LOGD(L"Jikan: fetching sequel relations for {} watching anime.",
       state->watching_ids.size());

  ProcessNextAnime(state);
}

void FetchSequelRelationsForAnime(int anime_id,
                                  std::function<void()> on_complete) {
  if (!taiga::settings.GetRecognitionAutoResolveSequels()) {
    if (on_complete) on_complete();
    return;
  }

  if (sync::GetCurrentServiceId() != sync::ServiceId::MyAnimeList) {
    if (on_complete) on_complete();
    return;
  }

  if (!anime::IsValidId(anime_id)) {
    if (on_complete) on_complete();
    return;
  }

  LOGD(L"Jikan: fetching sequel relations for single MAL ID {}.", anime_id);

  auto state = std::make_shared<FetchState>();
  state->on_complete = std::move(on_complete);
  state->watching_ids.push_back(anime_id);

  ProcessNextAnime(state);
}

}  // namespace track::jikan
