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

}  // namespace recent
