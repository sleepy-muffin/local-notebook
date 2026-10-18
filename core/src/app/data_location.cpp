#include "app/data_location.hpp"

#include <QCommandLineOption>
#include <QCommandLineParser>
#include <QStandardPaths>
#include <QString>

#include "support/log.hpp"

namespace lnb::app {

std::filesystem::path resolve_data_root(const QStringList& arguments) {
  QCommandLineParser parser;
  parser.setApplicationDescription(QStringLiteral("Local Notebook"));

  const QCommandLineOption user_data(
      QStringLiteral("user-data"),
      QStringLiteral(
          "Store settings and databases in <dir> instead of the per-user "
          "application data directory."
      ),
      QStringLiteral("dir"));
  parser.addOption(user_data);

  // parse() never terminates the process (unlike process()), so an unrecognised
  // argument only logs and leaves the default in place: a GUI must still open.
  if (!parser.parse(arguments)) {
    log::error("unrecognised command line; using the default data directory");
  }

  const QString override = parser.value(user_data);
  if (!override.isEmpty()) {
    return std::filesystem::path(override.toStdString());
  }

  const QString location =
      QStandardPaths::writableLocation(QStandardPaths::AppDataLocation);
  if (!location.isEmpty()) {
    return std::filesystem::path(location.toStdString());
  }

  // Qt reports no location (rare); keep the historical relative default.
  return std::filesystem::path("data");
}

} // namespace lnb::app
