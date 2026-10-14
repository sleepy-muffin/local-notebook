#pragma once

#include <QAbstractListModel>

#include <cstdint>
#include <vector>

#include "model.hpp"

namespace ui {

// The sidebar session list. It holds a snapshot of the sessions ordered by the
// time of their newest message (the order `store::database::sessions_by_recent`
// returns); the window reloads it after every change.
class SessionListModel : public QAbstractListModel {
public:
  // Custom roles so the delegate can read the whole session row.
  enum Role {
    TitleRole = Qt::UserRole + 1,
    IdRole,
  };

  explicit SessionListModel(QObject* parent = nullptr);

  int rowCount(const QModelIndex& parent = QModelIndex()) const override;

  QVariant data(
      const QModelIndex& index, int role = Qt::DisplayRole
  ) const override;

  void set_sessions(std::vector<lnb::Session> sessions);

  [[nodiscard]] lnb::Session session_at(int row) const;
  [[nodiscard]] std::uint32_t session_id_at(int row) const;
  [[nodiscard]] int row_for(std::uint32_t session_id) const;

private:
  std::vector<lnb::Session> sessions_;
};

} // namespace ui
