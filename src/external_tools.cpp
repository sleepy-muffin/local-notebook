#include "external_tools.hpp"

#include <QJsonDocument>
#include <QJsonParseError>
#include <QJsonValue>

#include <algorithm>
#include <fstream>
#include <sstream>
#include <vector>

namespace lnb::external_tools {

fs::path checked_repo_path(
    const std::string& root_str, const std::string& relative
) {
  const fs::path root = fs::canonical(root_str);

  fs::path input(relative);
  if (input.is_absolute()) {
    throw std::runtime_error("Absolute paths are not allowed");
  }

  const fs::path candidate = fs::canonical(root / input);

  // fs::canonical() resolves symlinks, so verify the final path still lives
  // inside the root before handing it back.
  const fs::path rel = candidate.lexically_relative(root);
  if (rel.empty() || rel.is_absolute() || *rel.begin() == "..") {
    throw std::runtime_error("Path escapes the repository");
  }

  return candidate;
}

QString json_string_or(
    const QJsonObject& obj, const QString& key, const QString& fallback
) {
  const QJsonValue value = obj.value(key);
  return value.isString() ? value.toString() : fallback;
}

int json_int_or(const QJsonObject& obj, const QString& key, int fallback) {
  const QJsonValue value = obj.value(key);
  return value.isDouble() ? value.toInt(fallback) : fallback;
}

bool is_source_file(const fs::path& path) {
  static const std::vector<std::string> extensions = {
      ".h", ".hpp", ".c", ".cc", ".cpp", ".cxx",
      ".cu", ".cuh", ".py", ".inl", ".mm"
  };

  const std::string ext = path.extension().string();
  return std::find(extensions.begin(), extensions.end(), ext) != extensions.end();
}

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

  std::ostringstream out;
  int matches = 0;
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

      if (!it->is_regular_file() || !is_source_file(it->path())) {
        continue;
      }

      if (it->file_size() > 2 * 1024 * 1024) {
        continue;
      }

      std::ifstream in(it->path());
      if (!in) continue;

      std::vector<std::string> lines;
      std::string line;
      while (std::getline(in, line)) {
        lines.push_back(std::move(line));
        if (lines.size() > 200000) break;
      }

      for (std::size_t i = 0; i < lines.size() && matches < MAX_MATCHES; ++i) {
        if (lines[i].find(query) == std::string::npos) {
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
    return "No matches found.";
  }

  if (matches == MAX_MATCHES) {
    out << "\n[Result limit reached; narrow the search path or query.]\n";
  }

  return out.str();
}

QString execute_local_tool(const ToolCall& call, const std::string& root_path) {
  try {
    QJsonParseError parse_error;
    const QJsonDocument document =
        QJsonDocument::fromJson(call.arguments.toUtf8(), &parse_error);

    if (parse_error.error != QJsonParseError::NoError || !document.isObject()) {
      throw std::runtime_error("Tool arguments must be a JSON object");
    }

    const QJsonObject args = document.object();

    if (call.name == "list_directory") {
      const QString path = json_string_or(args, "path", QStringLiteral("."));

      return QString::fromStdString(
          list_directory(root_path, path.toStdString())
      );
    }

    if (call.name == "search_code") {
      const QString query = json_string_or(args, "query");
      const QString path = json_string_or(args, "path");

      return QString::fromStdString(
          search_code(query.toStdString(), root_path, path.toStdString())
      );
    }

    if (call.name == "read_file") {
      const QString path = json_string_or(args, "path");
      const int start = json_int_or(args, "start_line", 1);
      const int end = json_int_or(args, "end_line", start + 100);

      return QString::fromStdString(read_file_range(
          checked_repo_path(root_path, path.toStdString()), start, end
      ));
    }

    throw std::runtime_error("Unknown tool: " + call.name.toStdString());
  } catch (const std::exception& e) {
    // Tool errors are returned to the model so it can retry with better
    // arguments instead of aborting the whole generation.
    return QStringLiteral("Tool error: ") + QString::fromUtf8(e.what());
  }
}

} // namespace lnb::external_tools
