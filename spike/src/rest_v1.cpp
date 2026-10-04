#include "rest_v1.hpp"

#include <algorithm>
#include <cctype>
#include <cstddef>
#include <nlohmann/json.hpp>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace sitometron::spike {
namespace {

using nlohmann::json;

constexpr std::string_view k_jobs = "/v1/jobs";
constexpr std::size_t k_max_application_id = 128;

HttpResponse Error(int status, std::string_view domain, std::string_view code,
                   std::string_view message) {
  return {status, ErrorEnvelope(domain, code, message), {}};
}

HttpResponse MethodNotAllowed(std::string allow) {
  auto response =
      Error(405, "request", "method_not_allowed", "the method is not allowed for this resource");
  response.headers.emplace_back("Allow", std::move(allow));
  return response;
}

// The canonical lowercase UUIDv7 text the daemon issues.
bool IsJobId(std::string_view text) {
  if (text.size() != 36) return false;
  for (std::size_t i = 0; i < text.size(); ++i) {
    const char c = text[i];
    if (i == 8 || i == 13 || i == 18 || i == 23) {
      if (c != '-') return false;
    } else if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'))) {
      return false;
    }
  }
  return text[14] == '7' &&
         (text[19] == '8' || text[19] == '9' || text[19] == 'a' || text[19] == 'b');
}

// True for "application/json", with optional parameters, in any letter case.
bool IsJsonMediaType(std::string value) {
  if (const auto semicolon = value.find(';'); semicolon != std::string::npos) {
    value.erase(semicolon);
  }
  while (!value.empty() && (value.back() == ' ' || value.back() == '\t')) value.pop_back();
  std::ranges::transform(value, value.begin(),
                         [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
  return value == "application/json";
}

// Parses one JSON text and refuses duplicate object keys, which RFC 8259 leaves undefined.
std::optional<json> ParseStrict(const std::string& text) {
  std::vector<std::set<std::string, std::less<>>> keys;
  bool duplicate = false;
  const auto callback = [&keys, &duplicate](int, json::parse_event_t event, json& parsed) {
    if (event == json::parse_event_t::object_start) {
      keys.emplace_back();
    } else if (event == json::parse_event_t::object_end) {
      keys.pop_back();
    } else if (event == json::parse_event_t::key && !keys.empty() &&
               !keys.back().insert(parsed.get<std::string>()).second) {
      duplicate = true;
    }
    return true;
  };
  auto parsed = json::parse(text, callback, false);
  if (parsed.is_discarded() || duplicate) return std::nullopt;
  return parsed;
}

HttpResponse Refused(CreateRefusal refusal) {
  switch (refusal) {
    case CreateRefusal::kNotReady:
      return Error(503, "service", "not_ready", "the daemon is not accepting new Jobs");
    case CreateRefusal::kCapacityExhausted:
      return Error(503, "service", "capacity_exhausted", "the resident Job capacity is exhausted");
    case CreateRefusal::kBusy:
      return Error(503, "service", "busy", "the ingress queue is full");
    default:
      return Error(503, "service", "service_failed", "the writer has failed closed");
  }
}

HttpResponse CreateJob(JobDriver& driver, const Applications& applications,
                       const HttpRequest& request) {
  if (const auto media = request.headers.find("content-type");
      !request.body.empty() &&
      (media == request.headers.end() || !IsJsonMediaType(media->second))) {
    return Error(415, "request", "unsupported_media_type", "the body must be application/json");
  }
  const auto body = ParseStrict(request.body);
  if (!body) return Error(400, "request", "malformed_json", "the body is not well-formed JSON");
  const auto id = body->is_object() ? body->find("application_id") : body->end();
  if (!body->is_object() || body->size() != 1 || id == body->end() || !id->is_string()) {
    return Error(422, "request", "validation_failed",
                 "the body must be an object with exactly the string field application_id");
  }
  const auto& application_id = id->get_ref<const std::string&>();
  if (application_id.empty() || application_id.size() > k_max_application_id) {
    return Error(422, "request", "validation_failed", "application_id must be 1 to 128 characters");
  }
  const auto application = applications.find(application_id);
  if (application == applications.end()) {
    return Error(422, "job", "unknown_application", "the application is not registered");
  }
  LaunchSpec spec;
  spec.application_id = application_id;
  spec.executable = "/bin/sh";
  spec.arguments = {"-c", application->second};
  const auto outcome = driver.Create(std::move(spec));
  if (outcome.refusal != CreateRefusal::kNone) return Refused(outcome.refusal);
  auto resource = driver.Resource(outcome.job_id);
  if (resource.is_null()) return Refused(CreateRefusal::kServiceFailed);
  HttpResponse response{202, resource.dump(), {}};
  response.headers.emplace_back("Location", std::string(k_jobs) + "/" + outcome.job_id);
  return response;
}

HttpResponse Ready(const JobDriver& driver) {
  auto reasons = driver.ReadinessReasons();
  const bool ready = reasons.empty();
  // This 503 body is the readiness object, not the error envelope (ADR-0008 Section 6).
  return {ready ? 200 : 503, json{{"ready", ready}, {"reasons", std::move(reasons)}}.dump(), {}};
}

}  // namespace

HttpResponse RouteV1(JobDriver& driver, const Applications& applications,
                     const HttpRequest& request) {
  const std::string_view target = request.target;
  const bool get = request.method == "GET";
  // ADR-0008 defines no query parameters: a target with a query is no route of this contract.
  if (target.find('?') != std::string_view::npos) {
    return Error(404, "request", "route_not_found", "there is no such resource");
  }
  if (target == "/v1/health") {
    return get ? HttpResponse{200, R"({"status":"ok"})", {}} : MethodNotAllowed("GET");
  }
  if (target == "/v1/ready") return get ? Ready(driver) : MethodNotAllowed("GET");
  if (target == k_jobs) {
    if (get) return {200, json{{"jobs", driver.Resources()}}.dump(), {}};
    if (request.method == "POST") return CreateJob(driver, applications, request);
    return MethodNotAllowed("GET, POST");
  }
  if (target.size() > k_jobs.size() + 1 && target.starts_with("/v1/jobs/")) {
    const auto job_id = target.substr(k_jobs.size() + 1);
    // Anything below a Job (such as cancel, which this prototype does not serve yet) is no route.
    if (job_id.find('/') != std::string_view::npos) {
      return Error(404, "request", "route_not_found", "there is no such resource");
    }
    if (!get) return MethodNotAllowed("GET");
    if (!IsJobId(job_id)) {
      return Error(400, "request", "invalid_job_id",
                   "the Job identifier is not a canonical UUIDv7");
    }
    auto resource = driver.Resource(std::string(job_id));
    if (resource.is_null()) return Error(404, "job", "job_not_found", "there is no such Job");
    return {200, resource.dump(), {}};
  }
  return Error(404, "request", "route_not_found", "there is no such resource");
}

}  // namespace sitometron::spike
