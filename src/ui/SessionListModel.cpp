#include "ui/SessionListModel.hpp"

#include <QDate>
#include <QDateTime>

namespace ui {

namespace {

// The row's second line: a compact, human time — the clock time for today, the
// month and day within the current year, and the full date otherwise.
QString format_row_time(const std::string& iso) {
  if (iso.empty()) {
    return {};
  }

  const QDateTime parsed = QDateTime::fromString(QString::fromStdString(iso), Qt::ISODate);

  if (!parsed.isValid()) {
    return QString::fromStdString(iso);
  }

  const QDateTime local = parsed.toLocalTime();
  const QDate today = QDate::currentDate();

  if (local.date() == today) {
    return local.toString(QStringLiteral("HH:mm"));
  }
  if (local.date().year() == today.year()) {
    return local.toString(QStringLiteral("MMM d"));
  }
  return local.toString(QStringLiteral("yyyy-MM-dd"));
}

} // namespace

SessionListModel::SessionListModel(QObject* parent)
    : QAbstractListModel(parent) {}

int SessionListModel::rowCount(const QModelIndex& parent) const {
  if (parent.isValid()) {
    return 0;
  }

  return static_cast<int>(sessions_.size());
}

QVariant SessionListModel::data(const QModelIndex& index, int role) const {
  if (!index.isValid() || index.row() < 0 ||
      index.row() >= static_cast<int>(sessions_.size())) {
    return {};
  }

  const lnb::Session& session = sessions_[static_cast<std::size_t>(index.row())];

  switch (role) {
    case Qt::DisplayRole:
    case TitleRole: {
      const QString title = QString::fromStdString(session.title_);
      return title.isEmpty() ? QStringLiteral("New Session") : title;
    }
    case IdRole:
      return static_cast<uint>(session.id_);
    case TimeRole:
      return format_row_time(last_time_at(index.row()));
    default:
      return {};
  }
}

void SessionListModel::set_sessions(
    std::vector<lnb::Session> sessions, std::vector<std::string> last_times
) {
  beginResetModel();
  sessions_ = std::move(sessions);
  last_times_ = std::move(last_times);
  // Keep the two vectors parallel even if a caller passes a short list.
  last_times_.resize(sessions_.size());
  endResetModel();
}

lnb::Session SessionListModel::session_at(int row) const {
  if (row < 0 || row >= static_cast<int>(sessions_.size())) {
    return {};
  }

  return sessions_[static_cast<std::size_t>(row)];
}

std::uint32_t SessionListModel::session_id_at(int row) const {
  return session_at(row).id_;
}

const std::string& SessionListModel::last_time_at(int row) const {
  static const std::string empty;

  if (row < 0 || row >= static_cast<int>(last_times_.size())) {
    return empty;
  }

  return last_times_[static_cast<std::size_t>(row)];
}

int SessionListModel::row_for(std::uint32_t session_id) const {
  for (std::size_t i = 0; i < sessions_.size(); ++i) {
    if (sessions_[i].id_ == session_id) {
      return static_cast<int>(i);
    }
  }

  return -1;
}

} // namespace ui
