#pragma once

#include <QAbstractListModel>

#include <cstdint>
#include <string>
#include <vector>

#include "local-notebook-core/model.hpp"

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
    // The time of the session's newest message, formatted for the row's second
    // line (empty when the session has no messages).
    TimeRole,
    // Whether the session is pinned; the delegate draws the pin action in the
    // accent colour when it is.
    PinnedRole,
  };

  explicit SessionListModel(QObject* parent = nullptr);

  int rowCount(const QModelIndex& parent = QModelIndex()) const override;

  QVariant data(
      const QModelIndex& index, int role = Qt::DisplayRole
  ) const override;

  // Replaces the snapshot. `last_times` is parallel to `sessions`: the raw
  // ISO-8601 time of each session's newest message, or an empty string when it
  // has none.
  void set_sessions(
      std::vector<lnb::Session> sessions, std::vector<std::string> last_times
  );

  [[nodiscard]] lnb::Session session_at(int row) const;
  [[nodiscard]] std::uint32_t session_id_at(int row) const;
  [[nodiscard]] const std::string& last_time_at(int row) const;
  [[nodiscard]] bool pinned_at(int row) const;
  [[nodiscard]] int row_for(std::uint32_t session_id) const;

private:
  std::vector<lnb::Session> sessions_;
  std::vector<std::string> last_times_;  // parallel to sessions_
};

} // namespace ui
