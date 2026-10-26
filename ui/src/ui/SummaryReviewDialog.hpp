#pragma once

#include <QDialog>
#include <QWidget>

#include "local-notebook-core/summary.hpp"

namespace ui {

// Reviews one summarisation request before it is sent. It shows the exact
// payload as plain text and the watermark updates the pass will make, then asks
// the user whether it should run. Nothing here is editable.
class SummaryReviewDialog : public QDialog {
public:
  explicit SummaryReviewDialog(
      const lnb::SummaryPlan& plan, QWidget* parent = nullptr
  );
};

// Shows the review dialog and returns true when the user approves the pass.
// Safe to call from a worker thread: the dialog is shown on the interface
// thread and the caller blocks until it is dismissed.
bool review_summary(const lnb::SummaryPlan& plan);

} // namespace ui
