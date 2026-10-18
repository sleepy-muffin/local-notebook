#include "net/tools.hpp"

#include "net/http.hpp"
#include "net/search.hpp"
#include "net/search_results.hpp"

#include <boost/json/parse.hpp>
#include <boost/json/value.hpp>
#include <boost/regex.hpp>
#include <boost/system/error_code.hpp>
#include <boost/url.hpp>

#include <algorithm>
#include <array>
#include <cctype>
#include <fstream>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace lnb::net {

namespace {

// ASCII lower-case, used by the smart-case search. A locale-independent fold
// keeps the matching behaviour stable across platforms.
std::string to_lower(const std::string& text) {
  std::string result = text;
  std::transform(result.begin(), result.end(), result.begin(), [](unsigned char c) {
    return static_cast<char>(std::tolower(c));
  });
  return result;
}

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

  if (fs::file_size(file) > 2 * 1024 * 1024) {
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

} // namespace

fs::path checked_repo_path(
    const std::string& root_str, const std::string& relative
) {
  const fs::path root = fs::canonical(root_str);

  // Models routinely pass the shared directory verbatim, so accept absolute
  // paths too and let the containment check below decide whether they are
  // allowed. A relative path is resolved against the root as before.
  const fs::path input(relative);
  const fs::path candidate = fs::canonical(input.is_absolute() ? input : root / input);

  // fs::canonical() resolves symlinks, so verify the final path still lives
  // inside the root before handing it back.
  const fs::path rel = candidate.lexically_relative(root);
  if (rel.empty() || rel.is_absolute() || *rel.begin() == "..") {
    throw std::runtime_error("Path is outside the shared directory");
  }

  return candidate;
}

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

bool is_binary_file(const fs::path& path) {
  // A file counts as binary when a NUL byte appears in its first block. This is
  // the same convention git and ripgrep use, and it lets search_code cover
  // every text language without maintaining an extension whitelist.
  std::ifstream in(path, std::ios::binary);
  if (!in) {
    return true;
  }

  std::array<char, 8192> buffer{};
  in.read(buffer.data(), static_cast<std::streamsize>(buffer.size()));
  const std::streamsize count = in.gcount();

  return std::find(buffer.begin(), buffer.begin() + count, '\0') !=
      buffer.begin() + count;
}

std::string list_directory(
    const std::string& root_path, const std::string& relative_path
) {
  const fs::path directory = checked_repo_path(root_path, relative_path);

  if (!fs::is_directory(directory)) {
    throw std::runtime_error("Path is not a directory");
  }

  std::vector<std::string> entries;
  constexpr std::size_t MAX_ENTRIES = 500;

  for (const fs::directory_entry& entry: fs::directory_iterator(
           directory, fs::directory_options::skip_permission_denied
       )) {
    const std::string name = entry.path().filename().generic_string();
    entries.push_back(entry.is_directory() ? name + "/" : name);

    if (entries.size() >= MAX_ENTRIES) {
      break;
    }
  }

  std::sort(entries.begin(), entries.end());

  std::ostringstream out;
  if (entries.empty()) {
    return "Directory is empty.";
  }

  for (const std::string& entry: entries) {
    out << entry << '\n';
  }

  if (entries.size() == MAX_ENTRIES) {
    out << "[Entry limit reached; narrow the directory path.]\n";
  }

  return out.str();
}

std::string search_code(
    const std::string& query, const std::string& root_path,
    const std::string& relative_path
) {
  if (query.empty() || query.size() > 256) {
    throw std::runtime_error("Invalid search query");
  }

  const fs::path directory = checked_repo_path(root_path, relative_path);

  if (!fs::is_directory(directory)) {
    throw std::runtime_error("Search path is not a directory");
  }

  // Smart case: a query without uppercase letters matches case-insensitively,
  // mirroring ripgrep/Zed. This only ever adds matches; an all-lowercase query
  // that the model would otherwise miss now succeeds.
  const bool case_sensitive = std::any_of(
      query.begin(), query.end(),
      [](unsigned char c) { return std::isupper(c) != 0; }
  );
  const std::string needle = case_sensitive ? query : to_lower(query);

  std::ostringstream out;
  int matches = 0;
  int files_scanned = 0;
  constexpr int MAX_MATCHES = 30;
  constexpr int CONTEXT_LINES = 2;

  for (fs::recursive_directory_iterator it(
           directory, fs::directory_options::skip_permission_denied
       ),
       end;
       it != end && matches < MAX_MATCHES;
       ++it) {
    try {
      if (it->is_symlink()) {
        if (it->is_directory()) it.disable_recursion_pending();
        continue;
      }

      if (!it->is_regular_file()) {
        continue;
      }

      if (it->file_size() > 2 * 1024 * 1024) {
        continue;
      }

      if (is_binary_file(it->path())) {
        continue;
      }

      ++files_scanned;

      std::ifstream in(it->path());
      if (!in) continue;

      std::vector<std::string> lines;
      std::string line;
      while (std::getline(in, line)) {
        lines.push_back(std::move(line));
        if (lines.size() > 200000) break;
      }

      for (std::size_t i = 0; i < lines.size() && matches < MAX_MATCHES; ++i) {
        const std::string haystack = case_sensitive ? lines[i] : to_lower(lines[i]);
        if (haystack.find(needle) == std::string::npos) {
          continue;
        }

        const fs::path root = fs::canonical(root_path);
        const auto relative = fs::relative(it->path(), root).generic_string();

        out << "\n--- " << relative << ":" << (i + 1) << " ---\n";

        const std::size_t begin = i > CONTEXT_LINES ? i - CONTEXT_LINES : 0;
        const std::size_t finish = std::min(lines.size() - 1, i + CONTEXT_LINES);

        for (std::size_t j = begin; j <= finish; ++j) {
          out << (j + 1) << ": " << lines[j] << '\n';
        }

        ++matches;
      }
    } catch (const fs::filesystem_error&) {
      // Skip files that cannot be read (permissions, races, ...).
      continue;
    }
  }

  if (matches == 0) {
    // Report the resolved directory and how much was scanned so the model can
    // discover the real search root and retry instead of assuming the tool is
    // broken. Binary files are skipped by design.
    std::ostringstream message;
    message << "No matches for \"" << query << "\" in \""
            << directory.generic_string() << "\" (" << files_scanned
            << " text files scanned). Binary files are skipped; try a "
               "different term or a broader path.";
    return message.str();
  }

  if (matches == MAX_MATCHES) {
    out << "\n[Result limit reached; narrow the search path or query.]\n";
  }

  return out.str();
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

  return trim(decode_entities(text));
}

// Reads a `name="value"` attribute from a tag's attribute string.
std::string attribute(const std::string& attributes, const std::string& name) {
  static const boost::regex quoted(
      "\\b([a-zA-Z:_-]+)\\s*=\\s*\"([^\"]*)\"", boost::regex_constants::perl
  );

  for (boost::sregex_iterator it(attributes.begin(), attributes.end(), quoted), end;
       it != end; ++it) {
    if (equals_ignore_case((*it)[1].str(), name)) {
      return (*it)[2].str();
    }
  }

  return {};
}

// One element carrying a class marker (DuckDuckGo result link or snippet).
struct MarkedElement {
  std::string href;
  std::string text;
};

// Collects every anchor whose tag carries `marker` in its class attribute.
//
// Only `<a>` elements are considered: a generic "any element" match would let a
// large container (e.g. `<body>`) swallow the anchors nested inside it, so the
// result links would never be seen individually.
std::vector<MarkedElement> collect_marked(
    const std::string& html, const std::string& marker
) {
  static const boost::regex anchor(
      "<a\\b([^>]*)>(.*?)</a>",
      boost::regex_constants::perl | boost::regex_constants::icase |
          boost::regex_constants::mod_s
  );

  const std::string lowered_marker = to_lower(marker);

  std::vector<MarkedElement> out;

  for (boost::sregex_iterator it(html.begin(), html.end(), anchor), end;
       it != end; ++it) {
    const std::string attributes = (*it)[1].str();

    if (to_lower(attributes).find(lowered_marker) == std::string::npos) {
      continue;
    }

    MarkedElement item;
    item.href = attribute(attributes, "href");
    item.text = html_to_text((*it)[2].str());
    out.push_back(std::move(item));
  }

  return out;
}

// Recovers the real target from a DuckDuckGo redirect link (`//duckduckgo.com/
// l/?uddg=<encoded>`); a direct link is returned unchanged.
std::string resolve_ddg_url(const std::string& href) {
  std::string candidate = href;

  if (candidate.starts_with("//")) {
    candidate = "https:" + candidate;
  }

  const auto result = boost::urls::parse_uri(candidate);

  if (!result) {
    return candidate;
  }

  for (const auto& param: result->params()) {
    if (param.key == "uddg") {
      return std::string(param.value);
    }
  }

  return candidate;
}

} // namespace

bool is_ddg_challenge(const std::string& html) {
  const std::string lowered = to_lower(html);

  // DuckDuckGo answers its bot-mitigation page with HTTP 202, so the response
  // status cannot distinguish it from a result page. These markers are unique
  // to that page ("anomaly" alone would also match a query for that word).
  return lowered.find("anomaly-modal") != std::string::npos ||
      lowered.find("bots use duckduckgo") != std::string::npos ||
      lowered.find("select all squares containing a duck") != std::string::npos;
}

std::vector<SearchResult> parse_ddg_results(const std::string& html) {
  const auto links = collect_marked(html, "result__a");
  const auto snippets = collect_marked(html, "result__snippet");

  std::vector<SearchResult> out;
  out.reserve(links.size());

  for (std::size_t i = 0; i < links.size(); ++i) {
    SearchResult result;
    result.title = links[i].text;
    result.url = resolve_ddg_url(links[i].href);

    if (result.title.empty() || result.url.empty()) {
      continue;
    }

    if (i < snippets.size()) {
      result.snippet = snippets[i].text;
    }

    out.push_back(std::move(result));
  }

  return out;
}

std::vector<SearchResult> parse_brave_results(const std::string& html) {
  static const boost::regex anchor(
      "<a\\b([^>]*)>(.*?)</a>",
      boost::regex_constants::perl | boost::regex_constants::icase |
          boost::regex_constants::mod_s
  );
  static const boost::regex title_tag(
      "<[^>]*\\sclass\\s*=\\s*\"[^\"]*\\bsearch-snippet-title\\b[^\"]*\"[^>]*>",
      boost::regex_constants::perl | boost::regex_constants::icase
  );
  static const boost::regex snippet_block(
      "<div\\b[^>]*\\sclass\\s*=\\s*\"[^\"]*\\bgeneric-snippet\\b[^\"]*\""
      "[^>]*>\\s*<div\\b[^>]*\\sclass\\s*=\\s*\"[^\"]*\\bcontent\\b"
      "[^\"]*\"[^>]*>(.*?)</div>",
      boost::regex_constants::perl | boost::regex_constants::icase |
          boost::regex_constants::mod_s
  );

  struct Anchor {
    std::string attributes;
    std::string inner;
    std::size_t begin = 0;
    std::size_t end = 0;
  };

  std::vector<Anchor> anchors;

  for (boost::sregex_iterator it(html.begin(), html.end(), anchor), end;
       it != end; ++it) {
    const boost::smatch& match = *it;
    const std::size_t begin =
        static_cast<std::size_t>(match.position());

    anchors.push_back({
        match[1].str(),
        match[2].str(),
        begin,
        begin + static_cast<std::size_t>(match.length(0)),
    });
  }

  // Only anchors wrapping a result title are results; the page has many other
  // anchors (navigation, thumbnails, related queries).
  std::vector<std::size_t> result_anchors;

  for (std::size_t i = 0; i < anchors.size(); ++i) {
    if (anchors[i].inner.find("search-snippet-title") != std::string::npos) {
      result_anchors.push_back(i);
    }
  }

  std::vector<SearchResult> out;
  out.reserve(result_anchors.size());

  for (std::size_t n = 0; n < result_anchors.size(); ++n) {
    const Anchor& anchor = anchors[result_anchors[n]];

    SearchResult result;
    result.url = decode_entities(attribute(anchor.attributes, "href"));

    if (result.url.starts_with("//")) {
      result.url = "https:" + result.url;
    }

    if (!result.url.starts_with("http://") &&
        !result.url.starts_with("https://")) {
      continue;
    }

    const auto parsed = boost::urls::parse_uri(result.url);

    if (parsed) {
      const std::string host(parsed->host());

      if (equals_ignore_case(host, "search.brave.com")) {
        continue;
      }
    }

    boost::smatch title_match;

    if (boost::regex_search(anchor.inner, title_match, title_tag)) {
      result.title = decode_entities(attribute(title_match.str(), "title"));
    }

    if (result.title.empty()) {
      result.title = html_to_text(anchor.inner);
    }

    if (result.title.empty()) {
      continue;
    }

    // The snippet follows the anchor. Scope it to the text before the next
    // result anchor so one result cannot pick up the next result's snippet.
    const std::size_t block_end = n + 1 < result_anchors.size()
        ? anchors[result_anchors[n + 1]].begin
        : html.size();

    if (anchor.end < block_end) {
      const std::string block = html.substr(anchor.end, block_end - anchor.end);
      boost::smatch snippet_match;

      if (boost::regex_search(block, snippet_match, snippet_block)) {
        result.snippet = html_to_text(snippet_match[1].str());
      }
    }

    out.push_back(std::move(result));
  }

  return out;
}

std::vector<SearchResult> parse_bing_rss(const std::string& xml) {
  static const boost::regex item(
      "<item\\b[^>]*>(.*?)</item>",
      boost::regex_constants::perl | boost::regex_constants::icase |
          boost::regex_constants::mod_s
  );
  static const boost::regex title_tag(
      "<title\\b[^>]*>(.*?)</title>",
      boost::regex_constants::perl | boost::regex_constants::icase |
          boost::regex_constants::mod_s
  );
  static const boost::regex link_tag(
      "<link\\b[^>]*>(.*?)</link>",
      boost::regex_constants::perl | boost::regex_constants::icase |
          boost::regex_constants::mod_s
  );
  static const boost::regex description_tag(
      "<description\\b[^>]*>(.*?)</description>",
      boost::regex_constants::perl | boost::regex_constants::icase |
          boost::regex_constants::mod_s
  );

  // Reads one child element and unwraps a CDATA section when present, so the
  // markup inside a description is not mistaken for XML tags.
  const auto field = [](const std::string& block, const boost::regex& tag) {
    boost::smatch match;

    if (!boost::regex_search(block, match, tag)) {
      return std::string{};
    }

    std::string value = match[1].str();
    constexpr std::string_view kCdataOpen = "<![CDATA[";

    if (value.starts_with(kCdataOpen)) {
      const std::size_t close = value.rfind("]]>");

      if (close != std::string::npos) {
        value = value.substr(kCdataOpen.size(), close - kCdataOpen.size());
      }
    }

    return value;
  };

  std::vector<SearchResult> out;

  for (boost::sregex_iterator it(xml.begin(), xml.end(), item), end;
       it != end; ++it) {
    const std::string block = (*it)[1].str();

    SearchResult result;
    result.title = html_to_text(field(block, title_tag));
    result.url = decode_entities(field(block, link_tag));
    result.snippet = html_to_text(field(block, description_tag));

    if (result.title.empty() || result.url.empty()) {
      continue;
    }

    if (!result.url.starts_with("http://") &&
        !result.url.starts_with("https://")) {
      continue;
    }

    // The feed's own navigation links point back at Bing; they are not results.
    const auto parsed = boost::urls::parse_uri(result.url);

    if (parsed) {
      const std::string host(parsed->host());

      if (equals_ignore_case(host, "bing.com") ||
          equals_ignore_case(host, "www.bing.com")) {
        continue;
      }
    }

    out.push_back(std::move(result));
  }

  return out;
}

// Public search and content endpoints reject non-browser clients: DuckDuckGo
// serves its bot-challenge page to the plain `local-notebook` User-Agent that
// `http_get_body` sends by default. These headers make the request look like an
// ordinary page load, which is what keeps the HTML result pages readable.
const std::vector<HttpHeader> kBrowserHeaders{
    {"User-Agent",
     "Mozilla/5.0 (X11; Linux x86_64) AppleWebKit/537.36 (KHTML, like Gecko) "
     "Chrome/124.0 Safari/537.36"},
    {"Accept", "text/html,application/xhtml+xml,application/xml;q=0.9,*/*;q=0.8"},
    {"Accept-Language", "en-US,en;q=0.9"},
};

std::string http_get(
    const std::string& url, const std::optional<std::string>& proxy, int max_bytes
) {
  return http_get_body(url, proxy, max_bytes, std::stop_token{}, kBrowserHeaders);
}

std::string fetch_url(
    const std::string& url, const std::optional<std::string>& proxy
) {
  const std::string trimmed_url = trim(url);

  if (trimmed_url.empty()) {
    throw std::runtime_error("A URL is required");
  }

  const std::string text = html_to_text(http_get(trimmed_url, proxy, kFetchBodyLimit));

  if (text.empty()) {
    return "The page returned no readable text.";
  }

  if (text.size() > static_cast<std::size_t>(kFetchTextLimit)) {
    return text.substr(0, kFetchTextLimit) +
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

} // namespace

std::string web_search(
    const std::string& query, const std::optional<std::string>& proxy
) {
  const std::string trimmed_query = trim(query);

  if (trimmed_query.empty()) {
    throw std::runtime_error("A search query is required");
  }

  std::vector<std::string> failures;

  // DuckDuckGo's HTML endpoint is the primary source, but it answers automated
  // traffic with a bot-challenge page (HTTP 202), so a parsed page is the only
  // reliable success signal.
  try {
    const std::string url =
        "https://html.duckduckgo.com/html/?q=" + url_encode(trimmed_query);
    const std::string html = http_get(url, proxy, kFetchBodyLimit);

    if (is_ddg_challenge(html)) {
      failures.emplace_back("DuckDuckGo returned a bot challenge");
    } else {
      const auto results = parse_ddg_results(html);

      if (!results.empty()) {
        return format_search_results(results);
      }

      failures.emplace_back("DuckDuckGo returned no parseable results");
    }
  } catch (const std::exception& error) {
    failures.emplace_back(std::string("DuckDuckGo failed: ") + error.what());
  }

  // Bing's RSS feed is the second source. Unlike the HTML page it is designed
  // for machine consumption, needs no JavaScript, and answers HTTP/1.1. The
  // market parameters keep a geo-located proxy from returning local results.
  try {
    const std::string url = "https://www.bing.com/search?q=" +
        url_encode(trimmed_query) +
        "&format=rss&mkt=en-US&setlang=en-US&cc=US";
    const std::string xml = http_get(url, proxy, kFetchBodyLimit);
    const auto results = parse_bing_rss(xml);

    if (!results.empty()) {
      return format_search_results(results);
    }

    failures.emplace_back("Bing returned no parseable results");
  } catch (const std::exception& error) {
    failures.emplace_back(std::string("Bing failed: ") + error.what());
  }

  // Brave is kept last: it serves HTTP/2 normally but rate-limits (HTTP 429)
  // many HTTP/1.1 clients, so it is the least reliable of the three here.
  try {
    const std::string url = "https://search.brave.com/search?q=" +
        url_encode(trimmed_query) + "&source=web";
    const std::string html = http_get(url, proxy, kFetchBodyLimit);
    const auto results = parse_brave_results(html);

    if (!results.empty()) {
      return format_search_results(results);
    }

    failures.emplace_back("Brave returned no parseable results");
  } catch (const std::exception& error) {
    failures.emplace_back(std::string("Brave failed: ") + error.what());
  }

  std::ostringstream out;
  out << "Web search failed (";

  for (std::size_t i = 0; i < failures.size(); ++i) {
    if (i != 0) {
      out << "; ";
    }

    out << failures[i];
  }

  out << "). Try fetch_url on a known URL, or check the assistant's proxy settings.";

  return out.str();
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

boost::json::array tool_definitions(bool filesystem) {
  // Filesystem tools: only offered when the session has a shared directory.
  boost::json::object list_function;
  list_function["name"] = "list_directory";
  list_function["description"] =
      "List files and subdirectories immediately inside a directory in the "
      "configured shared directory. Directories end with /. Call it with no "
      "arguments to list the shared directory itself.";
  {
    boost::json::object properties;
    properties["path"] = property(
        "string",
        "Directory to list: either a path relative to the shared directory or an "
        "absolute path inside it. Omit to list the shared directory."
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
      "Search text files in the configured shared directory. Returns matching "
      "file paths, line numbers, and surrounding context. Matching is "
      "case-insensitive unless the query contains an uppercase letter; binary "
      "files are skipped.";
  {
    boost::json::object properties;
    properties["query"] = property("string", "Symbol name or text to search for");
    properties["path"] = property(
        "string",
        "Directory to search: a path relative to the shared directory or an "
        "absolute path inside it. Defaults to the shared directory."
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
      "Search text files in the configured shared directory with a regular "
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
        "Directory to search: a path relative to the shared directory or an "
        "absolute path inside it. Defaults to the shared directory."
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
      "List files in the configured shared directory whose path matches a "
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
        "Directory to search: a path relative to the shared directory or an "
        "absolute path inside it. Defaults to the shared directory."
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
      "Read a limited range of lines from a file in the configured local "
      "directories.";
  {
    boost::json::object properties;
    properties["path"] = property(
        "string",
        "File to read: a path relative to the shared directory or an absolute "
        "path inside it"
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

  if (filesystem) {
    tools.emplace_back(chat_tool(std::move(list_function)));
    tools.emplace_back(chat_tool(std::move(search_function)));
    tools.emplace_back(chat_tool(std::move(search_regex_function)));
    tools.emplace_back(chat_tool(std::move(find_files_function)));
    tools.emplace_back(chat_tool(std::move(read_function)));
  }

  tools.emplace_back(chat_tool(std::move(fetch_function)));
  tools.emplace_back(chat_tool(std::move(web_search_function)));

  return tools;
}

std::string execute_local_tool(
    const ToolCall& call, const std::optional<std::string>& root_path,
    const std::optional<std::string>& proxy
) {
  try {
    boost::system::error_code parse_error;
    const boost::json::value parsed =
        boost::json::parse(call.arguments, parse_error);

    if (parse_error || !parsed.is_object()) {
      throw std::runtime_error("Tool arguments must be a JSON object");
    }

    const boost::json::object& args = parsed.as_object();

    // Filesystem tools need a shared directory; network tools do not.
    const bool needs_root = call.name == "list_directory" ||
        call.name == "search_code" || call.name == "search_code_regex" ||
        call.name == "find_files" || call.name == "read_file";

    if (needs_root && (!root_path.has_value() || root_path->empty())) {
      return "Tool error: this session has no shared directory configured";
    }

    if (call.name == "list_directory") {
      const std::string path = json_string_or(args, "path", ".");
      return list_directory(*root_path, path);
    }

    if (call.name == "search_code") {
      const std::string query = json_string_or(args, "query");
      const std::string path = json_string_or(args, "path", ".");
      return search_code(query, *root_path, path);
    }

    if (call.name == "search_code_regex") {
      const std::string pattern = json_string_or(args, "pattern");
      const std::string path = json_string_or(args, "path", ".");
      const std::string glob = json_string_or(args, "glob");
      return search_regex(pattern, *root_path, path, glob);
    }

    if (call.name == "find_files") {
      const std::string glob = json_string_or(args, "glob");
      const std::string path = json_string_or(args, "path", ".");
      return find_files(glob, *root_path, path);
    }

    if (call.name == "read_file") {
      const std::string path = json_string_or(args, "path");
      const int start = json_int_or(args, "start_line", 1);
      const int end = json_int_or(args, "end_line", start + 100);

      return read_file_range(checked_repo_path(*root_path, path), start, end);
    }

    if (call.name == "fetch_url") {
      const std::string url = json_string_or(args, "url");
      return fetch_url(url, proxy);
    }

    if (call.name == "web_search") {
      const std::string query = json_string_or(args, "query");
      return web_search(query, proxy);
    }

    throw std::runtime_error("Unknown tool: " + call.name);
  } catch (const std::exception& e) {
    // Tool errors are returned to the model so it can retry with better
    // arguments instead of aborting the whole generation.
    return "Tool error: " + std::string(e.what());
  }
}

} // namespace lnb::net
