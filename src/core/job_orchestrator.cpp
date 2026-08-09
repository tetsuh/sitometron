#include "job_orchestrator.hpp"

#include <algorithm>
#include <array>
#include <limits>
#include <nlohmann/json.hpp>
#include <string_view>
#include <utility>

namespace sitometron::core::internal {
namespace {}

struct CallbackHandle::Control {
  mutable std::mutex mutex;
  std::condition_variable cv;
  JobOrchestrator* target = nullptr;
  bool sealed = false;
  bool failed = false;
  bool allocated = false;
  bool active = false;
  bool leased = false;
  // Every path that touches target owns one invocation reference, including
  // the concurrent-invocation corruption path.
  std::size_t invocations = 0;
  std::size_t leases = 0;
};

struct JobOrchestrator::Impl {
  // These modes are modified and read only while mutex is held.  They make the
  // shutdown marker the sole running-to-draining transition.
  enum class Mode { kRunning, kQuiescing, kDraining, kSealed, kStopped };
  enum class GateKind : std::size_t {
    kTerminate,
    kPreparationTimer,
    kExecutionTimer,
    kCooperativeStopTimer,
    kProcessExitTimer,
    kWorker,
    kProcessExit,
    kResourcesReleased,
    kCleanup,
    kCount
  };
  static constexpr std::size_t kNoResident = std::numeric_limits<std::size_t>::max();
  // The reducer's closed EffectId set has no transition producing more than four
  // effects (the terminal stopping turn is the maximum).  These are startup
  // proofs, not tunable runtime capacities.
  static constexpr std::size_t kMaxPreparedEffects = 4;
  static constexpr std::size_t kMaxMappedDestinations = kMaxPreparedEffects;
  static constexpr std::size_t kMaxAckDestinations = 1;
  // Retry identities contain only the typed semantic fields needed for exact
  // coalescing.  They deliberately do not retain raw JSON or impose a raw
  // input-size limit on reducer payloads.
  struct GateIdentity {
    std::array<char, 36> job_id{};
    std::array<char, 36> worker_id{};
    std::uint8_t event_kind = 0;
    std::uint8_t timer_phase = 0;
    std::uint64_t event_sequence = 0;
    std::uint64_t timer_generation = 0;
    bool valid = false;
  };
  static constexpr std::uint8_t kNoIdentitySlot = std::numeric_limits<std::uint8_t>::max();
  struct Gate {
    bool registered = false;
    bool pending = false;
    bool retained = false;
    bool permit = false;
    bool callback_lease = false;
    std::uint64_t ingress_sequence = 0;
    std::uint8_t identity_slot = kNoIdentitySlot;
  };
  struct GeneratedIdentityBundle {
    ::sitometron::core::Uuid job_session;
    ::sitometron::core::Uuid worker;
    ::sitometron::core::StableId launch_operation;
  };
  struct Entry {
    enum class Kind { kCandidate, kCommand, kShutdown };
    Kind kind = Kind::kCandidate;
    RawCandidateEvent candidate;
    Command command;
    std::uint64_t sequence = 0;
    // A completion slot is reserved at the same mutex-protected
    // linearization point as FIFO insertion.  It remains owned until Take.
    std::size_t completion_index = kNoResident;
    std::uint64_t completion_generation = 0;
    GateIdentity identity{};
    bool identity_ready = false;
    bool critical = false;
    GateKind gate = GateKind::kCount;
    std::size_t resident_index = kNoResident;
    std::optional<GeneratedIdentityBundle> creation_identities;
    bool authorized = false;
  };
  struct CompletionSlot {
    bool reserved = false;
    bool completed = false;
    std::uint64_t generation = 0;
    std::uint64_t sequence = 0;
    Completion value{};
  };
  struct PreparedEffect {
    EffectId id = EffectId::kInvalid;
    TraceKind mapped_kind = TraceKind::kEffect;
    std::string action;
    bool has_launch = false;
    bool has_stop = false;
    bool has_session = false;
    std::uint8_t timer_index = 0;
    bool timer_update = false;
    std::uint64_t timer_generation_after = 0;
    ApplicationLaunchRequest launch{};
    ApplicationStopRequest stop{};
    SessionRetainRequest session{};
  };
  struct Resident {
    ::sitometron::core::Uuid id;
    ApplyResult banks[2];
    // Both banks own startup-reserved turn/effect storage. A turn only resets
    // the inactive bank before commit; no postcommit vector operation is needed.
    std::array<std::vector<PreparedEffect>, 2> prepared_effects;
    std::array<std::vector<TraceRecord>, 2> prepared_trace;
    int active = 0;
    bool exists = false;
    std::size_t claims = 0;
    bool worker_pending = false;
    std::uint64_t worker_sequence = 0;
    std::uint64_t worker_ingress_sequence = 0;
    RawCandidateEvent worker_payload;
    std::optional<::sitometron::core::Uuid> generated_worker;
    std::optional<::sitometron::core::StableId> generated_launch_operation;
    std::optional<GeneratedIdentityBundle> generated_identities;
    std::array<std::uint64_t, 4> timer_generation{};
    // Only the four timer gates and the Worker gate own retry identity storage. Process-exit,
    // resource-release, and cleanup gates deliberately have no identity slot.
    std::array<GateIdentity, 5> identity_slots{};
    std::array<Gate, static_cast<std::size_t>(GateKind::kCount)> gates{};
  };

  explicit Impl(Config value) : config(std::move(value)), mutex() {
    if (config.max_jobs == 0 || config.normal_capacity == 0 || config.critical_reserve() == 0 ||
        config.total_capacity() == 0 || config.trace_capacity == 0 ||
        config.completion_capacity < config.total_capacity() ||
        config.trace_capacity < config.total_capacity() ||
        config.handoff_capacity < kMaxPreparedEffects ||
        config.handoff_capacity < kMaxMappedDestinations || config.ack_capacity < config.max_jobs ||
        config.ack_capacity < kMaxAckDestinations || config.callback_registration_capacity < 2 ||
        config.initial_ingress_sequence == 0 || config.initial_journal_sequence == 0 ||
        config.ports.clock == nullptr || config.ports.journal == nullptr ||
        config.ports.runner == nullptr || config.ports.session == nullptr ||
        config.ports.identity == nullptr)
      throw std::bad_variant_access();
    fifo.resize(config.total_capacity());
    residents.resize(config.max_jobs);
    completions.resize(config.completion_capacity);
    trace.resize(config.trace_capacity);
    ingress.resize(config.trace_capacity);
    ack_observations.resize(config.ack_capacity);
    callbacks.resize(config.callback_registration_capacity);
    for (auto& resident : residents) {
      for (std::size_t bank = 0; bank != 2; ++bank) {
        resident.prepared_effects[bank].reserve(kMaxPreparedEffects);
        resident.prepared_trace[bank].reserve(config.trace_capacity);
      }
    }
  }

  Config config;
  // The ingress mutex is the sole synchronization boundary for admission,
  // fixed-slot ownership, resident metadata, and reader-visible observations.
  mutable std::mutex mutex;
  std::condition_variable cv;
  std::vector<Entry> fifo;
  std::size_t fifo_head = 0;
  std::size_t fifo_tail = 0;
  std::size_t fifo_count = 0;
  std::vector<Resident> residents;
  std::vector<CompletionSlot> completions;
  std::size_t completion_used = 0;
  std::size_t outstanding = 0;
  bool in_flight = false;
  std::uint64_t in_flight_sequence = 0;
  std::vector<TraceRecord> trace;
  std::size_t trace_count = 0;
  std::vector<std::uint64_t> ingress;
  std::size_t ingress_count = 0;
  std::vector<std::uint64_t> ack_observations;
  std::size_t ack_count = 0;
  std::vector<std::shared_ptr<CallbackHandle::Control>> callbacks;
  bool shutdown_gate_registered = true;
  bool shutdown_gate_pending = false;
  bool shutdown_gate_permit = false;
  std::size_t active_callback_leases = 0;
  std::size_t active_writer_turns = 0;
  std::size_t max_writer_active = 0;
  std::thread::id measured_writer_id{};
  std::uint64_t next_ingress = config.initial_ingress_sequence;
  std::uint64_t next_journal = config.initial_journal_sequence;
  bool failed = false;
  Mode mode = Mode::kRunning;
  bool marker_processed = false;
  bool creation_generation_in_progress = false;
  bool shutdown_pending = false;
  std::uint64_t shutdown_sequence = 0;
  bool precommit_failure = false;
  std::size_t create_count = 0;
  std::optional<::sitometron::core::Uuid> last_created;
  bool accounting_failure = false;
  std::size_t postcommit_construction_count = 0;
  std::size_t postcommit_allocation_count = 0;
  bool postcommit_phase = false;
  std::size_t prepared_turn_count = 0;
  std::size_t destination_capacity_checks = 0;
  std::size_t destination_capacity = config.handoff_capacity;
  std::size_t normal_count = 0;
  std::size_t critical_count = 0;
  std::size_t completion_total = 0;
  std::size_t disposed = 0;
  std::size_t apply_total = 0;
  std::size_t writer_turns = 0;
  std::size_t effects = 0;
  std::size_t acks = 0;
  std::size_t responses = 0;
  bool pause_before_dequeue = false;
  bool admission_pause = false;
  bool admission_reached = false;
  bool admission_inserting = false;
  std::size_t admission_attempts = 0;
  std::size_t wait_until_attempts = 0;
  WriterPhase barrier = WriterPhase::kTurnFinished;
  bool barrier_armed = false;
  bool barrier_reached = false;
  std::uint64_t barrier_sequence = 0;

  Resident* Find(const ::sitometron::core::Uuid& id) {
    for (auto& resident : residents)
      if (resident.id == id && (resident.exists || resident.claims != 0)) return &resident;
    return nullptr;
  }
  const Resident* Find(const ::sitometron::core::Uuid& id) const {
    for (const auto& resident : residents)
      if (resident.id == id && (resident.exists || resident.claims != 0)) return &resident;
    return nullptr;
  }
  static GateKind TimerGate(TimeoutPhase phase) {
    return static_cast<GateKind>(static_cast<std::size_t>(GateKind::kPreparationTimer) +
                                 static_cast<std::size_t>(phase));
  }
  static GateKind GateFor(const Entry& entry) {
    if (entry.kind == Entry::Kind::kCommand) return GateKind::kTerminate;
    if (entry.kind != Entry::Kind::kCandidate) return GateKind::kCount;
    if (entry.candidate.event_type == "timeout_expired") {
      if (entry.candidate.payload_json.find("\"preparation\"") != std::string::npos)
        return GateKind::kPreparationTimer;
      if (entry.candidate.payload_json.find("\"execution\"") != std::string::npos)
        return GateKind::kExecutionTimer;
      if (entry.candidate.payload_json.find("\"cooperative_stop\"") != std::string::npos)
        return GateKind::kCooperativeStopTimer;
      return GateKind::kProcessExitTimer;
    }
    if (entry.candidate.event_type == "worker_completed" ||
        entry.candidate.event_type == "worker_failed")
      return GateKind::kWorker;
    if (entry.candidate.event_type == "process_exit_confirmed") return GateKind::kProcessExit;
    if (entry.candidate.event_type == "resources_released") return GateKind::kResourcesReleased;
    if (entry.candidate.event_type == "cleanup_status_recorded") return GateKind::kCleanup;
    return GateKind::kCount;
  }
  static bool IsIdentityGate(GateKind kind) noexcept {
    return kind == GateKind::kWorker ||
           (kind >= GateKind::kPreparationTimer && kind <= GateKind::kProcessExitTimer);
  }
  static std::size_t IdentitySlot(GateKind kind) noexcept {
    return kind == GateKind::kWorker ? 4U
                                     : static_cast<std::size_t>(kind) -
                                           static_cast<std::size_t>(GateKind::kPreparationTimer);
  }
  static bool CopyCanonicalUuid(std::string_view value, std::array<char, 36>& out) noexcept {
    if (value.size() != out.size()) return false;
    for (std::size_t i = 0; i != value.size(); ++i) {
      const bool hyphen = i == 8 || i == 13 || i == 18 || i == 23;
      const char c = value[i];
      const bool hex = (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F');
      if ((hyphen && c != '-') || (!hyphen && !hex)) return false;
      out[i] = c;
    }
    return true;
  }
  static bool IsUuidVersion(std::string_view value, char version) noexcept {
    std::array<char, 36> ignored{};
    if (!CopyCanonicalUuid(value, ignored) || value[14] != version ||
        (value[19] != '8' && value[19] != '9' && value[19] != 'a' && value[19] != 'b'))
      return false;
    for (std::size_t i = 0; i != value.size(); ++i)
      if (i != 8 && i != 13 && i != 18 && i != 23 && value[i] >= 'A' && value[i] <= 'F')
        return false;
    return true;
  }
  static std::size_t JsonKey(std::string_view json, std::string_view key) noexcept {
    for (std::size_t pos = json.find('"'); pos != std::string_view::npos;
         pos = json.find('"', pos + 1)) {
      if (pos + key.size() + 1 < json.size() && json.substr(pos + 1, key.size()) == key &&
          json[pos + key.size() + 1] == '"')
        return pos;
    }
    return std::string_view::npos;
  }
  static std::string_view JsonString(std::string_view json, std::string_view key) noexcept {
    const auto at = JsonKey(json, key);
    if (at == std::string_view::npos) return {};
    auto pos = json.find(':', at + key.size() + 2);
    if (pos == std::string_view::npos) return {};
    ++pos;
    while (pos < json.size() &&
           (json[pos] == ' ' || json[pos] == '\t' || json[pos] == '\n' || json[pos] == '\r'))
      ++pos;
    if (pos == json.size() || json[pos] != '"') return {};
    const auto begin = ++pos;
    while (pos < json.size()) {
      if (json[pos] == '"' && (pos == begin || json[pos - 1] != '\\'))
        return json.substr(begin, pos - begin);
      ++pos;
    }
    return {};
  }
  static bool JsonUint(std::string_view json, std::string_view key, std::uint64_t& out) noexcept {
    const auto at = JsonKey(json, key);
    if (at == std::string_view::npos) return false;
    auto pos = json.find(':', at + key.size() + 2);
    if (pos == std::string_view::npos) return false;
    ++pos;
    while (pos < json.size() &&
           (json[pos] == ' ' || json[pos] == '\t' || json[pos] == '\n' || json[pos] == '\r'))
      ++pos;
    if (pos == json.size() || json[pos] < '0' || json[pos] > '9') return false;
    std::uint64_t value = 0;
    for (; pos < json.size() && json[pos] >= '0' && json[pos] <= '9'; ++pos) {
      const auto digit = static_cast<std::uint64_t>(json[pos] - '0');
      if (value > (std::numeric_limits<std::uint64_t>::max() - digit) / 10) return false;
      value = value * 10 + digit;
    }
    out = value;
    return true;
  }
  static bool PrepareGateIdentity(GateKind kind, const RawCandidateEvent& event,
                                  GateIdentity& result) noexcept {
    result = {};
    if (!IsUuidVersion(event.job_id.value, '7') ||
        !CopyCanonicalUuid(event.job_id.value, result.job_id))
      return false;
    if (kind == GateKind::kWorker) {
      // Decode with the same JSON object/value rules as ParseWorkerEventPayload.
      // The parser callback makes duplicate keys explicitly ambiguous for
      // coalescing, while nlohmann's normal last-value semantics supplies the
      // effective value used by the reducer for non-duplicate payloads.
      std::size_t worker_keys = 0;
      std::size_t sequence_keys = 0;
      try {
        const auto payload = nlohmann::json::parse(
            event.payload_json,
            [&worker_keys, &sequence_keys](int, nlohmann::json::parse_event_t type,
                                           nlohmann::json& value) {
              if (type == nlohmann::json::parse_event_t::key && value.is_string()) {
                const auto& key = value.get_ref<const std::string&>();
                if (key == "worker_id") ++worker_keys;
                if (key == "event_sequence") ++sequence_keys;
              }
              return true;
            });
        if (!payload.is_object() || payload.size() != 2 || worker_keys != 1 || sequence_keys != 1 ||
            !payload.contains("worker_id") || !payload.contains("event_sequence"))
          return false;
        const auto& worker = payload.at("worker_id");
        const auto& sequence = payload.at("event_sequence");
        if (!worker.is_string() || !sequence.is_number_unsigned()) return false;
        const auto worker_value = worker.get<std::string>();
        if (!IsUuidVersion(worker_value, '4') || !CopyCanonicalUuid(worker_value, result.worker_id))
          return false;
        result.event_sequence = sequence.get<std::uint64_t>();
        if (result.event_sequence == 0) return false;
      } catch (...) {
        return false;
      }
      result.event_kind = event.event_type == "worker_completed" ? 1 : 2;
    } else if (kind >= GateKind::kPreparationTimer && kind <= GateKind::kProcessExitTimer) {
      const auto phase = JsonString(event.payload_json, "phase");
      constexpr std::string_view phases[] = {"preparation", "execution", "cooperative_stop",
                                             "process_exit_confirmation"};
      std::size_t index = 0;
      for (; index != std::size(phases) && phase != phases[index]; ++index) {
      }
      if (index == std::size(phases) ||
          !JsonUint(event.payload_json, "timer_generation", result.timer_generation))
        return false;
      result.timer_phase = static_cast<std::uint8_t>(index);
    } else {
      return false;
    }
    result.valid = true;
    return true;
  }
  static bool SameIdentity(const GateIdentity& lhs, const GateIdentity& rhs) noexcept {
    return lhs.valid && rhs.valid && lhs.job_id == rhs.job_id && lhs.worker_id == rhs.worker_id &&
           lhs.event_kind == rhs.event_kind && lhs.timer_phase == rhs.timer_phase &&
           lhs.event_sequence == rhs.event_sequence && lhs.timer_generation == rhs.timer_generation;
  }
  std::size_t IndexOf(const Resident* resident) const {
    return static_cast<std::size_t>(resident - residents.data());
  }
  void RegisterGateLocked(Resident& resident, GateKind kind) {
    if (kind == GateKind::kCount) return;
    auto& gate = resident.gates[static_cast<std::size_t>(kind)];
    if (gate.registered && gate.permit) return;
    // The producer permit is acquired before this source is exposed by any
    // post-commit handoff/arm operation. R is therefore a real bound, not a
    // test seam that can be acquired after exposure.
    if (critical_count >= config.critical_reserve()) {
      FailLocked();
      return;
    }
    gate.registered = true;
    gate.permit = true;
    ++critical_count;
  }
  bool RetainGate(const ::sitometron::core::Uuid& id, GateKind kind) {
    std::lock_guard lock(mutex);
    auto* resident = Find(id);
    if (resident == nullptr) return false;
    auto& gate = resident->gates[static_cast<std::size_t>(kind)];
    if (mode != Mode::kRunning && mode != Mode::kQuiescing) return false;
    if (!gate.registered || !gate.permit || gate.retained) return false;
    // A retained test lease only holds a permit acquired at registration; it
    // can never manufacture a source permit after an exposure.
    gate.retained = true;
    gate.callback_lease = true;
    return true;
  }
  std::size_t PlanDerivedSources(const ApplyResult& applied, EventType event_type,
                                 std::array<GateKind, 9>& plan) const noexcept {
    std::size_t count = 0;
    auto add = [&](GateKind kind) {
      if (count < plan.size()) plan[count++] = kind;
    };
    add(GateKind::kTerminate);
    if (event_type == EventType::kResourcesCommitted) add(GateKind::kResourcesReleased);
    if (event_type == EventType::kWorkerLaunchIntent) {
      add(GateKind::kWorker);
      add(GateKind::kProcessExit);
    }
    if (event_type == EventType::kTerminalOutcomeCommitted) add(GateKind::kCleanup);
    for (const auto& effect : applied.effects) {
      switch (effect.id) {
        case EffectId::kArmPreparationTimeout:
          add(GateKind::kPreparationTimer);
          break;
        case EffectId::kArmExecutionTimeout:
          add(GateKind::kExecutionTimer);
          break;
        case EffectId::kArmCooperativeStopTimeout:
          add(GateKind::kCooperativeStopTimer);
          break;
        case EffectId::kArmProcessExitConfirmationTimeoutIfNeeded:
          add(GateKind::kProcessExitTimer);
          break;
        default:
          break;
      }
    }
    return count;
  }
  bool CanActivateSourcesLocked(const Resident& resident, const std::array<GateKind, 9>& plan,
                                std::size_t count) const noexcept {
    std::size_t additional = 0;
    for (std::size_t i = 0; i != count; ++i) {
      const auto& gate = resident.gates[static_cast<std::size_t>(plan[i])];
      if (!gate.permit) ++additional;
    }
    return critical_count <= config.critical_reserve() &&
           additional <= config.critical_reserve() - critical_count;
  }
  void ActivateSourcesLocked(Resident& resident, const std::array<GateKind, 9>& plan,
                             std::size_t count) noexcept {
    for (std::size_t i = 0; i != count; ++i) {
      auto& gate = resident.gates[static_cast<std::size_t>(plan[i])];
      if (gate.permit) continue;
      gate.registered = true;
      gate.permit = true;
      ++critical_count;
    }
  }
  void ReleaseGateLocked(Entry& entry, bool successful) {
    if (!entry.critical) {
      if (normal_count != 0) --normal_count;
      return;
    }
    if (entry.kind == Entry::Kind::kShutdown) {
      // The global shutdown permit remains held through quiescing and
      // draining. It is retired only by the atomic sealed transition.
      return;
    }
    if (entry.resident_index == kNoResident || entry.resident_index >= residents.size()) {
      FailLocked();
      return;
    }
    auto& gate = residents[entry.resident_index].gates[static_cast<std::size_t>(entry.gate)];
    gate.pending = false;
    if (entry.gate == GateKind::kWorker && successful) {
      gate.retained = true;  // Exact ACK or confirmed process exit retires this identity.
      return;
    }
    if (entry.gate >= GateKind::kPreparationTimer && entry.gate <= GateKind::kProcessExitTimer) {
      // A delivered timer registration retires with its matching writer turn;
      // a later arm creates a fresh generation/registration.
      gate.registered = false;
    }
    if (entry.gate == GateKind::kProcessExit && successful) {
      auto& worker =
          residents[entry.resident_index].gates[static_cast<std::size_t>(GateKind::kWorker)];
      worker.pending = false;
      worker.retained = false;
      worker.callback_lease = false;
      residents[entry.resident_index].identity_slots[IdentitySlot(GateKind::kWorker)] = {};
      worker.ingress_sequence = 0;
      if (worker.permit) {
        worker.permit = false;
        if (critical_count != 0) --critical_count;
      }
    }
    if (!gate.retained && IsIdentityGate(entry.gate))
      residents[entry.resident_index].identity_slots[IdentitySlot(entry.gate)] = {};
    if (!gate.retained && gate.permit) {
      gate.permit = false;
      gate.ingress_sequence = 0;
      if (critical_count != 0) --critical_count;
    }
  }
  void RetireShutdownGateLocked() {
    shutdown_gate_pending = false;
    shutdown_gate_registered = false;
    if (shutdown_gate_permit) {
      shutdown_gate_permit = false;
      if (critical_count != 0) --critical_count;
    }
  }
  void CancelGatesLocked() {
    for (auto& resident : residents) {
      for (auto& gate : resident.gates) {
        gate.pending = false;
        gate.retained = false;
        gate.callback_lease = false;
        gate.identity_slot = kNoIdentitySlot;
        gate.ingress_sequence = 0;
        if (gate.permit) {
          gate.permit = false;
          if (critical_count != 0) --critical_count;
        }
      }
      for (auto& identity : resident.identity_slots) identity = {};
    }
  }
  void RetireGateLocked(Resident& resident, GateKind kind) {
    auto& gate = resident.gates[static_cast<std::size_t>(kind)];
    gate.pending = false;
    // A test-owned lease deliberately keeps an already-acquired producer
    // permit through later phases; disarm only retires an unleased source.
    if (gate.retained) return;
    gate.callback_lease = false;
    if (kind >= GateKind::kPreparationTimer && kind <= GateKind::kProcessExitTimer)
      gate.registered = false;
    if (IsIdentityGate(kind)) resident.identity_slots[IdentitySlot(kind)] = {};
    gate.identity_slot = kNoIdentitySlot;
    gate.ingress_sequence = 0;
    if (gate.permit) {
      gate.permit = false;
      if (critical_count != 0) --critical_count;
    }
  }
  void ReleaseCreationClaimLocked(Entry& entry) {
    if (entry.kind == Entry::Kind::kCandidate && entry.candidate.event_type == "job_created" &&
        entry.resident_index != kNoResident && entry.resident_index < residents.size()) {
      auto& resident = residents[entry.resident_index];
      if (resident.claims != 0) --resident.claims;
      if (!resident.exists && resident.claims == 0) {
        for (auto& gate : resident.gates) {
          if (gate.permit && critical_count != 0) --critical_count;
        }
        // Preserve startup-reserved bank storage across provisional-claim rollback.
        auto prepared_effects = std::move(resident.prepared_effects);
        auto prepared_trace = std::move(resident.prepared_trace);
        resident = Resident{};
        resident.prepared_effects = std::move(prepared_effects);
        resident.prepared_trace = std::move(prepared_trace);
      }
    }
  }
  void Checkpoint(std::uint64_t sequence, WriterPhase phase) {
    std::unique_lock lock(mutex);
    if (!barrier_armed || barrier != phase) return;
    if (barrier_sequence != 0 && barrier_sequence != sequence) return;
    barrier_sequence = sequence;
    barrier_reached = true;
    cv.notify_all();
    cv.wait(lock, [this, sequence, phase] {
      return !barrier_armed || barrier != phase || barrier_sequence != sequence;
    });
  }
  Resident* Reserve(const ::sitometron::core::Uuid& id) {
    if (auto* known = Find(id)) {
      ++known->claims;
      return known;
    }
    for (auto& resident : residents) {
      if (!resident.exists && resident.claims == 0) {
        resident.id = id;
        resident.claims = 1;
        resident.banks[0] =
            ApplyResult{::sitometron::core::InitialSnapshot(id, id), {}, std::nullopt};
        resident.banks[1] = resident.banks[0];
        RegisterGateLocked(resident, GateKind::kTerminate);
        return &resident;
      }
    }
    return nullptr;
  }
  void CompleteLocked(const Entry& entry, Completion completion) {
    // Never search for spare storage on completion: the entry's exact slot and
    // generation were reserved atomically with FIFO insertion.
    if (entry.completion_index >= completions.size()) {
      FailLocked();
      return;
    }
    auto& slot = completions[entry.completion_index];
    if (!slot.reserved || slot.generation != entry.completion_generation ||
        slot.sequence != entry.sequence) {
      FailLocked();
      return;
    }
    if (slot.completed) return;
    slot.completed = true;
    slot.value = std::move(completion);
    ++completion_total;
    if (outstanding != 0) --outstanding;
    if (in_flight && in_flight_sequence == entry.sequence) in_flight = false;
  }
  void Complete(const Entry& entry, Completion completion) {
    std::lock_guard lock(mutex);
    CompleteLocked(entry, std::move(completion));
  }
  void PublishPrepared(Resident& resident, int bank, std::size_t& cursor) noexcept {
    auto& prepared = resident.prepared_trace[static_cast<std::size_t>(bank)];
    if (cursor >= prepared.size()) {
      std::lock_guard lock(mutex);
      FailLocked();
      return;
    }
    std::lock_guard lock(mutex);
    if (trace_count == trace.size()) {
      FailLocked();
      return;
    }
    trace[trace_count] = std::move(prepared[cursor++]);
    trace[trace_count].ordinal = static_cast<std::uint64_t>(trace_count + 1);
    trace[trace_count].ingress_mutex_held = false;
    trace[trace_count].writer_context = measured_writer_id == std::this_thread::get_id();
    ++trace_count;
  }
  void PrepareTrace(Resident& resident, int bank, std::uint64_t journal_sequence,
                    const LogicalJobEvent& event) {
    if (postcommit_phase) ++postcommit_construction_count;
    auto& prepared = resident.prepared_trace[static_cast<std::size_t>(bank)];
    if (prepared.capacity() < config.trace_capacity) ++postcommit_allocation_count;
    prepared.clear();
    auto add = [&](TraceKind kind, EffectId effect = EffectId::kInvalid, std::string action = {}) {
      prepared.push_back(
          {0, kind, journal_sequence, event.event_type, effect, std::move(action), false, true});
    };
    add(TraceKind::kJournalAttempt);
    add(TraceKind::kJournalCommitted);
    add(TraceKind::kSnapshotActivated);
    auto add_source = [&](std::string action) {
      add(TraceKind::kSourceRegistered, EffectId::kInvalid, std::move(action));
    };
    if (event.event_type == EventType::kResourcesCommitted) {
      add_source("source:timer:preparation");
      add_source("source:resources_released");
    } else if (event.event_type == EventType::kWorkerLaunchIntent) {
      add_source("source:worker");
      add_source("source:process_exit");
    } else if (event.event_type == EventType::kTerminalOutcomeCommitted) {
      add_source("source:timer:process_exit_confirmation");
      add_source("source:cleanup");
    } else if (event.event_type == EventType::kCancelAccepted) {
      add_source("source:timer:cooperative_stop");
    } else if (event.event_type == EventType::kTerminateAccepted) {
      add_source("source:timer:process_exit_confirmation");
    }
    for (const auto& operation : resident.prepared_effects[static_cast<std::size_t>(bank)]) {
      add(TraceKind::kEffect, operation.id);
      add(operation.mapped_kind, operation.id, operation.action);
    }
    add(TraceKind::kResponseReleased);
  }
  static_assert(noexcept(std::declval<TraceRecord&>() = std::declval<TraceRecord&&>()));
  void FailLocked() { failed = true; }
  void Fail() {
    std::lock_guard lock(mutex);
    FailLocked();
  }
  IngressResult ResultLocked(IngressCode code, std::uint64_t sequence = 0) const {
    return {code, sequence, completion_total};
  }
  IngressResult Result(IngressCode code, std::uint64_t sequence = 0) const {
    std::lock_guard lock(mutex);
    return ResultLocked(code, sequence);
  }
  static bool IsCriticalCandidate(const RawCandidateEvent& event) {
    return event.event_type == "timeout_expired" || event.event_type == "worker_completed" ||
           event.event_type == "worker_failed" || event.event_type == "process_exit_confirmed" ||
           event.event_type == "resources_released" ||
           event.event_type == "cleanup_status_recorded";
  }
  IngressResult Candidate(RawCandidateEvent event,
                          std::optional<GeneratedIdentityBundle> identities = std::nullopt,
                          std::unique_lock<std::mutex>* held_lock = nullptr) {
    Entry entry;
    entry.kind = Entry::Kind::kCandidate;
    entry.candidate = std::move(event);
    entry.creation_identities = std::move(identities);
    return Enqueue(std::move(entry), false, false, held_lock);
  }
  IngressResult Enqueue(Entry entry, bool /*ignored_normal*/, bool /*ignored_reserve_job*/ = false,
                        std::unique_lock<std::mutex>* held_lock = nullptr) {
    std::unique_lock<std::mutex> local_lock(mutex, std::defer_lock);
    const bool critical_candidate =
        entry.kind == Entry::Kind::kCandidate && IsCriticalCandidate(entry.candidate);
    const auto identity_gate = critical_candidate ? GateFor(entry) : GateKind::kCount;
    GateIdentity prepared_identity{};
    const bool identity_required = IsIdentityGate(identity_gate);
    const bool identity_ready =
        !identity_required ||
        PrepareGateIdentity(identity_gate, entry.candidate, prepared_identity);
    // Build the typed retry identity before taking ingress ownership. Invalid identity fields do
    // not reject the reducer payload; they simply disable exact coalescing for that relationship.
    entry.identity_ready = identity_required && identity_ready;
    if (entry.identity_ready) entry.identity = std::move(prepared_identity);
    if (held_lock == nullptr) local_lock.lock();
    auto& lock = held_lock == nullptr ? local_lock : *held_lock;
    struct AdmissionInsertionWindow {
      Impl& impl;
      bool owns_window = false;
      ~AdmissionInsertionWindow() {
        if (owns_window) {
          impl.admission_inserting = false;
          impl.cv.notify_all();
        }
      }
    } admission_window{*this};
    ++admission_attempts;
    cv.notify_all();
    if (admission_pause && critical_candidate && !admission_reached) {
      admission_reached = true;
      admission_inserting = true;
      admission_window.owns_window = true;
      cv.notify_all();
      cv.wait(lock, [&] { return !admission_pause; });
    } else if (admission_inserting && critical_candidate) {
      cv.notify_all();
      cv.wait(lock, [&] { return !admission_inserting; });
    }
    (void)identity_ready;
    if (failed) return ResultLocked(IngressCode::kServiceFailed);
    if (mode == Mode::kSealed || mode == Mode::kStopped)
      return ResultLocked(IngressCode::kAdmissionClosed);

    const auto job_id =
        entry.kind == Entry::Kind::kCommand ? entry.command.job_id : entry.candidate.job_id;
    const bool known = Find(job_id) != nullptr;
    const bool normal = entry.kind == Entry::Kind::kCandidate
                            ? !IsCriticalCandidate(entry.candidate)
                        : entry.kind == Entry::Kind::kCommand
                            ? !(entry.command.command_type == CommandType::kTerminate && known)
                            : false;
    const bool reserve_job =
        entry.kind == Entry::Kind::kCandidate && entry.candidate.event_type == "job_created";
    entry.critical = !normal;
    if (destination_capacity == 0) {
      ++destination_capacity_checks;
      FailLocked();
      return ResultLocked(IngressCode::kServiceFailed);
    }
    if ((mode == Mode::kQuiescing || mode == Mode::kDraining) && normal)
      return ResultLocked(IngressCode::kAdmissionClosed);
    if (next_ingress == 0) {
      FailLocked();
      return ResultLocked(IngressCode::kServiceFailed);
    }
    // Resident capacity has deterministic precedence over normal FIFO capacity.
    if (reserve_job && !known && residents.size() == ResidentCount())
      return ResultLocked(IngressCode::kResidentLimit);
    if (normal && normal_count >= config.normal_capacity)
      return ResultLocked(IngressCode::kNormalFull);
    if (fifo_count == fifo.size() || ingress_count == ingress.size() ||
        completion_used == completions.size()) {
      FailLocked();
      return ResultLocked(IngressCode::kServiceFailed);
    }

    // Resolve an occupied critical delivery identity before allocating the
    // ingress sequence. Coalesced and rejected deliveries never burn one.
    Resident* resident = nullptr;
    Gate* gate = nullptr;
    if (!normal && entry.kind != Entry::Kind::kShutdown) {
      resident = Find(job_id);
      if (resident == nullptr) {
        FailLocked();
        return ResultLocked(IngressCode::kServiceFailed);
      }
      entry.gate = GateFor(entry);
      if (entry.gate == GateKind::kCount) {
        FailLocked();
        return ResultLocked(IngressCode::kServiceFailed);
      }
      gate = &resident->gates[static_cast<std::size_t>(entry.gate)];
      if (!gate->registered) {
        FailLocked();
        return ResultLocked(IngressCode::kServiceFailed);
      }
      if (!gate->permit) {
        if (critical_count >= config.critical_reserve()) {
          FailLocked();
          return ResultLocked(IngressCode::kServiceFailed);
        }
        gate->permit = true;
        ++critical_count;
      }
      if (gate->pending ||
          (entry.gate == GateKind::kWorker && gate->retained && gate->ingress_sequence != 0)) {
        const bool timer_gate =
            entry.gate >= GateKind::kPreparationTimer && entry.gate <= GateKind::kProcessExitTimer;
        const bool exact_retry =
            (entry.gate == GateKind::kWorker || timer_gate) &&
            SameIdentity(resident->identity_slots[IdentitySlot(entry.gate)], entry.identity);
        return exact_retry ? ResultLocked(IngressCode::kCoalescedPending, gate->ingress_sequence)
                           : ResultLocked(IngressCode::kAlreadyPending);
      }
      entry.resident_index = IndexOf(resident);
    }
    if (entry.kind == Entry::Kind::kShutdown) {
      if (mode != Mode::kQuiescing || !shutdown_gate_registered || shutdown_gate_pending)
        return ResultLocked(IngressCode::kAdmissionClosed);
      if (critical_count >= config.critical_reserve()) {
        FailLocked();
        return ResultLocked(IngressCode::kServiceFailed);
      }
    }
    if (reserve_job) {
      resident = Find(entry.candidate.job_id);
      if (resident == nullptr) {
        resident = Reserve(entry.candidate.job_id);
        if (resident == nullptr) return ResultLocked(IngressCode::kResidentLimit);
      } else {
        ++resident->claims;
      }
      entry.resident_index = IndexOf(resident);
      if (entry.creation_identities &&
          entry.creation_identities->job_session == entry.candidate.job_id) {
        resident->generated_identities = std::move(entry.creation_identities);
      }
    }
    const auto sequence = next_ingress;
    next_ingress = next_ingress == std::numeric_limits<std::uint64_t>::max() ? 0 : next_ingress + 1;
    entry.sequence = sequence;
    // Reserve an exact result destination before publishing any gate/FIFO
    // ownership. Coalesced and non-admitted paths above never reach here.
    for (std::size_t index = 0; index != completions.size(); ++index) {
      auto& slot = completions[index];
      if (!slot.reserved) {
        slot.reserved = true;
        slot.completed = false;
        slot.sequence = sequence;
        ++slot.generation;
        entry.completion_index = index;
        entry.completion_generation = slot.generation;
        ++completion_used;
        break;
      }
    }
    if (entry.completion_index == kNoResident) {
      FailLocked();
      return ResultLocked(IngressCode::kServiceFailed);
    }
    if (normal) {
      ++normal_count;
    } else if (entry.kind == Entry::Kind::kShutdown) {
      shutdown_gate_pending = true;
      shutdown_gate_permit = true;
      ++critical_count;
    } else {
      gate->pending = true;
      gate->ingress_sequence = sequence;
      if (entry.identity_ready) {
        gate->identity_slot = static_cast<std::uint8_t>(IdentitySlot(entry.gate));
        resident->identity_slots[IdentitySlot(entry.gate)] = std::move(entry.identity);
      }
    }
    const auto completion_before = completion_total + outstanding;
    fifo[fifo_tail] = std::move(entry);
    fifo_tail = (fifo_tail + 1) % fifo.size();
    ++fifo_count;
    ++outstanding;
    ingress[ingress_count++] = sequence;
    return {IngressCode::kAdmitted, sequence, completion_before};
  }
  bool Dequeue(Entry& out) {
    std::unique_lock lock(mutex);
    if (fifo_count == 0) return false;
    const auto sequence = fifo[fifo_head].sequence;
    // This is the true pre-removal barrier. Once released, the entry is
    // atomically transferred to the writer's authorized in-flight state.
    if (barrier_armed && barrier == WriterPhase::kBeforeDequeue &&
        (barrier_sequence == 0 || barrier_sequence == sequence)) {
      barrier_sequence = sequence;
      barrier_reached = true;
      cv.notify_all();
      cv.wait(lock, [this, sequence] {
        return !barrier_armed || barrier != WriterPhase::kBeforeDequeue ||
               barrier_sequence != sequence;
      });
    }
    out = std::move(fifo[fifo_head]);
    fifo[fifo_head] = Entry{};
    fifo_head = (fifo_head + 1) % fifo.size();
    --fifo_count;
    // A failure latched while the entry was paused before removal still
    // revokes the not-yet-authorized submission. After this point the entry
    // is irrevocably the writer's turn.
    out.authorized = !failed;
    in_flight = true;
    in_flight_sequence = out.sequence;
    return true;
  }
  void DisposeLocked(Entry& entry) {
    if (entry.kind != Entry::Kind::kShutdown) ++disposed;
    ReleaseCreationClaimLocked(entry);
    ReleaseGateLocked(entry, false);
    CompleteLocked(entry, {Completion::Code::kServiceFailed, std::nullopt});
    entry = Entry{};
  }
  void DisposeQueuedLocked() {
    while (fifo_count != 0) {
      Entry entry = std::move(fifo[fifo_head]);
      fifo[fifo_head] = Entry{};
      fifo_head = (fifo_head + 1) % fifo.size();
      --fifo_count;
      DisposeLocked(entry);
    }
    fifo_tail = fifo_head;
  }
  std::size_t ResidentCount() const {
    std::size_t count = 0;
    for (const auto& resident : residents)
      if (resident.exists || resident.claims != 0) ++count;
    return count;
  }
  std::size_t RequiredTraceSlots(EventType event_type, const ApplyResult& applied) const {
    std::size_t slots = 1;  // Journal attempt.
    slots += 2;             // Commit observation and snapshot activation.
    switch (event_type) {
      case EventType::kResourcesCommitted:
      case EventType::kWorkerLaunchIntent:
      case EventType::kTerminalOutcomeCommitted:
        slots += 2;
        break;
      case EventType::kCancelAccepted:
      case EventType::kTerminateAccepted:
        ++slots;
        break;
      default:
        break;
    }
    for (const auto& effect : applied.effects)
      if (effect.id != EffectId::kInvalid) slots += 2;
    return slots + 1;  // Response release.
  }
  bool HasTraceCapacity(std::size_t required) const {
    std::lock_guard lock(mutex);
    return required <= trace.size() - trace_count;
  }
  static TraceKind MappedKind(EffectId id) noexcept {
    switch (id) {
      case EffectId::kArmPreparationTimeout:
      case EffectId::kArmExecutionTimeout:
      case EffectId::kArmCooperativeStopTimeout:
      case EffectId::kArmProcessExitConfirmationTimeoutIfNeeded:
      case EffectId::kDisarmPreparationTimeout:
      case EffectId::kDisarmExecutionTimeout:
      case EffectId::kDisarmCooperativeStopTimeout:
      case EffectId::kDisarmProcessExitConfirmationTimeout:
        return TraceKind::kTimerAction;
      case EffectId::kLaunchWorkerOnce:
      case EffectId::kRetainSessionSameIdentity:
      case EffectId::kRequestCooperativeStop:
      case EffectId::kRequestForcedStop:
        return TraceKind::kCapabilityHandoff;
      case EffectId::kAckTerminalWorkerEventIfPending:
      case EffectId::kAckLateWorkerEvent:
        return TraceKind::kAckAuthorized;
      case EffectId::kPublishTerminalResult:
        return TraceKind::kTerminalPublished;
      case EffectId::kQuarantineResources:
      case EffectId::kSetReadinessFalse:
        return TraceKind::kSafetyAction;
      case EffectId::kInvalid:
        return TraceKind::kEffect;
    }
    return TraceKind::kEffect;
  }
  bool PrepareEffects(Resident& resident, int inactive, const LogicalJobEvent& event,
                      const ApplyResult& applied) {
    if (postcommit_phase) ++postcommit_construction_count;
    auto& prepared = resident.prepared_effects[static_cast<std::size_t>(inactive)];
    if (prepared.capacity() < kMaxPreparedEffects) {
      ++postcommit_allocation_count;
      return false;
    }
    const auto valid_effects = static_cast<std::size_t>(
        std::count_if(applied.effects.begin(), applied.effects.end(),
                      [](const Effect& effect) { return effect.id != EffectId::kInvalid; }));
    if (valid_effects > kMaxPreparedEffects || valid_effects > kMaxMappedDestinations ||
        valid_effects > config.handoff_capacity)
      return false;
    prepared.clear();
    for (const auto& effect : applied.effects) {
      if (effect.id == EffectId::kInvalid) continue;
      if (prepared.size() == prepared.capacity()) return false;
      PreparedEffect operation;
      operation.id = effect.id;
      operation.mapped_kind = MappedKind(effect.id);
      switch (effect.id) {
        case EffectId::kArmPreparationTimeout:
          if (resident.timer_generation[0] == std::numeric_limits<std::uint64_t>::max())
            throw std::bad_alloc();
          operation.timer_index = 0;
          operation.timer_update = true;
          operation.timer_generation_after = resident.timer_generation[0] + 1;
          operation.action =
              "timer:arm:preparation:" + std::to_string(operation.timer_generation_after);
          break;
        case EffectId::kArmExecutionTimeout:
          if (resident.timer_generation[1] == std::numeric_limits<std::uint64_t>::max())
            throw std::bad_alloc();
          operation.timer_index = 1;
          operation.timer_update = true;
          operation.timer_generation_after = resident.timer_generation[1] + 1;
          operation.action =
              "timer:arm:execution:" + std::to_string(operation.timer_generation_after);
          break;
        case EffectId::kArmCooperativeStopTimeout:
          if (resident.timer_generation[2] == std::numeric_limits<std::uint64_t>::max())
            throw std::bad_alloc();
          operation.timer_index = 2;
          operation.timer_update = true;
          operation.timer_generation_after = resident.timer_generation[2] + 1;
          operation.action =
              "timer:arm:cooperative_stop:" + std::to_string(operation.timer_generation_after);
          break;
        case EffectId::kArmProcessExitConfirmationTimeoutIfNeeded:
          if (resident.timer_generation[3] == std::numeric_limits<std::uint64_t>::max())
            throw std::bad_alloc();
          operation.timer_index = 3;
          operation.timer_update = true;
          operation.timer_generation_after = resident.timer_generation[3] + 1;
          operation.action = "timer:arm:process_exit_confirmation:" +
                             std::to_string(operation.timer_generation_after);
          break;
        case EffectId::kDisarmPreparationTimeout:
          operation.action =
              "timer:disarm:preparation:" + std::to_string(resident.timer_generation[0]);
          break;
        case EffectId::kDisarmExecutionTimeout:
          operation.action =
              "timer:disarm:execution:" + std::to_string(resident.timer_generation[1]);
          break;
        case EffectId::kDisarmCooperativeStopTimeout:
          operation.action =
              event.event_type == EventType::kProcessExitConfirmed
                  ? "timer:disarm:cooperative_stop"
                  : "timer:disarm:cooperative_stop:" + std::to_string(resident.timer_generation[2]);
          break;
        case EffectId::kDisarmProcessExitConfirmationTimeout:
          operation.action = "timer:disarm:process_exit_confirmation:" +
                             std::to_string(resident.timer_generation[3]);
          break;
        case EffectId::kLaunchWorkerOnce: {
          const auto& payload = std::get<WorkerLaunchIntentPayload>(event.payload);
          operation.has_launch = true;
          operation.launch = ApplicationLaunchRequest{event.job_id, payload};
          operation.action = "handoff:launch:" + payload.operation_id.value;
          break;
        }
        case EffectId::kRetainSessionSameIdentity:
          operation.has_session = true;
          operation.session = SessionRetainRequest{event.job_id, applied.snapshot.session_id};
          operation.action = "handoff:session-retain";
          break;
        case EffectId::kAckTerminalWorkerEventIfPending:
          operation.action = "ack:terminal:" + std::to_string(resident.worker_sequence);
          break;
        case EffectId::kAckLateWorkerEvent: {
          auto worker_sequence = resident.worker_sequence;
          if (const auto* worker_payload = std::get_if<WorkerEventPayload>(&event.payload))
            worker_sequence = worker_payload->event_sequence;
          else if (const auto* late_payload = std::get_if<LateWorkerEventPayload>(&event.payload))
            worker_sequence = late_payload->event_sequence;
          operation.action = "ack:late:" + std::to_string(worker_sequence);
          break;
        }
        case EffectId::kPublishTerminalResult: {
          std::string outcome = "invalid";
          if (const auto* payload = std::get_if<TerminalOutcomePayload>(&event.payload)) {
            switch (payload->outcome) {
              case TerminalOutcome::kSucceeded:
                outcome = "succeeded";
                break;
              case TerminalOutcome::kFailed:
                outcome = "failed";
                break;
              case TerminalOutcome::kCancelled:
                outcome = "cancelled";
                break;
              case TerminalOutcome::kTerminated:
                outcome = "terminated";
                break;
              case TerminalOutcome::kTimedOut:
                outcome = "timed_out";
                break;
              case TerminalOutcome::kInvalid:
                break;
            }
          }
          operation.action = "publish:" + outcome;
          break;
        }
        case EffectId::kQuarantineResources:
          operation.action = "safety:quarantine";
          break;
        case EffectId::kSetReadinessFalse:
          operation.action = "safety:set_readiness_false";
          break;
        case EffectId::kRequestCooperativeStop:
          if (!applied.snapshot.launch_operation_id || !applied.snapshot.worker_id)
            throw std::bad_alloc();
          operation.has_stop = true;
          operation.stop = ApplicationStopRequest{
              event.job_id, *applied.snapshot.launch_operation_id, *applied.snapshot.worker_id};
          operation.action = "handoff:cooperative-stop";
          break;
        case EffectId::kRequestForcedStop:
          if (!applied.snapshot.launch_operation_id || !applied.snapshot.worker_id)
            throw std::bad_alloc();
          operation.has_stop = true;
          operation.stop = ApplicationStopRequest{
              event.job_id, *applied.snapshot.launch_operation_id, *applied.snapshot.worker_id};
          operation.action = "handoff:forced-stop";
          break;
        case EffectId::kInvalid:
          break;
      }
      prepared.push_back(std::move(operation));
    }
    return true;
  }
  void DispatchPrepared(Resident& resident, int bank, std::size_t& trace_cursor) noexcept {
    auto& prepared = resident.prepared_effects[static_cast<std::size_t>(bank)];
    for (auto& operation : prepared) {
      PublishPrepared(resident, bank, trace_cursor);  // Effect declaration.
      {
        std::lock_guard lock(mutex);
        ++effects;
        if (operation.timer_update)
          resident.timer_generation[operation.timer_index] = operation.timer_generation_after;
      }
      switch (operation.id) {
        case EffectId::kDisarmPreparationTimeout: {
          std::lock_guard lock(mutex);
          RetireGateLocked(resident, GateKind::kPreparationTimer);
          break;
        }
        case EffectId::kDisarmExecutionTimeout: {
          std::lock_guard lock(mutex);
          RetireGateLocked(resident, GateKind::kExecutionTimer);
          break;
        }
        case EffectId::kDisarmCooperativeStopTimeout: {
          std::lock_guard lock(mutex);
          RetireGateLocked(resident, GateKind::kCooperativeStopTimer);
          break;
        }
        case EffectId::kDisarmProcessExitConfirmationTimeout: {
          std::lock_guard lock(mutex);
          RetireGateLocked(resident, GateKind::kProcessExitTimer);
          break;
        }
        case EffectId::kLaunchWorkerOnce:
          config.ports.runner->HandoffLaunch(std::move(operation.launch));
          break;
        case EffectId::kRetainSessionSameIdentity:
          config.ports.session->HandoffRetainSameIdentity(std::move(operation.session));
          break;
        case EffectId::kAckTerminalWorkerEventIfPending:
        case EffectId::kAckLateWorkerEvent: {
          std::lock_guard lock(mutex);
          ++acks;
          break;
        }
        case EffectId::kRequestCooperativeStop:
          config.ports.runner->HandoffCooperativeStop(std::move(operation.stop));
          break;
        case EffectId::kRequestForcedStop:
          config.ports.runner->HandoffForcedStop(std::move(operation.stop));
          break;
        default:
          break;
      }
      PublishPrepared(resident, bank, trace_cursor);  // Explicit mapped action.
    }
  }
  void FailTurn(Entry& entry) {
    {
      std::lock_guard lock(mutex);
      FailLocked();
      ++disposed;
      ReleaseCreationClaimLocked(entry);
      ReleaseGateLocked(entry, false);
      CompleteLocked(entry, {Completion::Code::kServiceFailed, std::nullopt});
      // Source registrations were only provisional before commit. Failure owns
      // their cancellation as well as the dequeued and queued input.
      CancelGatesLocked();
      DisposeQueuedLocked();
    }
    cv.notify_all();
    Checkpoint(entry.sequence, WriterPhase::kFailureDisposed);
  }
  void ProcessOne() noexcept {
    Entry entry;
    if (!Dequeue(entry)) return;
    try {
      ProcessEntry(entry);
    } catch (...) {
      // Keep the owning Entry live through foreign Clock/Apply/materialization
      // failures so the one failure-disposal path can release every resource.
      FailTurn(entry);
    }
  }
  void ProcessEntry(Entry& entry) {
    Checkpoint(entry.sequence, WriterPhase::kAfterDequeueAuthorized);
    postcommit_phase = false;
    {
      std::lock_guard lock(mutex);
      if (failed && !entry.authorized) {
        if (entry.kind == Entry::Kind::kShutdown) {
          ReleaseGateLocked(entry, true);
          CompleteLocked(entry, {Completion::Code::kSuccess, std::nullopt});
        } else {
          DisposeLocked(entry);
        }
        return;
      }
      ++writer_turns;
    }
    if (entry.kind == Entry::Kind::kShutdown) {
      {
        std::lock_guard lock(mutex);
        // Only the admitted global marker enters draining. It is never a
        // reducer/Journal input.
        mode = Mode::kDraining;
        marker_processed = true;
        CompleteLocked(entry, {Completion::Code::kSuccess, std::nullopt});
      }
      Checkpoint(entry.sequence, WriterPhase::kShutdownMarker);
      return;
    }
    Resident* resident = nullptr;
    const auto unknown_job =
        entry.kind == Entry::Kind::kCommand ? entry.command.job_id : entry.candidate.job_id;
    {
      std::lock_guard lock(mutex);
      resident = Find(unknown_job);
      if (!resident && entry.kind == Entry::Kind::kCandidate &&
          entry.candidate.event_type == "job_created")
        resident = Reserve(entry.candidate.job_id);
    }
    ::sitometron::core::Snapshot before =
        ::sitometron::core::InitialSnapshot(unknown_job, unknown_job);
    {
      std::lock_guard lock(mutex);
      if (resident != nullptr) before = resident->banks[resident->active].snapshot;
    }
    Decision decision{Rejection{1, RejectionReason::kInvalidEventPayload}};
    if (entry.kind == Entry::Kind::kCommand) {
      decision = ::sitometron::core::DecideCommand(before, entry.command);
    } else {
      const NormalizedCandidate normalized =
          ::sitometron::core::NormalizeCandidate(before, entry.candidate);
      if (std::holds_alternative<Rejection>(normalized.value)) {
        {
          std::lock_guard lock(mutex);
          ReleaseCreationClaimLocked(entry);
          // A rejected Worker input never gains an ACK obligation.
          ReleaseGateLocked(entry, false);
        }
        Complete(entry,
                 {Completion::Code::kReducerRejection, std::get<Rejection>(normalized.value)});
        return;
      }
      decision = ::sitometron::core::DecideEvent(before, std::get<InternalEvent>(normalized.value));
    }
    Checkpoint(entry.sequence, WriterPhase::kAfterDecision);
    if (std::holds_alternative<Rejection>(decision.value)) {
      auto rejection = std::get<Rejection>(decision.value);
      {
        std::lock_guard lock(mutex);
        ReleaseCreationClaimLocked(entry);
        ReleaseGateLocked(entry, false);
      }
      Complete(entry, {Completion::Code::kReducerRejection, rejection});
      return;
    }
    bool cannot_materialize = false;
    {
      std::lock_guard lock(mutex);
      cannot_materialize = precommit_failure || destination_capacity == 0 || next_journal == 0;
      ++destination_capacity_checks;
    }
    if (cannot_materialize) {
      FailTurn(entry);
      return;
    }
    const auto proposal = std::get<PreEnvelopeProposal>(decision.value);
    {
      std::lock_guard lock(mutex);
      ++destination_capacity_checks;
    }
    ApplyResult prospective = Apply(before, proposal);
    if (prospective.rejection) {
      ++destination_capacity_checks;
      FailTurn(entry);
      return;
    }
    const int inactive = 1 - resident->active;
    // The inactive bank is the prepared, unpublished destination.  Its snapshot,
    // effect list, requests, and action text are complete before the Journal call.
    resident->banks[inactive] = std::move(prospective);
    if (!HasTraceCapacity(RequiredTraceSlots(proposal.event_type, resident->banks[inactive]))) {
      FailTurn(entry);
      return;
    }
    // Every fallible input, port read, source plan, request/effect, and trace
    // is complete before consuming the durable sequence. Sequence stamping is
    // scalar-only and cannot make an accepted turn partially materialized.
    const auto reading = config.ports.clock->Read();
    LogicalJobEvent event{
        1, 0, proposal.event_type, reading.recorded_at, proposal.job_id, proposal.payload};
    std::array<GateKind, 9> source_plan{};
    const auto source_count =
        PlanDerivedSources(resident->banks[inactive], proposal.event_type, source_plan);
    if (!PrepareEffects(*resident, inactive, event, resident->banks[inactive])) {
      FailTurn(entry);
      return;
    }
    PrepareTrace(*resident, inactive, 0, event);
    bool source_capacity_ok = false;
    {
      std::lock_guard lock(mutex);
      source_capacity_ok = CanActivateSourcesLocked(*resident, source_plan, source_count);
    }
    if (!source_capacity_ok) {
      FailTurn(entry);
      return;
    }
    std::uint64_t journal_sequence = 0;
    {
      std::lock_guard lock(mutex);
      journal_sequence = next_journal;
      next_journal =
          next_journal == std::numeric_limits<std::uint64_t>::max() ? 0 : next_journal + 1;
    }
    event.sequence = journal_sequence;
    for (auto& record : resident->prepared_trace[static_cast<std::size_t>(inactive)])
      record.journal_sequence = journal_sequence;
    Checkpoint(entry.sequence, WriterPhase::kBeforeCommit);
    std::size_t trace_cursor = 0;
    PublishPrepared(*resident, inactive, trace_cursor);  // JournalAttempt, before Commit().
    const auto commit = config.ports.journal->Commit(event);
    if (commit != LogicalCommitResult::kCommitted) {
      FailTurn(entry);
      return;
    }
    postcommit_phase = true;
    PublishPrepared(*resident, inactive, trace_cursor);  // JournalCommitted.
    Checkpoint(entry.sequence, WriterPhase::kAfterCommit);
    {
      std::lock_guard lock(mutex);
      // Commit success only flips a scalar index; no prospective value is moved
      // or constructed after the logical boundary.
      resident->active = inactive;
      resident->exists = resident->banks[inactive].snapshot.entity_exists;
      if (event.event_type == EventType::kJobCreated && resident->exists && resident->claims != 0)
        --resident->claims;
      ++apply_total;
      ++prepared_turn_count;
      ActivateSourcesLocked(*resident, source_plan, source_count);
    }
    PublishPrepared(*resident, inactive, trace_cursor);  // SnapshotActivated.
    Checkpoint(entry.sequence, WriterPhase::kAfterApply);
    const std::size_t trace_source_count =
        event.event_type == EventType::kResourcesCommitted ||
                event.event_type == EventType::kWorkerLaunchIntent ||
                event.event_type == EventType::kTerminalOutcomeCommitted
            ? 2
        : event.event_type == EventType::kCancelAccepted ||
                event.event_type == EventType::kTerminateAccepted
            ? 1
            : 0;
    for (std::size_t source = 0; source != trace_source_count; ++source)
      PublishPrepared(*resident, inactive, trace_cursor);
    if (event.event_type == EventType::kWorkerCompleted ||
        event.event_type == EventType::kWorkerFailed ||
        event.event_type == EventType::kLateWorkerEvent) {
      if (const auto* payload =
              std::get_if<::sitometron::core::WorkerEventPayload>(&event.payload)) {
        {
          std::lock_guard lock(mutex);
          resident->worker_pending = true;
          resident->worker_sequence = payload->event_sequence;
        }
      } else if (const auto* late =
                     std::get_if<::sitometron::core::LateWorkerEventPayload>(&event.payload)) {
        {
          std::lock_guard lock(mutex);
          resident->worker_pending = true;
          resident->worker_sequence = late->event_sequence;
        }
      }
    }
    DispatchPrepared(*resident, inactive, trace_cursor);
    Checkpoint(entry.sequence, WriterPhase::kAfterEffects);
    {
      std::lock_guard lock(mutex);
      ++responses;
    }
    PublishPrepared(*resident, inactive, trace_cursor);  // ResponseReleased.
    Checkpoint(entry.sequence, WriterPhase::kResponseReleased);
    {
      std::lock_guard lock(mutex);
      ReleaseCreationClaimLocked(entry);
      ReleaseGateLocked(entry, true);
    }
    Complete(entry, {Completion::Code::kSuccess, std::nullopt});
    Checkpoint(entry.sequence, WriterPhase::kTurnFinished);
  }
};
JobOrchestrator::JobOrchestrator(Config config) : impl_(std::make_unique<Impl>(std::move(config))) {
  // Callback controls are fully allocated before the writer or any producer is exposed.
  for (auto& control : impl_->callbacks) {
    control = std::make_shared<CallbackHandle::Control>();
    control->target = this;
  }
  std::lock_guard lock(mutex_);
  started_ = true;
  writer_ = std::thread(&JobOrchestrator::Run, this);
}
JobOrchestrator::~JobOrchestrator() { Stop(); }

void JobOrchestrator::Run() noexcept {
  {
    std::lock_guard lock(mutex_);
    writer_id_ = std::this_thread::get_id();
  }
  {
    std::lock_guard lock(impl_->mutex);
    impl_->measured_writer_id = std::this_thread::get_id();
  }
  for (;;) {
    {
      std::unique_lock lock(mutex_);
      wake_.wait(lock, [this] { return stopping_ || pending_ != 0; });
      if (stopping_ && pending_ == 0) break;
      --pending_;
      ++active_;
    }
    {
      std::lock_guard lock(impl_->mutex);
      ++impl_->active_writer_turns;
      impl_->max_writer_active = std::max(impl_->max_writer_active, impl_->active_writer_turns);
    }
    impl_->ProcessOne();
    {
      std::lock_guard lock(impl_->mutex);
      --impl_->active_writer_turns;
    }
    TrySealFailure();
    {
      std::lock_guard lock(mutex_);
      --active_;
      if (pending_ == 0 && active_ == 0) idle_.notify_all();
    }
  }
  std::lock_guard lock(mutex_);
  writer_id_ = {};
  idle_.notify_all();
}
void JobOrchestrator::Stop() noexcept {
  {
    std::lock_guard lock(mutex_);
    if (!started_) return;
  }
  // Seal controls before changing scheduler state. An invocation which already
  // holds active is waited out, so any admitted entry has scheduled work.
  for (const auto& control : impl_->callbacks) {
    std::unique_lock control_lock(control->mutex);
    control->sealed = true;
    control->cv.wait(control_lock, [&] { return control->invocations == 0; });
    control->target = nullptr;
  }
  {
    std::lock_guard lock(mutex_);
    stopping_ = true;
  }
  {
    std::lock_guard lock(impl_->mutex);
    impl_->pause_before_dequeue = false;
    impl_->barrier_armed = false;
    impl_->admission_pause = false;
  }
  impl_->cv.notify_all();
  wake_.notify_all();
  if (writer_.joinable()) writer_.join();
  {
    std::lock_guard ingress_lock(impl_->mutex);
    if (impl_->mode == Impl::Mode::kSealed) impl_->mode = Impl::Mode::kStopped;
  }
  std::lock_guard lock(mutex_);
  started_ = false;
}

CallbackHandle JobOrchestrator::RetainedCallback() {
  std::lock_guard lock(impl_->mutex);
  if (impl_->mode != Impl::Mode::kRunning) return {};
  for (const auto& control : impl_->callbacks) {
    std::lock_guard control_lock(control->mutex);
    if (!control->allocated) {
      control->allocated = true;
      return CallbackHandle(control);
    }
  }
  return {};
}
IngressResult CallbackHandle::Invoke(const RawCandidateEvent& event) const {
  try {
    // Materialize before entering the admission protocol. A copied lvalue can
    // fail here but never while the ingress mutex owns a partially inserted
    // relationship.
    RawCandidateEvent owned = event;
    return Invoke(std::move(owned));
  } catch (...) {
    if (!control_) return {IngressCode::kServiceFailed, 0, 0};
    JobOrchestrator* target = nullptr;
    {
      std::lock_guard lock(control_->mutex);
      if (!control_->sealed && control_->target != nullptr) {
        target = control_->target;
        ++control_->invocations;
      }
    }
    if (target != nullptr) {
      target->LatchFailureFromCallback();
      // Keep the invocation reference while the final target dereference runs. Stop waits for
      // this count before severing target and destroying writer-owned state.
      target->TrySealFailure(true);
      {
        std::lock_guard lock(control_->mutex);
        --control_->invocations;
        control_->cv.notify_all();
      }
    }
    return {IngressCode::kServiceFailed, 0, 0};
  }
}
IngressResult CallbackHandle::Invoke(RawCandidateEvent&& event) const noexcept {
  if (!control_) return {IngressCode::kAdmissionClosed, 0, 0};
  JobOrchestrator* target = nullptr;
  bool concurrent = false;
  {
    std::unique_lock lock(control_->mutex);
    if (control_->sealed || control_->target == nullptr)
      return {control_->failed ? IngressCode::kServiceFailed : IngressCode::kAdmissionClosed, 0, 0};
    target = control_->target;
    ++control_->invocations;
    if (control_->active || control_->leased) {
      control_->failed = true;
      concurrent = true;
    } else {
      control_->active = true;
    }
  }
  struct InvocationGuard {
    CallbackHandle::Control& control;
    JobOrchestrator* target;
    bool active;
    ~InvocationGuard() {
      // Clear active first but retain the invocation reference across the
      // sealing attempt: Stop cannot sever target until this returns.
      {
        std::lock_guard lock(control.mutex);
        if (active) control.active = false;
      }
      // Keep invocations nonzero through the final target dereference; this mirrors the
      // ReleaseLease ordering and prevents Stop from nulling/destroying target prematurely.
      target->TrySealFailure(true);
      {
        std::lock_guard lock(control.mutex);
        --control.invocations;
        control.cv.notify_all();
      }
    }
  } guard{*control_, target, !concurrent};
  if (concurrent) {
    target->LatchFailureFromCallback();
    return {IngressCode::kServiceFailed, 0, 0};
  }
  try {
    return target->SubmitCandidate(std::move(event));
  } catch (...) {
    target->LatchFailureFromCallback();
    return {IngressCode::kServiceFailed, 0, 0};
  }
}
bool CallbackHandle::HoldLease() const {
  if (!control_) return false;
  std::lock_guard lock(control_->mutex);
  if (control_->sealed || control_->target == nullptr || control_->active || control_->leased)
    return false;
  control_->leased = true;
  control_->leases = 1;
  return true;
}
bool CallbackHandle::ReleaseLease() const {
  if (!control_) return false;
  JobOrchestrator* target = nullptr;
  {
    std::lock_guard lock(control_->mutex);
    if (!control_->leased) return false;
    control_->leased = false;
    control_->leases = 0;
    target = control_->target;
    if (target != nullptr) ++control_->invocations;
    control_->cv.notify_all();
  }
  if (target != nullptr) {
    // The release itself is the sole invocation reference, so sealing may
    // complete without dropping protection for this call.
    target->TrySealFailure(true);
    std::lock_guard lock(control_->mutex);
    --control_->invocations;
    control_->cv.notify_all();
  }
  return true;
}

bool JobOrchestrator::IsWriterThread() const noexcept {
  std::lock_guard lock(mutex_);
  return writer_id_ == std::this_thread::get_id();
}
void JobOrchestrator::ScheduleAcceptedEntry() noexcept {
  {
    std::lock_guard lock(mutex_);
    ++pending_;
  }
  wake_.notify_one();
}
void JobOrchestrator::Notify() noexcept { ScheduleAcceptedEntry(); }
static IngressResult KickIf(JobOrchestrator* orchestrator, IngressResult r) noexcept {
  if (r.code == IngressCode::kAdmitted) orchestrator->Notify();
  return r;
}
IngressResult JobOrchestrator::Create() {
  // Identity sources are allowed to throw. Reserve one finite creation
  // operation under ingress ownership, then call the port with no ingress lock.
  {
    std::lock_guard lock(impl_->mutex);
    if (impl_->failed) return impl_->ResultLocked(IngressCode::kServiceFailed);
    if (impl_->mode != Impl::Mode::kRunning)
      return impl_->ResultLocked(IngressCode::kAdmissionClosed);
    if (impl_->creation_generation_in_progress)
      return impl_->ResultLocked(IngressCode::kAlreadyPending);
    impl_->creation_generation_in_progress = true;
  }
  JobSessionIdentityResult job_result;
  WorkerIdentityResult worker_result;
  LaunchOperationIdentityResult launch_result;
  try {
    auto& source = *impl_->config.ports.identity;
    job_result = source.GenerateJobSessionIdentity();
    worker_result = source.GenerateWorkerIdentity();
    launch_result = source.GenerateLaunchOperationIdentity();
  } catch (...) {
    {
      std::lock_guard lock(impl_->mutex);
      impl_->creation_generation_in_progress = false;
      impl_->cv.notify_all();
    }
    LatchReadinessFailure();
    return impl_->Result(IngressCode::kServiceFailed);
  }
  if (!std::holds_alternative<GeneratedJobSessionIdentity>(job_result) ||
      !std::holds_alternative<GeneratedWorkerIdentity>(worker_result) ||
      !std::holds_alternative<GeneratedLaunchOperationIdentity>(launch_result)) {
    {
      std::lock_guard lock(impl_->mutex);
      impl_->creation_generation_in_progress = false;
      impl_->cv.notify_all();
    }
    LatchReadinessFailure();
    return impl_->Result(IngressCode::kServiceFailed);
  }
  const auto id = std::get<GeneratedJobSessionIdentity>(job_result).value;
  auto identities = Impl::GeneratedIdentityBundle{
      id, std::get<GeneratedWorkerIdentity>(worker_result).value,
      std::get<GeneratedLaunchOperationIdentity>(launch_result).value};
  IngressResult result;
  try {
    result = impl_->Candidate({1, id, "job_created", "{\"session_id\":\"" + id.value + "\"}"},
                              std::move(identities));
  } catch (...) {
    {
      std::lock_guard lock(impl_->mutex);
      impl_->creation_generation_in_progress = false;
      impl_->cv.notify_all();
    }
    LatchReadinessFailure();
    return impl_->Result(IngressCode::kServiceFailed);
  }
  {
    std::lock_guard lock(impl_->mutex);
    impl_->creation_generation_in_progress = false;
    if (result.code == IngressCode::kAdmitted) {
      ++impl_->create_count;
      impl_->last_created = id;
    }
    impl_->cv.notify_all();
  }
  return KickIf(this, result);
}
IngressResult JobOrchestrator::SubmitCandidate(const RawCandidateEvent& e) {
  if (e.event_type == "timeout_expired") return impl_->Result(IngressCode::kAdmissionClosed);
  try {
    RawCandidateEvent owned = e;
    return SubmitCandidate(std::move(owned));
  } catch (...) {
    LatchReadinessFailure();
    return impl_->Result(IngressCode::kServiceFailed);
  }
}
IngressResult JobOrchestrator::SubmitCandidate(RawCandidateEvent&& e) noexcept {
  if (e.event_type == "timeout_expired") return impl_->Result(IngressCode::kAdmissionClosed);
  try {
    return KickIf(this, impl_->Candidate(std::move(e)));
  } catch (...) {
    LatchReadinessFailure();
    return impl_->Result(IngressCode::kServiceFailed);
  }
}
IngressResult JobOrchestrator::SubmitCommand(const Command& c) {
  try {
    Command owned = c;
    return SubmitCommand(std::move(owned));
  } catch (...) {
    LatchReadinessFailure();
    return impl_->Result(IngressCode::kServiceFailed);
  }
}
IngressResult JobOrchestrator::SubmitCommand(Command&& c) noexcept {
  try {
    Impl::Entry entry;
    entry.kind = Impl::Entry::Kind::kCommand;
    entry.command = std::move(c);
    return KickIf(this, impl_->Enqueue(std::move(entry), false));
  } catch (...) {
    LatchReadinessFailure();
    return impl_->Result(IngressCode::kServiceFailed);
  }
}
IngressResult JobOrchestrator::SubmitWorker(const RawCandidateEvent& e) {
  return SubmitCandidate(e);
}
IngressResult JobOrchestrator::SubmitWorker(RawCandidateEvent&& e) noexcept {
  return SubmitCandidate(std::move(e));
}
JobOrchestrator::TimerSubmitResult JobOrchestrator::SubmitTimeout(
    const TimerNotification& notification) {
  try {
    auto snapshot_state = [&](const Impl::Resident& resident) {
      TimerState state;
      state.job_id = notification.job_id;
      state.preparation_armed =
          resident.gates[static_cast<std::size_t>(Impl::GateKind::kPreparationTimer)].registered;
      state.execution_armed =
          resident.gates[static_cast<std::size_t>(Impl::GateKind::kExecutionTimer)].registered;
      state.cooperative_stop_armed =
          resident.gates[static_cast<std::size_t>(Impl::GateKind::kCooperativeStopTimer)]
              .registered;
      state.process_exit_confirmation_armed =
          resident.gates[static_cast<std::size_t>(Impl::GateKind::kProcessExitTimer)].registered;
      state.preparation_generation = resident.timer_generation[0];
      state.execution_generation = resident.timer_generation[1];
      state.cooperative_stop_generation = resident.timer_generation[2];
      state.process_exit_confirmation_generation = resident.timer_generation[3];
      return state;
    };
    TimeoutCandidate emitted;
    {
      std::unique_lock lock(impl_->mutex);
      if (impl_->failed)
        return TimerSubmitResult{false, impl_->ResultLocked(IngressCode::kServiceFailed)};
      auto* resident = impl_->Find(notification.job_id);
      if (resident == nullptr) {
        TimerState unknown;
        unknown.job_id = notification.job_id;
        const auto ingress = IngestTimer(unknown, notification);
        if (ingress.kind == TimerIngressKind::kFailClosed) {
          lock.unlock();
          LatchReadinessFailure();
          return TimerSubmitResult{false, impl_->Result(IngressCode::kServiceFailed)};
        }
        return TimerSubmitResult{true, {}};
      }
      const auto ingress = IngestTimer(snapshot_state(*resident), notification);
      if (ingress.kind == TimerIngressKind::kDiscardWithoutCandidate)
        return TimerSubmitResult{true, {}};
      if (ingress.kind != TimerIngressKind::kEmitCandidateEvent || !ingress.candidate) {
        lock.unlock();
        LatchReadinessFailure();
        return TimerSubmitResult{false, impl_->Result(IngressCode::kServiceFailed)};
      }
      emitted = std::move(*ingress.candidate);
    }

    // Construct all dynamic raw-candidate text before the atomic validation/insertion section.
    std::string phase;
    switch (emitted.payload.phase) {
      case TimeoutPhase::kPreparation:
        phase = "preparation";
        break;
      case TimeoutPhase::kExecution:
        phase = "execution";
        break;
      case TimeoutPhase::kCooperativeStop:
        phase = "cooperative_stop";
        break;
      case TimeoutPhase::kProcessExitConfirmation:
        phase = "process_exit_confirmation";
        break;
      case TimeoutPhase::kInvalid:
        LatchReadinessFailure();
        return TimerSubmitResult{false, impl_->Result(IngressCode::kServiceFailed)};
    }
    RawCandidateEvent candidate{1, emitted.job_id, "timeout_expired",
                                "{\"phase\":\"" + phase + "\",\"timer_generation\":" +
                                    std::to_string(emitted.payload.timer_generation) + "}"};

    std::unique_lock lock(impl_->mutex);
    if (impl_->failed)
      return TimerSubmitResult{false, impl_->ResultLocked(IngressCode::kServiceFailed)};
    auto* resident = impl_->Find(notification.job_id);
    if (resident == nullptr) return TimerSubmitResult{true, {}};
    // Re-run the pure ingress decision under the insertion lock. A disarm or generation change
    // during text construction therefore discards rather than failing or entering the reducer.
    const auto ingress = IngestTimer(snapshot_state(*resident), notification);
    if (ingress.kind == TimerIngressKind::kDiscardWithoutCandidate)
      return TimerSubmitResult{true, {}};
    if (ingress.kind != TimerIngressKind::kEmitCandidateEvent || !ingress.candidate) {
      lock.unlock();
      LatchReadinessFailure();
      return TimerSubmitResult{false, impl_->Result(IngressCode::kServiceFailed)};
    }
    if (ingress.candidate->payload.phase != emitted.payload.phase ||
        ingress.candidate->payload.timer_generation != emitted.payload.timer_generation) {
      return TimerSubmitResult{true, {}};
    }
    const auto admitted = impl_->Candidate(std::move(candidate), std::nullopt, &lock);
    if (admitted.code == IngressCode::kAdmitted) Notify();
    return TimerSubmitResult{false, admitted};
  } catch (...) {
    LatchReadinessFailure();
    return TimerSubmitResult{false, impl_->Result(IngressCode::kServiceFailed)};
  }
}
IngressResult JobOrchestrator::SubmitShutdown() {
  // One ingress transaction owns closure, nonzero sequence allocation, marker
  // slot insertion, and the pending global gate publication.
  IngressResult result;
  {
    std::lock_guard lock(impl_->mutex);
    if (impl_->failed) return impl_->ResultLocked(IngressCode::kServiceFailed);
    if (impl_->mode == Impl::Mode::kQuiescing && impl_->shutdown_gate_pending)
      return impl_->ResultLocked(IngressCode::kCoalescedPending, impl_->shutdown_sequence);
    if (impl_->mode != Impl::Mode::kRunning)
      return impl_->ResultLocked(IngressCode::kAdmissionClosed);
    if (!impl_->shutdown_gate_registered || impl_->next_ingress == 0 ||
        impl_->fifo_count == impl_->fifo.size() || impl_->ingress_count == impl_->ingress.size() ||
        impl_->completion_used == impl_->completions.size() ||
        impl_->critical_count >= impl_->config.critical_reserve()) {
      impl_->FailLocked();
      return impl_->ResultLocked(IngressCode::kServiceFailed);
    }
    const auto sequence = impl_->next_ingress;
    impl_->next_ingress = sequence == std::numeric_limits<std::uint64_t>::max() ? 0 : sequence + 1;
    Impl::Entry marker;
    marker.kind = Impl::Entry::Kind::kShutdown;
    marker.sequence = sequence;
    for (std::size_t index = 0; index != impl_->completions.size(); ++index) {
      auto& slot = impl_->completions[index];
      if (!slot.reserved) {
        slot.reserved = true;
        slot.completed = false;
        slot.sequence = sequence;
        ++slot.generation;
        marker.completion_index = index;
        marker.completion_generation = slot.generation;
        ++impl_->completion_used;
        break;
      }
    }
    if (marker.completion_index == Impl::kNoResident) {
      impl_->FailLocked();
      return impl_->ResultLocked(IngressCode::kServiceFailed);
    }
    marker.critical = true;
    impl_->mode = Impl::Mode::kQuiescing;
    impl_->shutdown_gate_pending = true;
    impl_->shutdown_gate_permit = true;
    impl_->shutdown_sequence = sequence;
    ++impl_->critical_count;
    const auto before = impl_->completion_total + impl_->outstanding;
    impl_->fifo[impl_->fifo_tail] = std::move(marker);
    impl_->fifo_tail = (impl_->fifo_tail + 1) % impl_->fifo.size();
    ++impl_->fifo_count;
    ++impl_->outstanding;
    impl_->ingress[impl_->ingress_count++] = sequence;
    result = {IngressCode::kAdmitted, sequence, before};
  }
  return KickIf(this, result);
}
bool JobOrchestrator::RetainTimerLease(const Uuid& id, TimeoutPhase phase) {
  return impl_->RetainGate(id, Impl::TimerGate(phase));
}
bool JobOrchestrator::RetainWorkerLease(const Uuid& id) {
  return impl_->RetainGate(id, Impl::GateKind::kWorker);
}
bool JobOrchestrator::RetainProcessExitLease(const Uuid& id) {
  return impl_->RetainGate(id, Impl::GateKind::kProcessExit);
}
bool JobOrchestrator::RetainResourcesReleasedLease(const Uuid& id) {
  return impl_->RetainGate(id, Impl::GateKind::kResourcesReleased);
}
bool JobOrchestrator::RetainCleanupLease(const Uuid& id) {
  return impl_->RetainGate(id, Impl::GateKind::kCleanup);
}
bool JobOrchestrator::RetireWorkerAck(const RawCandidateEvent& acked) {
  Impl::GateIdentity ack_identity{};
  if (!Impl::PrepareGateIdentity(Impl::GateKind::kWorker, acked, ack_identity)) return false;
  std::lock_guard lock(impl_->mutex);
  auto* resident = impl_->Find(acked.job_id);
  if (resident == nullptr) return false;
  auto& gate = resident->gates[static_cast<std::size_t>(Impl::GateKind::kWorker)];
  // The authoritative retry identity includes Job, Worker payload/event
  // sequence, and event discriminator: no sequence-only ACK can retire it.
  if (!gate.retained || gate.ingress_sequence == 0 ||
      !Impl::SameIdentity(resident->identity_slots[Impl::IdentitySlot(Impl::GateKind::kWorker)],
                          ack_identity))
    return false;
  gate.pending = false;
  gate.retained = false;
  gate.callback_lease = false;
  gate.identity_slot = Impl::kNoIdentitySlot;
  resident->identity_slots[Impl::IdentitySlot(Impl::GateKind::kWorker)] = {};
  gate.ingress_sequence = 0;
  if (gate.permit) {
    gate.permit = false;
    if (impl_->critical_count != 0) --impl_->critical_count;
  }
  return true;
}
void JobOrchestrator::LatchFailureFromCallback() noexcept { LatchReadinessFailure(); }

void JobOrchestrator::TrySealFailure(bool allow_one_invocation) noexcept {
  bool can_seal = false;
  {
    std::lock_guard lock(impl_->mutex);
    can_seal = impl_->failed && impl_->fifo_count == 0 && !impl_->in_flight;
  }
  if (!can_seal) return;
  std::size_t invocations = 0;
  for (const auto& control : impl_->callbacks) {
    std::lock_guard lock(control->mutex);
    if (control->active || control->leased) return;
    invocations += control->invocations;
  }
  if (invocations != 0 && (!allow_one_invocation || invocations != 1)) return;
  std::lock_guard lock(impl_->mutex);
  if (!impl_->failed || impl_->fifo_count != 0 || impl_->in_flight) return;
  impl_->CancelGatesLocked();
  impl_->RetireShutdownGateLocked();
  impl_->mode = Impl::Mode::kSealed;
  for (const auto& control : impl_->callbacks) {
    std::lock_guard control_lock(control->mutex);
    // Failure sealing severs every callback path once all invocation refs have
    // quiesced; sealed handles retain no writer target.
    if (control->invocations != 0 && (!allow_one_invocation || control->invocations != 1)) return;
    control->sealed = true;
    control->failed = true;
    control->target = nullptr;
  }
}
bool JobOrchestrator::LatchReadinessFailure() {
  {
    std::lock_guard lock(impl_->mutex);
    impl_->FailLocked();
    impl_->mode = Impl::Mode::kQuiescing;
  }
  for (const auto& control : impl_->callbacks) {
    std::lock_guard lock(control->mutex);
    control->failed = true;
    control->sealed = true;
  }
  wake_.notify_one();
  TrySealFailure();
  return true;
}
bool JobOrchestrator::BeginShutdown() {
  {
    std::lock_guard lock(impl_->mutex);
    if (!impl_->failed && !impl_->marker_processed) return false;
  }
  std::unique_lock lock(mutex_);
  idle_.wait(lock, [this] { return pending_ == 0 && active_ == 0; });
  lock.unlock();
  return FinishShutdown();
}
bool JobOrchestrator::FinishShutdown() {
  bool failure = false;
  bool accounting_failure = false;
  // Close callback registration first. An invocation which won the control lock before this
  // transition is allowed to finish and may schedule a final FIFO turn; it is never raced by
  // the final ingress check below.
  for (const auto& control : impl_->callbacks) {
    std::unique_lock control_lock(control->mutex);
    control->sealed = true;
    if (control->active || control->invocations != 0) {
      control->cv.wait(control_lock, [&] { return control->invocations == 0; });
    }
    // Leases are an external completion obligation. Keep the target alive and let the owner
    // release the lease before retrying final shutdown; sealed controls still permit release.
    if (control->leases != 0) return false;
  }

  {
    std::lock_guard ingress_lock(impl_->mutex);
    failure = impl_->failed;
    const bool mode_ready =
        failure || (impl_->marker_processed && impl_->mode == Impl::Mode::kDraining);
    if (!mode_ready || impl_->fifo_count != 0 || impl_->in_flight || impl_->normal_count != 0) {
      // A callback that was active before closure may have admitted work. The writer remains
      // responsible for draining it, and a later FinishShutdown call retries the protocol.
      if (impl_->fifo_count != 0 || impl_->in_flight) wake_.notify_one();
      return false;
    }
    // Retire all source gates only after the stable empty check. The shutdown permit itself is
    // held until the same mutex-protected sealed transition.
    impl_->CancelGatesLocked();
    impl_->RetireShutdownGateLocked();
    if (impl_->critical_count != 0) {
      impl_->FailLocked();
      failure = true;
      accounting_failure = true;
    } else {
      impl_->mode = Impl::Mode::kSealed;
    }
  }
  if (accounting_failure) {
    TrySealFailure();
    return false;
  }

  // Targets are severed only after the sealed transition and after every invocation reference
  // was observed at zero above. ReleaseLease remains valid for a held lease, but leases are
  // explicitly rejected by the stable protocol before reaching this point.
  for (const auto& control : impl_->callbacks) {
    std::lock_guard control_lock(control->mutex);
    control->sealed = true;
    control->failed = control->failed || failure;
    if (control->invocations == 0) control->target = nullptr;
  }
  return true;
}
bool JobOrchestrator::ArmPause(WriterPhase p) { return ArmBarrier(p); }
bool JobOrchestrator::ArmBarrier(WriterPhase p) {
  std::lock_guard lock(impl_->mutex);
  impl_->barrier = p;
  impl_->barrier_armed = true;
  impl_->barrier_reached = false;
  impl_->barrier_sequence = 0;
  return true;
}
bool JobOrchestrator::ArmAdmissionPause() {
  std::lock_guard lock(impl_->mutex);
  impl_->admission_pause = true;
  return true;
}
bool JobOrchestrator::WaitForAdmissionPause() {
  std::unique_lock lock(impl_->mutex);
  impl_->cv.wait(lock, [this] { return impl_->admission_reached; });
  return true;
}
bool JobOrchestrator::WaitForAdmissionAttempts(std::size_t c) {
  std::unique_lock lock(impl_->mutex);
  impl_->cv.wait(lock, [this, c] { return impl_->admission_attempts >= c; });
  return true;
}
bool JobOrchestrator::WaitForWaitUntilAttempts(std::size_t c) {
  std::unique_lock lock(impl_->mutex);
  impl_->cv.wait(lock, [this, c] { return impl_->wait_until_attempts >= c; });
  return true;
}
bool JobOrchestrator::ReleaseAdmissionPause() {
  {
    std::lock_guard lock(impl_->mutex);
    impl_->admission_pause = false;
  }
  impl_->cv.notify_all();
  return true;
}
bool JobOrchestrator::WaitUntil(std::uint64_t seq, WriterPhase phase) {
  {
    std::unique_lock lock(impl_->mutex);
    if (impl_->barrier_armed && impl_->barrier == phase) {
      ++impl_->wait_until_attempts;
      impl_->cv.notify_all();
      impl_->cv.wait(lock, [this, seq, phase] {
        return (impl_->barrier_reached && impl_->barrier == phase &&
                impl_->barrier_sequence == seq) ||
               impl_->failed || impl_->mode == Impl::Mode::kStopped;
      });
      return impl_->barrier_reached && impl_->barrier == phase && impl_->barrier_sequence == seq;
    }
    for (const auto& completion : impl_->completions)
      if (completion.reserved && completion.completed && completion.sequence == seq) return true;
  }
  std::unique_lock lock(mutex_);
  idle_.wait(lock, [this] { return pending_ == 0 && active_ == 0; });
  std::lock_guard ingress_lock(impl_->mutex);
  for (const auto& completion : impl_->completions)
    if (completion.reserved && completion.completed && completion.sequence == seq) return true;
  return false;
}
bool JobOrchestrator::Release(std::uint64_t seq, WriterPhase phase) {
  {
    std::lock_guard lock(impl_->mutex);
    if (!impl_->barrier_armed || impl_->barrier != phase ||
        (impl_->barrier_sequence != 0 && impl_->barrier_sequence != seq))
      return false;
    impl_->barrier_armed = false;
  }
  impl_->cv.notify_all();
  return true;
}

bool JobOrchestrator::failed() const noexcept {
  std::lock_guard lock(impl_->mutex);
  return impl_->failed;
}
bool JobOrchestrator::sealed() const noexcept {
  std::lock_guard lock(impl_->mutex);
  return impl_->mode == Impl::Mode::kSealed || impl_->mode == Impl::Mode::kStopped;
}
bool JobOrchestrator::stopped() const noexcept {
  std::lock_guard lock(impl_->mutex);
  return impl_->mode == Impl::Mode::kStopped;
}
std::optional<Uuid> JobOrchestrator::LastCreated() const {
  std::lock_guard lock(impl_->mutex);
  return impl_->last_created;
}
std::optional<Uuid> JobOrchestrator::GeneratedJob() const {
  std::lock_guard lock(impl_->mutex);
  return impl_->last_created;
}
std::optional<Uuid> JobOrchestrator::GeneratedWorker() const {
  std::lock_guard lock(impl_->mutex);
  const auto* resident = impl_->last_created ? impl_->Find(*impl_->last_created) : nullptr;
  return resident && resident->generated_identities
             ? std::optional<Uuid>(resident->generated_identities->worker)
             : std::nullopt;
}
std::optional<StableId> JobOrchestrator::GeneratedLaunch() const {
  std::lock_guard lock(impl_->mutex);
  const auto* resident = impl_->last_created ? impl_->Find(*impl_->last_created) : nullptr;
  return resident && resident->generated_identities
             ? std::optional<StableId>(resident->generated_identities->launch_operation)
             : std::nullopt;
}
std::optional<Uuid> JobOrchestrator::GeneratedWorker(const Uuid& id) const {
  std::lock_guard lock(impl_->mutex);
  const auto* resident = impl_->Find(id);
  return resident && resident->generated_identities
             ? std::optional<Uuid>(resident->generated_identities->worker)
             : std::nullopt;
}
std::optional<StableId> JobOrchestrator::GeneratedLaunch(const Uuid& id) const {
  std::lock_guard lock(impl_->mutex);
  const auto* resident = impl_->Find(id);
  return resident && resident->generated_identities
             ? std::optional<StableId>(resident->generated_identities->launch_operation)
             : std::nullopt;
}
std::size_t JobOrchestrator::journal_attempts() const noexcept { return 0; }
std::size_t JobOrchestrator::apply_count() const noexcept {
  std::lock_guard lock(impl_->mutex);
  return impl_->apply_total;
}
std::size_t JobOrchestrator::writer_turn_count() const noexcept {
  std::lock_guard lock(impl_->mutex);
  return impl_->writer_turns;
}
std::size_t JobOrchestrator::max_concurrent_writer_turns() const noexcept {
  std::lock_guard lock(impl_->mutex);
  return impl_->max_writer_active;
}
std::size_t JobOrchestrator::disposed_count() const noexcept {
  std::lock_guard lock(impl_->mutex);
  return impl_->disposed;
}
std::size_t JobOrchestrator::completion_count() const noexcept {
  std::lock_guard lock(impl_->mutex);
  return impl_->completion_total;
}
std::size_t JobOrchestrator::ack_authorization_count() const noexcept {
  std::lock_guard lock(impl_->mutex);
  return impl_->acks;
}
std::size_t JobOrchestrator::effect_count() const noexcept {
  std::lock_guard lock(impl_->mutex);
  return impl_->effects;
}
std::size_t JobOrchestrator::response_count() const noexcept {
  std::lock_guard lock(impl_->mutex);
  return impl_->responses;
}
std::size_t JobOrchestrator::resident_count() const noexcept {
  std::lock_guard lock(impl_->mutex);
  return impl_->ResidentCount();
}
std::size_t JobOrchestrator::critical_occupancy() const noexcept {
  std::lock_guard lock(impl_->mutex);
  // The observable reports the fixed critical-reserve inventory for each
  // provisional/resident Job plus the pending daemon marker. This is distinct
  // from the live permit counter used internally for admission.
  std::size_t inventory = impl_->shutdown_gate_pending ? 1U : 0U;
  for (const auto& resident : impl_->residents) {
    if (resident.exists) {
      inventory += 9U;
    } else if (resident.claims != 0) {
      for (const auto& gate : resident.gates)
        if (gate.registered) ++inventory;
    }
  }
  return inventory;
}
std::size_t JobOrchestrator::normal_occupancy() const noexcept {
  std::lock_guard lock(impl_->mutex);
  return impl_->normal_count;
}
std::size_t JobOrchestrator::total_occupancy() const noexcept {
  std::lock_guard lock(impl_->mutex);
  std::size_t inventory = impl_->shutdown_gate_pending ? 1U : 0U;
  for (const auto& resident : impl_->residents) {
    if (resident.exists) {
      inventory += 9U;
    } else if (resident.claims != 0) {
      for (const auto& gate : resident.gates)
        if (gate.registered) ++inventory;
    }
  }
  return impl_->normal_count + inventory;
}
std::size_t JobOrchestrator::creation_claim_count(const Uuid& id) const noexcept {
  std::lock_guard lock(impl_->mutex);
  if (auto* r = impl_->Find(id)) return r->claims;
  return 0;
}
std::optional<Completion> JobOrchestrator::TakeCompletion(std::uint64_t s) {
  std::lock_guard lock(impl_->mutex);
  for (auto& slot : impl_->completions) {
    if (slot.reserved && slot.completed && slot.sequence == s) {
      auto result = std::move(slot.value);
      slot.reserved = false;
      slot.completed = false;
      slot.sequence = 0;
      slot.value = {};
      --impl_->completion_used;
      return result;
    }
  }
  return std::nullopt;
}
std::optional<Snapshot> JobOrchestrator::SnapshotFor(const Uuid& id) const {
  std::lock_guard lock(impl_->mutex);
  if (auto* r = impl_->Find(id)) return r->banks[r->active].snapshot;
  return std::nullopt;
}
std::vector<TraceRecord> JobOrchestrator::CopyTrace() const {
  std::lock_guard lock(impl_->mutex);
  return {impl_->trace.begin(),
          impl_->trace.begin() + static_cast<std::ptrdiff_t>(impl_->trace_count)};
}
std::vector<LogicalJobEvent> JobOrchestrator::CopyJournalAttempts() const { return {}; }
std::vector<std::uint64_t> JobOrchestrator::CopyIngressSequences() const {
  std::lock_guard lock(impl_->mutex);
  return {impl_->ingress.begin(),
          impl_->ingress.begin() + static_cast<std::ptrdiff_t>(impl_->ingress_count)};
}
std::vector<ApplicationLaunchRequest> JobOrchestrator::CopyLaunchRequests() const { return {}; }
std::vector<SessionRetainRequest> JobOrchestrator::CopySessionRequests() const { return {}; }
std::optional<RawCandidateEvent> JobOrchestrator::TakeRunnerCandidate() { return {}; }
std::optional<RawCandidateEvent> JobOrchestrator::TakeSessionCandidate() { return {}; }
bool JobOrchestrator::CancelRunnerCandidate() { return false; }
bool JobOrchestrator::CancelSessionCandidate() { return false; }
bool JobOrchestrator::VerifyFakes() const noexcept {
  std::lock_guard lock(impl_->mutex);
  return !impl_->failed && impl_->postcommit_construction_count == 0 &&
         impl_->postcommit_allocation_count == 0;
}
bool JobOrchestrator::NoForbiddenPostcommitActions() const noexcept {
  std::lock_guard lock(impl_->mutex);
  return impl_->postcommit_construction_count == 0 && impl_->postcommit_allocation_count == 0;
}
bool JobOrchestrator::NoPostcommitAllocationOrCopy() const noexcept {
  std::lock_guard lock(impl_->mutex);
  return impl_->postcommit_construction_count == 0 && impl_->postcommit_allocation_count == 0 &&
         impl_->prepared_turn_count != 0;
}
bool JobOrchestrator::DestinationCapacityCheckedBeforeCommit() const noexcept {
  std::lock_guard lock(impl_->mutex);
  return impl_->destination_capacity_checks != 0;
}
void JobOrchestrator::InjectPrecommitMaterializationFailure() {
  std::lock_guard lock(impl_->mutex);
  impl_->precommit_failure = true;
}
void JobOrchestrator::InjectAccountingCorruption() {
  std::lock_guard lock(impl_->mutex);
  impl_->accounting_failure = true;
  impl_->FailLocked();
}
void JobOrchestrator::InjectResidualCriticalPermit() {
  std::lock_guard lock(impl_->mutex);
  impl_->critical_count = 1;
}
void JobOrchestrator::SetNextCommitResult(LogicalCommitResult r) {
  void (*setter)(void*, LogicalCommitResult) noexcept = nullptr;
  void* context = nullptr;
  {
    std::lock_guard lock(impl_->mutex);
    setter = impl_->config.ports.set_commit_result;
    context = impl_->config.ports.context;
  }
  if (setter != nullptr) setter(context, r);
}
void JobOrchestrator::SetDestinationCapacity(std::size_t n) {
  std::lock_guard lock(impl_->mutex);
  impl_->destination_capacity = n;
}

}  // namespace sitometron::core::internal
