#pragma once

#include <boost/json/array.hpp>
#include <boost/json/object.hpp>

#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace lnb::core {

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
// Where web_search sends its queries. Search engines answer a plain HTTP client
// with a bot challenge, so the query goes to a service that speaks SearXNG's
// JSON API: either a self-hosted SearXNG or the bundled browser bridge
// (scripts/search_bridge.py), which drives a real browser and answers in the
// same shape. That is why one parser covers both.
struct SearchSettings {
  // The base URL of the SearXNG-compatible service, e.g.
  // "http://127.0.0.1:8873". Empty means the user has not configured search, so
  // web_search reports that instead of guessing an endpoint.
  std::string endpoint;
};

struct ToolPermissions {
  bool enabled = true;
  bool filesystem = false;
  bool read = true;
  bool write = false;
  bool online = true;

  // How the online tools reach the web.
  SearchSettings search;

  [[nodiscard]] bool any() const {
    return enabled && (online || (filesystem && (read || write)));
  }
};


std::string json_string_or(
    const boost::json::object& object, std::string_view key,
    std::string fallback = {}
);
int json_int_or(
    const boost::json::object& object, std::string_view key, int fallback
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

// Searches the web through the SearXNG-compatible service at
// `settings.endpoint` and returns the top results as "Title / URL / snippet"
// blocks. An empty endpoint yields a message explaining that search is not
// configured, rather than an exception, so the model can tell the user. A result
// served from the bridge's cache is prefixed with a note saying it may be
// outdated, which is what lets repeated questions converge instead of flapping
// with the network. An empty result page yields a clear message rather than an
// exception. Requests go through `proxy` when one is configured, except for a
// loopback endpoint (the local bridge), which must never be sent to the proxy.
std::string web_search(
    const std::string& query, const std::optional<std::string>& proxy,
    const SearchSettings& settings = {}
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

} // namespace lnb::core
