// Regression test: a session summary or assistant memory pass runs its request
// on a throwaway task so its internal text never reaches the transcript. The
// Tasks window reads the visible task, so the throwaway task must mirror its live
// in-flight projections onto it; before this was fixed the row stayed at zero
// until the pass ended and only then jumped to the final totals.
#include "local_notebook/core/generation_task.hpp"

#include <iostream>
#include <string>

namespace {

int failures = 0;

void expect(bool condition, const std::string& label) {
  if (condition) {
    return;
  }

  ++failures;
  std::cerr << "FAIL: " << label << '\n';
}

} // namespace

int main() {
  using lnb::GenerationTask;

  {
    GenerationTask visible;
    visible.start();

    GenerationTask silent;
    silent.start();
    silent.mirror_streaming_to(&visible);

    silent.set_streaming_input(120);
    silent.set_streaming_output(30);

    GenerationTask::Snapshot snapshot = visible.snapshot();
    expect(
        snapshot.streaming_input_tokens == 120,
        "the input projection was not mirrored"
    );
    expect(
        snapshot.streaming_output_tokens == 30,
        "the output projection was not mirrored"
    );
    expect(
        snapshot.input_tokens == 0 && snapshot.output_tokens == 0,
        "mirroring must not add the measured totals"
    );

    // The caller adds the measured totals to the visible task once the request
    // ends; that replaces the projection, so nothing is double-counted.
    silent.add_usage(120, 30, true);
    visible.add_usage(120, 30, true);

    snapshot = visible.snapshot();
    expect(
        snapshot.input_tokens == 120 && snapshot.output_tokens == 30,
        "the measured totals were not added to the visible task"
    );
    expect(
        snapshot.streaming_input_tokens == 0 &&
            snapshot.streaming_output_tokens == 0,
        "the measured totals must clear the mirrored projection"
    );
  }

  {
    // An automatic fold that merely precedes a reply keeps its generated summary
    // out of the visible output total, so only the input is mirrored.
    GenerationTask visible;
    visible.start();

    GenerationTask silent;
    silent.start();
    silent.mirror_streaming_to(&visible, false);

    silent.set_streaming_input(80);
    silent.set_streaming_output(40);

    GenerationTask::Snapshot snapshot = visible.snapshot();
    expect(
        snapshot.streaming_input_tokens == 80,
        "the input projection must still be mirrored"
    );
    expect(
        snapshot.streaming_output_tokens == 0,
        "the output projection must not be mirrored"
    );

    silent.clear_streaming_usage();
    expect(
        visible.snapshot().streaming_input_tokens == 0,
        "clearing the projection must be mirrored"
    );
  }

  {
    // A visible task that is not running ignores projections, just as it ignores
    // its own, so a late mirror cannot animate a finished row.
    GenerationTask visible;

    GenerationTask silent;
    silent.start();
    silent.mirror_streaming_to(&visible);
    silent.set_streaming_input(10);

    expect(
        visible.snapshot().streaming_input_tokens == 0,
        "a task that is not running must not accept a mirrored projection"
    );
  }

  if (failures != 0) {
    std::cerr << failures << " failure(s)\n";
    return 1;
  }

  std::cout << "generation-task-mirror: ok\n";
  return 0;
}
