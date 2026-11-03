# ADR-0015: Register Applications with a verified registry file

## Status

Proposed — 2026-11-03

## Context

ADR-0008 Section 4 leaves registration to this decision: "How an `application_id` resolves to
something launchable belongs to the Application Registry ADR. This contract fixes only that an
identifier the deployment does not know is refused with `unknown_application` and creates nothing.
Until that ADR is Accepted, the registered set is a list of identifiers in the daemon's startup
configuration." Phase 1 registered nothing and offered no option to register (Gate #101, P1-4), so
every production create answers `422` `unknown_application`. Phase 2 (Gate #123, P2-1) needs the
first registered Application.

The code and the Accepted contracts fix these facts:

- `worker_launch_intent` binds a "verified Application identity" (ADR-0002, Decision). The Journal
  event schema (`schemas/core/v1/job-journal-event.schema.json`) requires its `application` object
  to hold exactly `application_id`, `version` (1 to 128 characters), and `bundle_sha256` (64
  lowercase hexadecimal characters), and the reducer checks the same fields. No document defines a
  bundle, and the walking skeleton wrote a fixed placeholder digest (`spike/src/job_driver.cpp`).
- ADR-0009 Section 9 already fixes the identifier: "A deployment registers only `application_id`
  values of the form `[a-z0-9][a-z0-9._:-]*` with 1 to 128 characters, and refuses to start with any
  other." The Phase 1 composition checks that form (`RegisteredApplicationForm` in
  `apps/sitometrond/composition.cpp`) on a list that nothing fills.
- `nlohmann-json` and `boost-hash2` are manifest ports. ADR-0004 lets `sitometron_core` link them;
  ADR-0011 lets `sitometron_http` link `nlohmann-json`. `sitometrond_composition` may link neither,
  and it exists only in the HTTP-enabled build.

The owner decided on 2026-11-03, as recorded in Issue #125: in Phase 2 a bundle is the one
executable file the runner starts, its digest is declared in the registration and verified at
startup, and the registration is a schema-validated JSON file named by one startup option.

## Decision

We will register Applications in one JSON file that `sitometrond` reads, validates, and verifies at
startup, and we will take each launch's Application identity from that verified entry. This ADR is
the Application Registry ADR that ADR-0008 Section 4 names, limited to the fields Phase 2 needs.

### 1. The registry file

`sitometrond --applications PATH` names the registry file. The option is optional and appears at
most once; a repeated or empty `--applications` is a command-line error like any other (Issue #105),
not a startup refusal of Section 2. Without it the registered set is empty, as in Phase 1. The
registered set changes only the registration lookup: ADR-0008 Section 5 still decides route, method,
media type, size, JSON, and validation errors first, and ADR-0009 Section 9 still makes an empty or
longer `application_id` `validation_failed`. A create that passes those checks names an
`application_id` of 1 to 128 characters and answers `422` `unknown_application` when it is not
registered.

The file is one UTF-8 JSON object. The implementation Issue adds its JSON Schema as
`schemas/application-registry/v1/application-registry.schema.json`, which states the rules of the
table below and no others:

```json
{
  "schema": "application-registry.v1",
  "applications": [
    {
      "application_id": "demo-worker",
      "version": "1.0.0",
      "executable": "/opt/demo/bin/demo-worker",
      "arguments": ["--mode", "batch"],
      "bundle_sha256": "<64 lowercase hexadecimal characters>"
    }
  ]
}
```

| Field | Rule |
|---|---|
| `schema` | exactly `application-registry.v1` |
| `applications` | an array of 0 to 1,024 entries; an empty array is an empty registered set |
| `application_id` | the ADR-0009 Section 9 form; unique in the file |
| `version` | 1 to 128 characters, as the Journal event schema requires |
| `executable` | 1 to 4,096 characters; an absolute path on the host platform |
| `arguments` | an optional array of 0 to 64 strings, each 0 to 1,024 characters; default empty |
| `bundle_sha256` | 64 lowercase hexadecimal characters |

Every length limit counts Unicode code points, as JSON Schema `maxLength` does, not bytes. No other
member is allowed at either level, and no string in the file may contain U+0000, which JSON can
carry only as the escape `\u0000`. The JSON Schema states both of these rules and every rule of the
table except that `executable` is absolute and that `application_id` is unique; Sitometron-owned
code checks those two, using the host platform's rule for an absolute path. The file is at most 1
MiB, and that cap governs: the per-field limits are independent of it, so a file of many maximal
entries is refused by its size. The loader parses the file from its full byte range and refuses a
raw NUL anywhere in it. It also refuses the other two inputs that nlohmann-json accepts (ADR-0011):
a duplicate member name in any one object, found with a parser callback as ADR-0011 describes, and a
leading byte-order mark. The library itself refuses a raw NUL inside a string but accepts one after
the value, which the full-range rule covers.

### 2. The bundle and its verification

In Phase 2 an Application's bundle is its `executable` file alone. At startup, before any listener
opens, `sitometrond` reads each `executable`, following symbolic links, and computes the SHA-256 of
its bytes. Startup refuses with the Issue #105 startup-refusal status, and opens no listener, when
the registry file is missing, unreadable, larger than 1 MiB, not UTF-8 JSON, fails a parsing rule of
Section 1, does not match the schema, repeats an `application_id`, names an `executable` that is not
absolute or is not a readable regular file, or names one whose digest differs from its
`bundle_sha256`. A refusal reports the reason on standard error, and the entry when one entry is at
fault.

The verified identity of a launch is the entry's `application_id`, `version`, and `bundle_sha256`.
The supervisor (ADR-0012) copies those three values into `worker_launch_intent` from the launch
inputs that the composition hands it for the Job's `application_id` (Section 4). Whether the runner
checks the digest again at launch is the runner's decision (Gate #123, P2-3); this ADR verifies at
startup only.

### 3. Lifetime

The registered set is read once at startup and does not change while the daemon runs. Changing an
Application, its `executable`, or its digest takes a restart. A Job admitted before a restart keeps
the identity its `worker_launch_intent` recorded; replay never reads the registry file. A replayed
Job that is not terminal, with or without `worker_launch_intent`, is unresolved: `OPS-004` keeps
admission closed while it stays, the supervisor does not drive it because it drives only the Jobs
the running daemon admits (`JOB-009`), and resolving it is outside this slice (Gate #123, P2-9). An
Application removed from the file, or given a new digest, therefore never launches for a Job
created before the restart.

### 4. Placement and dependencies

The loader lives in `sitometron_registry`, a static library under `src/registry/` with
Sitometron-owned public headers under `include/sitometron/registry/`. Its public headers expose a
registered Application as a struct of standard strings and vectors, and a load result that is
either the verified set or a refusal reason. `sitometron_core` is not changed.

Only these manifest ports, direct CMake targets, and headers are authorized for
`sitometron_registry`, in the format of ADR-0010 Section 1:

| Manifest port | Pinned-baseline version | Direct CMake target | Authorized headers |
|---|---:|---|---|
| `nlohmann-json` | 3.12.0#2 | `nlohmann_json::nlohmann_json` | `<nlohmann/json.hpp>` for parsing the registry file and value access |
| `boost-hash2` | 1.91.0 | `Boost::hash2` | `<boost/hash2/sha2.hpp>` for SHA-256 over the `executable` bytes |

The list restricts direct includes; headers that a listed header includes are permitted as its
implementation. The target links both privately, adds no manifest port, and grants nothing to
`sitometron_core`, which keeps its ADR-0004 allowlist. Sitometron-owned code checks every schema
rule; no JSON Schema engine runs at startup, as ADR-0004 states for the core. The implementation
Issue adds the dependency-allowlist and public-API-isolation scanners for this target in the
pattern of the `sitometron_identity` scanners.

The target is built only when HTTP is enabled, like `sitometrond_composition`, its only consumer.
The composition links it, refuses startup as Section 2 states, and passes the verified identifiers
to the Phase 1 Backend in place of today's empty list. For a create of a registered identifier, the
composition hands the supervisor the entry's `application_id`, `version`, and `bundle_sha256` as the
launch inputs of ADR-0012 Section 2, in the core's own types (`StableId`, a standard string, and
`Digest`, as `WorkerLaunchIntentPayload` holds them); no registry type enters `sitometron_core`. The
composition also constructs the runner with the verified set, and the runner resolves a launch
request's `application_id` to the entry's `executable` and `arguments` itself;
`ApplicationLaunchRequest` carries no executable path or command (ADR-0005), and this ADR does not
change it. How the runner uses them is the runner's design (Gate #123, P2-3). The HTTP-off
`sitometrond` reads no command-line option today and does not change.

### 5. What changes in the Accepted ADRs

- [ADR-0008](0008-define-the-external-rest-v1-job-surface.md) Section 4: the sentence "Until that
  ADR is Accepted, the registered set is a list of identifiers in the daemon's startup
  configuration" stops applying when this ADR is Accepted, by its own condition; no sentence of
  ADR-0008 is superseded. The registered set is the verified set of Section 2.
- [ADR-0002](0002-define-core-job-reducer-contract.md), Decision: "verified Application identity"
  receives its meaning (Section 2); the rule itself is unchanged.
- [ADR-0009](0009-settle-the-external-rest-v1-gaps-found-by-the-prototype.md) Section 9: unchanged
  and adopted as the `application_id` rule.
- The Journal event schema (`schemas/core/v1/job-journal-event.schema.json`) and the reducer:
  unchanged.

### 6. Not decided here

- What else the runner passes to the process (working directory, environment, the identities it
  receives) and whether it re-verifies the digest at launch (Gate #123, P2-3).
- Bundles of more than one file, signed bundles, and an Application qualification process.
- Reloading the registry without a restart.
- Admission policy beyond ADR-0008 and per-Application limits (#35).
- The External REST v1 I/O model. Owner decision A of 2026-10-24 (PR #112, Issue #106) keeps one
  I/O thread with synchronous completion waits and revisits it with Phase 2 registration. This ADR
  keeps that model, so from now on a committing create in production also reaches the wait. By
  owner decision of 2026-11-03, Issue #127 (the Worker protocol, Gate #123 P2-4) revisits it,
  because whether the Worker listener shares that thread is decided there.

## Requirements and checks

This ADR adds `APP-001` (MUST): register Applications only through the registry file of Section 1,
verify every entry's `executable` against its `bundle_sha256` at startup and refuse to start on any
invalid entry, keep the registered set empty when no file is named, and take each launch's
Application identity from the verified entry.

Its named checks, Planned until the implementation Issue activates them:

- `registry_schema_vectors`: the schema accepts the valid forms (no `arguments`, an empty
  `applications` array, an empty argument string, one entry at every minimum and every maximum
  length, an entry with exactly 64 `arguments`, exactly 1,024 entries with distinct identifiers, and
  a `version`, an `executable`, and an argument at their limits written in multibyte UTF-8
  characters) and rejects each near-miss, table-driven: a root or an entry that is not an object;
  `applications` or `arguments` that is not an array; a `schema`, `application_id`, `version`,
  `executable`, or `bundle_sha256` that is not a string; a wrong or missing `schema`; an unknown
  member at either level; a missing required member; an `application_id` that is empty, of 129
  characters, or outside the ADR-0009 form; a `version` that is empty or of 129 characters; an
  `executable` that is empty or of 4,097 characters; 65 `arguments`, an argument of 1,025
  characters, or an argument that is not a string; an escaped `\u0000` in `executable`, in an
  argument, and in each other string member; a `bundle_sha256` of 63 or 65 characters, with an
  uppercase letter, or with a non-hexadecimal character; 1,025 entries; and the multibyte values one
  character over their limits. The same vectors run against the JSON Schema and against the loader's
  own checks;
- `registry_startup_refuses_invalid`: a missing, unreadable, larger than 1 MiB, non-JSON,
  schema-invalid, duplicate-identifier, or relative-`executable` file, a file with a raw NUL after
  the value, a file with a duplicate member name in one object (at the root and in an entry), and a
  file with a leading byte-order mark each refuses startup with the startup-refusal status, opens no
  listener, and reports the reason on standard error, with the entry for an entry-level fault, while
  a valid file of exactly 1 MiB, padded with JSON whitespace, starts;
- `registry_bundle_digest_verified`: a matching digest is accepted, also through a symbolic link; a
  changed byte in the `executable`, a missing `executable`, a directory in its place, or a regular
  `executable` that cannot be opened for reading refuses startup with the startup-refusal status,
  opens no listener, and reports the entry and the reason on standard error. The unreadable case
  runs on POSIX with a file without read permission; Windows skips it, because a test fixture there
  has no simple portable way to remove read access from the test's own user;
- `registry_absent_keeps_empty_set`: without `--applications`, and with a valid file whose
  `applications` array is empty, startup succeeds and a create that passes the request checks is
  `422` `unknown_application`, while a malformed or invalid create keeps its earlier ADR-0008
  Section 5 answer;
- `rest_v1_sitometrond_options` (the existing Issue #105 check, extended): a repeated
  `--applications` and an empty `--applications` value are each a command-line error with the
  documented status `2`, and a single `--applications PATH` is accepted by the parser;
- `rest_v1_registered_application_creates`: with a verified file, a create for a registered
  identifier is `202` and a valid create for any other identifier of 1 to 128 characters is `422`
  `unknown_application`;
- `registry_launch_identity_from_entry`: the `application` object of a `worker_launch_intent` equals
  the verified entry's `application_id`, `version`, and `bundle_sha256` (activated by the supervisor
  implementation Issue, which first writes that event);
- `registry_dependency_allowlist` and `registry_public_api_isolation`: the scanners of Section 4,
  with planted violations.

## Consequences

- Good: the first production create can succeed, and "verified Application identity" means a digest
  checked against the `executable` bytes present at startup. That the same bytes run is guaranteed
  only if the runner re-verifies at launch (Section 6).
- Good: the registry is one schema-checked file, so later fields (working directory, limits) extend
  it without new command-line options.
- Bad: replacing an executable needs both a new digest in the file and a restart.
- Bad: the check is at startup only, so a file replaced while the daemon runs is not detected here;
  the runner may close that gap.
- Neutral: a new small adapter target with two scanners, in the existing pattern.

## Options considered

- **Compute the digest at startup without a declared value**: rejected on 2026-11-03 because nothing
  would be verified; the identity would only record whatever file was present.
- **Repeat a `--application` option per Application**: rejected on 2026-11-03 because options cannot
  carry a schema, quoting paths and arguments is fragile, and the Registry row promises a schema.
- **Parse the file in `sitometron_core` or the composition**: rejected because the core is the
  domain, not deployment configuration, and the composition may link no JSON library and exists
  only with HTTP.
- **Make the full Application Registry now**: rejected because Phase 2 needs only these fields;
  #35 keeps the rest.

## References

- Issue #125; Gate #123 (P2-1); Gate #101 (P1-4); Issue #35
- Requirements: `APP-001` (Planned)
- Contract Registry: `Application Registry schema`; `sitometrond` startup configuration and command
  line
- Related ADR: ADR-0002, ADR-0004, ADR-0008, ADR-0009, ADR-0011, ADR-0012, ADR-0013
