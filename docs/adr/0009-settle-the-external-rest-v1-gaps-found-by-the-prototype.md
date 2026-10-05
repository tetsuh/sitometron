# ADR-0009: Settle the External REST v1 gaps found by the prototype

## Status

Proposed — 2026-10-06

## Context

Accepted ADR-0008 defines the External REST v1 Job surface. Before a production adapter is
designed, a non-normative prototype of that surface was built in the walking skeleton (Issues #82
and #84, `spike/README.md`, section "External REST v1 prototype"). Building a listener showed
questions that ADR-0008 does not answer: requests that fail before any route is known, how the size
bound fits the check order, an ingress result with no status, query strings, a missing body,
methods such as `HEAD`, the characters of an Application identifier, what a JSON parser lets
through, and the body of a cancel. It also showed consequences of ADR-0008 that a client or operator
can easily miss.

An Accepted ADR is changed only by a superseding ADR (`docs/10_adr_process.md` Section 4). Every
rule below either fills a case ADR-0008 leaves open or states what ADR-0008 already implies; none
changes a rule of ADR-0008. The owner decided on 2026-10-06 under Issue #86:

- the gaps are settled in one supplementary ADR;
- creation is serialized by the adapter, and an `already_pending` that occurs anyway is `busy`;
- `details` stays empty in v1;
- a deployment registers only identifiers of the form `[a-z0-9][a-z0-9._:-]*`;
- the unresolved Job that keeps admission closed is stated as a consequence only;
- the body bound keeps the order of ADR-0008, and only an oversized header section is refused
  before routing;
- a body that stops arriving is `408` `request_timeout`; and
- the order in which the supervisor owes events after a cancel is not part of this ADR (Issue #73,
  H10).

## Decision

We will apply the following rules to External REST v1 in addition to ADR-0008. Section numbers in
parentheses refer to ADR-0008.

### 1. Refusals before a route is known

Some requests are refused before the order of ADR-0008 Section 5 can start, because no route can be
determined. They precede every condition of that order and use the error envelope (Section 5):

| Condition | Status | `domain` | `code` |
|---|---|---|---|
| The request line is not `METHOD SP origin-form SP HTTP/1.0` or `HTTP/1.1`, or the header section is not well-formed | `400` | `request` | `malformed_request` |
| The request line and header section are not received within the listener's time bound | `408` | `request` | `request_timeout` |
| The header section exceeds the listener's header bound | `431` | `request` | `headers_too_large` |

The time bound and the header bound are fixed at startup; their values belong to the
implementation Issue. The header bound is at least 8 KiB.

### 2. Body checks keep the order of ADR-0008

Route, method, and media type are decided from the request line and the header section. The body
bound (Section 5, `payload_too_large`) is applied after them: from `Content-Length` when it is
present, and otherwise while the body is read. An oversized body on a route that does not exist is
therefore `404`, and on a route that does not allow the method `405`.

The body is received within the listener's time bound. A body that is not complete within it is
answered `408` `request`/`request_timeout`. This is decided after route, method, media type, and a
size known from `Content-Length`, and before JSON and validation; nothing is submitted to the
writer.

### 3. `already_pending`

The adapter serializes creation so that the writer's ingress never answers `already_pending` to a
creation. If it does, the request is answered `503` `service`/`busy`, and nothing is created.

### 4. Query strings

v1 defines no query parameter. A request whose target has a query component is answered `404`
`request`/`route_not_found`, whatever its path. ADR-0008 Section 4 foresees one later addition of
this kind: an opt-in query parameter on `GET /v1/jobs` to expose more than the resident Jobs. Adding
it turns this refusal into success for that parameter only. Any other query parameter needs an ADR
that states its compatibility under Section 2.

### 5. Requests without a body

A request without a body has no media type to check. `POST /v1/jobs` without a body is `400`
`request`/`malformed_json`, because an empty body is not a JSON text. A cancel without a body is
valid (Section 4).

### 6. Methods a route does not allow

`HEAD`, `OPTIONS`, and every other method that a route does not list in Section 4 are answered
`405` `request`/`method_not_allowed` with an `Allow` header naming the methods the route has.

### 7. `details`

In v1 every error has `"details": {}`. A later version may add fields to it (Section 2).

### 8. Strict JSON input

A body is `malformed_json` (Section 5) when it is not exactly one JSON text in UTF-8. In
particular, each of the following is `malformed_json`, whatever the JSON library would accept:

- a raw NUL byte anywhere in the body;
- a byte sequence that is not valid UTF-8;
- data after the JSON value, other than whitespace; and
- a duplicate key in any object, at any depth.

An escaped `\u0000` inside a string is valid JSON and is decided by validation, not here. Each of
the four cases has its own named test, whatever JSON library the adapter uses.

### 9. Application identifiers

A deployment registers only `application_id` values of the form `[a-z0-9][a-z0-9._:-]*` with 1 to
128 characters, and refuses to start with any other. The request rules of Section 4 are unchanged:
a value of 1 to 128 characters that is not registered is `422` `job`/`unknown_application`
whatever its characters, and an empty or longer value is `422` `request`/`validation_failed`.

### 10. The cancel body

When a cancel request has a body (Section 4 allows only `{}`):

| Body | Status | `domain` | `code` |
|---|---|---|---|
| Present without `Content-Type: application/json` | `415` | `request` | `unsupported_media_type` |
| Not well-formed JSON (Section 8 of this ADR included) | `400` | `request` | `malformed_json` |
| Well-formed JSON other than an empty object | `422` | `request` | `validation_failed` |

### 11. Consequences of ADR-0008 stated for clients and operators

These add no obligation; they make explicit what ADR-0008 already decides.

- **`terminal` before cleanup.** `terminal` is `true` once the terminal outcome is committed
  (Section 3). Exit confirmation, resource release, and cleanup records can follow it, so a
  terminal Job can still occupy its resident slot for a short time.
- **The `202` of a cancel.** The Job resource in the `202` shows what is committed when it is read
  after `cancel_accepted` (Section 4). It is `stopping` when a process may exist, `finalizing` when
  the cancel arrived before a launch intent was recorded, and can already be terminal. A client
  relies on the status, not on the state.
- **The Job that keeps admission closed.** A cancel is a normal ingress input (Section 6), so a
  cancel of an unresolved replayed Job is `503` `service`/`not_ready` like any other cancel while
  admission is closed. That Job cannot be stopped through this API until its resolution, which is
  outside this contract.

### 12. Requirements and checks

This ADR adds `API-007` in `docs/01_requirements.md`. Its checks are listed in
`docs/06_build_test_packaging.md`.

## Consequences

- Good: an implementer of the production adapter has an answer for every case the prototype met,
  and the OpenAPI document can describe every status the adapter returns.
- Good: the check order of ADR-0008 holds for every request whose route can be known, so a client
  can rely on it.
- Good: a JSON library's leniency cannot reach the core, because the refusals are a contract with
  named tests, not a property of a library.
- Bad: three more error codes (`malformed_request`, `request_timeout`, `headers_too_large`) exist
  before any client uses them.
- Bad: creation is serialized in the adapter; concurrent creations wait for each other for the time
  of one Journal commit.
- Bad: the identifier form is fixed before the Application Registry ADR, which must keep it or
  supersede this rule.
- Neutral: the walking skeleton's listener refuses an oversized request with `413` before routing,
  which differs from the first paragraph of Section 2 of this ADR. The skeleton is not normative
  and keeps that behavior.

## Options considered

- **Refuse an oversized request with `413` before routing, as the prototype does**: rejected because
  it changes the order of ADR-0008 Section 5, which only a superseding ADR may do, and the order can
  be kept by checking the body bound after the header section.
- **Map an oversized header section to `413` `payload_too_large`**: rejected because the header
  section is not the payload, and a client that shortens its body would not clear the condition.
- **Settle the gaps only in the OpenAPI document**: rejected because the document cannot express the
  check order, the strict JSON rules, or the tests they require, and ADR-0008 Section 8 makes the
  ADR the authority.
- **Let the core accept a second creation during identity generation**: rejected for now because no
  measured need exists and the core change would need its own review.
- **Name the failing field in `details` for `validation_failed`**: rejected for v1 because the only
  request field is `application_id`, and the field vocabulary can be added later without a break.
- **Refuse an `application_id` outside the identifier form with `validation_failed`**: rejected
  because ADR-0008 Section 4 already gives every unregistered value of 1 to 128 characters the code
  `unknown_application`, and changing that would change an Accepted rule.
- **Answer a target with a query by ignoring the query**: rejected because a later query parameter
  would then change the meaning of a request a client already sends.
- **Allow a cancel of an unresolved Job while admission is closed**: rejected because it adds an
  exception to the ingress rule of ADR-0003 and needs a design for stopping a Job whose process
  state is unknown.

## References

- Issue #86
- Issues #82 and #84 (prototype), Issue #73 (H10)
- Requirements: `API-007`; relies on `API-001` to `API-006`
- Contract Registry: `External REST v1`
- Related ADR: ADR-0008, ADR-0003
