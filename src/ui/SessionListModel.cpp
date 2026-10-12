#include "SessionListModel.hpp"

#include <QSizeF>

namespace ui {

SessionListModel::SessionListModel(
    lnb::session_snapshot sessions,
    QObject* parent
)
    : QAbstractListModel(parent),
      sessions_(std::move(sessions))
{
}

int SessionListModel::rowCount(
    const QModelIndex& parent
) const {
  if (parent.isValid() || !sessions_) {
    return 0;
  }

  return static_cast<int>(sessions_->size());
}

QVariant SessionListModel::data(
    const QModelIndex& index,
    int role
) const {
  if (!index.isValid() || !sessions_) {
    return {};
  }

  if (index.row() < 0 ||
      static_cast<std::size_t>(index.row()) >= sessions_->size()) {
    return {};
      }

  if (role == Qt::SizeHintRole) {
    return QSize(0, 48);
  }

  if (role == Qt::DisplayRole) {
    const auto& session = (*sessions_)[static_cast<std::size_t>(index.row())];
    return QString::fromStdString(session.title_);
  }
  if (role == Qt::UserRole || role == Qt::UserRole + 1) {
    return {};
  }

  return {};
}


void SessionListModel::set_sessions(
    lnb::session_snapshot sessions
) {
  beginResetModel();
  sessions_ = std::move(sessions);
  endResetModel();
}

uint32_t SessionListModel::session_id_at(int row) const {
  if (!sessions_ ||
      row < 0 ||
      static_cast<std::size_t>(row) >= sessions_->size()) {
    return 0;
      }

  return (*sessions_)[static_cast<std::size_t>(row)].id_;
}


}
