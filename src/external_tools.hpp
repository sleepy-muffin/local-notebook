#pragma once

#include <QJsonObject>
#include <QString>

#include <filesystem>
#include <string>

namespace lnb::external_tools {

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
// the repository root (including via symlinks).
fs::path checked_repo_path(const std::string& root_str, const std::string& relative);

// Small typed accessors for a tool-argument JSON object.
QString json_string_or(
    const QJsonObject& obj, const QString& key, const QString& fallback = {}
);

int json_int_or(const QJsonObject& obj, const QString& key, int fallback);

bool is_source_file(const fs::path& path);

std::string search_code(
    const std::string& query, const std::string& root_path,
    const std::string& relative_path
);

std::string list_directory(const std::string& root_path, const std::string& relative_path);


// Executes one tool call against `root_path`. Tool failures are returned to the
// model as text (rather than thrown), so it can correct its arguments.
QString execute_local_tool(const ToolCall& call, const std::string& root_path);

} // namespace lnb::external_tools
