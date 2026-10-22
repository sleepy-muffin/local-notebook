#pragma once

#include <boost/json/array.hpp>
#include <boost/json/object.hpp>

#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace lnb::net {

namespace fs = std::filesystem;

// A single tool call requested by the model. Fields are plain strings because
// they travel straight to and from the JSON protocol.
struct ToolCall {
  int index = -1;
  std::string id;
  std::string name;
  std::string arguments;
};

// What a session allows the model to do. The master switch disables every tool
// without changing the individual preferences, so turning tools off and on again
// restores the previous choice. `filesystem` records whether the session has at
// least one shared directory; without one the read and write tools are not
// offered. Reading is the filesystem read/search tools, writing is
// create/overwrite/edit/delete in the shared directories, and online is
// web_search and fetch_url. Writing defaults to off.
struct ToolPermissions {
  bool enabled = true;
  bool filesystem = false;
  bool read = true;
  bool write = false;
  bool online = true;

  [[nodiscard]] bool any() const {
    return enabled && (online || (filesystem && (read || write)));
  }
};

// Resolves a model-supplied `relative` path against `root_str` and rejects
// anything that would escape the root (including via symlinks). Absolute paths
// are rejected: the model only ever works with relative paths.
fs::path checked_repo_path(
    const std::string& root_str, const std::string& relative
);

std::string json_string_or(
    const boost::json::object& object, std::string_view key,
    std::string fallback = {}
);
int json_int_or(
    const boost::json::object& object, std::string_view key, int fallback
);

// True when the file looks binary (a NUL byte in its first block), so
// search_code can cover every text language without an extension whitelist.
bool is_binary_file(const fs::path& path);

std::string list_directory(
    const std::string& root_path, const std::string& relative_path
);

// Performs an HTTP GET on `url` (http/https only) and returns the raw body,
// capped to `max_bytes`. Throws on a transport error.
std::string http_get(
    const std::string& url, const std::optional<std::string>& proxy, int max_bytes
);

// Fetches `url` and returns its visible text (HTML tags and entities stripped,
// truncated to a readable size).
std::string fetch_url(
    const std::string& url, const std::optional<std::string>& proxy
);

// Searches the web and returns the top results as "Title / URL / snippet"
// blocks. Best-effort: it scrapes DuckDuckGo's HTML endpoint, falls back to
// Bing's RSS feed, then to Brave when the earlier sources are blocked. An empty
// or changed result page yields a clear message rather than an exception. All
// requests go through `proxy` when one is configured.
std::string web_search(
    const std::string& query, const std::optional<std::string>& proxy
);

// The JSON tool schemas offered to the model. Read tools are included only when
// the session has a shared directory and reading is permitted; write tools only
// when writing is permitted; network tools only when online access is permitted.
// A disabled master switch yields an empty array.
boost::json::array tool_definitions(const ToolPermissions& permissions);

// Executes one tool call. Filesystem tools require at least one configured
// shared directory and report a clear error when there is none; network tools
// use `proxy` and work without one. A tool whose category is not permitted is
// refused. The configured directories are presented to the model as a synthetic,
// relative tree; their actual paths are never exposed in tool results. Tool
// failures are returned to the model as text (rather than thrown), so it can
// correct its arguments.
std::string execute_local_tool(
    const ToolCall& call, const std::vector<std::string>& shared_directories,
    const std::optional<std::string>& proxy,
    const ToolPermissions& permissions = {}
);

} // namespace lnb::net
