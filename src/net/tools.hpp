#pragma once

#include <QJsonArray>
#include <QJsonObject>
#include <QString>

#include <filesystem>
#include <optional>
#include <string>

namespace lnb::net {

namespace fs = std::filesystem;

// A single tool call requested by the model. Fields are kept as Qt strings
// because they travel straight to and from the JSON protocol.
struct ToolCall {
  int index = -1;
  QString id;
  QString name;
  QString arguments;
};

// Resolves `relative` against `root_str` and rejects anything that would escape
// the repository root (including via symlinks). An absolute path is accepted as
// long as it resolves inside the root.
fs::path checked_repo_path(const std::string& root_str, const std::string& relative);

QString json_string_or(
    const QJsonObject& object, const QString& key, const QString& fallback = {}
);
int json_int_or(const QJsonObject& object, const QString& key, int fallback);

// True when the file looks binary (a NUL byte in its first block), so
// search_code can cover every text language without an extension whitelist.
bool is_binary_file(const fs::path& path);

std::string search_code(
    const std::string& query, const std::string& root_path,
    const std::string& relative_path
);

std::string list_directory(const std::string& root_path, const std::string& relative_path);

// Performs an HTTP GET on `url` (http/https only) and returns the raw body,
// capped to `max_bytes`. Runs a nested event loop on the calling (worker)
// thread, mirroring the streaming client. Throws on a transport error.
QString http_get(const QString& url, const std::optional<std::string>& proxy, int max_bytes);

// Fetches `url` and returns its visible text (HTML tags and entities stripped,
// truncated to a readable size).
QString fetch_url(const QString& url, const std::optional<std::string>& proxy);

// Searches the web and returns the top results as "Title / URL / snippet"
// blocks. Best-effort: it scrapes the DuckDuckGo HTML endpoint, so an empty or
// changed result page yields a clear message rather than an exception.
QString web_search(const QString& query, const std::optional<std::string>& proxy);

// The JSON tool schemas offered to the model. Filesystem tools are included
// only when `filesystem` is true (the session has a shared directory).
QJsonArray tool_definitions(bool filesystem);

// Executes one tool call. Filesystem tools require `root_path` (the session's
// shared directory) and report a clear error when it is missing; network tools
// use `proxy` and work without a shared directory. Tool failures are returned
// to the model as text (rather than thrown), so it can correct its arguments.
QString execute_local_tool(
    const ToolCall& call, const std::optional<std::string>& root_path,
    const std::optional<std::string>& proxy
);

} // namespace lnb::net
