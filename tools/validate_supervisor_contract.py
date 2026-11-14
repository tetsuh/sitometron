"""Offline ADR-0012 vectors and reducer-derived completeness (development tooling only)."""
from __future__ import annotations

import argparse
from collections import defaultdict, deque
import itertools
import json
from pathlib import Path

from json_schema_validator import LocalDraft202012Schemas

JOB = "01890f30-7b54-7cc3-98c4-dc0c0c07398f"
WORKER = "550e8400-e29b-41d4-a716-446655440000"
DIGEST = "44136fa355b3678a1146ad16f7e8649e94fb4fc21fe77e8310c060f61caaff8a"
TERMINAL = {"succeeded", "failed", "cancelled", "terminated", "timed_out"}
OTHER = {"cancel_accepted", "terminate_accepted", "timeout_expired", "worker_launch_observed",
         "process_exit_confirmed", "worker_running", "worker_completed", "worker_failed", "session_retained"}
PHASES = ("preparation", "execution", "cooperative_stop", "process_exit_confirmation")
WORKER_EVENTS = {"worker_completed", "worker_failed"}
WORKER_DELIVERY_PREFIX = "worker_delivery:"


def require(condition, message):
    if not condition:
        raise ValueError(message)


def key(value):
    return json.dumps(value, sort_keys=True, separators=(",", ":"))


def canonical(value):
    if isinstance(value, list):
        return [canonical(v) for v in value]
    if not isinstance(value, dict):
        return value
    result = {}
    for field, item in value.items():
        if item is not None:
            if field in ("job_id", "session_id"):
                item = JOB
            elif field in ("worker_id", "pending_worker_id"):
                item = WORKER
            elif field in ("operation_id", "launch_operation_id"):
                item = "launch-op-1"
            elif field == "allocation_id":
                item = "empty-allocation"
            elif field == "allocation_digest":
                item = DIGEST
            elif field in ("event_sequence", "pending_worker_event_sequence"):
                item = 1
            else:
                item = canonical(item)
        result[field] = item
    return result


def matches(when, snapshot, payload):
    operation = when["operator"]
    if operation in ("always", "otherwise"):
        return True
    require(operation == "all", f"unsupported predicate {operation}")
    sources = {"snapshot": snapshot, "payload": payload}
    for predicate in when["predicates"]:
        source = predicate["source"]
        require(source in sources, f"unsupported source {source}")
        left = sources[source][predicate["field"]]
        op = predicate["operator"]
        if op == "eq":
            result = left == predicate["value"]
        elif op == "not_eq":
            result = left != predicate["value"]
        elif op == "in":
            result = left in predicate["values"]
        elif op == "eq_field":
            result = left == sources[predicate["right_source"]][predicate["right_field"]]
        else:
            raise ValueError(f"unsupported predicate operator {op}")
        if not result:
            return False
    return True


def select(row, snapshot, payload):
    for branch in row["cases"]:
        if matches(branch["when"], snapshot, payload):
            return branch
    raise ValueError(f"non-total reducer row {row['pair_id']}")


def fold(snapshot, branch, payload):
    result = dict(snapshot)
    if "next_state" in branch:
        result["state"] = branch["next_state"]
    for field, update in branch.get("updates", {}).items():
        if isinstance(update, dict):
            source = update["from"]
            require(source == "input.job_id" or source.startswith("payload."), f"unsupported update {source}")
            update = JOB if source == "input.job_id" else payload[source[8:]]
        result[field] = update
    return result


def finalizing_decision(s):
    if s["session_retention_status"] == "not_started":
        return 6, "session_retain_requested", []
    if s["session_retention_status"] == "requested":
        return 7, "", ["session_retainer"]
    if s["session_retention_status"] == "retained" and s["finalization_status"] == "pending":
        return 8, "finalization_completed", []
    if s["finalization_status"] in ("completed", "failed"):
        return 9, "terminal_outcome_committed", []
    raise ValueError("uncovered finalizing class")


def terminal_decision(s):
    if not s["process_exit_confirmed"] and s["cleanup_status"] == "pending":
        return 10, "", ["application_runner", "timer_port"]
    if s["process_exit_confirmed"] and s["resource_status"] == "committed":
        return 11, "resources_released", []
    if s["cleanup_status"] == "pending":
        return 12, "cleanup_status_recorded", []
    return 13, "", []


def decision_kind(event, sources):
    if event:
        return "Submit"
    if sources:
        return "Wait"
    return "Done"


def decide(s, launch):
    rule, event, sources = 13, "", []
    if s is None:
        rule = 1
    elif s["state"] == "admitted":
        rule, event = 2, "resources_committed"
    elif s["state"] == "preparing" and s["worker_launch_status"] == "not_started":
        rule, event = (4, "worker_launch_intent") if launch else (3, "")
    elif s["state"] in ("preparing", "running", "stopping"):
        rule, sources = 5, ["application_runner", "worker_protocol", "timer_port", "commands"]
    elif s["state"] == "finalizing":
        rule, event, sources = finalizing_decision(s)
    elif s["state"] in TERMINAL:
        rule, event, sources = terminal_decision(s)
    else:
        raise ValueError("unknown snapshot state")
    payload = {2: "fixed_empty_allocation", 4: "creation_launch_identity_and_committed_allocation",
               6: "bound_session", 8: "empty", 9: "latched_reason_or_success_candidate",
               11: "bound_allocation", 12: "cleanup_completed"}.get(rule, "none")
    return {"rule": rule, "kind": decision_kind(event, sources), "event": event,
            "sources": sources, "payload_rule": payload}


def load(root):
    directory = root.resolve(strict=True) / "schemas" / "core" / "v1"
    schemas = LocalDraft202012Schemas.load(directory)
    def document(name):
        return json.loads((directory / name).read_text(encoding="utf-8"))
    contract = document("supervisor-decision-contract.json")
    reducer = document("job-reducer-contract.json")
    vectors = document("job-reducer-vectors.json")
    schemas.validate_instance(directory / "supervisor-decision-contract.schema.json", contract)
    schemas.validate_instance(directory / "job-reducer-contract.schema.json", reducer)
    classes = {c["id"]: c["snapshot"] for c in contract["classes"]}
    require(len(classes) == len(contract["classes"]), "duplicate class ID")
    return contract, reducer, vectors, classes


def validate_supervisor_vectors(root):
    contract, _, _, classes = load(root)
    ids, covered = set(), set()
    for vector in contract["vectors"]:
        require(vector["id"] not in ids, "duplicate vector ID")
        ids.add(vector["id"])
        pair = (vector["class_id"], vector["launch_inputs_present"])
        require(pair not in covered, "duplicate class/presence vector")
        covered.add(pair)
        require(pair[0] is None or pair[0] in classes, "unknown vector class")
        snapshot = None if pair[0] is None else classes[pair[0]]
        require(vector["expected"] == decide(snapshot, pair[1]), f"wrong expected decision: {vector['id']}")
    require(covered == {(c, p) for c in [None, *classes] for p in (False, True)}, "missing class/presence vector")
    runtime_ids = [v["id"] for v in contract["runtime_vectors"]]
    require(len(runtime_ids) == len(set(runtime_ids)), "duplicate ordering vector")
    print(f"Validated {len(ids)} supervisor decision vectors exactly once")


def payload_prototypes(vectors):
    prototypes = defaultdict(dict)
    for vector in vectors["case_vectors"]:
        if vector["matrix"] != "event" or vector["expected"]["disposition"] == "reject":
            continue
        inp = vector["input"]
        payload = canonical(inp["payload"])
        prototypes[inp["event_type"]][key(payload)] = payload
    return prototypes


def add_predicate_domains(row, domains):
    for branch in row["cases"]:
        for predicate in branch["when"].get("predicates", []):
            if predicate["source"] == "payload" and predicate["operator"] in ("eq", "in"):
                domains[predicate["field"]].update(predicate.get("values", [predicate.get("value")]))


def expand_payloads(reducer, event, prototypes):
    require(prototypes, f"missing payload domain {event}")
    domains = defaultdict(set)
    for payload in prototypes.values():
        for field in ("phase", "outcome", "completion_mode", "status"):
            if field in payload:
                domains[field].add(payload[field])
    for row in reducer["event_matrix"]:
        if row["event_kind"] == event:
            add_predicate_domains(row, domains)
    expanded = {}
    for prototype in prototypes.values():
        fields = [f for f in domains if f in prototype]
        for values in itertools.product(*(sorted(domains[f], key=str) for f in fields)):
            payload = dict(prototype, **dict(zip(fields, values)))
            expanded[key(payload)] = payload
    return list(expanded.values())


def update_timer_history(history, effects, exit_confirmed):
    for effect in effects:
        for phase in PHASES:
            if effect == f"disarm_{phase}_timeout":
                history.discard(phase)
            if effect == f"arm_{phase}_timeout" or (effect == f"arm_{phase}_timeout_if_needed" and not exit_confirmed):
                history.add(phase)


class Graph:
    def __init__(self, reducer, vectors):
        self.reducer = reducer
        self.rows = {(r["entity_position"], r["event_kind"]): r for r in reducer["event_matrix"]}
        self.commands = {(r["entity_position"], r["command_kind"]): r for r in reducer["command_matrix"]}
        prototypes = payload_prototypes(vectors)
        self.payloads = {event: expand_payloads(reducer, event, prototypes[event])
                         for event in reducer["journal_event_kinds"]}
        start = fold({"schema_version": 1}, self.rows["absent", "job_created"]["cases"][0], {"session_id": JOB})
        self.start = key(start)
        self.classes = {self.start: start}
        self.edges = []
        self.outgoing = defaultdict(list)
        self.derive_classes()

    def accepted_edges(self, before, snapshot):
        for event in self.reducer["journal_event_kinds"]:
            if event in ("job_created", "late_worker_event"):
                continue
            for payload in self.payloads[event]:
                if event in ("cancel_accepted", "terminate_accepted") and select(
                    self.commands[snapshot["state"], event.removesuffix("_accepted")], snapshot, payload
                )["disposition"] == "reject":
                    continue
                row = self.rows[snapshot["state"], event]
                branch = select(row, snapshot, payload)
                if branch["disposition"] == "reject":
                    continue
                after = fold(snapshot, branch, payload)
                edge = (before, key(after), row["pair_id"], branch["case_id"], event, payload,
                        branch.get("post_sync_effects", []))
                yield edge, after

    def derive_classes(self):
        pending = deque([self.start])
        while pending:
            before = pending.popleft()
            for edge, after in self.accepted_edges(before, self.classes[before]):
                self.edges.append(edge)
                self.outgoing[before].append(edge)
                if edge[1] not in self.classes:
                    self.classes[edge[1]] = after
                    pending.append(edge[1])

    def producer_allows(self, history, edge):
        before, _, _, _, event, payload, _ = edge
        if event == "timeout_expired" and payload["phase"] not in history:
            return False
        return not (event in WORKER_EVENTS and self.classes[before]["pending_worker_event_ack"]
                    and WORKER_DELIVERY_PREFIX + event not in history)

    def producer_after(self, history, edge):
        _, after, _, _, event, payload, effects = edge
        next_history = set(history)
        if event == "timeout_expired":
            next_history.discard(payload["phase"])
        update_timer_history(next_history, effects, self.classes[after]["process_exit_confirmed"])
        if not self.classes[after]["pending_worker_event_ack"]:
            next_history.difference_update(WORKER_DELIVERY_PREFIX + e for e in WORKER_EVENTS)
        elif event in WORKER_EVENTS:
            next_history.add(WORKER_DELIVERY_PREFIX + event)
        return frozenset(next_history)

    def producer_histories(self):
        reached = defaultdict(set)
        reached[self.start].add(frozenset())
        pending = deque([(self.start, frozenset())])
        while pending:
            before, history = pending.popleft()
            for edge in self.outgoing[before]:
                if not self.producer_allows(history, edge):
                    continue
                after = edge[1]
                frozen = self.producer_after(history, edge)
                if frozen not in reached[after]:
                    reached[after].add(frozen)
                    pending.append((after, frozen))
        return reached


def assigned_submit_is_accepted(graph, snapshot, event):
    accepted = False
    for payload in graph.payloads[event]:
        if event == "terminal_outcome_committed" and payload["outcome"] != (snapshot["latched_reason"] or snapshot["completion_candidate"]):
            continue
        if event == "cleanup_status_recorded" and payload["status"] != "completed":
            continue
        accepted |= select(graph.rows[snapshot["state"], event], snapshot, payload)["disposition"] != "reject"
    return accepted


def validate_assigned_submits(graph):
    for snapshot in graph.classes.values():
        for launch in (False, True):
            answer = decide(snapshot, launch)
            if answer["kind"] == "Submit":
                require(assigned_submit_is_accepted(graph, snapshot, answer["event"]), "assigned Submit is rejected by reducer")


def validate_witnesses(contract, graph):
    for item in contract["classes"]:
        require(item["witness"][0] == "absent__job_created/create", "witness is not rooted at creation")
        positions = {graph.start}
        for step in item["witness"][1:]:
            positions = {e[1] for position in positions for e in graph.outgoing[position] if e[2] + "/" + e[3] == step}
        require(key(item["snapshot"]) in positions, f"invalid reachable witness {item['id']}")


def validate_late_timeout(before, after, graph):
    require(before["finalization_status"] == "completed" and before["completion_candidate"] == "succeeded"
            and after["latched_reason"] == "timed_out" and after["completion_candidate"] is None,
            "S25 is not ADR-0012 Context's late timeout")
    stale = select(graph.rows["finalizing", "terminal_outcome_committed"], after, {"outcome": "succeeded"})
    require(stale["rejection_reason"] == "terminal_outcome_mismatch", "S25 stale success not rejected")


def validate_seeds(contract, reducer, classes, graph):
    seed_ids, cases = set(), set()
    for seed in contract["seeds"]:
        require(seed["id"] not in seed_ids, "duplicate seed ID")
        seed_ids.add(seed["id"])
        before, after = classes[seed["before_class"]], classes[seed["after_class"]]
        event = seed["pair_id"].split("__")[1]
        row = graph.rows[before["state"], event]
        branch = select(row, before, seed["payload"])
        require(row["pair_id"] == seed["pair_id"] and branch["case_id"] == seed["case_id"]
                and branch["disposition"] != "reject", "seed selects wrong accepting case")
        require(fold(before, branch, seed["payload"]) == after, "seed binding/transition mismatch")
        if seed["id"] == "S25":
            validate_late_timeout(before, after, graph)
        else:
            pair = (seed["pair_id"], seed["case_id"])
            require(pair not in cases, "duplicate control-branch seed")
            cases.add(pair)
    expected_cases = {(r["pair_id"], b["case_id"]) for r in reducer["event_matrix"]
                      if r["event_kind"] in ("cancel_accepted", "terminate_accepted", "timeout_expired")
                      for b in r["cases"] if b["disposition"] != "reject"}
    require(seed_ids == {f"S{i:02}" for i in range(1, 26)} and cases == expected_cases, "25-seed reconciliation incomplete")


def undeliverable_reason(snapshot, histories, event, payload):
    if not histories:
        return "no_producer_deliverable_witness"
    if event == "timeout_expired" and not any(payload["phase"] in h for h in histories):
        return "no_current_armed_phase_for_this_class"
    if event in WORKER_EVENTS and snapshot["pending_worker_event_ack"] and not any(
        WORKER_DELIVERY_PREFIX + event in h for h in histories
    ):
        return "worker_delivery_identity_conflicts_with_pending"
    return None


def classify_overtakes(graph, ids):
    histories = graph.producer_histories()
    missing, deliverable = [], 0
    for before, _, pair, case, event, payload, _ in graph.edges:
        snapshot = graph.classes[before]
        if event not in OTHER or decide(snapshot, True)["kind"] != "Submit":
            continue
        reason = undeliverable_reason(snapshot, histories[before], event, payload)
        if reason:
            missing.append({"class_id": ids[before], "pair_id": pair, "case_id": case,
                            "payload": payload, "reason": reason})
        else:
            deliverable += 1
    return missing, deliverable


def validate_supervisor_complete(root):
    validate_supervisor_vectors(root)
    contract, reducer, vectors, classes = load(root)
    graph = Graph(reducer, vectors)
    declared = {key(s) for s in classes.values()}
    require(len(declared) == len(classes), "duplicate snapshot class")
    require(declared == set(graph.classes), "class inventory differs from reducer fixed point")
    ids = {key(s): identifier for identifier, s in classes.items()}
    validate_assigned_submits(graph)
    validate_witnesses(contract, graph)
    validate_seeds(contract, reducer, classes, graph)
    missing, deliverable = classify_overtakes(graph, ids)
    declared_missing = [key(v) for v in contract["reducer_accepted_but_undeliverable"]]
    require(len(declared_missing) == len(set(declared_missing)), "duplicate undeliverable edge")
    require(set(declared_missing) == {key(v) for v in missing}, "undeliverable source list differs from producer histories")
    for item in sorted(missing, key=key):
        print("REDUCER_ACCEPTED_BUT_UNDELIVERABLE", key(item))
    print(f"Validated {len(graph.classes)} classes, {len(graph.edges)} edges, 25 seeds; {deliverable} deliverable and {len(missing)} named undeliverable overtakes")


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--check", choices=("vectors", "complete", "both"), default="both")
    args = parser.parse_args()
    root = Path(__file__).resolve().parents[1]
    if args.check in ("vectors", "both"):
        validate_supervisor_vectors(root)
    if args.check in ("complete", "both"):
        validate_supervisor_complete(root)
