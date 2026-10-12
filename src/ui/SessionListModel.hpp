#pragma once

#include "../ui.hpp"

#include <QAbstractListModel>

namespace ui {

class SessionListModel : public QAbstractListModel {
public:
  explicit SessionListModel(
      lnb::session_snapshot sessions,
      QObject* parent = nullptr
  );

  int rowCount(
      const QModelIndex& parent = QModelIndex()
  ) const override;

  QVariant data(
      const QModelIndex& index,
      int role = Qt::DisplayRole
  ) const override;

  void set_sessions(lnb::session_snapshot sessions);

  uint32_t session_id_at(int row) const;

private:
  lnb::session_snapshot sessions_;
};

}