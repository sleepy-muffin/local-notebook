#include "ui/SessionListModel.hpp"

namespace ui {

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
    default:
      return {};
  }
}

void SessionListModel::set_sessions(std::vector<lnb::Session> sessions) {
  beginResetModel();
  sessions_ = std::move(sessions);
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

int SessionListModel::row_for(std::uint32_t session_id) const {
  for (std::size_t i = 0; i < sessions_.size(); ++i) {
    if (sessions_[i].id_ == session_id) {
      return static_cast<int>(i);
    }
  }

  return -1;
}

} // namespace ui
