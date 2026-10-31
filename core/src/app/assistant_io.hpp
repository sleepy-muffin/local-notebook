#pragma once

#include <string>

#include "local_notebook/core/model.hpp"

namespace lnb {

// Reads/writes one assistant's settings at data/<uuid>/settings.json
// (design-1.md §8). The database (data/<uuid>/data.db) is managed separately by
// store::database.
Assistant load_assistant(const std::string& uuid);
void save_assistant(const std::string& uuid, const Assistant& assistant);

} // namespace lnb
