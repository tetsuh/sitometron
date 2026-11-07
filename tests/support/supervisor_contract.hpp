#ifndef SITOMETRON_TESTS_SUPPORT_SUPERVISOR_CONTRACT_HPP_
#define SITOMETRON_TESTS_SUPPORT_SUPERVISOR_CONTRACT_HPP_

#include <algorithm>
#include <deque>
#include <fstream>
#include <map>
#include <nlohmann/json.hpp>
#include <set>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

// Independent test oracle: interpret the normative reducer, never the supervisor's code.
namespace sitometron::test::supervisor {
using Json = nlohmann::json;
inline Json Load(std::string_view relative) {
  std::ifstream stream(std::string(SITOMETRON_TEST_SOURCE_DIR) + "/" + std::string(relative));
  if (!stream) throw std::runtime_error("cannot read " + std::string(relative));
  return Json::parse(stream);
}
inline const std::string kJob = "01890f30-7b54-7cc3-98c4-dc0c0c07398f";
inline const std::string kWorker = "550e8400-e29b-41d4-a716-446655440000";
inline const std::string kAllocation = "phase1-empty-allocation";
inline const std::string kDigest =
    "44136fa355b3678a1146ad16f7e8649e94fb4fc21fe77e8310c060f61caaff8a";
inline Json Canon(Json value) {
  if (value.is_array()) {
    for (auto& item : value) item = Canon(item);
  } else if (value.is_object()) {
    for (auto& [key, item] : value.items()) {
      if (item.is_null()) continue;
      if (key == "job_id" || key == "session_id")
        item = kJob;
      else if (key == "worker_id" || key == "pending_worker_id")
        item = kWorker;
      else if (key == "operation_id" || key == "launch_operation_id")
        item = "launch-op-1";
      else if (key == "allocation_id")
        item = kAllocation;
      else if (key == "allocation_digest")
        item = kDigest;
      else if (key == "event_sequence" || key == "pending_worker_event_sequence")
        item = 1;
      else
        item = Canon(item);
    }
  }
  return value;
}
inline bool Matches(const Json& when, const Json& snapshot, const Json& payload) {
  const auto operation = when.at("operator").get<std::string>();
  if (operation == "always" || operation == "otherwise") return true;
  if (operation != "all") throw std::runtime_error("unsupported predicate: " + operation);
  for (const auto& predicate : when.at("predicates")) {
    auto source = [&](const std::string& name) -> const Json& {
      if (name == "snapshot") return snapshot;
      if (name == "payload") return payload;
      throw std::runtime_error("unsupported predicate source: " + name);
    };
    const auto& left = source(predicate.at("source")).at(predicate.at("field").get<std::string>());
    const auto op = predicate.at("operator").get<std::string>();
    bool result = false;
    if (op == "eq")
      result = left == predicate.at("value");
    else if (op == "not_eq")
      result = left != predicate.at("value");
    else if (op == "in") {
      const auto& values = predicate.at("values");
      result = std::find(values.begin(), values.end(), left) != values.end();
    } else if (op == "eq_field")
      result =
          left ==
          source(predicate.at("right_source")).at(predicate.at("right_field").get<std::string>());
    else
      throw std::runtime_error("unsupported predicate operator: " + op);
    if (!result) return false;
  }
  return true;
}
inline const Json& Select(const Json& row, const Json& snapshot, const Json& payload) {
  for (const auto& branch : row.at("cases"))
    if (Matches(branch.at("when"), snapshot, payload)) return branch;
  throw std::runtime_error("non-total reducer row: " + row.at("pair_id").get<std::string>());
}
inline Json Fold(Json snapshot, const Json& branch, const Json& payload) {
  if (branch.contains("next_state")) snapshot["state"] = branch.at("next_state");
  if (branch.contains("updates")) {
    for (const auto& [field, update] : branch.at("updates").items()) {
      if (!update.is_object())
        snapshot[field] = update;
      else {
        const auto from = update.at("from").get<std::string>();
        if (from == "input.job_id")
          snapshot[field] = kJob;
        else if (from.starts_with("payload."))
          snapshot[field] = payload.at(from.substr(8));
        else
          throw std::runtime_error("unsupported update: " + from);
      }
    }
  }
  return snapshot;
}
inline Json Expected(const Json& snapshot, bool launch) {
  int rule = 13;
  std::string event;
  Json sources = Json::array();
  if (snapshot.is_null())
    rule = 1;
  else {
    const auto state = snapshot.at("state").get<std::string>();
    if (state == "admitted") {
      rule = 2;
      event = "resources_committed";
    } else if (state == "preparing" && snapshot.at("worker_launch_status") == "not_started") {
      rule = launch ? 4 : 3;
      if (launch) event = "worker_launch_intent";
    } else if (state == "preparing" || state == "running" || state == "stopping") {
      rule = 5;
      sources = {"application_runner", "worker_protocol", "timer_port", "commands"};
    } else if (state == "finalizing") {
      if (snapshot.at("session_retention_status") == "not_started") {
        rule = 6;
        event = "session_retain_requested";
      } else if (snapshot.at("session_retention_status") == "requested") {
        rule = 7;
        sources = {"session_retainer"};
      } else if (snapshot.at("session_retention_status") == "retained" &&
                 snapshot.at("finalization_status") == "pending") {
        rule = 8;
        event = "finalization_completed";
      } else if (snapshot.at("finalization_status") == "completed" ||
                 snapshot.at("finalization_status") == "failed") {
        rule = 9;
        event = "terminal_outcome_committed";
      } else
        throw std::runtime_error("uncovered finalizing class");
    } else if (state == "succeeded" || state == "failed" || state == "cancelled" ||
               state == "terminated" || state == "timed_out") {
      if (!snapshot.at("process_exit_confirmed").get<bool>() &&
          snapshot.at("cleanup_status") == "pending") {
        rule = 10;
        sources = {"application_runner", "timer_port"};
      } else if (snapshot.at("process_exit_confirmed").get<bool>() &&
                 snapshot.at("resource_status") == "committed") {
        rule = 11;
        event = "resources_released";
      } else if (snapshot.at("cleanup_status") == "pending") {
        rule = 12;
        event = "cleanup_status_recorded";
      }
    } else
      throw std::runtime_error("unknown state: " + state);
  }
  Json result{{"rule", rule},
              {"kind", event.empty() ? (sources.empty() ? "Done" : "Wait") : "Submit"},
              {"event", event},
              {"sources", sources},
              {"payload_rule", "none"}};
  if (rule == 2) result["payload_rule"] = "fixed_empty_allocation";
  if (rule == 4) result["payload_rule"] = "creation_launch_identity_and_committed_allocation";
  if (rule == 6) result["payload_rule"] = "bound_session";
  if (rule == 8) result["payload_rule"] = "empty";
  if (rule == 9) result["payload_rule"] = "latched_reason_or_success_candidate";
  if (rule == 11) result["payload_rule"] = "bound_allocation";
  if (rule == 12) result["payload_rule"] = "cleanup_completed";
  return result;
}
struct Edge {
  std::string before;
  std::string after;
  std::string pair;
  std::string branch;
  std::string event;
  Json payload;
  Json effects;
  std::string Name() const { return pair + "/" + branch + "/" + payload.dump(); }
};
inline bool ProducerAllows(const std::set<std::string>& history, const Json& snapshot,
                           std::string_view event, const Json& payload) {
  if (event == "timeout_expired") return history.contains(payload.at("phase").get<std::string>());
  if ((event == "worker_completed" || event == "worker_failed") &&
      snapshot.at("pending_worker_event_ack").get<bool>())
    return history.contains("worker_delivery:" + std::string(event));
  return true;
}
inline std::set<std::string> ProducerAfter(std::set<std::string> history, const Edge& edge,
                                           const Json& after) {
  // A delivered registration is one-shot even when the reducer emits no disarm effect.
  if (edge.event == "timeout_expired") history.erase(edge.payload.at("phase").get<std::string>());
  for (const auto& value : edge.effects) {
    const auto effect = value.get<std::string>();
    for (const auto* phase :
         {"preparation", "execution", "cooperative_stop", "process_exit_confirmation"}) {
      if (effect == "disarm_" + std::string(phase) + "_timeout") history.erase(phase);
      if (effect == "arm_" + std::string(phase) + "_timeout" ||
          (effect == "arm_" + std::string(phase) + "_timeout_if_needed" &&
           !after.at("process_exit_confirmed").get<bool>()))
        history.insert(phase);
    }
  }
  if (!after.at("pending_worker_event_ack").get<bool>()) {
    history.erase("worker_delivery:worker_completed");
    history.erase("worker_delivery:worker_failed");
  } else if (edge.event == "worker_completed" || edge.event == "worker_failed")
    history.insert("worker_delivery:" + edge.event);
  return history;
}
struct Graph {
  Json contract;
  std::map<std::string, Json> classes;
  std::vector<Edge> edges;
  std::map<std::string, std::vector<std::size_t>> paths;
  std::map<std::string, std::vector<Json>> payloads;
  const Json& Row(std::string_view position, std::string_view event, bool command = false) const {
    for (const auto& row : contract.at(command ? "command_matrix" : "event_matrix"))
      if (row.at("entity_position") == position &&
          row.at(command ? "command_kind" : "event_kind") == event)
        return row;
    throw std::runtime_error("missing reducer row");
  }
  Graph() : contract(Load("schemas/core/v1/job-reducer-contract.json")) {
    const auto vectors = Load("schemas/core/v1/job-reducer-vectors.json");
    std::map<std::string, std::map<std::string, Json>> prototypes;
    for (const auto& vector : vectors.at("case_vectors")) {
      if (vector.at("matrix") != "event" || vector.at("expected").at("disposition") == "reject")
        continue;
      const auto& input = vector.at("input");
      const auto payload = Canon(input.at("payload"));
      prototypes[input.at("event_type").get<std::string>()][payload.dump()] = payload;
    }
    // Partition every payload discriminator using the contract, not a list of selected overtakes.
    for (const auto& event_value : contract.at("journal_event_kinds")) {
      const auto event = event_value.get<std::string>();
      if (prototypes[event].empty()) throw std::runtime_error("missing payload domain: " + event);
      std::map<std::string, std::set<Json>> domains;
      for (const auto& [unused, payload] : prototypes[event]) {
        (void)unused;
        for (const auto* field : {"phase", "outcome", "completion_mode", "status"})
          if (payload.contains(field)) domains[field].insert(payload.at(field));
      }
      for (const auto& row : contract.at("event_matrix")) {
        if (row.at("event_kind") != event) continue;
        for (const auto& branch : row.at("cases")) {
          for (const auto& predicate : branch.at("when").value("predicates", Json::array())) {
            if (predicate.at("source") != "payload") continue;
            const auto op = predicate.at("operator").get<std::string>();
            const auto field = predicate.at("field").get<std::string>();
            if (op == "eq")
              domains[field].insert(predicate.at("value"));
            else if (op == "in")
              for (const auto& value : predicate.at("values")) domains[field].insert(value);
          }
        }
      }
      std::map<std::string, Json> expanded;
      for (const auto& [unused, prototype] : prototypes[event]) {
        (void)unused;
        std::vector<Json> candidates{prototype};
        for (const auto& [field, values] : domains) {
          if (!prototype.contains(field)) continue;
          std::vector<Json> next;
          for (const auto& candidate : candidates)
            for (const auto& value : values) {
              auto payload = candidate;
              payload[field] = value;
              next.push_back(std::move(payload));
            }
          candidates = std::move(next);
        }
        for (const auto& payload : candidates) expanded[payload.dump()] = payload;
      }
      for (const auto& [unused, payload] : expanded) {
        (void)unused;
        payloads[event].push_back(payload);
      }
    }
    const auto start =
        Fold(Json{{"schema_version", 1}}, Row("absent", "job_created").at("cases").at(0),
             Json{{"session_id", kJob}});
    classes[start.dump()] = start;
    paths[start.dump()] = {};
    std::deque<std::string> pending{start.dump()};
    while (!pending.empty()) {
      const auto key = pending.front();
      pending.pop_front();
      const auto snapshot = classes.at(key);
      const auto state = snapshot.at("state").get<std::string>();
      for (const auto& event_value : contract.at("journal_event_kinds")) {
        const auto event = event_value.get<std::string>();
        if (event == "job_created" || event == "late_worker_event") continue;
        for (const auto& payload : payloads.at(event)) {
          if ((event == "cancel_accepted" || event == "terminate_accepted") &&
              Select(Row(state, event.substr(0, event.size() - 9), true), snapshot, payload)
                      .at("disposition") == "reject")
            continue;
          const auto& row = Row(state, event);
          const auto& branch = Select(row, snapshot, payload);
          if (branch.at("disposition") == "reject") continue;
          const auto after = Fold(snapshot, branch, payload);
          const auto after_key = after.dump();
          edges.push_back({key, after_key, row.at("pair_id"), branch.at("case_id"), event, payload,
                           branch.value("post_sync_effects", Json::array())});
          if (!classes.contains(after_key)) {
            classes[after_key] = after;
            paths[after_key] = paths.at(key);
            paths[after_key].push_back(edges.size() - 1);
            pending.push_back(after_key);
          }
        }
      }
    }
  }
  bool AcceptsSubmit(const Json& snapshot, const Json& decision) const {
    if (decision.at("kind") != "Submit") return true;
    const auto event = decision.at("event").get<std::string>();
    for (const auto& payload : payloads.at(event)) {
      if (event == "terminal_outcome_committed" &&
          payload.at("outcome") != (snapshot.at("latched_reason").is_null()
                                        ? snapshot.at("completion_candidate")
                                        : snapshot.at("latched_reason")))
        continue;
      if (event == "cleanup_status_recorded" && payload.at("status") != "completed") continue;
      if (Select(Row(snapshot.at("state").get<std::string>(), event), snapshot, payload)
              .at("disposition") != "reject")
        return true;
    }
    return false;
  }
  // Propagate real arm/disarm histories and pending Worker delivery discriminators.
  // A conflicting Worker event cannot replace an unacknowledged critical-gate identity.
  std::map<std::string, std::set<std::set<std::string>>> ArmedPaths() const {
    std::map<std::string, std::set<std::set<std::string>>> reached;
    std::deque<std::pair<std::string, std::set<std::string>>> pending;
    const auto start = std::find_if(paths.begin(), paths.end(),
                                    [](const auto& item) { return item.second.empty(); });
    if (start == paths.end()) throw std::runtime_error("no creation seed");
    reached[start->first].insert(std::set<std::string>{});
    pending.emplace_back(start->first, std::set<std::string>{});
    std::map<std::string, std::vector<const Edge*>> outgoing;
    for (const auto& edge : edges) outgoing[edge.before].push_back(&edge);
    while (!pending.empty()) {
      auto [key, armed] = std::move(pending.front());
      pending.pop_front();
      for (const auto* edge : outgoing[key]) {
        if (!ProducerAllows(armed, classes.at(key), edge->event, edge->payload)) continue;
        const auto next = ProducerAfter(armed, *edge, classes.at(edge->after));
        if (reached[edge->after].insert(next).second) pending.emplace_back(edge->after, next);
      }
    }
    return reached;
  }
};
inline bool OtherProducer(std::string_view event) {
  return event == "cancel_accepted" || event == "terminate_accepted" ||
         event == "timeout_expired" || event == "worker_launch_observed" ||
         event == "process_exit_confirmed" || event == "worker_running" ||
         event == "worker_completed" || event == "worker_failed" || event == "session_retained";
}
}  // namespace sitometron::test::supervisor
#endif
