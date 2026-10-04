# ADR-0008: Define the External REST v1 Job surface

## Status

Proposed — 2026-10-04

## Context

Sitometron owns the external REST control plane (`docs/00_overview.md` Section 2). The Contract
Registry row `External REST v1` has been `Planned | Planned` since Phase 0A. The core Job lifecycle
(ADR-0002), the single-writer ingress (ADR-0003), and the durable JobJournal with replay (ADR-0006)
are implemented, so a client-facing contract can now be stated in terms of facts the daemon already
decides.

A non-normative walking skeleton (`spike/README.md`) served an unversioned `/jobs` surface. It
showed that `202` plus polling is enough for an operator (finding 12), and that a daemon needs a completion
per ingress sequence and the new identity from creation (findings 2 and 3). Its request named an
executable path, which the Application boundary forbids for the product: clients never submit
commands, modules, executable paths, or scripts (`docs/02_architecture.md` Section 4).

A wire contract is hard to change once clients exist. The owner decided on 2026-10-04 under
Issue #76 to freeze a small slice first:

- only the Job operations, health, and readiness; Admission tickets, roles, and the Application
  Registry are later ADRs that extend `/v1`;
- direct creation stays as the path that admits now or refuses, when a waiting Admission path is
  added later;
- `terminate` is decided together with roles;
- the principal recorded for a cancel is one configured name until roles exist; and
- the OpenAPI document is written after this ADR is Accepted, under its own Issue.

## Decision

We will define External REST v1 as the following contract. Sections 1 to 9 are normative for every
implementation; anything they do not state is not part of v1.

### 1. Transport and namespace

- The daemon serves HTTP/1.1 on a loopback address only. It refuses to start with a listen address
  that is not loopback. TLS and client authentication are the job of a reverse proxy in front of it.
- Every path of this contract starts with `/v1`. Paths outside `/v1` are not part of any contract.
- Request and response bodies are JSON (RFC 8259) in UTF-8. A response with a body carries
  `Content-Type: application/json`.
- Identifiers and enumeration values are lowercase ASCII. A `job_id` is the canonical lowercase
  UUIDv7 text that the daemon issued.

### 2. Compatibility

Within `/v1`:

- A later version may add endpoints, optional request fields, response fields, error codes, and
  readiness reason codes. A client ignores response fields it does not know and classifies an
  unknown error code by its HTTP status.
- A later version does not remove or rename an endpoint or field, change a field's type or meaning,
  make an optional request field required, change the status of an existing condition, or add a
  value to the `state` or `outcome` vocabularies.
- A change of the second kind uses a new prefix (`/v2`) and a superseding ADR.

### 3. The Job resource

A Job is represented by one JSON object:

| Field | Type | Meaning |
|---|---|---|
| `job_id` | string | The daemon-issued UUIDv7 |
| `state` | string | The ADR-0002 Job state: `admitted`, `preparing`, `running`, `stopping`, `finalizing`, `succeeded`, `failed`, `cancelled`, `terminated`, or `timed_out` |
| `terminal` | boolean | `true` exactly when `state` is `succeeded`, `failed`, `cancelled`, `terminated`, or `timed_out` |
| `outcome` | string or null | The terminal outcome, equal to `state` when `terminal` is `true`; `null` otherwise |

Every value reflects only events that are committed in the JobJournal (ADR-0006 Section 3). The
resource carries no timestamps, process details, or application data in this slice.

A Job is readable while the daemon holds it as a resident Job. A Job that an offline prune removed
from replay (ADR-0006 Section 8), or that a later retirement decision removes, is absent, and its
`job_id` is answered like an unknown one.

### 4. Endpoints

| Method and path | Purpose | Success |
|---|---|---|
| `POST /v1/jobs` | Create a Job | `202` with the Job resource and `Location: /v1/jobs/{job_id}` |
| `GET /v1/jobs/{job_id}` | Read one Job | `200` with the Job resource |
| `GET /v1/jobs` | List resident Jobs | `200` with `{"jobs": [Job, ...]}` |
| `POST /v1/jobs/{job_id}/cancel` | Request a cooperative cancel | `202` with the Job resource |
| `GET /v1/health` | Liveness | `200` with `{"status": "ok"}` |
| `GET /v1/ready` | Readiness | `200` or `503` with the readiness object of Section 6 |

**Create.** The request body is one object with exactly one field:

| Field | Type | Meaning |
|---|---|---|
| `application_id` | string, 1 to 128 characters | Names a deployment-registered Application |

- The client cannot choose or propose a `job_id`, and there is no idempotency key. A client that
  loses the response cannot learn whether a Job was created from this endpoint; it lists the Jobs.
- The daemon answers `202` only after `job_created` is committed. The Job in the response is at
  least `admitted`.
- Creation never waits for capacity. It admits now or refuses with the `service` errors of
  Section 5.
- How an `application_id` resolves to something launchable belongs to the Application Registry ADR.
  This contract fixes only that an identifier the deployment does not know is refused with
  `unknown_application` and creates nothing.

**Forward compatibility with Admission.** A later ADR may add a waiting Admission path under `/v1`
(for example tickets that are claimed when capacity frees). That path produces the same Job resource
with the same `job_id` semantics, and reading, listing, and cancelling such a Job use the endpoints
above unchanged. `POST /v1/jobs` remains the path that admits now or refuses; the later ADR may add
refusal codes to it but does not change its request, its success response, or make it wait.

**Read and list.** The list holds every resident Job, in creation order (ascending Journal sequence
of `job_created`). It is bounded by the daemon's resident Job capacity (ADR-0003), so this slice has
no pagination and no filters. A later ADR that exposes more than the resident Jobs does so through a
new endpoint or an opt-in query parameter, so that this request keeps returning the complete
resident list.

**Cancel.** The request has no body; a body, if present, must be an empty JSON object. The daemon
submits the ADR-0002 `cancel` command for the Job:

| Command result | Status | Error `code` |
|---|---|---|
| `cancel_accepted` committed | `202` with the Job resource after the event is applied | — |
| `job_not_found` | `404` | `job_not_found` |
| `stop_cause_already_latched` | `409` | `stop_cause_already_latched` |
| `command_not_allowed_in_state` | `409` | `command_not_allowed_in_state` |
| Not admitted by the writer's ingress | `503` | the `service` code of Section 5 for that ingress result |

- `202` means the cancel is recorded, not that the Job has stopped. The client reads the Job to see
  it reach a terminal state.
- A repeated cancel is safe: it is answered with one of the `409` codes and changes nothing.
- The principal recorded in `cancel_accepted` is one name from the daemon's startup configuration.
  No request header or field supplies or overrides it in this slice. The roles ADR decides how a
  proxy-authenticated principal replaces it.
- `terminate` has no endpoint in this slice.

### 5. Errors

Every response with a status of `400` or above carries one object:

```json
{"error": {"domain": "job", "code": "job_not_found", "message": "...", "details": {}}}
```

- `domain` and `code` are stable machine identifiers. `message` is for people and is not part of the
  contract. `details` is an object; its fields are optional and additive.
- A message or detail never contains a file-system path, a raw operating-system or library error, a
  Journal record, or a secret.

| Condition | Status | `domain` | `code` |
|---|---|---|---|
| Body is not well-formed JSON, or has duplicate keys | `400` | `request` | `malformed_json` |
| `job_id` in the path is not a canonical lowercase UUIDv7 | `400` | `request` | `invalid_job_id` |
| No route for the path | `404` | `request` | `route_not_found` |
| Route exists, method not allowed (with an `Allow` header) | `405` | `request` | `method_not_allowed` |
| Body exceeds the request size bound | `413` | `request` | `payload_too_large` |
| Body present without `Content-Type: application/json` | `415` | `request` | `unsupported_media_type` |
| Well-formed JSON that is not the expected object: unknown field, missing field, wrong type, or value out of range | `422` | `request` | `validation_failed` |
| `application_id` is not registered in this deployment | `422` | `job` | `unknown_application` |
| Unknown or absent Job | `404` | `job` | `job_not_found` |
| Cancel rejected by state | `409` | `job` | the ADR-0002 rejection reason, as in Section 4 |
| Admission is closed: readiness is false for `unresolved_jobs` or `shutting_down` (Section 6) | `503` | `service` | `not_ready` |
| The resident Job capacity is exhausted (`resident_limit`) | `503` | `service` | `capacity_exhausted` |
| The ingress queue is full (`normal_full`) | `503` | `service` | `busy` |
| The writer has failed closed, including a commit whose outcome is unknown | `503` | `service` | `service_failed` |
| Any other failure inside the daemon | `500` | `service` | `internal` |

- Validation and routing errors are decided before anything is submitted to the writer, so they
  create no Journal record (`JRN-003`).
- When several conditions hold, the first matching one in this order decides: route, method, media
  type, size, JSON, validation, the writer's ingress result (`service_failed` before `not_ready`,
  then `capacity_exhausted` before `busy`, as ADR-0003 orders them), then the ADR-0002 result. A
  cancel for an unknown Job is therefore `not_ready` while admission is closed.
- `busy` may be retried by the client. `capacity_exhausted` does not clear without operator action
  in this slice (ADR-0003 holds resident slots for the process lifetime). `service_failed` does not
  clear without a restart; after `service_failed` on a create or a cancel, the client must not
  assume either outcome and reads the state after the daemon is ready again.
- The request size bound is fixed at startup and is at least 4 KiB. Its default belongs to the
  implementation Issue.

### 6. Health and readiness

- `GET /v1/health` answers `200` whenever the listener serves requests. It says nothing about
  readiness.
- `GET /v1/ready` answers `200` with `{"ready": true, "reasons": []}` when the daemon accepts new
  Jobs, and `503` with `{"ready": false, "reasons": [...]}` otherwise. This `503` body is the
  readiness object, not the error envelope.
- Each reason is an object with a `code` and optional additive fields. The codes of this slice are:
  - `unresolved_jobs`, with `job_ids`: replay found Jobs that are not resolved (`OPS-004`);
  - `service_failed`: the writer's failure latch is set (ADR-0003);
  - `shutting_down`: admission is closed for shutdown.
- While readiness is false, `POST /v1/jobs` answers `503` `not_ready`. Reads and lists keep working
  as long as the listener serves requests. A cancel is a normal ingress input (ADR-0003), so it is
  refused with `not_ready` while admission is closed and with `service_failed` after the failure
  latch, whatever the state of the Job.

### 7. What the daemon must not do

- It does not accept an executable path, command line, module, script, environment, or working
  directory from a client.
- It does not report a state, outcome, or success that is not committed in the JobJournal.
- It does not block a request on capacity, on another Job, or on a Job reaching a state.

### 8. Machine-readable description and checks

- An OpenAPI 3.1 document for this contract is added under its own Issue after this ADR is Accepted
  and before the production adapter merges. This ADR is the authority; where the document and this
  ADR differ, the document is wrong.
- The production adapter is verified against that document by a contract check, and each row of the
  tables in Sections 4 to 6 has a named test. The names are listed in
  `docs/06_build_test_packaging.md`.

### 9. Requirements

This ADR adds the requirement domain `API` (external REST surface) and the requirements `API-001` to
`API-006` in `docs/01_requirements.md`.

## Consequences

- Good: the first client-facing contract is small enough to implement and qualify as one working
  increment, and it states only facts the core already decides.
- Good: the Application boundary is part of the wire contract from the first version, so no client
  ever learns to send an executable.
- Good: later Admission, roles, and Registry work extends `/v1` by addition.
- Bad: two creation paths will exist once a waiting Admission path is added, and their fairness has
  to be decided then.
- Bad: until roles exist, every cancel is recorded under the same configured principal, and nobody
  can force-stop a Job through the API.
- Bad: a client that loses a create response cannot match it to a Job, because there is no
  idempotency key. It lists the Jobs and may see one it did not expect.
- Bad: until the OpenAPI document exists, the contract is prose and tables only.
- Neutral: the production adapter depends on core changes tracked in Issue #73: a completion per
  ingress sequence, creation returning the new identity, and a request size cap. Resident retirement
  decides how long a terminal Job stays readable.
- Neutral: the choice of an HTTP library is a separate ADR-0004 dependency decision.

## Options considered

- **Freeze the whole outline (Admission tickets, blocking claim, roles, Registry) in one ADR**:
  rejected because it delays the first working increment and freezes mechanisms nothing exercises
  yet.
- **Treat direct creation as provisional and replace it when Admission arrives**: rejected because a
  contract marked provisional is not frozen, and replacing it breaks clients or forces `/v2`.
- **Include `terminate` now**: rejected because it would start without any role distinction, and
  restricting it later changes behavior for existing clients. No Worker process exists to stop
  before the Worker protocol.
- **Take the principal from a proxy header now**: rejected because it freezes half of the
  authentication contract before roles are designed and needs a rule against spoofing on a direct
  loopback connection.
- **Client-supplied Job identifier or idempotency key**: rejected because identity issue stays with
  the daemon (ADR-0005), and deduplication would need durable state that no ADR defines.
- **Answer create synchronously with the terminal result, or long-poll**: rejected because a request
  would wait on a Job; the walking skeleton showed polling is enough.
- **Reuse HTTP `problem+json` (RFC 9457) as the error body**: rejected for this slice because the
  stable `domain` and `code` pair is the contract, and a second envelope format adds nothing a
  client needs. A later ADR may add it by content negotiation.
- **Unversioned paths as in the walking skeleton**: rejected because there would be no place for a
  breaking change.
- **Write the OpenAPI document in the same pull request**: rejected by owner decision to keep this
  review to one artifact.

## References

- Issue #76
- Issue #73 (handed-over design inputs), Issue #35 (assignment tracker)
- Requirements: `API-001` to `API-006`; relies on `JRN-003`, `JOB-008`, `OPS-001`, `OPS-004`
- Contract Registry: `External REST v1`
- Related ADR: ADR-0002, ADR-0003, ADR-0005, ADR-0006
