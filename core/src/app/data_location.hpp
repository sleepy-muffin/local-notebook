#pragma once

#include <filesystem>

#include <QStringList>

namespace lnb::app {

// The directory that holds all on-disk state: the global settings.json, each
// assistant's <uuid>/ tree (settings, database, log) and the assistant registry.
//
// Precedence:
//   1. an explicit `--user-data <dir>` (or `--user-data=<dir>`) override;
//   2. otherwise the platform's per-user application data location, resolved with
//      QStandardPaths::AppDataLocation, e.g.
//        Linux:   ~/.local/share/Local Notebook
//        Windows: C:/Users/<user>/AppData/Roaming/Local Notebook
//   3. falling back to the relative "data" when Qt reports no location.
//
// `arguments` is the full argument vector (QCoreApplication::arguments()), whose
// first element is the program name. The result is handed to
// store::paths::set_data_root() by main().
[[nodiscard]] std::filesystem::path resolve_data_root(const QStringList& arguments);

} // namespace lnb::app
