#include "net/tools.hpp"

#include "shark/str.hpp"
#include "shark/net/http.hpp"
#include "shark/file/search.hpp"
#include "net/search_results.hpp"
#include "utils/utf8.hpp"

#include <boost/json/parse.hpp>
#include <boost/json/value.hpp>
#include <boost/regex.hpp>
#include <boost/system/error_code.hpp>
#include <boost/url.hpp>

#include <algorithm>
#include <array>
#include <cctype>
#include <cstddef>
#include <cstdio>
#include <fstream>
#include <iterator>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace lnb::core {

namespace {


bool equals_ignore_case(std::string_view a, std::string_view b) {
  if (a.size() != b.size()) {
    return false;
  }

  for (std::size_t i = 0; i < a.size(); ++i) {
    if (std::tolower(static_cast<unsigned char>(a[i])) !=
        std::tolower(static_cast<unsigned char>(b[i]))) {
      return false;
    }
  }

  return true;
}

void replace_all(std::string& text, std::string_view from, std::string_view to) {
  if (from.empty()) {
    return;
  }

  std::size_t at = 0;

  while ((at = text.find(from, at)) != std::string::npos) {
    text.replace(at, from.size(), to);
    at += to.size();
  }
}

std::string trim(std::string_view text) {
  const auto not_space = [](unsigned char c) { return std::isspace(c) == 0; };
  const auto begin = std::find_if(text.begin(), text.end(), not_space);
  const auto end = std::find_if(text.rbegin(), text.rend(), not_space).base();

  if (begin >= end) {
    return {};
  }

  return std::string(begin, end);
}

void append_utf8(std::string& out, char32_t code) {
  if (code <= 0x7F) {
    out.push_back(static_cast<char>(code));
  } else if (code <= 0x7FF) {
    out.push_back(static_cast<char>(0xC0 | (code >> 6)));
    out.push_back(static_cast<char>(0x80 | (code & 0x3F)));
  } else if (code <= 0xFFFF) {
    out.push_back(static_cast<char>(0xE0 | (code >> 12)));
    out.push_back(static_cast<char>(0x80 | ((code >> 6) & 0x3F)));
    out.push_back(static_cast<char>(0x80 | (code & 0x3F)));
  } else {
    out.push_back(static_cast<char>(0xF0 | (code >> 18)));
    out.push_back(static_cast<char>(0x80 | ((code >> 12) & 0x3F)));
    out.push_back(static_cast<char>(0x80 | ((code >> 6) & 0x3F)));
    out.push_back(static_cast<char>(0x80 | (code & 0x3F)));
  }
}

std::string url_encode(std::string_view text) {
  static constexpr char kHex[] = "0123456789ABCDEF";

  std::string out;
  out.reserve(text.size() * 3);

  for (unsigned char c: text) {
    if (std::isalnum(c) != 0 || c == '-' || c == '_' || c == '.' || c == '~') {
      out.push_back(static_cast<char>(c));
    } else {
      out.push_back('%');
      out.push_back(kHex[c >> 4]);
      out.push_back(kHex[c & 0x0F]);
    }
  }

  return out;
}

// Reads an inclusive line range, prefixing each line with its number. Kept out
// of the header because it is an implementation detail of read_file.
std::string read_file_range(const fs::path& file, int start_line, int end_line) {
  if (start_line < 1 || end_line < start_line || end_line - start_line > 300) {
    throw std::runtime_error("Invalid line range; maximum range is 301 lines");
  }

  std::error_code error;
  const std::uintmax_t size = fs::file_size(file, error);

  if (error) {
    throw std::runtime_error("Cannot read file");
  }

  if (size > 2 * 1024 * 1024) {
    throw std::runtime_error("File is too large");
  }

  std::ifstream in(file);
  if (!in) {
    throw std::runtime_error("Cannot open file");
  }

  std::ostringstream out;
  std::string line;
  int line_no = 0;

  while (std::getline(in, line)) {
    ++line_no;
    if (line_no < start_line) continue;
    if (line_no > end_line) break;

    out << line_no << ": " << line << '\n';
  }

  return out.str();
}

// The largest file the write tools accept, so one tool call cannot fill the disk.
constexpr std::size_t kMaxWriteBytes = 4 * 1024 * 1024;

// Resolves a model path that may not exist yet. The final component may name a
// new file, but the parent must exist. fs::weakly_canonical() resolves symlinks
// in every existing prefix, so a link that escapes the root is still rejected.
fs::path checked_repo_path_for_write(
    const std::string& root_str, const std::string& relative
) {
  std::error_code error;
  const fs::path root = fs::canonical(root_str, error);

  if (error) {
    throw std::runtime_error("The shared directory is unavailable");
  }

  const fs::path input(relative);

  if (input.is_absolute()) {
    throw std::runtime_error("Paths must be relative to the shared directories");
  }

  const fs::path normalized = input.lexically_normal();
  const std::string normalized_text = normalized.generic_string();

  if (normalized_text.empty() || normalized_text == "." ||
      normalized_text == ".." || normalized_text.rfind("../", 0) == 0) {
    throw std::runtime_error("Path is outside the shared directories");
  }

  const fs::path candidate = fs::weakly_canonical(root / normalized, error);

  if (error) {
    throw std::runtime_error("The path could not be resolved");
  }

  const fs::path rel = candidate.lexically_relative(root);
  if (rel.empty() || rel.is_absolute() || *rel.begin() == "..") {
    throw std::runtime_error("Path is outside the shared directories");
  }

  std::error_code type_error;

  if (fs::is_directory(candidate, type_error) && !type_error) {
    throw std::runtime_error("Path is a directory");
  }

  if (!fs::is_directory(candidate.parent_path(), type_error) || type_error) {
    throw std::runtime_error("Parent directory does not exist");
  }

  return candidate;
}

void write_file_contents(const fs::path& file, const std::string& content) {
  if (content.size() > kMaxWriteBytes) {
    throw std::runtime_error("File content is too large (maximum 4 MiB)");
  }

  std::ofstream out(file, std::ios::binary | std::ios::trunc);

  if (!out) {
    throw std::runtime_error("Cannot write file");
  }

  out.write(content.data(), static_cast<std::streamsize>(content.size()));

  if (!out) {
    throw std::runtime_error("Cannot write file");
  }
}

std::string read_whole_file(const fs::path& file) {
  std::error_code error;
  const std::uintmax_t size = fs::file_size(file, error);

  if (error) {
    throw std::runtime_error("Cannot read file");
  }

  if (size > kMaxWriteBytes) {
    throw std::runtime_error("File is too large to edit (maximum 4 MiB)");
  }

  std::ifstream in(file, std::ios::binary);

  if (!in) {
    throw std::runtime_error("Cannot open file");
  }

  return std::string(
      std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()
  );
}

// Resolves a path for deletion. Only regular files may be removed: a directory
// (or a symlink to one) is refused, and checked_repo_path() already rejects
// anything that resolves outside the root.
fs::path checked_repo_path_for_delete(
    const std::string& root_str, const std::string& relative
) {
  const fs::path file = shark::file::checked_repo_path(root_str, relative);
  std::error_code error;

  if (!fs::is_regular_file(file, error) || error) {
    throw std::runtime_error("Path is not a regular file");
  }

  return file;
}

} // namespace


std::string json_string_or(
    const boost::json::object& object, std::string_view key, std::string fallback
) {
  const boost::json::value* value = object.if_contains(key);

  return value != nullptr && value->is_string()
      ? std::string(value->as_string())
      : std::move(fallback);
}

int json_int_or(
    const boost::json::object& object, std::string_view key, int fallback
) {
  const boost::json::value* value = object.if_contains(key);

  if (value == nullptr || !value->is_number()) {
    return fallback;
  }

  if (value->is_int64()) {
    return static_cast<int>(value->as_int64());
  }

  if (value->is_uint64()) {
    return static_cast<int>(value->as_uint64());
  }

  return static_cast<int>(value->as_double());
}





// ------------------------------ network tools ------------------------------

constexpr int kFetchBodyLimit = 512 * 1024; // bytes read from one response
constexpr int kFetchTextLimit = 8000; // characters returned to the model

namespace {

// Decodes the HTML entities that occur in ordinary page text. Numeric
// references are decoded before `&amp;` so a literal "&amp;#39;" is preserved.
std::string decode_entities(const std::string& input) {
  std::string text = input;
  replace_all(text, "&nbsp;", " ");
  replace_all(text, "&lt;", "<");
  replace_all(text, "&gt;", ">");
  replace_all(text, "&quot;", "\"");
  replace_all(text, "&#39;", "'");
  replace_all(text, "&apos;", "'");

  static const boost::regex numeric(
      "&#(x?[0-9A-Fa-f]+);", boost::regex_constants::perl
  );

  std::string decoded;
  decoded.reserve(text.size());

  std::size_t last = 0;

  for (boost::sregex_iterator it(text.begin(), text.end(), numeric), end;
       it != end; ++it) {
    const boost::smatch& match = *it;
    const std::string body = match[1].str();

    bool ok = false;
    unsigned long code = 0;

    try {
      std::size_t consumed = 0;

      if (!body.empty() && (body.front() == 'x' || body.front() == 'X')) {
        code = std::stoul(body.substr(1), &consumed, 16);
        ok = consumed == body.size() - 1;
      } else {
        code = std::stoul(body, &consumed, 10);
        ok = consumed == body.size();
      }
    } catch (const std::exception&) {
      ok = false;
    }

    if (!ok || code == 0 || code > 0x10FFFF) {
      continue;
    }

    decoded.append(text, last, static_cast<std::size_t>(match.position()) - last);
    append_utf8(decoded, static_cast<char32_t>(code));
    last = static_cast<std::size_t>(match.position() + match.length());
  }

  decoded.append(text, last, std::string::npos);
  replace_all(decoded, "&amp;", "&");

  return decoded;
}

// Trims every line and collapses runs of blank lines into a single one. Pages
// are full of markup-only lines that leave nothing but whitespace behind once
// the tags are stripped, so without this the fetched text arrives as a sparse
// column of empty rows.
std::string tidy_lines(const std::string& text) {
  std::string out;
  out.reserve(text.size());

  // Set when a blank line was seen, so it is written back only if another line
  // of text follows. This also drops blank lines at the end.
  bool blank_pending = false;

  for (std::size_t begin = 0; begin < text.size();) {
    std::size_t end = text.find('\n', begin);

    if (end == std::string::npos) {
      end = text.size();
    }

    const std::string line = trim(std::string_view(text).substr(begin, end - begin));

    if (line.empty()) {
      blank_pending = !out.empty();
    } else {
      if (blank_pending) {
        out.push_back('\n');
        blank_pending = false;
      }

      out += line;
      out.push_back('\n');
    }

    begin = end + 1;
  }

  // The loop leaves a trailing newline; `trim` removes it along with any other
  // leading or trailing whitespace.
  return trim(out);
}

// Reduces an HTML fragment to readable text: drops script/style bodies, strips
// the remaining tags and decodes entities.
std::string html_to_text(const std::string& html) {
  static const boost::regex script(
      "<script\\b[^>]*>.*?</script>",
      boost::regex_constants::perl | boost::regex_constants::icase |
          boost::regex_constants::mod_s
  );
  static const boost::regex style(
      "<style\\b[^>]*>.*?</style>",
      boost::regex_constants::perl | boost::regex_constants::icase |
          boost::regex_constants::mod_s
  );
  static const boost::regex tag("<[^>]*>", boost::regex_constants::perl);
  // Stripping adjacent tags leaves runs of spaces ("<b>x</b> y" becomes
  // " x  y"); collapse those without touching newlines, which carry the
  // paragraph structure of a fetched page.
  static const boost::regex space_run(
      "[ \\t]{2,}", boost::regex_constants::perl
  );

  std::string text = boost::regex_replace(html, script, " ");
  text = boost::regex_replace(text, style, " ");
  text = boost::regex_replace(text, tag, " ");
  text = boost::regex_replace(text, space_run, " ");

  return tidy_lines(decode_entities(text));
}

} // namespace

// Public search and content endpoints reject non-browser clients: DuckDuckGo
// serves its bot-challenge page to the plain `local-notebook` User-Agent that
// `http_get_body` sends by default. These headers make the request look like an
// ordinary page load, which is what keeps the HTML result pages readable.
const std::vector<shark::net::HttpHeader> kBrowserHeaders{
    {"User-Agent",
     "Mozilla/5.0 (X11; Linux x86_64) AppleWebKit/537.36 (KHTML, like Gecko) "
     "Chrome/124.0 Safari/537.36"},
    {"Accept", "text/html,application/xhtml+xml,application/xml;q=0.9,*/*;q=0.8"},
    {"Accept-Language", "en-US,en;q=0.9"},
};

std::string http_get_with(
    const std::string& url, const std::optional<std::string>& proxy, int max_bytes,
    const std::vector<shark::net::HttpHeader>& headers
) {
  auto body = http_get_body(url, proxy, max_bytes, std::stop_token{}, headers);

  if (!body) {
    throw std::runtime_error(
        "HTTP request failed (HTTP " + std::to_string(body.error()) + ")"
    );
  }

  return std::move(*body);
}

std::string http_get(
    const std::string& url, const std::optional<std::string>& proxy, int max_bytes
) {
  return http_get_with(url, proxy, max_bytes, kBrowserHeaders);
}

std::string fetch_url(
    const std::string& url, const std::optional<std::string>& proxy
) {
  const std::string trimmed_url = trim(url);

  if (trimmed_url.empty()) {
    throw std::runtime_error("A URL is required");
  }

  // A page is bytes, not text: one served as GBK (or any other encoding) would
  // otherwise carry sequences that are not valid UTF-8 into the request body,
  // which a strict JSON server rejects outright. The same applies to the cut
  // below, which must land on a code-point boundary rather than in the middle
  // of a multi-byte character.
  const std::string text =
      sanitize_utf8(html_to_text(http_get(trimmed_url, proxy, kFetchBodyLimit)));

  if (text.empty()) {
    return "The page returned no readable text.";
  }

  if (text.size() > static_cast<std::size_t>(kFetchTextLimit)) {
    return truncate_utf8(text, kFetchTextLimit) +
        "\n\n[Page truncated; only the first part is shown.]";
  }

  return text;
}

namespace {

constexpr std::size_t kMaxSearchResults = 8;

// Renders parsed hits in the "Title / URL / snippet" shape the model expects.
std::string format_search_results(const std::vector<SearchResult>& results) {
  std::ostringstream out;
  std::size_t count = 0;

  for (const SearchResult& result: results) {
    if (result.title.empty() || result.url.empty()) {
      continue;
    }

    ++count;
    out << count << ". " << result.title << '\n';
    out << result.url << '\n';

    if (!result.snippet.empty()) {
      out << result.snippet << '\n';
    }

    out << '\n';

    if (count == kMaxSearchResults) {
      break;
    }
  }

  return out.str();
}

// The status and body of a GET, read even when the status is not 2xx. A local
// service reports a human-check failure in a 503 body, so the body must be read
// before deciding whether the request succeeded.
struct HttpResponse {
  int status = 0;
  std::string body;
};

HttpResponse http_get_all(
    const std::string& url, const std::optional<std::string>& proxy, int max_bytes,
    const std::vector<shark::net::HttpHeader>& headers, int read_timeout_ms
) {
  std::vector<shark::net::HttpHeader> merged{
      {"User-Agent", "Mozilla/5.0 (compatible; local-notebook/1.0)"},
  };

  // A caller-supplied header replaces the default of the same name.
  for (const shark::net::HttpHeader& extra: headers) {
    bool replaced = false;

    for (shark::net::HttpHeader& header: merged) {
      if (equals_ignore_case(header.name, extra.name)) {
        header = extra;
        replaced = true;
        break;
      }
    }

    if (!replaced) {
      merged.push_back(extra);
    }
  }

  HttpResponse response;
  const std::size_t limit =
      max_bytes > 0 ? static_cast<std::size_t>(max_bytes) : 0;

  const shark::net::ResponseHandler on_response =
      [&](int status, const std::vector<shark::net::HttpHeader>&) {
        response.status = status;
        return true;
      };

  const shark::net::BodyHandler on_body = [&](std::string_view chunk) {
    if (response.body.size() < limit) {
      response.body.append(chunk.substr(0, limit - response.body.size()));
    }

    return true;
  };

  http_request(
      "GET", url, proxy, merged, "", std::stop_token{}, on_response, on_body,
      30'000, 5, read_timeout_ms
  );

  return response;
}

// A search API answers JSON, not HTML.
const std::vector<shark::net::HttpHeader> kJsonHeaders{
    {"Accept", "application/json"},
};

// True when `url` points at this machine, where a proxy must not be used: the
// configured proxy would otherwise intercept the loopback request.
bool is_loopback_url(const std::string& url) {
  const auto parsed = boost::urls::parse_uri(url);

  if (!parsed) {
    return false;
  }

  std::string host(parsed->host());

  for (char& c: host) {
    c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  }

  return host == "127.0.0.1" || host == "localhost" || host == "::1" ||
      host == "[::1]" || host == "0.0.0.0";
}

// What one backend answered: the hits, a note about a cached/stale answer, and a
// human-readable reason when it produced none.
struct SearchOutcome {
  std::vector<SearchResult> results;
  std::string note;
  std::string message;
};

std::string searxng_error_message(const std::string& body, int status) {
  boost::system::error_code error;
  const boost::json::value parsed = boost::json::parse(body, error);

  if (!error && parsed.is_object()) {
    const boost::json::object& object = parsed.as_object();
    const std::string message = json_string_or(object, "error");
    const boost::json::value* challenge_value = object.if_contains("challenge");
    const bool challenge = challenge_value != nullptr &&
        challenge_value->is_bool() && challenge_value->as_bool();

    if (!message.empty()) {
      if (challenge) {
        return message +
            " Run `python scripts/search_bridge.py solve` to clear the check, "
            "then retry.";
      }

      return message;
    }
  }

  return "HTTP " + std::to_string(status);
}

SearchOutcome search_searxng(
    const SearchSettings& settings, const std::string& query,
    const std::optional<std::string>& proxy
) {
  std::string base = trim(settings.endpoint);

  while (!base.empty() && base.back() == '/') {
    base.pop_back();
  }

  // A loopback endpoint must bypass the proxy, which would otherwise intercept
  // the request to this machine.
  const std::optional<std::string> effective =
      is_loopback_url(base) ? std::nullopt : proxy;

  const std::string url = base + "/search?q=" + url_encode(query) + "&format=json";
  const HttpResponse response =
      http_get_all(url, effective, kFetchBodyLimit, kJsonHeaders, 180'000);

  if (response.status < 200 || response.status >= 300) {
    throw std::runtime_error(searxng_error_message(response.body, response.status));
  }

  SearchOutcome outcome;
  outcome.results = parse_searxng_results(response.body);

  if (outcome.results.empty()) {
    outcome.message = "no results";
    return outcome;
  }

  // A cached answer is slightly outdated but stable; saying so lets the model
  // treat a repeated question as converging instead of contradicting itself.
  boost::system::error_code error;
  const boost::json::value parsed = boost::json::parse(response.body, error);

  if (!error && parsed.is_object()) {
    const boost::json::value* lnb = parsed.as_object().if_contains("lnb");

    if (lnb != nullptr && lnb->is_object()) {
      const boost::json::object& note = lnb->as_object();
      const boost::json::value* stale_value = note.if_contains("stale");
      const bool stale = stale_value != nullptr && stale_value->is_bool() &&
          stale_value->as_bool();

      if (stale) {
        const int days = json_int_or(note, "age_days", 0);
        outcome.note =
            "[These results came from the local search cache, about " +
            std::to_string(days) + " day(s) old; the engines could not be "
            "reached.]";
      }
    }
  }

  return outcome;
}

} // namespace

std::vector<SearchResult> parse_searxng_results(const std::string& json) {
  std::vector<SearchResult> out;

  boost::system::error_code error;
  const boost::json::value parsed = boost::json::parse(json, error);

  if (error || !parsed.is_object()) {
    return out;
  }

  const boost::json::value* results = parsed.as_object().if_contains("results");

  if (results == nullptr || !results->is_array()) {
    return out;
  }

  for (const boost::json::value& entry: results->as_array()) {
    if (!entry.is_object()) {
      continue;
    }

    const boost::json::object& object = entry.as_object();
    SearchResult result;
    result.title = json_string_or(object, "title");
    result.url = json_string_or(object, "url");
    result.snippet = json_string_or(object, "content");

    if (result.title.empty() || result.url.empty()) {
      continue;
    }

    out.push_back(std::move(result));
  }

  return out;
}

std::string web_search(
    const std::string& query, const std::optional<std::string>& proxy,
    const SearchSettings& settings
) {
  const std::string trimmed_query = trim(query);

  if (trimmed_query.empty()) {
    throw std::runtime_error("A search query is required");
  }

  // With no endpoint configured there is nowhere to send the query. Report it as
  // a plain message rather than an exception, so the model can ask the user to
  // set one instead of treating the tool as broken.
  if (trim(settings.endpoint).empty()) {
    return "Web search is not configured. Ask the user to set the assistant's "
        "Search endpoint to a running SearXNG-compatible service (for example "
        "the bundled scripts/search_bridge.py at http://127.0.0.1:8873), then "
        "retry.";
  }

  try {
    const SearchOutcome outcome = search_searxng(settings, trimmed_query, proxy);

    if (!outcome.results.empty()) {
      std::string text = format_search_results(outcome.results);

      if (!outcome.note.empty()) {
        text = outcome.note + "\n\n" + text;
      }

      return text;
    }

    return "Web search returned " +
        (outcome.message.empty() ? std::string("no results") : outcome.message) +
        ". Try fetch_url on a known URL, or rephrase the query.";
  } catch (const std::exception& error) {
    return "Web search failed (" + std::string(error.what()) +
        "). Try fetch_url on a known URL, or check the assistant's Search "
        "endpoint setting.";
  }
}

namespace {

// Builds one property object.
boost::json::object property(std::string type, std::string description) {
  boost::json::object result;
  result["type"] = std::move(type);

  if (!description.empty()) {
    result["description"] = std::move(description);
  }

  return result;
}

// Wraps a function schema in the Chat shape (`{type, function}`).
boost::json::object chat_tool(boost::json::object function) {
  boost::json::object wrapper;
  wrapper["type"] = "function";
  wrapper["function"] = std::move(function);
  return wrapper;
}

} // namespace

boost::json::array tool_definitions(const ToolPermissions& permissions) {
  if (!permissions.enabled) {
    return {};
  }

  // Filesystem tools: only offered when the session has a shared directory.
  //
  // The model sees a synthetic tree of relative paths, never the configured
  // directories themselves. A single directory is mounted at the root; when
  // several are configured each gets a visible top-level name.
  const std::string path_help =
      "A path relative to the configured shared directories. When several "
      "directories are configured, the first segment names one of them; call "
      "list_directory with no path to see the names. Absolute paths are not "
      "accepted.";

  boost::json::object list_function;
  list_function["name"] = "list_directory";
  list_function["description"] =
      "List files and subdirectories immediately inside a directory of the "
      "configured shared directories. Directories end with /. Call it with no "
      "arguments to list the top level: the configured directories when there "
      "are several, or the contents of the single one.";
  {
    boost::json::object properties;
    properties["path"] = property(
        "string", "Directory to list. " + path_help + " Omit to list the top level."
    );

    boost::json::object parameters;
    parameters["type"] = "object";
    parameters["properties"] = std::move(properties);
    parameters["required"] = boost::json::array{};
    parameters["additionalProperties"] = false;

    list_function["parameters"] = std::move(parameters);
  }

  boost::json::object search_function;
  search_function["name"] = "search_code";
  search_function["description"] =
      "Search text files in the configured shared directories. Returns matching "
      "file paths, line numbers, and surrounding context. Matching is "
      "case-insensitive unless the query contains an uppercase letter; binary "
      "files are skipped.";
  {
    boost::json::object properties;
    properties["query"] = property("string", "Symbol name or text to search for");
    properties["path"] = property(
        "string",
        "Directory to search. " + path_help + " Omit to search every configured directory."
    );

    boost::json::object parameters;
    parameters["type"] = "object";
    parameters["properties"] = std::move(properties);
    parameters["required"] = boost::json::array{"query"};
    parameters["additionalProperties"] = false;

    search_function["parameters"] = std::move(parameters);
  }

  boost::json::object search_regex_function;
  search_regex_function["name"] = "search_code_regex";
  search_regex_function["description"] =
      "Search text files in the configured shared directories with a regular "
      "expression (PCRE syntax), honouring .gitignore/.ignore files and "
      "skipping hidden entries. Returns matching file paths, line numbers and "
      "surrounding context. Matching is case-sensitive; add an inline flag "
      "such as (?i) for case-insensitive matching, and \\b for word "
      "boundaries.";
  {
    boost::json::object properties;
    properties["pattern"] =
        property("string", "Regular expression to search for");
    properties["path"] = property(
        "string",
        "Directory to search. " + path_help + " Omit to search every configured directory."
    );
    properties["glob"] = property(
        "string",
        "Optional glob to restrict which files are searched, e.g. \"*.cpp\" or "
        "\"src/**/*.hpp\"."
    );

    boost::json::object parameters;
    parameters["type"] = "object";
    parameters["properties"] = std::move(properties);
    parameters["required"] = boost::json::array{"pattern"};
    parameters["additionalProperties"] = false;

    search_regex_function["parameters"] = std::move(parameters);
  }

  boost::json::object find_files_function;
  find_files_function["name"] = "find_files";
  find_files_function["description"] =
      "List files in the configured shared directories whose path matches a "
      "glob, honouring .gitignore/.ignore files and skipping hidden entries. "
      "Use it to discover files by name or extension without reading their "
      "contents.";
  {
    boost::json::object properties;
    properties["glob"] = property(
        "string", "Glob to match file paths, e.g. \"*.md\" or \"src/**/*.cpp\"."
    );
    properties["path"] = property(
        "string",
        "Directory to search. " + path_help + " Omit to search every configured directory."
    );

    boost::json::object parameters;
    parameters["type"] = "object";
    parameters["properties"] = std::move(properties);
    parameters["required"] = boost::json::array{"glob"};
    parameters["additionalProperties"] = false;

    find_files_function["parameters"] = std::move(parameters);
  }

  boost::json::object read_function;
  read_function["name"] = "read_file";
  read_function["description"] =
      "Read a limited range of lines from a file in the configured shared "
      "directories.";
  {
    boost::json::object properties;
    properties["path"] = property(
        "string", "File to read. " + path_help
    );
    properties["start_line"] = property("integer", "");
    properties["end_line"] = property("integer", "");

    boost::json::object parameters;
    parameters["type"] = "object";
    parameters["properties"] = std::move(properties);
    parameters["required"] =
        boost::json::array{"path", "start_line", "end_line"};
    parameters["additionalProperties"] = false;

    read_function["parameters"] = std::move(parameters);
  }

  boost::json::object write_function;
  write_function["name"] = "write_file";
  write_function["description"] =
      "Create or overwrite a text file in the configured shared directories. "
      "The parent directory must already exist. The whole file is replaced, so "
      "use edit_file for a targeted change.";
  {
    boost::json::object properties;
    properties["path"] = property("string", "File to write. " + path_help);
    properties["content"] = property("string", "Full file content to write");

    boost::json::object parameters;
    parameters["type"] = "object";
    parameters["properties"] = std::move(properties);
    parameters["required"] = boost::json::array{"path", "content"};
    parameters["additionalProperties"] = false;

    write_function["parameters"] = std::move(parameters);
  }

  boost::json::object edit_function;
  edit_function["name"] = "edit_file";
  edit_function["description"] =
      "Replace one exact occurrence of old_text with new_text in a file in the "
      "configured shared directories. Fails when the text is absent or occurs "
      "more than once, so include enough surrounding context to make it unique.";
  {
    boost::json::object properties;
    properties["path"] = property("string", "File to edit. " + path_help);
    properties["old_text"] = property("string", "Exact text to replace");
    properties["new_text"] = property("string", "Replacement text");

    boost::json::object parameters;
    parameters["type"] = "object";
    parameters["properties"] = std::move(properties);
    parameters["required"] =
        boost::json::array{"path", "old_text", "new_text"};
    parameters["additionalProperties"] = false;

    edit_function["parameters"] = std::move(parameters);
  }

  boost::json::object delete_function;
  delete_function["name"] = "delete_file";
  delete_function["description"] =
      "Delete one file in the configured shared directories. Directories are "
      "never removed, and deletion cannot be undone. Use it only when the user "
      "asked for the file to be removed.";
  {
    boost::json::object properties;
    properties["path"] = property("string", "File to delete. " + path_help);

    boost::json::object parameters;
    parameters["type"] = "object";
    parameters["properties"] = std::move(properties);
    parameters["required"] = boost::json::array{"path"};
    parameters["additionalProperties"] = false;

    delete_function["parameters"] = std::move(parameters);
  }

  // Network tools: always offered, they work without a shared directory.
  boost::json::object fetch_function;
  fetch_function["name"] = "fetch_url";
  fetch_function["description"] =
      "Fetch an http/https URL and return its visible text. Use it to read a "
      "web page referenced by the user. HTML is converted to plain text.";
  {
    boost::json::object properties;
    properties["url"] =
        property("string", "Absolute http:// or https:// URL to fetch");

    boost::json::object parameters;
    parameters["type"] = "object";
    parameters["properties"] = std::move(properties);
    parameters["required"] = boost::json::array{"url"};
    parameters["additionalProperties"] = false;

    fetch_function["parameters"] = std::move(parameters);
  }

  boost::json::object web_search_function;
  web_search_function["name"] = "web_search";
  web_search_function["description"] =
      "Search the web and return the top results with their titles, URLs and "
      "snippets. Use it to find sources online; then use fetch_url to read a "
      "result in full.";
  {
    boost::json::object properties;
    properties["query"] = property("string", "Search terms");

    boost::json::object parameters;
    parameters["type"] = "object";
    parameters["properties"] = std::move(properties);
    parameters["required"] = boost::json::array{"query"};
    parameters["additionalProperties"] = false;

    web_search_function["parameters"] = std::move(parameters);
  }

  boost::json::array tools;

  if (permissions.filesystem && permissions.read) {
    tools.emplace_back(chat_tool(std::move(list_function)));
    tools.emplace_back(chat_tool(std::move(search_function)));
    tools.emplace_back(chat_tool(std::move(search_regex_function)));
    tools.emplace_back(chat_tool(std::move(find_files_function)));
    tools.emplace_back(chat_tool(std::move(read_function)));
  }

  if (permissions.filesystem && permissions.write) {
    tools.emplace_back(chat_tool(std::move(write_function)));
    tools.emplace_back(chat_tool(std::move(edit_function)));
    tools.emplace_back(chat_tool(std::move(delete_function)));
  }

  if (permissions.online) {
    tools.emplace_back(chat_tool(std::move(fetch_function)));
    tools.emplace_back(chat_tool(std::move(web_search_function)));
  }

  return tools;
}

namespace {

// One configured directory. `name` is the only identifier the model sees; the
// actual path is never included in tool output.
struct SharedRoot {
  std::string name;
  fs::path path;
  bool available = false;
};

// Builds the synthetic tree's top level. A single directory is exposed at the
// root, so its visible name is unused; several are exposed as named children.
std::vector<SharedRoot> build_shared_roots(
    const std::vector<std::string>& directories
) {
  std::vector<SharedRoot> roots;

  for (const std::string& directory: directories) {
    if (directory.empty()) continue;

    const fs::path configured(directory);
    std::error_code error;
    const fs::path canonical = fs::canonical(configured, error);
    std::error_code type_error;
    const bool available = !error &&
        fs::is_directory(canonical, type_error) && !type_error;

    SharedRoot root;
    root.path = available ? canonical : configured;
    root.available = available;

    // The visible name is the directory's own name, never the path leading to
    // it. Duplicates get a stable suffix so each mount is addressable.
    std::string name =
        (available ? canonical : configured).filename().generic_string();

    if (name.empty() || name == "." || name == "..") {
      name = "root";
    }

    std::string unique = name;

    for (int suffix = 2;; ++suffix) {
      const bool taken = std::any_of(
          roots.begin(), roots.end(),
          [&unique](const SharedRoot& root) { return root.name == unique; }
      );

      if (!taken) break;

      unique = name + "-" + std::to_string(suffix);
    }

    root.name = std::move(unique);
    roots.push_back(std::move(root));
  }

  return roots;
}

// A resolved model path. `relative` is internal to the selected root and
// `display` is the equivalent path in the model-visible tree.
struct PathTarget {
  bool all_roots = false;
  const SharedRoot* root = nullptr;
  std::string relative;
  std::string display;
};

PathTarget resolve_target(
    const std::vector<SharedRoot>& roots, const std::string& requested,
    bool allow_all_roots
) {
  const fs::path input(requested.empty() ? "." : requested);

  if (input.is_absolute()) {
    throw std::runtime_error("Paths must be relative to the shared directories");
  }

  std::string relative = input.lexically_normal().generic_string();

  if (relative == ".") {
    relative.clear();
  }

  if (relative == ".." || relative.rfind("../", 0) == 0) {
    throw std::runtime_error("Path is outside the shared directories");
  }

  PathTarget target;

  if (roots.size() == 1) {
    target.root = &roots.front();
    target.relative = relative;
    target.display = relative;
  } else if (relative.empty()) {
    if (!allow_all_roots) {
      throw std::runtime_error(
          "A specific path is required when several shared directories are "
          "configured"
      );
    }

    target.all_roots = true;
    target.display = ".";
  } else {
    const std::size_t slash = relative.find('/');
    const std::string mounted = relative.substr(0, slash);

    const auto found = std::find_if(
        roots.begin(), roots.end(),
        [&mounted](const SharedRoot& root) { return root.name == mounted; }
    );

    if (found == roots.end()) {
      throw std::runtime_error(
          "Unknown shared directory \"" + mounted +
          "\". Call list_directory with no path to see the available "
          "directories."
      );
    }

    target.root = &*found;
    target.relative = slash == std::string::npos
        ? std::string{}
        : relative.substr(slash + 1);
    target.display = relative;
  }

  if (target.root != nullptr && !target.root->available) {
    if (roots.size() == 1) {
      throw std::runtime_error("The shared directory is unavailable");
    }

    throw std::runtime_error(
        "The shared directory \"" + target.root->name + "\" is unavailable"
    );
  }

  return target;
}

struct SearchSegment {
  const SharedRoot* root;
  std::string relative;
  std::string prefix;
};

struct SearchPlan {
  std::vector<SearchSegment> segments;
  std::string scope; // model-visible description, never an actual path
};

SearchPlan plan_search(
    const std::vector<SharedRoot>& roots, const std::string& requested
) {
  const PathTarget target = resolve_target(roots, requested, true);

  SearchPlan plan;

  if (target.all_roots) {
    plan.scope = "the configured shared directories";

    for (const SharedRoot& root: roots) {
      if (root.available) {
        plan.segments.push_back({&root, {}, root.name});
      }
    }

    if (plan.segments.empty()) {
      throw std::runtime_error("No shared directories are currently accessible");
    }
  } else {
    plan.scope = target.display.empty()
        ? "the shared directory"
        : "\"" + target.display + "\"";
    plan.segments.push_back(
        {target.root, target.relative,
         roots.size() > 1 ? target.root->name : std::string{}}
    );
  }

  return plan;
}

std::string run_search_code(
    const std::vector<SharedRoot>& roots, const std::string& query,
    const std::string& requested
) {
  const SearchPlan plan = plan_search(roots, requested);
  constexpr int kMaxMatches = 30;

  std::string text;
  int matches = 0;
  int files_scanned = 0;
  bool truncated = false;

  for (const SearchSegment& segment: plan.segments) {
    const shark::file::TextSearchResult result = shark::file::search_code(
        query, segment.root->path.string(), segment.relative, segment.prefix
    );

    text += result.text;
    matches += result.matches;
    files_scanned += result.files_scanned;
    truncated = truncated || result.truncated;

    if (matches >= kMaxMatches) {
      truncated = true;
      break;
    }
  }

  if (matches == 0) {
    return "No matches for \"" + query + "\" in " + plan.scope + " (" +
        std::to_string(files_scanned) +
        " text files scanned). Binary files are skipped; try a different term "
        "or a broader path.";
  }

  if (truncated) {
    text += "\n[Result limit reached; narrow the search path or query.]\n";
  }

  return text;
}

std::string run_search_regex(
    const std::vector<SharedRoot>& roots, const std::string& pattern,
    const std::string& requested, const std::string& glob
) {
  const SearchPlan plan = plan_search(roots, requested);
  constexpr int kMaxMatches = 30;

  std::string text;
  int matches = 0;
  int files_scanned = 0;
  bool truncated = false;

  for (const SearchSegment& segment: plan.segments) {
    const shark::file::TextSearchResult result = shark::file::search_regex(
        pattern, segment.root->path.string(), segment.relative, glob,
        segment.prefix
    );

    text += result.text;
    matches += result.matches;
    files_scanned += result.files_scanned;
    truncated = truncated || result.truncated;

    if (matches >= kMaxMatches) {
      truncated = true;
      break;
    }
  }

  if (matches == 0) {
    return "No matches for \"" + pattern + "\" in " + plan.scope + " (" +
        std::to_string(files_scanned) +
        " text files scanned). Binary files and ignored paths are skipped.";
  }

  if (truncated) {
    text +=
        "\n[Result limit reached; narrow the search path, glob or pattern.]\n";
  }

  return text;
}

std::string run_find_files(
    const std::vector<SharedRoot>& roots, const std::string& glob,
    const std::string& requested
) {
  const SearchPlan plan = plan_search(roots, requested);
  constexpr std::size_t kMaxListedFiles = 1000;

  std::vector<std::string> paths;
  std::size_t files_scanned = 0;
  bool truncated = false;

  for (const SearchSegment& segment: plan.segments) {
    shark::file::FileListResult result = shark::file::find_files(
        glob, segment.root->path.string(), segment.relative, segment.prefix
    );

    files_scanned += result.scanned;
    truncated = truncated || result.truncated;
    paths.insert(
        paths.end(), std::make_move_iterator(result.paths.begin()),
        std::make_move_iterator(result.paths.end())
    );
  }

  std::sort(paths.begin(), paths.end());

  if (paths.size() > kMaxListedFiles) {
    paths.resize(kMaxListedFiles);
    truncated = true;
  }

  if (paths.empty()) {
    return "No files match \"" + glob + "\" in " + plan.scope + " (" +
        std::to_string(files_scanned) +
        " files scanned). Hidden and ignored paths are skipped.";
  }

  std::ostringstream out;

  for (const std::string& path: paths) {
    out << path << '\n';
  }

  if (truncated) {
    out << "[File limit reached; narrow the path or glob.]\n";
  }

  return out.str();
}

bool is_top_level_request(const std::string& requested) {
  if (requested.empty()) return true;

  const fs::path input(requested);

  if (input.is_absolute()) return false;

  const std::string normalized = input.lexically_normal().generic_string();
  return normalized.empty() || normalized == ".";
}

std::string run_list_directory(
    const std::vector<SharedRoot>& roots, const std::string& requested
) {
  // With several directories, the top level is the list of mounts. The model
  // picks one by name; a single directory is transparent and its contents are
  // listed directly.
  if (roots.size() > 1 && is_top_level_request(requested)) {
    std::vector<std::string> entries;

    for (const SharedRoot& root: roots) {
      if (root.available) {
        entries.push_back(root.name + "/");
      }
    }

    if (entries.empty()) {
      return "No shared directories are currently accessible.";
    }

    std::sort(entries.begin(), entries.end());

    std::ostringstream out;

    for (const std::string& entry: entries) {
      out << entry << '\n';
    }

    return out.str();
  }

  const PathTarget target = resolve_target(roots, requested, false);
  return shark::file::list_directory(target.root->path.string(), target.relative);
}

} // namespace

std::string execute_local_tool(
    const ToolCall& call, const std::vector<std::string>& shared_directories,
    const std::optional<std::string>& proxy, const ToolPermissions& permissions
) {
  try {
    boost::system::error_code parse_error;
    const boost::json::value parsed =
        boost::json::parse(call.arguments, parse_error);

    if (parse_error || !parsed.is_object()) {
      throw std::runtime_error("Tool arguments must be a JSON object");
    }

    const boost::json::object& args = parsed.as_object();

    if (!permissions.enabled) {
      return "Tool error: tools are disabled for this session";
    }

    const bool is_read_tool = call.name == "list_directory" ||
        call.name == "search_code" || call.name == "search_code_regex" ||
        call.name == "find_files" || call.name == "read_file";
    const bool is_write_tool = call.name == "write_file" ||
        call.name == "edit_file" || call.name == "delete_file";
    const bool is_online_tool =
        call.name == "fetch_url" || call.name == "web_search";

    if (is_read_tool && !permissions.read) {
      return "Tool error: file reading is disabled for this session";
    }

    if (is_write_tool && !permissions.write) {
      return "Tool error: file writing is disabled for this session";
    }

    if (is_online_tool && !permissions.online) {
      return "Tool error: online access is disabled for this session";
    }

    // Filesystem tools need at least one shared directory; network tools do
    // not, so the gate lives here instead of around the whole call.
    const bool needs_root = is_read_tool || is_write_tool;

    const std::vector<SharedRoot> roots = needs_root
        ? build_shared_roots(shared_directories)
        : std::vector<SharedRoot>{};

    if (needs_root && roots.empty()) {
      return "Tool error: this session has no shared directories configured";
    }

    if (call.name == "list_directory") {
      return run_list_directory(roots, json_string_or(args, "path", "."));
    }

    if (call.name == "search_code") {
      const std::string query = json_string_or(args, "query");
      return run_search_code(roots, query, json_string_or(args, "path", "."));
    }

    if (call.name == "search_code_regex") {
      const std::string pattern = json_string_or(args, "pattern");
      const std::string glob = json_string_or(args, "glob");
      return run_search_regex(
          roots, pattern, json_string_or(args, "path", "."), glob
      );
    }

    if (call.name == "find_files") {
      const std::string glob = json_string_or(args, "glob");
      return run_find_files(roots, glob, json_string_or(args, "path", "."));
    }

    if (call.name == "read_file") {
      const std::string path = json_string_or(args, "path");
      const int start = json_int_or(args, "start_line", 1);
      const int end = json_int_or(args, "end_line", start + 100);

      const PathTarget target = resolve_target(roots, path, false);
      return read_file_range(
          shark::file::checked_repo_path(target.root->path.string(), target.relative), start,
          end
      );
    }

    if (call.name == "write_file") {
      const std::string path = json_string_or(args, "path");
      const boost::json::value* content_value = args.if_contains("content");

      if (content_value == nullptr || !content_value->is_string()) {
        throw std::runtime_error("Missing content");
      }

      const std::string content(content_value->as_string());
      const PathTarget target = resolve_target(roots, path, false);
      const fs::path file = checked_repo_path_for_write(
          target.root->path.string(), target.relative
      );

      write_file_contents(file, content);
      return "Wrote " + std::to_string(content.size()) + " bytes to " +
          target.display + ".";
    }

    if (call.name == "edit_file") {
      const std::string path = json_string_or(args, "path");
      const boost::json::value* old_value = args.if_contains("old_text");
      const boost::json::value* new_value = args.if_contains("new_text");

      if (old_value == nullptr || !old_value->is_string() || new_value == nullptr ||
          !new_value->is_string()) {
        throw std::runtime_error("Missing old_text or new_text");
      }

      const std::string old_text(old_value->as_string());
      const std::string new_text(new_value->as_string());

      if (old_text.empty()) {
        throw std::runtime_error("old_text must not be empty");
      }

      const PathTarget target = resolve_target(roots, path, false);
      const fs::path file = shark::file::checked_repo_path(
          target.root->path.string(), target.relative
      );
      std::string content = read_whole_file(file);
      const std::size_t first = content.find(old_text);

      if (first == std::string::npos) {
        throw std::runtime_error("The text to replace was not found");
      }

      if (content.find(old_text, first + old_text.size()) != std::string::npos) {
        throw std::runtime_error(
            "The text to replace occurs more than once; include more context"
        );
      }

      const auto line = 1 + std::count(
          content.begin(), content.begin() + static_cast<std::ptrdiff_t>(first),
          '\n'
      );

      content.replace(first, old_text.size(), new_text);
      write_file_contents(file, content);

      return "Replaced one occurrence at line " + std::to_string(line) +
          " in " + target.display + ".";
    }

    if (call.name == "delete_file") {
      const std::string path = json_string_or(args, "path");
      const PathTarget target = resolve_target(roots, path, false);
      const fs::path file = checked_repo_path_for_delete(
          target.root->path.string(), target.relative
      );

      std::error_code error;
      if (!fs::remove(file, error) || error) {
        throw std::runtime_error("Cannot remove file");
      }

      return "Deleted " + target.display + ".";
    }

    if (call.name == "fetch_url") {
      const std::string url = json_string_or(args, "url");
      return fetch_url(url, proxy);
    }

    if (call.name == "web_search") {
      const std::string query = json_string_or(args, "query");
      return web_search(query, proxy, permissions.search);
    }

    throw std::runtime_error("Unknown tool: " + call.name);
  } catch (const fs::filesystem_error&) {
    // A filesystem exception carries an actual path in its message, so it is
    // deliberately replaced with a path-free message before reaching the model.
    return "Tool error: The path could not be accessed";
  } catch (const std::exception& e) {
    // Tool errors are returned to the model so it can retry with better
    // arguments instead of aborting the whole generation.
    return "Tool error: " + std::string(e.what());
  }
}

} // namespace lnb::core
