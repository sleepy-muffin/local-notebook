// Headless tests for the wire-protocol layer (net/protocol.hpp): the Chat and
// Responses request bodies, the two tool-schema shapes, base-URL handling and
// the per-model reasoning levels. Dependency-free: a tiny CHECK macro and a
// main() that returns non-zero on the first failure.
//
// Run via CTest (`ctest --test-dir build/linux-x64-debug -R protocol`) or
// directly.

#include <cstdio>
#include <string>
#include <vector>

#include <QJsonArray>
#include <QJsonObject>
#include <QJsonValue>

#include "net/protocol.hpp"

namespace {

int g_failures = 0;

void check(bool condition, const char* expression, int line) {
  if (!condition) {
    std::fprintf(stderr, "FAIL line %d: %s\n", line, expression);
    ++g_failures;
  }
}

#define CHECK(expr) check((expr), #expr, __LINE__)

std::string text_of(const QJsonValue& value) {
  return value.toString().toStdString();
}

lnb::net::OutgoingMessage user_message(const std::string& content) {
  lnb::net::OutgoingMessage message;
  message.role = lnb::net::OutgoingMessage::Role::User;
  message.content = content;
  return message;
}

lnb::net::ToolCall make_call(
    const std::string& id, const std::string& name, const std::string& arguments
) {
  lnb::net::ToolCall call;
  call.id = QString::fromStdString(id);
  call.name = QString::fromStdString(name);
  call.arguments = QString::fromStdString(arguments);
  return call;
}

void test_chat_body() {
  lnb::net::Request request;
  request.model = "gpt-5.6-terra";
  request.instructions = "SYS";
  request.messages = {user_message("hello")};
  request.filesystem_tools = false;
  request.max_output_tokens = 1234;
  request.reasoning_effort = "low";

  const QJsonObject body = lnb::net::build_chat_request(request);

  CHECK(text_of(body.value("model")) == "gpt-5.6-terra");
  CHECK(body.value("stream").toBool());

  const QJsonArray messages = body.value("messages").toArray();
  CHECK(messages.size() == 2);
  CHECK(text_of(messages.at(0).toObject().value("role")) == "system");
  CHECK(text_of(messages.at(0).toObject().value("content")) == "SYS");
  CHECK(text_of(messages.at(1).toObject().value("role")) == "user");
  CHECK(text_of(messages.at(1).toObject().value("content")) == "hello");

  // Chat nests the schema under `function`.
  const QJsonArray tools = body.value("tools").toArray();
  CHECK(!tools.isEmpty());
  const QJsonObject tool = tools.first().toObject();
  CHECK(text_of(tool.value("type")) == "function");
  CHECK(tool.value("function").isObject());
  CHECK(tool.value("function").toObject().contains("name"));

  // Chat uses `max_tokens` and a top-level `reasoning_effort`.
  CHECK(body.value("max_tokens").toInt() == 1234);
  CHECK(text_of(body.value("reasoning_effort")) == "low");
  CHECK(!body.contains("max_output_tokens"));
  CHECK(!body.contains("input"));
  CHECK(!body.contains("reasoning"));
}

void test_chat_optional_fields() {
  lnb::net::Request request;
  request.model = "m";
  request.messages = {user_message("x")};
  request.max_output_tokens = 0;
  request.reasoning_effort = "";

  const QJsonObject body = lnb::net::build_chat_request(request);

  // No system message when the instructions are empty; no token/effort keys
  // when they are unset, so the model default applies.
  CHECK(body.value("messages").toArray().size() == 1);
  CHECK(!body.contains("max_tokens"));
  CHECK(!body.contains("reasoning_effort"));
}

void test_dispatch() {
  lnb::net::Request request;
  request.model = "m";
  request.instructions = "S";
  request.messages = {user_message("x")};

  const QJsonObject chat = lnb::net::build_request(lnb::net::ApiProtocol::ChatCompletions, request);
  const QJsonObject responses = lnb::net::build_request(lnb::net::ApiProtocol::Responses, request);

  CHECK(chat.contains("messages"));
  CHECK(responses.contains("input"));
  CHECK(!responses.contains("messages"));
  CHECK(responses.contains("instructions"));
}

void test_responses_body() {
  lnb::net::Request request;
  request.model = "gpt-6-astra";
  request.instructions = "SYS";
  request.messages = {user_message("hello")};
  request.max_output_tokens = 9000;
  request.reasoning_effort = "medium";

  const QJsonObject body = lnb::net::build_responses_request(request);

  CHECK(text_of(body.value("model")) == "gpt-6-astra");
  CHECK(text_of(body.value("instructions")) == "SYS");
  CHECK(body.value("stream").toBool());

  const QJsonArray input = body.value("input").toArray();
  CHECK(input.size() == 1);
  CHECK(text_of(input.first().toObject().value("role")) == "user");
  CHECK(text_of(input.first().toObject().value("content")) == "hello");

  // Responses flattens the schema onto the tool itself.
  const QJsonArray tools = body.value("tools").toArray();
  CHECK(!tools.isEmpty());
  const QJsonObject tool = tools.first().toObject();
  CHECK(text_of(tool.value("type")) == "function");
  CHECK(tool.contains("name"));
  CHECK(tool.contains("parameters"));
  CHECK(!tool.contains("function"));

  CHECK(body.value("max_output_tokens").toInt() == 9000);
  CHECK(text_of(body.value("reasoning").toObject().value("effort")) == "medium");
  CHECK(!body.contains("max_tokens"));
  CHECK(!body.contains("reasoning_effort"));
}

void test_responses_tool_round_trip() {
  lnb::net::Request request;
  request.model = "m";
  request.messages = {user_message("read it")};

  // An assistant turn that called a tool, echoed back as a raw item, and the
  // tool's result expressed as an outgoing ToolResult.
  lnb::net::OutgoingMessage assistant;
  assistant.role = lnb::net::OutgoingMessage::Role::Assistant;
  assistant.content = "Looking...";
  assistant.tool_calls = {make_call("call_1", "read_file", "{\"path\":\"a\"}")};
  request.messages.push_back(assistant);

  lnb::net::OutgoingMessage result;
  result.role = lnb::net::OutgoingMessage::Role::ToolResult;
  result.tool_call_id = "call_1";
  result.content = "file body";
  request.messages.push_back(result);

  // A reasoning item from the previous round, replayed verbatim.
  request.raw_items = {QJsonObject{
      {"type", "reasoning"},
      {"id", "rs_1"},
      {"encrypted_content", "opaque"},
  }};

  const QJsonArray input = lnb::net::build_responses_request(request).value("input").toArray();

  // user, assistant message, function_call, function_call_output, reasoning.
  CHECK(input.size() == 5);
  CHECK(text_of(input.at(0).toObject().value("role")) == "user");

  const QJsonObject message = input.at(1).toObject();
  CHECK(text_of(message.value("role")) == "assistant");
  CHECK(text_of(message.value("content")) == "Looking...");

  const QJsonObject call = input.at(2).toObject();
  CHECK(text_of(call.value("type")) == "function_call");
  CHECK(text_of(call.value("call_id")) == "call_1");
  CHECK(text_of(call.value("name")) == "read_file");
  CHECK(text_of(call.value("arguments")) == "{\"path\":\"a\"}");

  const QJsonObject output = input.at(3).toObject();
  CHECK(text_of(output.value("type")) == "function_call_output");
  CHECK(text_of(output.value("call_id")) == "call_1");
  CHECK(text_of(output.value("output")) == "file body");

  const QJsonObject reasoning = input.at(4).toObject();
  CHECK(text_of(reasoning.value("type")) == "reasoning");
  CHECK(text_of(reasoning.value("encrypted_content")) == "opaque");
}

void test_chat_ignores_raw_items() {
  lnb::net::Request request;
  request.model = "m";
  request.messages = {user_message("x")};
  request.raw_items = {QJsonObject{{"type", "reasoning"}, {"id", "rs_1"}}};

  const QJsonObject body = lnb::net::build_chat_request(request);
  CHECK(body.value("messages").toArray().size() == 1);
  CHECK(!body.contains("input"));
}

void test_normalize_base_url() {
  CHECK(lnb::net::normalize_base_url("https://api.example.com/v1/chat/completions")
        == "https://api.example.com/v1");
  CHECK(lnb::net::normalize_base_url("https://api.example.com/v1/completions")
        == "https://api.example.com/v1");
  CHECK(lnb::net::normalize_base_url("https://api.example.com/v1/responses")
        == "https://api.example.com/v1");
  CHECK(lnb::net::normalize_base_url("https://api.example.com/v1/") == "https://api.example.com/v1");
  CHECK(lnb::net::normalize_base_url("  https://api.example.com/v1  ") == "https://api.example.com/v1");
  CHECK(lnb::net::normalize_base_url("https://api.example.com/v1") == "https://api.example.com/v1");
}

void test_is_valid_base_url() {
  CHECK(lnb::net::is_valid_base_url("https://api.example.com/v1"));
  CHECK(lnb::net::is_valid_base_url("http://localhost:8080/v1"));
  CHECK(lnb::net::is_valid_base_url("https://api.example.com/v1/"));

  // Must end in /v1, must be http(s), no query or fragment, must have a host.
  CHECK(!lnb::net::is_valid_base_url("https://api.example.com/v1/chat/completions"));
  CHECK(!lnb::net::is_valid_base_url("https://api.example.com"));
  CHECK(!lnb::net::is_valid_base_url("ftp://api.example.com/v1"));
  CHECK(!lnb::net::is_valid_base_url("api.example.com/v1"));
  CHECK(!lnb::net::is_valid_base_url("https://api.example.com/v1?x=1"));
  CHECK(!lnb::net::is_valid_base_url("https://api.example.com/v1#f"));
  CHECK(!lnb::net::is_valid_base_url(""));
}

void test_ensure_scheme() {
  CHECK(lnb::net::ensure_scheme("example.com/v1") == "https://example.com/v1");
  CHECK(lnb::net::ensure_scheme("  example.com/v1 ") == "https://example.com/v1");
  CHECK(lnb::net::ensure_scheme("http://example.com/v1") == "http://example.com/v1");
  CHECK(lnb::net::ensure_scheme("https://example.com/v1") == "https://example.com/v1");
  CHECK(lnb::net::ensure_scheme("HTTPS://example.com/v1") == "HTTPS://example.com/v1");
  CHECK(lnb::net::ensure_scheme("./v1") == "https://./v1");
}

void test_derive_endpoint() {
  CHECK(lnb::net::derive_endpoint("https://api.example.com/v1", lnb::net::ApiProtocol::Responses)
        == "https://api.example.com/v1/responses");
  CHECK(lnb::net::derive_endpoint("https://api.example.com/v1", lnb::net::ApiProtocol::ChatCompletions)
        == "https://api.example.com/v1/chat/completions");
  // A legacy full URL normalises to its base first.
  CHECK(lnb::net::derive_endpoint("https://api.example.com/v1/chat/completions", lnb::net::ApiProtocol::Responses)
        == "https://api.example.com/v1/responses");
}

void test_reasoning_lists() {
  const std::vector<std::string> deepseek =
      lnb::net::reasoning_efforts_for_model("deepseek v4.1 flash");
  CHECK((deepseek == std::vector<std::string>{"none", "low", "high", "max"}));

  const std::vector<std::string> luna = lnb::net::reasoning_efforts_for_model("GPT-6.1 Luna");
  CHECK((luna == std::vector<std::string>{"none", "medium", "low", "high", "xhigh", "max"}));

  const std::vector<std::string> astra = lnb::net::reasoning_efforts_for_model("gpt-6-astra");
  CHECK((astra == std::vector<std::string>{"medium", "low", "high", "xhigh", "max"}));
  CHECK(lnb::net::reasoning_efforts_for_model("GPT-6.1 Sol")
        == lnb::net::reasoning_efforts_for_model("gpt-6.1-sol"));

  const std::vector<std::string> fallback = lnb::net::reasoning_efforts_for_model("some-model");
  CHECK((fallback == std::vector<std::string>{"none", "low", "high"}));
  CHECK((lnb::net::reasoning_efforts_for_model("") == std::vector<std::string>{"none", "low", "high"}));
}

void test_reasoning_defaults_and_support() {
  CHECK(lnb::net::default_reasoning_effort("deepseek v4.1 flash") == "none");
  CHECK(lnb::net::default_reasoning_effort("gpt-6-astra") == "medium");
  CHECK(lnb::net::default_reasoning_effort("unknown") == "none");

  // Empty always means "the model's default" and is allowed.
  CHECK(lnb::net::reasoning_effort_supported("gpt-6-astra", ""));
  CHECK(lnb::net::reasoning_effort_supported("gpt-6-astra", "medium"));

  // `none` is not available on the models that reject it.
  CHECK(!lnb::net::reasoning_effort_supported("gpt-6-astra", "none"));
  CHECK(!lnb::net::reasoning_effort_supported("GPT-6.1 Sol", "none"));
  CHECK(!lnb::net::reasoning_effort_supported("gpt-6-astra", "minimal"));

  // Membership is case-insensitive.
  CHECK(lnb::net::reasoning_effort_supported("gpt-6-astra", "MEDIUM"));
}

void test_reasoning_with_stored() {
  // A supported stored level is not duplicated.
  CHECK((lnb::net::reasoning_efforts_with_stored("gpt-6-astra", "high") ==
         std::vector<std::string>{"medium", "low", "high", "xhigh", "max"}));

  // An unsupported stored level is appended, so a selector can still show it.
  CHECK((lnb::net::reasoning_efforts_with_stored("gpt-6-astra", "none") ==
         std::vector<std::string>{"medium", "low", "high", "xhigh", "max", "none"}));

  // An empty stored level adds nothing (it means "the model's default").
  CHECK((lnb::net::reasoning_efforts_with_stored("gpt-6-astra", "") ==
         lnb::net::reasoning_efforts_for_model("gpt-6-astra")));
}

void test_protocol_names() {
  CHECK(lnb::net::protocol_name(lnb::net::ApiProtocol::Responses) == "responses");
  CHECK(lnb::net::protocol_name(lnb::net::ApiProtocol::ChatCompletions) == "chat_completions");

  CHECK(lnb::net::protocol_from_name("chat_completions") == lnb::net::ApiProtocol::ChatCompletions);
  CHECK(lnb::net::protocol_from_name("responses") == lnb::net::ApiProtocol::Responses);
  // Unknown or missing defaults to Responses.
  CHECK(lnb::net::protocol_from_name("") == lnb::net::ApiProtocol::Responses);
  CHECK(lnb::net::protocol_from_name("nonsense") == lnb::net::ApiProtocol::Responses);
}

} // namespace

int main() {
  test_chat_body();
  test_chat_optional_fields();
  test_dispatch();
  test_responses_body();
  test_responses_tool_round_trip();
  test_chat_ignores_raw_items();
  test_normalize_base_url();
  test_is_valid_base_url();
  test_ensure_scheme();
  test_derive_endpoint();
  test_reasoning_lists();
  test_reasoning_defaults_and_support();
  test_reasoning_with_stored();
  test_protocol_names();

  if (g_failures == 0) {
    std::printf("protocol tests: all checks passed\n");
    return 0;
  }

  std::fprintf(stderr, "protocol tests: %d check(s) failed\n", g_failures);
  return 1;
}
