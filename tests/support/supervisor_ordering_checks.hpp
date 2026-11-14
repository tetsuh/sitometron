// Included inside job_supervisor_test.cpp's anonymous namespace, after Fixture.
// All scenario execution lives in test code. Production hooks only pause, inspect,
// or inject a single result; they never run a scenario or certify its assertions.
bool InjectCode(JobOrchestrator& writer, IngressCode code) {
  return writer.InjectSupervisorIngressCodeForTest(code);
}
bool InjectRejection(JobOrchestrator& writer, RejectionReason reason, unsigned count) {
  return writer.InjectSupervisorRejectionForTest(reason, count);
}
bool Advance(JobOrchestrator& writer, std::string_view point, std::string_view event = {}) {
  return writer.ReleaseSupervisorBarrierAndArmForTest(point, event);
}
std::uint64_t WaitSubmission(JobOrchestrator& writer, const Uuid& job) {
  return writer.WaitForSupervisorSubmissionForTest(job);
}
void SupervisorStateExists(JobOrchestrator& writer, const Uuid& job) {
  Require(View(writer, job).at("supported"),
          "supervisor per-slot accounting/diagnostics not implemented");
}
void ShutdownJoined(JobOrchestrator& writer, const Uuid& job) {
  const auto marker = writer.SubmitShutdown();
  Require(marker.code == IngressCode::kAdmitted, "shutdown marker not admitted");
  Completed(writer, marker);
  Require(writer.FinishShutdown() && writer.sealed(), "shutdown failed to reach sealed");
  const auto state = View(writer, job);
  Require(state.at("supported") && state.at("joined") && state.at("stopped"),
          "supervisor must be actually joined before sealed");
  Require(state.at("in_flight") == 0 && state.at("reads_after_sealed") == 0,
          "untaken own completion or snapshot read after sealing");
}
void SubmitControl(JobOrchestrator& writer, const Uuid& job, bool terminate = false) {
  Completed(writer,
            writer.SubmitCommand({1, terminate ? CommandType::kTerminate : CommandType::kCancel,
                                  job, "operator@example"}));
}
void StaleDecision(const Json& vector) {
  Fixture fixture;
  const auto scenario = vector.at("scenario").get<std::string>();
  if (scenario == "stale_success_timeout") {
    const auto job = fixture.New();
    const auto request = fixture.Running(job);
    Require(Arm(fixture.writer, "after_decision", "terminal_outcome_committed"),
            "missing terminal decision barrier");
    Completed(fixture.writer, fixture.writer.SubmitWorker(
                                  fixtures::MakeWorkerCompleted(job, request.intent.worker_id, 1)));
    fixture.Retain(false);
    Require(WaitBarrier(fixture.writer), "prepared success was not held before admission");
    const auto slot = fixture.writer.InspectSlotForTest(job);
    Require(slot.has_value(), "missing current execution source");
    const auto timeout =
        fixture.writer.SubmitTimeout({job, TimeoutPhase::kExecution, slot->timer_generation[1]});
    Require(!timeout.discarded, "execution timer must remain armed through terminal commit");
    Completed(fixture.writer, timeout.admitted);
    Require(ReleaseBarrier(fixture.writer), "terminal barrier not released");
    fixture.Exit(job, request);
    fixture.Closed(job, "timed_out");
    const auto attempts = fixture.ports.Journal();
    for (const auto& attempt : attempts)
      if (attempt.job_id == job && attempt.event_type == EventType::kTerminalOutcomeCommitted)
        Require(
            std::get<TerminalOutcomePayload>(attempt.payload).outcome == TerminalOutcome::kTimedOut,
            "rejected stale success reached Journal instead of re-derivation");
  } else {
    const bool launch = scenario == "stale_launch_terminate";
    Require(Arm(fixture.writer, "after_decision",
                launch ? "worker_launch_intent" : "resources_committed"),
            "missing stale decision barrier");
    const auto job = fixture.New();
    Require(WaitBarrier(fixture.writer), "decision was not held");
    SubmitControl(fixture.writer, job, launch);
    Require(ReleaseBarrier(fixture.writer), "stale decision not released");
    fixture.Retain();
    fixture.Closed(job, launch ? "terminated" : "cancelled");
    const auto state = View(fixture.writer, job);
    Require(state.at("supported") && state.at("breaches") == 0,
            "one stale rejection incorrectly stopped the Job");
  }
}
void RejectionHistory(const Json& vector) {
  Fixture fixture;
  const auto scenario = vector.at("scenario").get<std::string>();
  Require(Arm(fixture.writer, "after_decision", "resources_committed"),
          "missing rejection decision checkpoint");
  const auto job = fixture.New();
  Require(WaitBarrier(fixture.writer), "resource decision was not held");
  Require(InjectRejection(fixture.writer, RejectionReason::kEventNotAllowedInState, 1),
          "missing deterministic single-rejection injection");
  Require(Advance(fixture.writer, "after_rejected_completion"),
          "missing post-rejection checkpoint");
  Require(WaitBarrier(fixture.writer), "rejected completion did not trigger fresh read");
  const auto first = View(fixture.writer, job);
  Require(first.at("supported") && first.at("breaches") == 0 && first.at("decisions") >= 1,
          "first rejection was not re-derived");
  if (scenario == "two_identical_rejections") {
    Require(InjectRejection(fixture.writer, RejectionReason::kEventNotAllowedInState, 1),
            "second rejection injection failed");
    Require(ReleaseBarrier(fixture.writer), "rejection checkpoint not released");
    fixture.Idle();
    const auto second = View(fixture.writer, job);
    Require(second.at("breaches") == 1 && second.at("submissions") == 2 && !fixture.writer.failed(),
            "two identical rejected inputs must stop only that Job before a third submission");
    const auto other = fixture.New();
    fixture.Idle();
    Require(fixture.writer.SnapshotFor(other)->state == JobState::kPreparing,
            "per-Job breach stopped another Job");
  } else {
    if (scenario == "same_input_different_code")
      Require(InjectRejection(fixture.writer, RejectionReason::kInvariantViolation, 1),
              "different rejection injection failed");
    if (scenario == "different_payload_same_code") {
      SubmitControl(fixture.writer, job);
      Require(InjectRejection(fixture.writer, RejectionReason::kEventNotAllowedInState, 1),
              "different-input same-code rejection injection failed");
    }
    Require(ReleaseBarrier(fixture.writer), "rejection checkpoint not released");
    fixture.Idle();
    const auto second = View(fixture.writer, job);
    Require(second.at("breaches") == 0, "nonidentical rejection history caused a breach");
    if (scenario == "success_resets_rejection_history") {
      Require(fixture.writer.SnapshotFor(job)->resource_status == ResourceStatus::kCommitted &&
                  second.at("history_empty"),
              "successful completion did not clear consecutive-rejection history");
    }
  }
}
void CapacityVector(const Json& vector) {
  Fixture fixture;
  Require(Arm(fixture.writer, "after_decision", "resources_committed"),
          "missing capacity decision barrier");
  const auto job = fixture.New();
  Require(WaitBarrier(fixture.writer), "supervisor did not prepare resource input");
  Require(fixture.writer.ArmPause(WriterPhase::kBeforeDequeue),
          "writer cannot hold capacity epoch");
  const auto first = fixture.writer.SubmitCandidate(fixtures::MakeSessionRetained(job));
  const auto second = fixture.writer.SubmitCandidate(fixtures::MakeSessionRetained(job));
  Require(first.code == IngressCode::kAdmitted && second.code == IngressCode::kAdmitted &&
              fixture.writer.normal_occupancy() == 2,
          "normal FIFO saturation premise failed");
  const auto gap = vector.at("parameters").at("gap").get<std::string>();
  const auto point = gap == "before_park"   ? "after_normal_full"
                     : gap == "before_wait" ? "after_park_before_wait"
                                            : "after_park";
  Require(Advance(fixture.writer, point), "cannot order normal_full against parking");
  Require(WaitBarrier(fixture.writer), "supervisor did not refuse/park at capacity");
  const auto parked = View(fixture.writer, job);
  Require(parked.at("supported") && parked.at("submissions") == 1,
          "normal_full busy retried before any capacity signal");
  Require(fixture.writer.Release(first.ingress_sequence, WriterPhase::kBeforeDequeue),
          "writer capacity pause release failed");
  const auto completion = fixture.writer.AwaitCompletion(first.ingress_sequence);
  Require(completion.has_value(), "saturated normal input completion lost");
  Require(completion->code == Completion::Code::kReducerRejection,
          "invalid normal turn must reject while freeing capacity");
  Require(ReleaseBarrier(fixture.writer), "parked supervisor release failed");
  const auto other_completion = fixture.writer.AwaitCompletion(second.ingress_sequence);
  Require(other_completion.has_value(), "second completion lost");
  fixture.Idle();
  Require(fixture.writer.SnapshotFor(job)->worker_launch_status == LaunchStatus::kIntentRecorded,
          "capacity freed by a rejected normal turn before/after parking was lost");
}
void IngressCodeVector(const Json& vector) {
  Fixture fixture;
  const auto code_name = vector.at("parameters").at("code").get<std::string>();
  const std::map<std::string, IngressCode> codes{
      {"admitted", IngressCode::kAdmitted},
      {"coalesced_pending", IngressCode::kCoalescedPending},
      {"already_pending", IngressCode::kAlreadyPending},
      {"resident_limit", IngressCode::kResidentLimit},
      {"admission_closed", IngressCode::kAdmissionClosed},
      {"service_failed", IngressCode::kServiceFailed}};
  Require(Arm(fixture.writer, "after_decision", "resources_committed"),
          "missing ingress-result decision barrier");
  const auto job = fixture.New();
  Require(WaitBarrier(fixture.writer), "missing ingress result epoch");
  Require(InjectCode(fixture.writer, codes.at(code_name)),
          "missing deterministic ingress-result injection");
  Require(ReleaseBarrier(fixture.writer), "result barrier not released");
  fixture.Idle();
  const auto state = View(fixture.writer, job);
  Require(state.at("supported") && state.at("max_in_flight") <= 1,
          "per-Job one-flight accounting missing");
  if (code_name == "already_pending" || code_name == "resident_limit")
    Require(state.at("breaches") == 1 && !fixture.writer.failed(),
            "impossible result must report bounded per-Job breach");
  else if (code_name == "admission_closed" || code_name == "service_failed")
    Require(state.at("stopped") && fixture.writer.SnapshotFor(job)->state == JobState::kAdmitted,
            "closed/failed result did not stop driving at committed snapshot");
  else
    Require(fixture.writer.SnapshotFor(job)->state == JobState::kPreparing &&
                state.at("in_flight") == 0,
            "admitted/coalesced own sequence did not drain exactly once");
}
#include "supervisor_additional_checks.hpp"
void OrderingVector(const Json& vector) {
  const auto scenario = vector.at("scenario").get<std::string>();
  if (scenario.starts_with("stale_"))
    StaleDecision(vector);
  else if (scenario == "two_identical_rejections" || scenario == "same_input_different_code" ||
           scenario == "different_payload_same_code" ||
           scenario == "success_resets_rejection_history")
    RejectionHistory(vector);
  else if (scenario == "capacity")
    CapacityVector(vector);
  else if (scenario == "ingress_code")
    IngressCodeVector(vector);
  else if (scenario == "coalesced_marks")
    MarkVector(vector);
  else if (scenario == "one_flight_per_job" || scenario == "mark_during_flight")
    FlightVector(vector);
  else if (scenario == "rejected_turn_no_mark")
    RejectedNoMark();
  else if (scenario == "multiple_jobs_and_spare")
    ManyJobs();
  else if (scenario == "retirement_resets_marks" || scenario == "old_completion_new_occupant")
    RetirementVector(vector);
  else if (scenario == "close_after_decision")
    CloseAfterDecision(vector);
  else if (scenario == "close_in_flight")
    CloseInFlight(vector);
  else if (vector.at("check") == "job_supervisor_no_writer_reentry")
    ThreadVector(vector);
  else
    throw std::runtime_error("unconsumed ordering vector: " + scenario);
}
