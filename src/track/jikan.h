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

#include <functional>

namespace track::jikan {

// Loads cached sequel chain data from disk and injects synthetic
// redirection rules into the recognition relations map.
// Called on startup (no network). Returns true if cache was loaded.
bool LoadCache();

// Queries Jikan API for sequel relations of all "Watching" anime,
// builds sequel chains, generates synthetic rules, injects them,
// and saves results to cache. Called from the "Check for updates" flow.
// Calls on_complete when all async work is done.
void FetchSequelRelations(std::function<void()> on_complete);

}  // namespace track::jikan
