#include "ConversationController.hpp"
#include "ConversationView.hpp"

#include "../external.hpp"

#include <QJsonObject>
#include <QString>

#include <exception>
#include <optional>
#include <utility>

namespace ui {

ConversationController::ConversationController(
    lnb::ApplicationState& state,
    lnb::session session_info,
    ConversationView* view
)
    : state_(state),
      session_info_(std::move(session_info)),
      view_(view) {
  reload();
}

ConversationController::~ConversationController() {
  // Stop an in-flight generation; the worker observes the stop token and
  // unwinds quickly.
  if (task_) {
    task_->cancel();
  }
}

void ConversationController::reload() {
  const auto messages =
      state_.uni_db_->get_messages_for_session(session_info_.id_, state_.self_id_);

  view_->set_messages(messages, state_.self_id_);
}

void ConversationController::send(const std::string& text) {
  if (busy_ || text.empty()) {
    return;
  }

  // Persisting the outgoing message can fail on a stale or unavailable database.
  try {
    state_.uni_db_->insert_message(state_.self_id_, session_info_.id_, text);
    reload();
  } catch (const std::exception& error) {
    view_->show_error(QString::fromUtf8(error.what()));
    return;
  }

  task_ = std::make_shared<lnb::GenerationTask>();
  task_->start();

  busy_ = true;
  view_->set_busy(true);
  view_->set_streaming_status("Thinking...");
  view_->set_streaming_text(QString{});

  auto task = task_;
  const auto stop_token = task_->stop_token();
  const auto api_url = session_info_.api_url_;
  const auto api_key = session_info_.authentication_token_;
  const auto proxy = session_info_.proxy_;
  const auto shared_directory = session_info_.shared_directory_;

  state_.multi_executor_->push(
      [task,
       stop_token,
       state = &state_,
       session_id = session_info_.id_,
       self_id = state_.self_id_,
       model = session_info_.model_,
       api_url,
       api_key,
       proxy,
       shared_directory]() mutable {
        try {
          // Fold older turns into the session memory before building the
          // payload, so the request always carries
          // custom instructions + memory summary + recent messages.
          lnb::maybe_summarize(
              *state, session_id, self_id, *task, stop_token, model, api_url, api_key, proxy
          );

          auto payload = lnb::prepare_payload(*state, session_id, self_id, model);
          if (!payload) throw std::runtime_error("Unable to prepare the request payload");
          lnb::generate_reply(
              std::move(*payload), *task, stop_token, api_url, api_key, proxy, shared_directory,
              *state->uni_config_
          );

          task->complete();
        } catch (const std::exception& error) {
          // A stop request is a normal outcome, not a failure: cancel() has
          // already moved the task into the CANCELLED state.
          if (stop_token.stop_requested()) {
            return;
          }

          task->fail(error.what());
        } catch (...) {
          if (stop_token.stop_requested()) {
            return;
          }

          task->fail("Unknown generation error");
        }
      }
  );
}

void ConversationController::cancel() {
  if (task_) {
    task_->cancel();
  }
}

void ConversationController::delete_message(uint32_t message_id) {
  if (message_id == 0) {
    return;
  }

  state_.uni_db_->remove_message_by_id(message_id);
  reload();
}

void ConversationController::edit_message(
    uint32_t message_id,
    const std::string& text
) {
  if (message_id == 0) {
    return;
  }

  state_.uni_db_->update_message(message_id, text);
  reload();
}

void ConversationController::poll() {
  if (!task_) {
    return;
  }

  const auto snapshot = task_->snapshot();

  if (snapshot.status == lnb::GenerationTask::Status::RUNNING) {
    if (!snapshot.operation_status.empty()) {
      view_->set_streaming_status(
          QString::fromUtf8(
              snapshot.operation_status.data(),
              static_cast<qsizetype>(snapshot.operation_status.size())
          )
      );
    }

    view_->set_streaming_text(
        QString::fromUtf8(
            snapshot.display.data(),
            static_cast<qsizetype>(snapshot.display.size())
        )
    );
    return;
  }

  const bool completed =
      snapshot.status == lnb::GenerationTask::Status::COMPLETED;
  const bool failed =
      snapshot.status == lnb::GenerationTask::Status::FAILED;

  const QString error = QString::fromUtf8(
      snapshot.error.data(),
      static_cast<qsizetype>(snapshot.error.size())
  );

  // Only completed turns are persisted. A cancelled turn is deliberately
  // dropped: reload() below also removes the partial output from the screen.
  if (completed && !snapshot.content.empty()) {
    state_.uni_db_->insert_message(
        session_info_.id_, state_.self_id_, snapshot.content
    );
  }

  task_.reset();
  busy_ = false;
  view_->set_busy(false);
  reload();

  if (failed) {
    view_->show_error(error);
  }
}

} // namespace ui
