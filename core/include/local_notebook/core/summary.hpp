#pragma once

#include <boost/json/object.hpp>

#include <cstddef>
#include <functional>
#include <string>

namespace lnb {

// A description of one summarisation request, built immediately before it is
// transmitted so the UI can show the user exactly what will leave the machine.
// `request` is the exact body; `kind` names the pass; `automatic` is true when
// the token threshold triggered the pass rather than the user asking for it.
//
// Always strict.
struct SummaryPlan {
  std::string kind;
  boost::json::object request;
  std::string watermark_text;

  // Approximate model-facing input size of `request`, using the same
  // character-based estimate as the rest of the app.
  std::size_t estimated_input_tokens = 0;

  // True when the pass continues in further requests after this one (a large
  // overflow is folded in several bounded batches).
  bool more_requests = false;
};

// Called on the worker thread immediately before a summarisation request is
// transmitted. Return true to send it, false to skip the pass. An empty
// approver always proceeds, so headless callers and tests are unaffected.
using SummaryApprover = std::function<bool(const SummaryPlan&)>;

} // namespace lnb
