#pragma once
#include "db.hpp"
#include "ui.hpp"
#include <QJsonArray>
namespace lnb::context {
struct Selection { QJsonArray messages; std::size_t estimated_tokens = 0; };
Selection select_recent(const std::vector<message>&, uint32_t, const std::string&, const std::string&, const general&);
bool should_summarize(const std::vector<message>&, const std::string&, const general&);
}
