# ADR-0011: Use nlohmann-json for the External REST v1 adapter

## Status

Accepted — 2026-10-10

## Context

Accepted ADR-0010 authorizes Boost.Beast and Boost.Asio as the HTTP dependencies of the adapter
target `sitometron_http`. Its Section 1 covers the HTTP dependencies only and leaves the JSON
parsing of request bodies to a separate dependency decision. This ADR is that decision.

Accepted ADR-0009 Section 8 makes a body `malformed_json` when it is not exactly one JSON text in
UTF-8, whatever the JSON library would accept. It names four cases, each with its own test: a raw
NUL byte anywhere, a byte sequence that is not valid UTF-8, data after the value other than
whitespace, and a duplicate key in any object at any depth.

`nlohmann-json` 3.12.0#2 is already in the manifest at the pinned baseline
`40f3c709db80acf154ac4b17a1f83c564ebd022e`. ADR-0004 authorizes it for `sitometron_core`, and the
Journal adapter uses it. ADR-0004 also fixes how the core parses with it: from an explicit byte
range, with a raw NUL anywhere refused.

A probe outside the repository parsed 21 bodies with this version from an explicit iterator range
(Issue #94). It ran on Linux only. The library:

- refuses invalid UTF-8 (a `0xff` byte, an overlong form, encoded surrogates, and a truncated
  sequence), non-ASCII bytes outside a string, data after the value, a raw NUL inside a string, an
  empty body, and an escaped lone surrogate such as `"\ud800"`; and
- accepts a raw NUL after the value, which it takes as the end of input even when data follows,
  a duplicate key, which it resolves silently to the last value, and a leading byte-order mark,
  which it skips.

A parser callback that keeps one key set per open object found duplicate keys at any depth, and
reported none for the same key in sibling objects or at two depths. The core already uses a parser
callback to count the keys of Worker event payloads, which is how duplicates of those keys are
detected.

The owner decided on 2026-10-10 under Issue #94:

- a byte-order mark before the JSON text is `malformed_json`, because ADR-0009 Section 8 accepts
  exactly one JSON text and a byte-order mark is not part of one, although RFC 8259 Section 8.1
  lets a parser ignore it; and
- an escaped lone surrogate is `malformed_json`, as the library refuses it. RFC 8259 Section 8.2
  says the behavior of software that receives such a string is unpredictable.

## Decision

We will use nlohmann-json in `sitometron_http` to parse request bodies and to serialize response
bodies.

### 1. Direct dependency of `sitometron_http`

ADR-0010 Section 1 leaves the JSON dependency of `sitometron_http` to a separate decision. This row
is that decision, in the same format:

| Manifest port | Pinned-baseline version | Direct CMake target | Authorized headers |
|---|---:|---|---|
| `nlohmann-json` | 3.12.0#2 | `nlohmann_json::nlohmann_json` | `<nlohmann/json.hpp>` for parsing request bodies, value access, and serializing response bodies |

The rules of ADR-0010 Section 1 apply to it: public headers of `sitometron_http` expose no
nlohmann-json type, and the row grants nothing to `sitometron_core`, which keeps its own ADR-0004
allowlist. The manifest and the resolved closure do not change, because the core already resolves
this port. nlohmann-json uses the MIT License; distribution already keeps its notice under
ADR-0004.

### 2. The rule for `malformed_json`

The library decides JSON syntax and UTF-8. The adapter refuses, as `malformed_json`, every case of
ADR-0009 Section 8 that the library accepts. This ADR states the rule, not a complete list. The
cases known now are inputs to the production adapter Issue, which has a named test for each of
them. Two of them are among the four named tests of ADR-0009 Section 8; the byte-order mark adds
one more:

- a raw NUL anywhere in the body, checked over the whole body before parsing;
- a duplicate key at any depth, detected while parsing; and
- a byte-order mark before the JSON text, as the owner decided.

### 3. Known narrowing

An escaped lone surrogate is `malformed_json`, as the owner decided. A JSON text that RFC 8259
allows is refused here. RFC 8259 Section 8.2 leaves the result of such a string unpredictable, and
no request field of v1 needs one.

### 4. Parsing form

The adapter parses a body from an explicit byte range, never from a NUL-terminated pointer, as
ADR-0004 requires of the core. It maps every library parse error to `malformed_json`. No library
exception leaves the adapter.

## Consequences

- Good: the adapter uses the JSON library the project already resolves, reviews, and distributes;
  the manifest and the closure do not change.
- Good: the cases where the library is more lenient than ADR-0009 Section 8 are named, and the
  production adapter Issue proves the complete set with tests.
- Bad: the adapter adds a NUL scan, a byte-order mark check, and duplicate-key detection on top of
  the library.
- Bad: an escaped lone surrogate, which RFC 8259 allows, is refused.
- Neutral: the integration Issue links `nlohmann_json::nlohmann_json` to `sitometron_http` together
  with the ADR-0010 dependencies.

## Options considered

- **Accept a leading byte-order mark, as the library does**: rejected because ADR-0009 Section 8
  accepts exactly one JSON text, and the Journal codec already treats a byte-order mark as
  non-canonical.
- **Accept an escaped lone surrogate and leave it to validation**: rejected because the library
  cannot represent it, so the adapter would need its own string decoding for a value no v1 field
  uses.
- **Another JSON library, such as RapidJSON or simdjson**: rejected because it adds a second JSON
  implementation and license to the closure, while the leniencies of nlohmann-json are few, known,
  and covered by the adapter.
- **List every refusal in this ADR**: rejected for the reason ADR-0010 Section 2 gives; the
  complete set is proven by tests in the production adapter Issue.

## References

- Issue #94
- Requirements: relies on `API-007`
- Contract Registry: `HTTP adapter dependency boundary and allowlist`
- Related ADR: ADR-0004, ADR-0009, ADR-0010
- RFC 8259
- Pinned vcpkg builtin baseline: `40f3c709db80acf154ac4b17a1f83c564ebd022e`
