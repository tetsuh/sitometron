// Additional concrete threading/ownership interleavings; included in the test namespace.
bool EmptySupervisorSlot(const JobOrchestrator::SlotState& slot) {
  return slot.supervisor_mark_empty && slot.supervisor_parked_empty &&
         slot.supervisor_accounting_empty && slot.launch_inputs_empty;
}
void MarkVector(const Json&) {
  Fixture fixture;
  const auto job = fixture.New();
  fixture.Idle();
  auto request = fixture.ports.TakeLaunch();
  Require(request.has_value(), "missing autonomous launch before mark coalescing");
  Require(Arm(fixture.writer, "before_mark_drain"), "missing coalesced-mark checkpoint");
  Completed(fixture.writer,
            fixture.writer.SubmitCandidate(
                {1, job, "worker_launch_observed",
                 Json{{"operation_id", request->intent.operation_id.value}, {"outcome", "started"}}
                     .dump()}));
  Require(WaitBarrier(fixture.writer), "first external mark was not held");
  const auto before = View(fixture.writer, job);
  Completed(fixture.writer, fixture.writer.SubmitCandidate(
                                fixtures::MakeWorkerRunning(job, request->intent.worker_id)));
  Require(Advance(fixture.writer, "after_decision"), "cannot drain latest coalesced mark");
  Require(WaitBarrier(fixture.writer), "latest state did not produce a decision");
  const auto after = View(fixture.writer, job);
  Require(after.at("supported") && after.at("physical_slots") == 5 &&
              after.at("decisions").get<std::uint64_t>() ==
                  before.at("decisions").get<std::uint64_t>() + 1 &&
              fixture.writer.SnapshotFor(job)->state == JobState::kRunning,
          "two undrained commits did not lead to one decision on latest running snapshot");
  Require(ReleaseBarrier(fixture.writer), "mark barrier not released");
  Completed(fixture.writer, fixture.writer.SubmitWorker(
                                fixtures::MakeWorkerCompleted(job, request->intent.worker_id, 1)));
  fixture.Exit(job, *request);
  fixture.Retain();
  fixture.Closed(job, "succeeded");
}
void FlightVector(const Json& vector) {
  Fixture fixture;
  Require(Arm(fixture.writer, "after_decision", "resources_committed"),
          "missing one-flight decision barrier");
  const auto job = fixture.New();
  Require(WaitBarrier(fixture.writer), "missing first supervisor decision");
  Require(fixture.writer.ArmBarrier(WriterPhase::kAfterApply), "writer apply barrier unavailable");
  Require(ReleaseBarrier(fixture.writer), "resource decision not released");
  const auto sequence = WaitSubmission(fixture.writer, job);
  Require(sequence != 0 && fixture.writer.WaitUntil(sequence, WriterPhase::kAfterApply),
          "own input did not reach installed-but-incomplete epoch");
  const auto held = View(fixture.writer, job);
  Require(held.at("supported") && held.at("in_flight") == 1 && held.at("max_in_flight") == 1,
          "own completion/mark allowed a second in-flight submission");
  const auto cancel =
      fixture.writer.SubmitCommand({1, CommandType::kCancel, job, "operator@example"});
  Require(cancel.code == IngressCode::kAdmitted, "external control could not queue during flight");
  Require(fixture.writer.Release(sequence, WriterPhase::kAfterApply),
          "own apply barrier not released");
  Completed(fixture.writer, cancel);
  fixture.Retain();
  fixture.Closed(job, "cancelled");
  Require(View(fixture.writer, job).at("max_in_flight") == 1,
          "mark during completion issued another own input");
  (void)vector;
}
void RejectedNoMark() {
  Fixture fixture;
  const auto job = fixture.New();
  fixture.Idle();
  const auto before = View(fixture.writer, job);
  Require(before.at("supported"), "post-commit marks not inspectable");
  const auto result = fixture.writer.SubmitCandidate(fixtures::MakeSessionRetained(job));
  Require(result.code == IngressCode::kAdmitted,
          "invalid normal input not queued for reducer rejection");
  const auto completion = fixture.writer.AwaitCompletion(result.ingress_sequence);
  Require(completion && completion->code == Completion::Code::kReducerRejection,
          "negative control unexpectedly committed");
  fixture.Idle();
  const auto after = View(fixture.writer, job);
  Require(after.at("decisions") == before.at("decisions") && !after.at("marked").get<bool>(),
          "rejected turn emitted a post-commit mark");
}
void ManyJobs() {
  Fixture fixture;
  std::vector<Uuid> jobs;
  for (unsigned index = 0; index < 4; ++index) {
    jobs.push_back(fixture.New());
    fixture.Idle();
    Require(fixture.ports.TakeLaunch().has_value(), "one resident lost its launch mark");
    const auto view = View(fixture.writer, jobs.back());
    Require(view.at("supported") && view.at("physical_slots") == 5 && view.at("max_in_flight") <= 1,
            "mark/parking/accounting omitted max_jobs + 1 physical slots");
  }
  const auto overflow = Create(fixture.writer);
  Require(overflow.ingress.code == IngressCode::kResidentLimit,
          "all-open resident population must not steal provisional spare for another Job");
  ShutdownJoined(fixture.writer, jobs.front());
}
void CloseWithoutWorker(Fixture& fixture, const Uuid& job) {
  const auto launch = fixture.Running(job);
  SubmitControl(fixture.writer, job);
  fixture.Exit(job, launch);
  fixture.Retain();
  fixture.Closed(job, "cancelled");
}
void RetirementVector(const Json& vector) {
  Fixture fixture(1);
  const auto job = fixture.New();
  std::optional<std::size_t> old_index;
  const bool held_completion = vector.at("scenario") == "old_completion_new_occupant";
  if (held_completion) {
    const auto launch = fixture.Running(job);
    SubmitControl(fixture.writer, job);
    fixture.Exit(job, launch);
    Require(Arm(fixture.writer, "before_completion_take", "cleanup_status_recorded"),
            "missing completed-old-flight checkpoint");
    fixture.Retain(false);
    Require(WaitBarrier(fixture.writer), "last own cleanup completion was not held");
  } else
    CloseWithoutWorker(fixture, job);
  const auto old_slot = fixture.writer.InspectSlotForTest(job);
  Require(old_slot.has_value(), "victim slot not inspectable");
  old_index = old_slot->index;
  const auto replacement = fixture.New();
  Require(!fixture.writer.SnapshotFor(job) && fixture.writer.SnapshotFor(replacement),
          "closed victim did not retire");
  if (held_completion)
    Require(ReleaseBarrier(fixture.writer), "old completed flight was not reaped");
  fixture.Idle();
  const auto reset = fixture.writer.InspectSlotForTest(*old_index);
  Require(reset && EmptySupervisorSlot(*reset),
          "retirement did not reset marks, parking, accounting and launch inputs");
  Require(fixture.writer.SnapshotFor(replacement)->worker_launch_status ==
              LaunchStatus::kIntentRecorded,
          "reaping old completion erased new occupant's mark");
  CloseWithoutWorker(fixture, replacement);
  const auto third = fixture.New();
  fixture.Idle();
  const auto reused = fixture.writer.InspectSlotForTest(third);
  Require(reused && reused->index == *old_index,
          "second retirement did not rotate into the original reset spare");
}
void CloseAfterDecision(const Json& vector) {
  Fixture fixture;
  const auto event = vector.at("parameters").at("event").get<std::string>();
  Require(Arm(fixture.writer, "after_decision", event), "missing shutdown decision checkpoint");
  const auto job = fixture.New();
  if (event != "resources_committed" && event != "worker_launch_intent") {
    const auto launch = fixture.Running(job);
    Completed(fixture.writer, fixture.writer.SubmitWorker(
                                  fixtures::MakeWorkerCompleted(job, launch.intent.worker_id, 1)));
    // Return the Worker producer's ownership through a real matching process exit,
    // without waiting for the supervisor (which may already be held at retention).
    Completed(fixture.writer, fixture.writer.SubmitCandidate(
                                  {1, job, "process_exit_confirmed",
                                   Json{{"completion_mode", "cooperative"},
                                        {"launch_operation_id", launch.intent.operation_id.value}}
                                       .dump()}));
    if (event != "session_retain_requested") fixture.Retain(false);
  }
  Require(WaitBarrier(fixture.writer), "target own input did not reach after-decision boundary");
  const auto before = fixtures::SnapshotJson(*fixture.writer.SnapshotFor(job));
  const auto marker = fixture.writer.SubmitShutdown();
  Require(marker.code == IngressCode::kAdmitted && !fixture.writer.ready(),
          "normal admission did not close atomically");
  Require(ReleaseBarrier(fixture.writer), "shutdown-held supervisor not released");
  Completed(fixture.writer, marker);
  Require(fixture.writer.FinishShutdown() && fixture.writer.sealed(), "shutdown failed to seal");
  Require(fixtures::SnapshotJson(*fixture.writer.SnapshotFor(job)) == before,
          "supervisor admitted a new input after shutdown, including a critical continuation");
  const auto state = View(fixture.writer, job);
  Require(state.at("supported") && state.at("joined") && state.at("in_flight") == 0,
          "shutdown sealed before supervisor joined/drained");
}
void CloseInFlight(const Json& vector) {
  Fixture fixture;
  Require(Arm(fixture.writer, "after_decision", "resources_committed"),
          "missing in-flight decision checkpoint");
  const auto job = fixture.New();
  Require(WaitBarrier(fixture.writer), "initial decision not held");
  const std::map<std::string, WriterPhase> phases{
      {"before_dequeue", WriterPhase::kBeforeDequeue},
      {"before_commit", WriterPhase::kBeforeCommit},
      {"after_commit", WriterPhase::kAfterCommit},
      {"after_apply", WriterPhase::kAfterApply},
      {"after_effects", WriterPhase::kAfterEffects},
      {"response_released", WriterPhase::kResponseReleased},
      {"turn_finished", WriterPhase::kTurnFinished}};
  const auto phase = phases.at(vector.at("parameters").at("phase").get<std::string>());
  Require(fixture.writer.ArmBarrier(phase), "writer in-flight barrier unavailable");
  Require(Advance(fixture.writer, "before_completion_take", "resources_committed"),
          "in-flight completion ownership was not held through closure");
  const auto sequence = WaitSubmission(fixture.writer, job);
  Require(sequence != 0 && fixture.writer.WaitUntil(sequence, phase),
          "own input missed selected writer phase");
  const auto marker = fixture.writer.SubmitShutdown();
  Require(marker.code == IngressCode::kAdmitted, "shutdown marker not ordered after own input");
  Require(fixture.writer.Release(sequence, phase), "in-flight writer barrier not released");
  Completed(fixture.writer, marker);
  Require(fixture.writer.FinishShutdown() && fixture.writer.sealed(),
          "pre-marker own completion was not drained/joined");
  const auto snapshot = fixture.writer.SnapshotFor(job);
  Require(snapshot && snapshot->state == JobState::kPreparing &&
              snapshot->worker_launch_status == LaunchStatus::kNotStarted,
          "successful already-admitted completion started a next input after closure");
  const auto state = View(fixture.writer, job);
  Require(state.at("supported") && state.at("joined") && state.at("in_flight") == 0 &&
              state.at("reads_after_sealed") == 0,
          "completion taken twice/lost, join missed, or snapshot read after sealing");
}
// The observer owns a producer lock before shutdown starts. Once the stop hook tries that
// lock, the observer reads ingress state before releasing it. Calling the hook under ingress
// ownership creates a deterministic lock cycle, not a scheduler-dependent observation race.
void ProducerStopLockBoundary(std::string_view mode) {
  struct Probe {
    std::mutex producer_mutex;
    std::barrier<> held{2};
    std::barrier<> entered{2};
    std::atomic_bool first{true};
    std::atomic_bool stopped{false};
    std::atomic_bool joined{false};
    std::atomic_bool stop_before_join{true};
    bool closed = false;
    void Stop() noexcept {
      if (first.exchange(false)) entered.arrive_and_wait();
      std::lock_guard lock(producer_mutex);
      stopped.store(true);
    }
    bool Join() noexcept {
      if (!stopped.load()) stop_before_join.store(false);
      joined.store(true);
      return true;
    }
  } probe;
  Ports ports;
  auto config = Fixture::Configuration(ports);
  config.hold_supervisor_for_test = true;
  config.producer_context = &probe;
  config.stop_producers = [](void* context) noexcept { static_cast<Probe*>(context)->Stop(); };
  config.join_producers = [](void* context) noexcept {
    return static_cast<Probe*>(context)->Join();
  };
  auto writer = std::make_unique<JobOrchestrator>(config);
  auto* const ingress = writer.get();
  std::thread observer([&probe, ingress] {
    std::lock_guard lock(probe.producer_mutex);
    probe.held.arrive_and_wait();
    probe.entered.arrive_and_wait();
    probe.closed = !ingress->ready() && ingress->InspectSupervisorForTest(Uuid{}).stopped;
  });
  probe.held.arrive_and_wait();
  std::cout << "producer stop hook lock boundary: " << mode << '\n' << std::flush;
  IngressResult marker;
  if (mode == "shutdown")
    marker = writer->SubmitShutdown();
  else if (mode == "failure")
    writer->LatchReadinessFailure();
  else
    writer.reset();
  observer.join();
  Require(probe.closed, "producer stop ran before admission closure/supervisor stop publication");
  if (mode == "shutdown") {
    Completed(*writer, marker);
    Require(writer->FinishShutdown(), "producer lock boundary shutdown did not seal");
  } else if (mode == "failure") {
    Require(writer->WaitForWriterIdle() && writer->sealed(),
            "producer lock boundary failure did not seal");
  }
  Require(probe.stopped.load() && probe.joined.load() && probe.stop_before_join.load(),
          "producer join ran before out-of-lock stop completed");
}
std::size_t LiveSupervisors() { return JobOrchestrator::LiveSupervisorThreadsForTest(); }
void DestructionVector(bool barrier) {
  Ports ports;
  const auto initial = LiveSupervisors();
  struct JoinHold {
    std::mutex mutex;
    std::condition_variable wake;
    bool entered = false;
    bool released = false;
    bool Join() noexcept {
      std::unique_lock lock(mutex);
      entered = true;
      wake.notify_all();
      wake.wait(lock, [&] { return released; });
      return true;
    }
  } hold;
  auto config = Fixture::Configuration(ports);
  config.producer_context = &hold;
  config.join_producers = [](void* context) noexcept {
    return static_cast<JoinHold*>(context)->Join();
  };
  auto writer = std::make_unique<JobOrchestrator>(config);
  if (barrier)
    Require(Arm(*writer, "after_decision", "resources_committed"),
            "destructor barrier unavailable");
  const auto created = Create(*writer);
  Completed(*writer, created.ingress);
  if (barrier)
    Require(WaitBarrier(*writer), "destructor test missed held supervisor");
  else
    Require(Drain(*writer), "destructor fixture did not drain");
  const auto endpoint = writer->RetainedCallback();
  std::thread destroyer([&] { writer.reset(); });
  {
    std::unique_lock lock(hold.mutex);
    hold.wake.wait(lock, [&] { return hold.entered; });
  }
  // The retained endpoint remains lifetime-safe while destruction joins producers.
  // Normal admission must already be closed, not left open until scheduler teardown.
  const auto during = endpoint.Invoke(fixtures::MakeSessionRetained(*created.job_id));
  {
    std::lock_guard lock(hold.mutex);
    hold.released = true;
    hold.wake.notify_all();
  }
  destroyer.join();
  Require(during.code == IngressCode::kAdmissionClosed,
          "destruction left normal admission open while joining producers");
  Require(LiveSupervisors() == initial, "destruction leaked a live supervisor thread");
  Require(endpoint.Invoke(fixtures::MakeSessionRetained(*created.job_id)).code ==
              IngressCode::kAdmissionClosed,
          "normal destruction must close lifetime-safe endpoint access");
}
void ReplayClosedVector() {
  Ports ports;
  auto config = Fixture::Configuration(ports);
  const auto document = Contract();
  const auto found =
      std::find_if(document.at("classes").begin(), document.at("classes").end(),
                   [](const auto& row) { return row.at("snapshot").at("state") == "admitted"; });
  Require(found != document.at("classes").end(), "missing admitted replay representative");
  config.replayed_jobs = {fixtures::SnapshotFrom(found->at("snapshot"))};
  config.unresolved_jobs = {Uuid{kJob}};
  JobOrchestrator writer(config);
  Require(Drain(writer) && !writer.ready() && ports.Journal().empty(),
          "replayed unresolved Job was enrolled or driven");
  SupervisorStateExists(writer, Uuid{kJob});
  Require(View(writer, Uuid{kJob}).at("submissions") == 0,
          "replay consulted launch inputs or submitted a continuation");
}
bool CapacityWait(JobOrchestrator& writer) { return writer.WaitForNormalCapacityOrClosure(); }
void ParkedShutdown() {
  Fixture fixture;
  Require(Arm(fixture.writer, "after_decision", "resources_committed"),
          "parked shutdown decision barrier unavailable");
  const auto job = fixture.New();
  Require(WaitBarrier(fixture.writer), "initial decision not held");
  Require(fixture.writer.ArmPause(WriterPhase::kBeforeDequeue),
          "writer saturation pause unavailable");
  const auto first = fixture.writer.SubmitCandidate(fixtures::MakeSessionRetained(job));
  const auto second = fixture.writer.SubmitCandidate(fixtures::MakeSessionRetained(job));
  Require(first.code == IngressCode::kAdmitted && second.code == IngressCode::kAdmitted,
          "capacity not saturated");
  Require(Advance(fixture.writer, "after_park") && WaitBarrier(fixture.writer),
          "normal_full did not park");
  const auto marker = fixture.writer.SubmitShutdown();
  Require(marker.code == IngressCode::kAdmitted, "parked shutdown marker refused");
  Require(ReleaseBarrier(fixture.writer), "parked supervisor not woken by closure");
  Require(fixture.writer.Release(first.ingress_sequence, WriterPhase::kBeforeDequeue),
          "writer not released");
  Require(fixture.writer.AwaitCompletion(first.ingress_sequence).has_value() &&
              fixture.writer.AwaitCompletion(second.ingress_sequence).has_value(),
          "queued completions lost at closure");
  Completed(fixture.writer, marker);
  Require(fixture.writer.FinishShutdown() && View(fixture.writer, job).at("joined"),
          "parked supervisor did not join before sealing");
  Require(fixture.writer.SnapshotFor(job)->state == JobState::kAdmitted,
          "capacity wake restarted a parked Job after closure");
}
void SessionProducerVector(bool close_at_capacity) {
  Fixture fixture;
  const auto job = fixture.New();
  const auto launch = fixture.Running(job);
  SubmitControl(fixture.writer, job);
  fixture.Exit(job, launch);
  fixture.Idle();
  auto request = fixture.ports.TakeSession();
  Require(request && request->session_id == job,
          "asynchronous Session mailbox has no bound request");
  if (!close_at_capacity) {
    std::optional<Completion> completion;
    std::thread producer([&] {
      const auto result =
          fixture.writer.SubmitCandidate(fixtures::MakeSessionRetained(request->job_id));
      if (result.code == IngressCode::kAdmitted)
        completion = fixture.writer.AwaitCompletion(result.ingress_sequence);
    });
    producer.join();
    Require(completion && completion->code == Completion::Code::kSuccess,
            "separate Session producer did not commit retained fact");
    fixture.Closed(job, "cancelled");
    ShutdownJoined(fixture.writer, job);
    return;
  }

  Require(fixture.writer.ArmPause(WriterPhase::kBeforeDequeue),
          "Session capacity pause unavailable");
  const auto first = fixture.writer.SubmitCandidate(fixtures::MakeSessionRetained(job));
  const auto second = fixture.writer.SubmitCandidate(fixtures::MakeSessionRetained(job));
  Require(first.code == IngressCode::kAdmitted && second.code == IngressCode::kAdmitted,
          "Session normal FIFO not full");
  std::barrier refusal(2);
  bool full = false;
  bool resumed = true;
  std::thread producer([&] {
    full = fixture.writer.SubmitCandidate(fixtures::MakeSessionRetained(job)).code ==
           IngressCode::kNormalFull;
    refusal.arrive_and_wait();
    resumed = CapacityWait(fixture.writer);
  });
  refusal.arrive_and_wait();
  const auto marker = fixture.writer.SubmitShutdown();
  producer.join();
  Require(full && !resumed, "Session normal_full capacity wait ignored admission closure");
  Require(fixture.writer.Release(first.ingress_sequence, WriterPhase::kBeforeDequeue),
          "Session writer pause not released");
  Require(fixture.writer.AwaitCompletion(first.ingress_sequence).has_value() &&
              fixture.writer.AwaitCompletion(second.ingress_sequence).has_value(),
          "admitted Session results did not drain");
  Completed(fixture.writer, marker);
  Require(fixture.writer.FinishShutdown() && View(fixture.writer, job).at("joined"),
          "Session producer/core did not join before sealed");
}
void ThreadVector(const Json& vector) {
  const auto scenario = vector.at("scenario").get<std::string>();
  if (scenario == "destruction_without_shutdown" || scenario == "destruction_with_barrier") {
    DestructionVector(scenario == "destruction_with_barrier");
    return;
  }
  if (scenario == "replay_closed_no_drive") {
    ReplayClosedVector();
    return;
  }
  if (scenario == "parked_shutdown") {
    ParkedShutdown();
    return;
  }
  if (scenario == "session_async_mailbox" || scenario == "session_capacity_closure") {
    SessionProducerVector(scenario == "session_capacity_closure");
    return;
  }
  Fixture fixture;
  if (scenario == "idle_shutdown") {
    ShutdownJoined(fixture.writer, Uuid{});
    return;
  }
  Require(scenario == "failure_inside_supervisor_ingress" || scenario == "readiness_failure" ||
              scenario == "separate_thread_no_handoff_reentry",
          "unconsumed threading vector");
  if (scenario == "failure_inside_supervisor_ingress")
    Require(Arm(fixture.writer, "after_decision", "resources_committed"),
            "failure ingress barrier unavailable");
  const auto job = fixture.New();
  if (scenario == "failure_inside_supervisor_ingress") {
    Require(WaitBarrier(fixture.writer), "supervisor did not reach failing ingress call");
    fixture.writer.InjectAccountingCorruption();
    Require(ReleaseBarrier(fixture.writer), "failing supervisor not released");
    fixture.Idle();
  } else
    fixture.Idle();
  SupervisorStateExists(fixture.writer, job);
  if (scenario == "readiness_failure")
    Require(fixture.writer.LatchReadinessFailure(), "readiness failure did not latch");
  if (scenario == "readiness_failure" || scenario == "failure_inside_supervisor_ingress") {
    fixture.Idle();
    Require(fixture.writer.failed() && View(fixture.writer, job).at("joined"),
            "failure path did not join before automatic sealing");
  } else
    ShutdownJoined(fixture.writer, job);
  const auto state = View(fixture.writer, job);
  Require(state.at("distinct_threads") && state.at("writer_reentries") == 0 &&
              state.at("handoff_reentries") == 0 && state.at("reads_after_sealed") == 0,
          "supervisor thread identity/reentry/sealing discipline violated");
}
