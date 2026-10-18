#pragma once

#include <boost/json/array.hpp>
#include <boost/json/object.hpp>

#include <filesystem>
#include <optional>
#include <string>
#include <string_view>

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

// Resolves `relative` against `root_str` and rejects anything that would escape
// the repository root (including via symlinks). An absolute path is accepted as
// long as it resolves inside the root.
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

std::string search_code(
    const std::string& query, const std::string& root_path,
    const std::string& relative_path
);

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

// The JSON tool schemas offered to the model. Filesystem tools are included
// only when `filesystem` is true (the session has a shared directory).
boost::json::array tool_definitions(bool filesystem);

// Executes one tool call. Filesystem tools require `root_path` (the session's
// shared directory) and report a clear error when it is missing; network tools
// use `proxy` and work without a shared directory. Tool failures are returned
// to the model as text (rather than thrown), so it can correct its arguments.
std::string execute_local_tool(
    const ToolCall& call, const std::optional<std::string>& root_path,
    const std::optional<std::string>& proxy
);

} // namespace lnb::net
