#pragma once

#include <string>
#include <vector>

// Lightweight persistent list of recently opened ISO paths, stored as one path
// per line in ~/.config/umdplay/recent.txt (override with $UMDPLAY_CONFIG_DIR).
// Most-recent first; duplicates are moved to the front.
namespace recent {

// Absolute path to the recent-list file (the directory is created on save).
std::string config_path();

// Existing paths from the list, newest first. Missing files are skipped.
std::vector<std::string> load();

// Move `iso_path` to the front (deduped), trim to `max_entries`, and write.
// Returns false if the list could not be written.
bool add(const std::string& iso_path, size_t max_entries = 12);

// --- resume positions ------------------------------------------------------
// "Where I stopped watching" per disc, stored beside the recent list as
// `path<TAB>seconds` per line and keyed by the same canonical path, so the
// same disc opened any way offers the same resume point.

// Absolute path to the resume file.
std::string positions_path();

// Seconds saved for `iso_path`, or -1 when there is none (or it is unreadable).
double load_position(const std::string& iso_path);

// Save `seconds` for `iso_path` (<= 0 erases the entry). Trimmed to
// `max_entries` most-recently-written discs. Returns false on a write error.
bool save_position(const std::string& iso_path, double seconds, size_t max_entries = 32);

}  // namespace recent
