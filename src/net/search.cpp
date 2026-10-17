#include "net/search.hpp"

#include "net/tools.hpp"

#include <QRegularExpression>
#include <QString>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <fstream>
#include <functional>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace lnb::net {

namespace {

// Bounds mirror search_code: text files only, small enough to read whole, and a
// hard cap on matches so one call cannot flood the model's context. The walk
// itself is deliberately unbounded — a glob must never truncate the coverage, or
// a large checkout would report "no matches" simply because the target sorts
// beyond an arbitrary file count (the bug this replaced).
constexpr std::uintmax_t kMaxFileBytes = 2 * 1024 * 1024;
constexpr int kMaxMatches = 30;
constexpr int kContextLines = 2;
constexpr std::size_t kMaxPatternLength = 512;

// The most paths find_files will list before it stops and says so.
constexpr std::size_t kMaxListedFiles = 1000;

// Translates a shell glob to an anchored regular expression. In `path_mode` a
// single '*' does not cross '/', and "**/" matches zero or more directories;
// otherwise '*' matches any run of characters (used for basename-style filters).
QString glob_to_regex(const QString& glob, bool path_mode) {
  QString out;
  out.reserve(glob.size() * 2 + 2);
  out += QLatin1Char('^');

  for (qsizetype i = 0; i < glob.size(); ++i) {
    const QChar c = glob.at(i);

    if (c == QLatin1Char('*')) {
      if (i + 1 < glob.size() && glob.at(i + 1) == QLatin1Char('*')) {
        i += 1;
        if (i + 1 < glob.size() && glob.at(i + 1) == QLatin1Char('/')) {
          i += 1;
          out += QStringLiteral("(?:.*/)?");
        } else {
          out += QStringLiteral(".*");
        }
      } else {
        out += path_mode ? QStringLiteral("[^/]*") : QStringLiteral(".*");
      }
    } else if (c == QLatin1Char('?')) {
      out += path_mode ? QStringLiteral("[^/]") : QStringLiteral(".");
    } else if (c == QLatin1Char('[')) {
      // Character class: pass through, translating a leading '!' to '^'.
      const qsizetype close = glob.indexOf(QLatin1Char(']'), i + 1);

      if (close == -1) {
        out += QStringLiteral("\\[");
        continue;
      }

      QString body = glob.mid(i + 1, close - i - 1);
      if (body.startsWith(QLatin1Char('!'))) {
        body = QLatin1Char('^') + body.mid(1);
      }

      QString escaped;
      for (const QChar member: body) {
        if (member == QLatin1Char('\\') || member == QLatin1Char(']') ||
            member == QLatin1Char('[')) {
          escaped += QLatin1Char('\\');
        }
        escaped += member;
      }

      out += QLatin1Char('[') + escaped + QLatin1Char(']');
      i = close;
    } else {
      if (QStringLiteral(".^$|(){}+\\").contains(c)) {
        out += QLatin1Char('\\');
      }
      out += c;
    }
  }

  out += QLatin1Char('$');
  return out;
}

// A glob's longest run of literal characters, as UTF-8. Any path matching the
// glob must contain this substring, so it is a sound prefilter: it can only ever
// reject a path that the regex would also reject. Empty when the glob has no run
// worth using.
std::string glob_literal(const QString& glob) {
  QString best;
  QString run;

  for (qsizetype i = 0; i < glob.size(); ++i) {
    const QChar c = glob.at(i);

    if (c == QLatin1Char('*')) {
      // A "**/" compiles to an optional "(?:.*/)?", so the slash after "**"
      // is not a required character and must not start a literal run; clear the
      // run past it. A lone '*' (and a "**" not followed by '/') is a plain
      // wildcard. This is what keeps the prefilter sound for patterns like
      // "*.log" (glob "**/*.log"), whose literal is ".log", not "/.log".
      run.clear();
      if (i + 1 < glob.size() && glob.at(i + 1) == QLatin1Char('*')) {
        ++i;
        if (i + 1 < glob.size() && glob.at(i + 1) == QLatin1Char('/')) ++i;
      }
    } else if (c == QLatin1Char('?')) {
      run.clear();
    } else if (c == QLatin1Char('[')) {
      // The class contents are not literal; skip to its end.
      run.clear();
      const qsizetype close = glob.indexOf(QLatin1Char(']'), i + 1);
      if (close != -1) i = close;
    } else if (c == QLatin1Char('\\')) {
      // glob_to_regex emits '\\' for a backslash and then processes the next
      // character on its own; it does not treat '\' as an escape. Mirror that
      // exactly, so the literal is one the compiled regex really requires.
      run += c;
    } else {
      run += c;
    }

    if (run.size() > best.size()) best = run;
  }

  // A single character matches almost everything, so it is not worth the check.
  if (best.size() < 2) return {};

  return best.toUtf8().toStdString();
}

// A path filter: the compiled glob plus its required literal. An empty regex
// means "no filter" (every file passes).
struct GlobFilter {
  std::optional<QRegularExpression> regex;
  std::string literal;

  bool matches(const std::string& relative) const {
    if (!regex.has_value()) return true;

    if (!literal.empty() &&
        relative.find(literal) == std::string::npos) {
      return false;
    }

    return regex
        ->match(QString::fromUtf8(
            relative.data(), static_cast<qsizetype>(relative.size())
        ))
        .hasMatch();
  }
};

GlobFilter compile_glob_filter(const std::string& glob) {
  GlobFilter filter;

  if (glob.empty()) return filter;

  const QString pattern = QString::fromStdString(glob);
  filter.literal = glob_literal(pattern);

  QRegularExpression re(
      glob_to_regex(pattern, pattern.contains(QLatin1Char('/'))),
      QRegularExpression::UseUnicodePropertiesOption
  );

  if (!re.isValid()) {
    throw std::runtime_error("Invalid glob pattern");
  }

  filter.regex = re;
  return filter;
}

// A single .gitignore/.ignore directive, already compiled. `base` is the
// directory the directive was declared in, relative to the shared root ("" at
// the root), so `regex` is matched against the candidate path relative to it.
// `literal` is the directive glob's required substring, checked before the regex
// so the hundreds of rules in a large .gitignore stay cheap.
struct IgnoreRule {
  QRegularExpression regex;
  std::string literal;
  bool negated = false;
  bool dir_only = false;
  std::string base;
};

// A pattern with a slash is anchored to its own directory; otherwise it may
// match at any depth (mirroring git's rule for leading/middle separators).
void load_ignore_rules(
    const std::filesystem::path& dir, const std::string& base,
    std::vector<IgnoreRule>& rules
) {
  for (const char* name: {".gitignore", ".ignore"}) {
    std::ifstream in(dir / name);
    if (!in) continue;

    std::string line;
    while (std::getline(in, line)) {
      if (!line.empty() && line.back() == '\r') line.pop_back();

      QString pattern = QString::fromUtf8(
                            line.data(), static_cast<qsizetype>(line.size())
      ).trimmed();

      if (pattern.isEmpty() || pattern.startsWith(QLatin1Char('#'))) continue;

      bool negated = false;
      if (pattern.startsWith(QLatin1Char('!'))) {
        negated = true;
        pattern = pattern.mid(1);
      }

      bool dir_only = false;
      if (pattern.endsWith(QLatin1Char('/'))) {
        dir_only = true;
        pattern.chop(1);
      }

      if (pattern.isEmpty()) continue;

      const bool anchored = pattern.contains(QLatin1Char('/'));
      if (pattern.startsWith(QLatin1Char('/'))) pattern = pattern.mid(1);
      if (pattern.isEmpty()) continue;

      const QString glob = anchored ? pattern : QStringLiteral("**/") + pattern;

      QRegularExpression re(
          glob_to_regex(glob, true), QRegularExpression::UseUnicodePropertiesOption
      );

      if (!re.isValid()) continue;

      IgnoreRule rule;
      rule.regex = re;
      rule.literal = glob_literal(glob);
      rule.negated = negated;
      rule.dir_only = dir_only;
      rule.base = base;
      rules.push_back(std::move(rule));
    }
  }
}

// Later rules win, so a deeper .gitignore overrides a shallower one and the last
// matching line of a single file overrides earlier ones. The cheap rejects are
// inlined into the loop because this runs once per rule per entry of the walk,
// which is by far the hottest code in a search of a large tree.
bool is_ignored(
    const std::vector<IgnoreRule>& rules, const std::string& rel, bool is_dir
) {
  bool ignored = false;

  for (const IgnoreRule& rule: rules) {
    // A directory-only pattern never matches a file directly; its files are
    // excluded by pruning the directory during the walk (as git does, a parent
    // exclusion cannot be re-included by a child).
    if (rule.dir_only && !is_dir) continue;

    // A view, not a copy: this runs for every rule of every entry, so copying the
    // path here (a heap allocation for any path past the small-string buffer) adds
    // up badly across a large tree.
    std::string_view candidate = rel;
    if (!rule.base.empty()) {
      if (rel.size() <= rule.base.size()) continue;
      if (candidate.substr(0, rule.base.size()) != rule.base) continue;
      if (rel[rule.base.size()] != '/') continue;
      candidate = candidate.substr(rule.base.size() + 1);
    }

    // The required literal rejects most rules without building a QString.
    if (!rule.literal.empty() &&
        candidate.find(rule.literal) == std::string_view::npos) {
      continue;
    }

    if (rule.regex
            .match(QString::fromUtf8(
                candidate.data(), static_cast<qsizetype>(candidate.size())
            ))
            .hasMatch()) {
      ignored = !rule.negated;
    }
  }

  return ignored;
}

// Called with a found file's absolute path and its path relative to the shared
// root; returning false stops the walk.
using FileVisitor =
    std::function<bool(const std::filesystem::path&, const std::string&)>;

// Restores the rule stack on leaving a directory, so the rules a subtree adds
// do not leak into its siblings. Taking the rules by reference (rather than
// copying the whole, large root rule set into every recursive call) is what
// keeps a walk of a big checkout from allocating the rule vector per directory.
struct RuleScope {
  std::vector<IgnoreRule>& rules;
  std::size_t count;
  ~RuleScope() { rules.resize(count); }
};

// Walks `dir`, honouring the ignore rules, and calls `visit` for every regular
// file whose relative path passes `filter`. Every regular file considered after
// the ignore rules bumps `examined` when it is not null. Returns true when the
// walk ran to completion, false when `visit` stopped it. The walk itself never
// stops on a count, so no glob can silently truncate the coverage.
bool walk_files(
    const std::filesystem::path& dir, const std::string& rel_dir,
    std::vector<IgnoreRule>& rules, const GlobFilter& filter,
    const FileVisitor& visit, std::size_t* examined
) {
  const RuleScope scope{rules, rules.size()};
  load_ignore_rules(dir, rel_dir, rules);

  std::error_code ec;
  std::filesystem::directory_iterator it(
      dir, std::filesystem::directory_options::skip_permission_denied, ec
  );

  if (ec) return true;

  for (const std::filesystem::directory_entry& entry: it) {
    if (entry.is_symlink(ec) || ec) {
      ec.clear();
      continue;
    }

    const std::string name = entry.path().filename().generic_string();

    // Hidden entries are skipped, matching ripgrep's default. The .gitignore
    // files themselves are read explicitly, so hiding them is harmless.
    if (name.empty() || name.front() == '.') continue;

    const bool is_dir = entry.is_directory(ec);
    if (ec) {
      ec.clear();
      continue;
    }

    const bool is_file = !is_dir && entry.is_regular_file(ec);
    if (ec) {
      ec.clear();
      continue;
    }

    if (!is_dir && !is_file) continue;

    const std::string rel = rel_dir.empty() ? name : rel_dir + "/" + name;

    if (is_ignored(rules, rel, is_dir)) continue;

    if (is_dir) {
      if (!walk_files(entry.path(), rel, rules, filter, visit, examined)) {
        return false;
      }
    } else {
      if (examined != nullptr) ++*examined;

      if (filter.matches(rel) && !visit(entry.path(), rel)) {
        return false;
      }
    }
  }

  return true;
}

// The resolved start directory, its path relative to the shared root, and the
// ignore rules declared above it (so a subdirectory search honours a parent
// .gitignore).
struct WalkSetup {
  std::filesystem::path start;
  std::string rel_base;
  std::vector<IgnoreRule> rules;
};

WalkSetup prepare_walk(
    const std::string& root_path, const std::string& relative_path
) {
  WalkSetup setup;
  const std::filesystem::path root = std::filesystem::canonical(root_path);
  setup.start = checked_repo_path(root_path, relative_path);

  if (!std::filesystem::is_directory(setup.start)) {
    throw std::runtime_error("Search path is not a directory");
  }

  if (setup.start != root) {
    setup.rel_base =
        std::filesystem::relative(setup.start, root).generic_string();
  }

  load_ignore_rules(root, "", setup.rules);

  std::size_t pos = 0;
  while (pos < setup.rel_base.size()) {
    const std::size_t slash = setup.rel_base.find('/', pos);

    if (slash == std::string::npos) break;

    const std::string dir_rel = setup.rel_base.substr(0, slash);
    load_ignore_rules(root / dir_rel, dir_rel, setup.rules);
    pos = slash + 1;
  }

  return setup;
}

// The literal substring every match must contain, or nullopt when the pattern is
// not a simple literal (it has alternation, classes, quantifiers, inline flags,
// or any construct this does not model). It is used only to skip the expensive
// UTF-8 decode and regex for files that provably cannot match, so the results
// are identical with or without it.
std::optional<std::string> required_literal(const std::string& pattern) {
  if (pattern.find("(?") != std::string::npos) return std::nullopt;

  std::string text = pattern;

  const auto strip_front = [&text](std::string_view token) {
    if (text.rfind(token, 0) == 0) {
      text.erase(0, token.size());
      return true;
    }
    return false;
  };

  const auto strip_back = [&text](std::string_view token) {
    if (text.size() >= token.size() &&
        text.compare(text.size() - token.size(), token.size(), token) == 0) {
      text.erase(text.size() - token.size());
      return true;
    }
    return false;
  };

  // Peel the anchors and word boundaries that do not contribute a literal, e.g.
  // "\bfoo\b", "^foo$".
  for (bool changed = true; changed;) {
    changed = false;
    changed = strip_front("\\b") || changed;
    changed = strip_front("\\B") || changed;
    changed = strip_front("^") || changed;
    changed = strip_back("\\b") || changed;
    changed = strip_back("\\B") || changed;
    changed = strip_back("$") || changed;
  }

  if (text.empty()) return std::nullopt;

  // Anything still regex-special means `text` is not a guaranteed substring.
  if (text.find_first_of("\\.^$|?*+()[]{}") != std::string::npos) {
    return std::nullopt;
  }

  return text;
}

} // namespace

WalkResult collect_files(
    const std::string& root_path, const std::string& relative_path,
    const std::string& glob, std::size_t max_files
) {
  WalkSetup setup = prepare_walk(root_path, relative_path);
  const GlobFilter filter = compile_glob_filter(glob);

  WalkResult result;
  result.directory = setup.start;

  walk_files(
      setup.start, setup.rel_base, setup.rules, filter,
      [&](const std::filesystem::path& absolute, const std::string& relative) {
        if (result.files.size() >= max_files) {
          result.truncated = true;
          return false;
        }

        result.files.push_back(FoundFile{absolute, relative});
        return true;
      },
      &result.scanned
  );

  return result;
}

std::string search_regex(
    const std::string& pattern, const std::string& root_path,
    const std::string& relative_path, const std::string& glob
) {
  if (pattern.empty() || pattern.size() > kMaxPatternLength) {
    throw std::runtime_error("Invalid search pattern");
  }

  const QRegularExpression re(
      QString::fromStdString(pattern), QRegularExpression::UseUnicodePropertiesOption
  );

  if (!re.isValid()) {
    throw std::runtime_error(
        "Invalid regular expression: " + re.errorString().toStdString()
    );
  }

  WalkSetup setup = prepare_walk(root_path, relative_path);
  const GlobFilter filter = compile_glob_filter(glob);
  const std::optional<std::string> literal = required_literal(pattern);

  std::ostringstream out;
  int matches = 0;
  int files_scanned = 0;

  walk_files(
      setup.start, setup.rel_base, setup.rules, filter,
      [&](const std::filesystem::path& absolute, const std::string& relative) {
        // Reached the match cap: stop the walk. Coverage is bounded by the text
        // actually read, never by an arbitrary file count.
        if (matches >= kMaxMatches) return false;

        std::error_code ec;
        const std::uintmax_t size = std::filesystem::file_size(absolute, ec);

        if (ec || size > kMaxFileBytes) return true;

        std::ifstream in(absolute, std::ios::binary);
        if (!in) return true;

        // Bulk read: the size is known from the stat above, and a single read()
        // is far faster than streaming the file character by character.
        std::string bytes(static_cast<std::size_t>(size), '\0');
        in.read(bytes.data(), static_cast<std::streamsize>(size));
        bytes.resize(static_cast<std::size_t>(in.gcount()));

        // A NUL byte in the first block marks a binary file (the same
        // convention as search_code), checked on the bytes already in hand.
        {
          const std::size_t block = std::min<std::size_t>(bytes.size(), 8192);
          const auto end = bytes.begin() + static_cast<std::ptrdiff_t>(block);

          if (std::find(bytes.begin(), end, '\0') != end) {
            return true;
          }
        }

        ++files_scanned;

        // A cheap literal check first: a file that does not contain the required
        // literal cannot match, so the decode and regex are skipped for it.
        if (literal.has_value() &&
            bytes.find(*literal) == std::string::npos) {
          return true;
        }

        const QString content =
            QString::fromUtf8(bytes.data(), static_cast<qsizetype>(bytes.size()));

        // The match is the expensive part and most files in a large tree do not
        // contain the pattern, so the per-line bookkeeping is built only once a
        // match is known to exist.
        auto it = re.globalMatch(content);
        if (!it.hasNext()) return true;

        // Line-start offsets, so a match offset maps back to a line number.
        std::vector<qsizetype> line_starts;
        line_starts.push_back(0);

        for (qsizetype i = 0; i < content.size(); ++i) {
          if (content.at(i) == QLatin1Char('\n')) line_starts.push_back(i + 1);
        }

        const qsizetype line_count = static_cast<qsizetype>(line_starts.size());

        qsizetype last_line = -1;

        while (it.hasNext() && matches < kMaxMatches) {
          const QRegularExpressionMatch match = it.next();
          const qsizetype offset = match.capturedStart();

          const auto upper =
              std::upper_bound(line_starts.begin(), line_starts.end(), offset);
          const qsizetype line =
              static_cast<qsizetype>(upper - line_starts.begin()) - 1;

          // One block per line, as search_code does, so repeated matches on a
          // line do not duplicate the surrounding context.
          if (line == last_line) continue;
          last_line = line;

          out << "\n--- " << relative << ':' << (line + 1) << " ---\n";

          const qsizetype begin = line > kContextLines ? line - kContextLines : 0;
          const qsizetype finish = std::min(line + kContextLines, line_count - 1);

          for (qsizetype l = begin; l <= finish; ++l) {
            const qsizetype line_start = line_starts[static_cast<std::size_t>(l)];
            const qsizetype line_end = (l + 1 < line_count)
                ? line_starts[static_cast<std::size_t>(l + 1)] - 1
                : content.size();

            QString text = content.mid(line_start, line_end - line_start);
            if (text.endsWith(QLatin1Char('\r'))) text.chop(1);

            out << (l + 1) << ": " << text.toStdString() << '\n';
          }

          ++matches;
        }

        return true;
      },
      nullptr
  );

  if (matches == 0) {
    std::ostringstream message;
    message << "No matches for \"" << pattern << "\" in \""
            << setup.start.generic_string() << "\" (" << files_scanned
            << " text files scanned). Binary files and ignored paths are "
               "skipped.";
    return message.str();
  }

  if (matches >= kMaxMatches) {
    out << "\n[Result limit reached; narrow the search path, glob or pattern.]\n";
  }

  return out.str();
}

std::string find_files(
    const std::string& glob, const std::string& root_path,
    const std::string& relative_path
) {
  if (glob.empty()) {
    throw std::runtime_error("A glob pattern is required");
  }

  const WalkResult walked =
      collect_files(root_path, relative_path, glob, kMaxListedFiles);

  std::vector<std::string> paths;
  paths.reserve(walked.files.size());

  for (const FoundFile& file: walked.files) {
    paths.push_back(file.relative);
  }

  std::sort(paths.begin(), paths.end());

  if (paths.empty()) {
    std::ostringstream message;
    message << "No files match \"" << glob << "\" in \""
            << walked.directory.generic_string() << "\" (" << walked.scanned
            << " files scanned). Hidden and ignored paths are skipped.";
    return message.str();
  }

  std::ostringstream out;

  for (const std::string& path: paths) {
    out << path << '\n';
  }

  if (walked.truncated) {
    out << "[File limit reached; narrow the path or glob.]\n";
  }

  return out.str();
}

} // namespace lnb::net
