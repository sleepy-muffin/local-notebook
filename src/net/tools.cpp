#include "net/tools.hpp"
#include "net/proxy.hpp"
#include "net/search.hpp"

#include <QEventLoop>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonParseError>
#include <QJsonValue>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QRegularExpression>
#include <QUrl>
#include <QUrlQuery>

#include <algorithm>
#include <array>
#include <cctype>
#include <fstream>
#include <sstream>
#include <stdexcept>
#include <string>
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

fs::path checked_repo_path(const std::string& root_str, const std::string& relative) {
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

QString json_string_or(
    const QJsonObject& object, const QString& key, const QString& fallback
) {
  const QJsonValue value = object.value(key);
  return value.isString() ? value.toString() : fallback;
}

int json_int_or(const QJsonObject& object, const QString& key, int fallback) {
  const QJsonValue value = object.value(key);
  return value.isDouble() ? value.toInt(fallback) : fallback;
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
//
// The model can reach the network through two best-effort tools. Requests go
// through the session proxy when one is configured (see proxy.hpp). The
// implementation mirrors the streaming client: a QNetworkAccessManager driven by
// a nested QEventLoop on the calling worker thread, so a tool call blocks only
// that thread and stays bounded by the transfer timeout.

constexpr int kFetchBodyLimit = 512 * 1024; // bytes read from one response
constexpr int kFetchTextLimit = 8000; // characters returned to the model

namespace {

// Decodes the HTML entities that occur in ordinary page text. Numeric
// references are decoded before `&amp;` so a literal "&amp;#39;" is preserved.
QString decode_entities(const QString& input) {
  QString text = input;
  text.replace(QStringLiteral("&nbsp;"), QStringLiteral(" "));
  text.replace(QStringLiteral("&lt;"), QStringLiteral("<"));
  text.replace(QStringLiteral("&gt;"), QStringLiteral(">"));
  text.replace(QStringLiteral("&quot;"), QStringLiteral("\""));
  text.replace(QStringLiteral("&#39;"), QStringLiteral("'"));
  text.replace(QStringLiteral("&apos;"), QStringLiteral("'"));

  static const QRegularExpression numeric(QStringLiteral("&#(x?[0-9A-Fa-f]+);"));

  QString decoded;
  decoded.reserve(text.size());

  qsizetype last = 0;
  auto it = numeric.globalMatch(text);

  while (it.hasNext()) {
    const QRegularExpressionMatch match = it.next();
    const QString body = match.captured(1);

    bool ok = false;
    const uint code = (body.startsWith('x') || body.startsWith('X'))
        ? body.mid(1).toUInt(&ok, 16)
        : body.toUInt(&ok, 10);

    if (!ok || code == 0 || code > 0x10FFFF) {
      continue;
    }

    decoded += text.mid(last, match.capturedStart() - last);

    if (code <= 0xFFFF) {
      decoded += QChar(static_cast<char16_t>(code));
    } else {
      const char32_t point = code;
      decoded += QString::fromUcs4(&point, 1);
    }

    last = match.capturedEnd();
  }

  decoded += text.mid(last);
  decoded.replace(QStringLiteral("&amp;"), QStringLiteral("&"));

  return decoded;
}

// Reduces an HTML fragment to readable text: drops script/style bodies, strips
// the remaining tags and decodes entities.
QString html_to_text(const QString& html) {
  QString text = html;

  static const QRegularExpression script(
      QStringLiteral("<script\\b[^>]*>.*?</script>"),
      QRegularExpression::DotMatchesEverythingOption |
          QRegularExpression::CaseInsensitiveOption
  );
  static const QRegularExpression style(
      QStringLiteral("<style\\b[^>]*>.*?</style>"),
      QRegularExpression::DotMatchesEverythingOption |
          QRegularExpression::CaseInsensitiveOption
  );
  static const QRegularExpression tag(QStringLiteral("<[^>]*>"));

  text.remove(script);
  text.remove(style);
  text.replace(tag, QStringLiteral(" "));

  return decode_entities(text).trimmed();
}

// Reads a `name="value"` attribute from a tag's attribute string.
QString attribute(const QString& attributes, const QString& name) {
  static const QRegularExpression quoted(
      QStringLiteral("\\b([a-zA-Z:_-]+)\\s*=\\s*\"([^\"]*)\"")
  );

  auto it = quoted.globalMatch(attributes);

  while (it.hasNext()) {
    const QRegularExpressionMatch match = it.next();

    if (match.captured(1).compare(name, Qt::CaseInsensitive) == 0) {
      return match.captured(2);
    }
  }

  return {};
}

// One element carrying a class marker (DuckDuckGo result link or snippet).
struct MarkedElement {
  QString href;
  QString text;
};

// Collects every anchor whose tag carries `marker` in its class attribute.
//
// Only `<a>` elements are considered: a generic "any element" match would let a
// large container (e.g. `<body>`) swallow the anchors nested inside it, so the
// result links would never be seen individually.
std::vector<MarkedElement> collect_marked(
    const QString& html, const QString& marker
) {
  static const QRegularExpression anchor(
      QStringLiteral("<a\\b([^>]*)>(.*?)</a>"),
      QRegularExpression::DotMatchesEverythingOption |
          QRegularExpression::CaseInsensitiveOption
  );

  std::vector<MarkedElement> out;
  auto it = anchor.globalMatch(html);

  while (it.hasNext()) {
    const QRegularExpressionMatch match = it.next();
    const QString attributes = match.captured(1);

    if (!attributes.contains(marker, Qt::CaseInsensitive)) {
      continue;
    }

    MarkedElement item;
    item.href = attribute(attributes, QStringLiteral("href"));
    item.text = html_to_text(match.captured(2));
    out.push_back(std::move(item));
  }

  return out;
}

// Recovers the real target from a DuckDuckGo redirect link (`//duckduckgo.com/
// l/?uddg=<encoded>`); a direct link is returned unchanged.
QString resolve_ddg_url(const QString& href) {
  QString candidate = href;

  if (candidate.startsWith(QStringLiteral("//"))) {
    candidate.prepend(QStringLiteral("https:"));
  }

  const QUrl url(candidate);

  if (!url.isValid()) {
    return candidate;
  }

  const QUrlQuery query(url);

  if (query.hasQueryItem(QStringLiteral("uddg"))) {
    return query.queryItemValue(QStringLiteral("uddg"), QUrl::FullyDecoded);
  }

  return candidate;
}

} // namespace

QString http_get(
    const QString& url, const std::optional<std::string>& proxy, int max_bytes
) {
  const QUrl target(url);

  if (!target.isValid() ||
      (target.scheme() != QStringLiteral("http") &&
       target.scheme() != QStringLiteral("https"))) {
    throw std::runtime_error("Only http/https URLs can be fetched");
  }

  QNetworkAccessManager manager;
  manager.setTransferTimeout(20'000);

  configure_proxy(manager, proxy);

  QNetworkRequest request(target);
  request.setHeader(
      QNetworkRequest::UserAgentHeader,
      QStringLiteral("Mozilla/5.0 (compatible; local-notebook/1.0)")
  );
  request.setRawHeader("Accept", "text/html,application/xhtml+xml,text/plain");
  request.setAttribute(
      QNetworkRequest::RedirectPolicyAttribute,
      QNetworkRequest::NoLessSafeRedirectPolicy
  );

  QNetworkReply* reply = manager.get(request);

  QEventLoop loop;
  QByteArray body;

  QObject::connect(reply, &QNetworkReply::readyRead, &loop, [&]() {
    const QByteArray chunk = reply->readAll();

    if (body.size() < max_bytes) {
      body.append(chunk.left(max_bytes - body.size()));
    }
  });

  QObject::connect(reply, &QNetworkReply::finished, &loop, [&]() { loop.quit(); });

  loop.exec();

  const QByteArray trailing = reply->readAll();

  if (body.size() < max_bytes) {
    body.append(trailing.left(max_bytes - body.size()));
  }

  const QNetworkReply::NetworkError error = reply->error();
  const QString error_string = reply->errorString();
  const int status =
      reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();

  delete reply;

  if (error != QNetworkReply::NoError) {
    std::string message = "HTTP request failed: " + error_string.toStdString();

    if (status > 0) {
      message += " (HTTP " + std::to_string(status) + ")";
    }

    throw std::runtime_error(message);
  }

  return QString::fromUtf8(body);
}

QString fetch_url(const QString& url, const std::optional<std::string>& proxy) {
  if (url.trimmed().isEmpty()) {
    throw std::runtime_error("A URL is required");
  }

  const QString text = html_to_text(http_get(url.trimmed(), proxy, kFetchBodyLimit));

  if (text.isEmpty()) {
    return QStringLiteral("The page returned no readable text.");
  }

  if (text.size() > kFetchTextLimit) {
    return text.left(kFetchTextLimit) +
        QStringLiteral("\n\n[Page truncated; only the first part is shown.]");
  }

  return text;
}

QString web_search(const QString& query, const std::optional<std::string>& proxy) {
  if (query.trimmed().isEmpty()) {
    throw std::runtime_error("A search query is required");
  }

  QUrl url(QStringLiteral("https://html.duckduckgo.com/html/"));
  QUrlQuery params;
  params.addQueryItem(QStringLiteral("q"), query.trimmed());
  url.setQuery(params);

  const QString html = http_get(url.toString(), proxy, kFetchBodyLimit);

  const auto links = collect_marked(html, QStringLiteral("result__a"));
  const auto snippets = collect_marked(html, QStringLiteral("result__snippet"));

  constexpr int kMaxResults = 8;

  std::ostringstream out;
  int count = 0;

  for (std::size_t i = 0; i < links.size() && count < kMaxResults; ++i) {
    const QString title = links[i].text;
    const QString target = resolve_ddg_url(links[i].href);

    if (title.isEmpty() || target.isEmpty()) {
      continue;
    }

    ++count;
    out << count << ". " << title.toStdString() << '\n';
    out << target.toStdString() << '\n';

    if (i < snippets.size() && !snippets[i].text.isEmpty()) {
      out << snippets[i].text.toStdString() << '\n';
    }

    out << '\n';
  }

  if (count == 0) {
    return QStringLiteral(
        "No search results could be parsed. DuckDuckGo's HTML endpoint may be "
        "unreachable or its markup changed; try fetch_url on a known URL."
    );
  }

  return QString::fromStdString(out.str());
}

QJsonArray tool_definitions(bool filesystem) {
  // Filesystem tools: only offered when the session has a shared directory.
  const QJsonObject list_function{
      {"name", "list_directory"},
      {"description",
       "List files and subdirectories immediately inside a directory in the "
       "configured shared directory. Directories end with /. Call it with no "
       "arguments to list the shared directory itself."},
      {"parameters",
       QJsonObject{
           {"type", "object"},
           {"properties",
            QJsonObject{{"path",
                         QJsonObject{
                             {"type", "string"},
                             {"description", "Directory to list: either a path relative to the shared directory or an absolute path inside it. Omit to list the shared directory."}}}}},
           {"required", QJsonArray{}},
           {"additionalProperties", false}}},
  };

  const QJsonObject search_function{
      {"name", "search_code"},
      {"description",
       "Search text files in the configured shared directory. Returns matching file paths, line numbers, and surrounding context. Matching is case-insensitive unless the query contains an uppercase letter; binary files are skipped."},
      {"parameters",
       QJsonObject{
           {"type", "object"},
           {"properties",
            QJsonObject{
                {"query", QJsonObject{{"type", "string"}, {"description", "Symbol name or text to search for"}}},
                {"path", QJsonObject{{"type", "string"}, {"description", "Directory to search: a path relative to the shared directory or an absolute path inside it. Defaults to the shared directory."}}}
            }},
           {"required", QJsonArray{"query"}},
           {"additionalProperties", false}
       }}
  };

  const QJsonObject search_regex_function{
      {"name", "search_code_regex"},
      {"description",
       "Search text files in the configured shared directory with a regular "
       "expression (PCRE2 syntax), honouring .gitignore/.ignore files and "
       "skipping hidden entries. Returns matching file paths, line numbers and "
       "surrounding context. Matching is case-sensitive; add an inline flag "
       "such as (?i) for case-insensitive matching, and \\b for word "
       "boundaries."},
      {"parameters",
       QJsonObject{
           {"type", "object"},
           {"properties",
            QJsonObject{
                {"pattern", QJsonObject{{"type", "string"}, {"description", "Regular expression to search for"}}},
                {"path", QJsonObject{{"type", "string"}, {"description", "Directory to search: a path relative to the shared directory or an absolute path inside it. Defaults to the shared directory."}}},
                {"glob", QJsonObject{{"type", "string"}, {"description", "Optional glob to restrict which files are searched, e.g. \"*.cpp\" or \"src/**/*.hpp\"."}}}
            }},
           {"required", QJsonArray{"pattern"}},
           {"additionalProperties", false}
       }}
  };

  const QJsonObject find_files_function{
      {"name", "find_files"},
      {"description",
       "List files in the configured shared directory whose path matches a "
       "glob, honouring .gitignore/.ignore files and skipping hidden entries. "
       "Use it to discover files by name or extension without reading their "
       "contents."},
      {"parameters",
       QJsonObject{
           {"type", "object"},
           {"properties",
            QJsonObject{
                {"glob", QJsonObject{{"type", "string"}, {"description", "Glob to match file paths, e.g. \"*.md\" or \"src/**/*.cpp\"."}}},
                {"path", QJsonObject{{"type", "string"}, {"description", "Directory to search: a path relative to the shared directory or an absolute path inside it. Defaults to the shared directory."}}}
            }},
           {"required", QJsonArray{"glob"}},
           {"additionalProperties", false}
       }}
  };

  const QJsonObject read_function{
      {"name", "read_file"},
      {"description",
       "Read a limited range of lines from a file in the configured local "
       "directories."},
      {"parameters",
       QJsonObject{
           {"type", "object"},
           {"properties",
            QJsonObject{
                {"path",
                 QJsonObject{
                     {"type", "string"},
                     {"description", "File to read: a path relative to the shared directory or an absolute path inside it"}}},
                {"start_line", QJsonObject{{"type", "integer"}}},
                {"end_line", QJsonObject{{"type", "integer"}}}}},
           {"required", QJsonArray{"path", "start_line", "end_line"}},
           {"additionalProperties", false}}},
  };

  // Network tools: always offered, they work without a shared directory.
  const QJsonObject fetch_function{
      {"name", "fetch_url"},
      {"description",
       "Fetch an http/https URL and return its visible text. Use it to read a "
       "web page referenced by the user. HTML is converted to plain text."},
      {"parameters",
       QJsonObject{
           {"type", "object"},
           {"properties",
            QJsonObject{{"url",
                         QJsonObject{
                             {"type", "string"},
                             {"description", "Absolute http:// or https:// URL to fetch"}}}}},
           {"required", QJsonArray{"url"}},
           {"additionalProperties", false}}},
  };

  const QJsonObject web_search_function{
      {"name", "web_search"},
      {"description",
       "Search the web and return the top results with their titles, URLs and "
       "snippets. Use it to find sources online; then use fetch_url to read a "
       "result in full."},
      {"parameters",
       QJsonObject{
           {"type", "object"},
           {"properties",
            QJsonObject{{"query",
                         QJsonObject{
                             {"type", "string"},
                             {"description", "Search terms"}}}}},
           {"required", QJsonArray{"query"}},
           {"additionalProperties", false}}},
  };

  QJsonArray tools;

  if (filesystem) {
    tools.append(QJsonObject{{"type", "function"}, {"function", list_function}});
    tools.append(QJsonObject{{"type", "function"}, {"function", search_function}});
    tools.append(QJsonObject{{"type", "function"}, {"function", search_regex_function}});
    tools.append(QJsonObject{{"type", "function"}, {"function", find_files_function}});
    tools.append(QJsonObject{{"type", "function"}, {"function", read_function}});
  }

  tools.append(QJsonObject{{"type", "function"}, {"function", fetch_function}});
  tools.append(QJsonObject{{"type", "function"}, {"function", web_search_function}});

  return tools;
}

QString execute_local_tool(
    const ToolCall& call, const std::optional<std::string>& root_path,
    const std::optional<std::string>& proxy
) {
  try {
    QJsonParseError parse_error;
    const QJsonDocument document =
        QJsonDocument::fromJson(call.arguments.toUtf8(), &parse_error);

    if (parse_error.error != QJsonParseError::NoError || !document.isObject()) {
      throw std::runtime_error("Tool arguments must be a JSON object");
    }

    const QJsonObject args = document.object();

    // Filesystem tools need a shared directory; network tools do not.
    const bool needs_root = call.name == "list_directory" ||
        call.name == "search_code" || call.name == "search_code_regex" ||
        call.name == "find_files" || call.name == "read_file";

    if (needs_root && (!root_path.has_value() || root_path->empty())) {
      return QStringLiteral(
          "Tool error: this session has no shared directory configured"
      );
    }

    if (call.name == "list_directory") {
      const QString path = json_string_or(args, "path", QStringLiteral("."));

      return QString::fromStdString(list_directory(*root_path, path.toStdString()));
    }

    if (call.name == "search_code") {
      const QString query = json_string_or(args, "query");
      const QString path = json_string_or(args, "path", QStringLiteral("."));

      return QString::fromStdString(
          search_code(query.toStdString(), *root_path, path.toStdString())
      );
    }

    if (call.name == "search_code_regex") {
      const QString pattern = json_string_or(args, "pattern");
      const QString path = json_string_or(args, "path", QStringLiteral("."));
      const QString glob = json_string_or(args, "glob");

      return QString::fromStdString(search_regex(
          pattern.toStdString(), *root_path, path.toStdString(), glob.toStdString()
      ));
    }

    if (call.name == "find_files") {
      const QString glob = json_string_or(args, "glob");
      const QString path = json_string_or(args, "path", QStringLiteral("."));

      return QString::fromStdString(
          find_files(glob.toStdString(), *root_path, path.toStdString())
      );
    }

    if (call.name == "read_file") {
      const QString path = json_string_or(args, "path");
      const int start = json_int_or(args, "start_line", 1);
      const int end = json_int_or(args, "end_line", start + 100);

      return QString::fromStdString(read_file_range(
          checked_repo_path(*root_path, path.toStdString()), start, end
      ));
    }

    if (call.name == "fetch_url") {
      const QString url = json_string_or(args, "url");
      return fetch_url(url, proxy);
    }

    if (call.name == "web_search") {
      const QString query = json_string_or(args, "query");
      return web_search(query, proxy);
    }

    throw std::runtime_error("Unknown tool: " + call.name.toStdString());
  } catch (const std::exception& e) {
    // Tool errors are returned to the model so it can retry with better
    // arguments instead of aborting the whole generation.
    return QStringLiteral("Tool error: ") + QString::fromUtf8(e.what());
  }
}

} // namespace lnb::net
