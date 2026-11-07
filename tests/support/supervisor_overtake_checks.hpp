// Included inside the supervisor test namespace. Witness setup may submit lifecycle
// inputs while the supervisor is held; after the overtake ONLY external producers act.
std::vector<const Edge*> ProducerWitness(const Graph& graph, const Edge& target) {
  struct Node {
    std::string key;
    std::set<std::string> armed;
    std::vector<const Edge*> path;
  };
  const auto start = std::find_if(graph.paths.begin(), graph.paths.end(),
                                  [](const auto& item) { return item.second.empty(); });
  Require(start != graph.paths.end(), "missing job_created seed");
  std::deque<Node> pending{{start->first, {}, {}}};
  std::set<std::string> seen;
  std::map<std::string, std::vector<const Edge*>> outgoing;
  for (const auto& edge : graph.edges) outgoing[edge.before].push_back(&edge);
  while (!pending.empty()) {
    auto node = std::move(pending.front());
    pending.pop_front();
    if (!seen.insert(node.key + Json(node.armed).dump()).second) continue;
    if (node.key == target.before &&
        ProducerAllows(node.armed, graph.classes.at(node.key), target.event, target.payload))
      return node.path;
    for (const auto* edge : outgoing[node.key]) {
      if (!ProducerAllows(node.armed, graph.classes.at(node.key), edge->event, edge->payload))
        continue;
      Node next{edge->after, ProducerAfter(node.armed, *edge, graph.classes.at(edge->after)),
                node.path};
      next.path.push_back(edge);
      pending.push_back(std::move(next));
    }
  }
  throw std::runtime_error("no deliverable witness for " + target.Name());
}
void SubmitExternalOrSetup(Fixture& fixture, const std::string& event, Json payload) {
  const Uuid job{kJob};
  if (event == "cancel_accepted" || event == "terminate_accepted") {
    Completed(fixture.writer,
              fixture.writer.SubmitCommand(
                  {1, event == "cancel_accepted" ? CommandType::kCancel : CommandType::kTerminate,
                   job, payload.at("principal_subject").get<std::string>()}));
  } else if (event == "timeout_expired") {
    const auto phase_name = payload.at("phase").get<std::string>();
    const std::array<std::string_view, 4> phases{"preparation", "execution", "cooperative_stop",
                                                 "process_exit_confirmation"};
    const auto found = std::find(phases.begin(), phases.end(), phase_name);
    Require(found != phases.end(), "unknown timeout discriminator");
    const auto index = static_cast<std::size_t>(found - phases.begin());
    const auto slot = fixture.writer.InspectSlotForTest(job);
    Require(slot.has_value(), "timer source lacks resident slot");
    const auto result = fixture.writer.SubmitTimeout(
        {job, static_cast<TimeoutPhase>(index), slot->timer_generation[index]});
    Require(!result.discarded, "generated deliverable timeout was not actually armed");
    Completed(fixture.writer, result.admitted);
  } else {
    const RawCandidateEvent input{1, job, event, payload.dump()};
    const auto result = event == "worker_completed" || event == "worker_failed"
                            ? fixture.writer.SubmitWorker(input)
                            : fixture.writer.SubmitCandidate(input);
    if (result.code == IngressCode::kCoalescedPending) {
      Require(fixture.consumed_producer_completions.contains(result.ingress_sequence),
              "coalesced retry does not reference this producer's already-consumed delivery");
      Require(!fixture.writer.TakeCompletion(result.ingress_sequence),
              "coalesced retry must not consume or manufacture another completion");
    } else {
      Completed(fixture.writer, result);
      Require(fixture.consumed_producer_completions.insert(result.ingress_sequence).second,
              "external producer consumed one completion twice");
      if (event == "worker_completed" || event == "worker_failed")
        fixture.pending_worker_delivery = input;
    }
  }
  const auto snapshot = fixture.writer.SnapshotFor(job);
  if (fixture.pending_worker_delivery && snapshot &&
      (snapshot->process_exit_confirmed || IsTerminalState(snapshot->state))) {
    if (!snapshot->process_exit_confirmed)
      Require(fixture.writer.RetireWorkerAck(*fixture.pending_worker_delivery),
              "producer could not return its authorized exact Worker delivery identity");
    fixture.pending_worker_delivery.reset();
  }
}
Json MaterializeOracle(const Graph& graph, const Json& snapshot, const std::string& event) {
  for (const auto& payload : graph.payloads.at(event)) {
    if (event == "terminal_outcome_committed" &&
        payload.at("outcome") != (snapshot.at("latched_reason").is_null()
                                      ? snapshot.at("completion_candidate")
                                      : snapshot.at("latched_reason")))
      continue;
    if (event == "cleanup_status_recorded" && payload.at("status") != "completed") continue;
    if (event == "worker_launch_observed" && payload.at("outcome") != "started") continue;
    if (event == "process_exit_confirmed" && payload.at("completion_mode") != "cooperative")
      continue;
    const auto& branch =
        Select(graph.Row(snapshot.at("state").get<std::string>(), event), snapshot, payload);
    if (branch.at("disposition") != "reject") return payload;
  }
  throw std::runtime_error("oracle cannot materialize an accepted continuation: " + event);
}
std::string ExternalContinuation(const Json& snapshot) {
  const auto state = snapshot.at("state").get<std::string>();
  if (state == "preparing")
    return snapshot.at("worker_launch_status") == "intent_recorded" ? "worker_launch_observed"
                                                                    : "worker_running";
  if (state == "running") return "worker_completed";
  if (state == "stopping") return "process_exit_confirmed";
  if (state == "finalizing" && snapshot.at("session_retention_status") == "requested")
    return "session_retained";
  return "process_exit_confirmed";
}
Json ExpectedFinish(const Graph& graph, Json snapshot) {
  std::set<std::string> visited;
  for (;;) {
    Require(visited.insert(snapshot.dump()).second, "oracle continuation made no progress");
    const auto decision = Expected(snapshot, true);
    if (decision.at("kind") == "Done") return snapshot;
    const auto event = decision.at("kind") == "Submit" ? decision.at("event").get<std::string>()
                                                       : ExternalContinuation(snapshot);
    const auto payload = MaterializeOracle(graph, snapshot, event);
    snapshot =
        Fold(snapshot,
             Select(graph.Row(snapshot.at("state").get<std::string>(), event), snapshot, payload),
             payload);
  }
}
void FinishExternalProducers(Fixture& fixture, const Graph& graph) {
  std::set<std::string> visited;
  for (;;) {
    fixture.Idle();
    const auto snapshot = fixtures::SnapshotJson(*fixture.writer.SnapshotFor(Uuid{kJob}));
    const auto decision = Expected(snapshot, true);
    if (decision.at("kind") == "Done") return;
    Require(decision.at("kind") == "Wait", "supervisor stopped while a Submit was still owed");
    Require(visited.insert(snapshot.dump()).second, "external continuation made no progress");
    const auto event = ExternalContinuation(snapshot);
    if (event == "worker_launch_observed")
      Require(fixture.ports.TakeLaunch().has_value(), "runner fact has no handoff");
    if (event == "session_retained")
      Require(fixture.ports.TakeSession().has_value(), "retainer fact has no handoff");
    SubmitExternalOrSetup(fixture, event, MaterializeOracle(graph, snapshot, event));
  }
}
void InjectOvertake(Fixture& fixture, const Graph& graph, const Edge& edge) {
  Require(Arm(fixture.writer, "before_mark_drain"),
          "supervisor cannot hold generated witness setup");
  Require(fixture.New().value == kJob, "witness identity differs from generated creation");
  Require(WaitBarrier(fixture.writer), "creation mark not held during witness setup");
  for (const auto* input : ProducerWitness(graph, edge))
    SubmitExternalOrSetup(fixture, input->event, input->payload);
  Require(fixtures::SnapshotJson(*fixture.writer.SnapshotFor(Uuid{kJob})) ==
              graph.classes.at(edge.before),
          "real reducer setup differs from generated committed class");
  const auto expected = Expected(graph.classes.at(edge.before), true);
  Require(Advance(fixture.writer, "after_decision", expected.at("event").get<std::string>()),
          "cannot place overtake after decision");
  Require(WaitBarrier(fixture.writer), "generated Submit decision was not held");
  SubmitExternalOrSetup(fixture, edge.event, edge.payload);
  Require(fixtures::SnapshotJson(*fixture.writer.SnapshotFor(Uuid{kJob})) ==
              graph.classes.at(edge.after),
          "overtake did not commit the reducer-derived snapshot");
  Require(ReleaseBarrier(fixture.writer), "overtaken decision not released");
  FinishExternalProducers(fixture, graph);
  const auto actual = fixtures::SnapshotJson(*fixture.writer.SnapshotFor(Uuid{kJob}));
  Require(actual == ExpectedFinish(graph, graph.classes.at(edge.after)),
          "autonomous overtake ended in wrong outcome/cleanup");
  const auto state = View(fixture.writer, Uuid{kJob});
  Require(state.at("supported") && state.at("max_in_flight") <= 1 && state.at("breaches") == 0,
          "generated overtake violated one-flight or stale-rejection handling");
}
