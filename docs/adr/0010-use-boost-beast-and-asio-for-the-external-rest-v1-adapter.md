# ADR-0010: Use Boost.Beast and Boost.Asio for the External REST v1 adapter

## Status

Proposed — 2026-10-08

## Context

Accepted ADR-0008 and ADR-0009 define External REST v1. ADR-0009 Section 1 makes the HTTP message
syntax RFC 9112's and names the refusals before routing: `400`, `408`, `414`, `431`, and `501`.
ADR-0009 Section 2 decides route, method, and media type before the body bound, and ADR-0009
Section 6 answers a method a route does not list with `405` and `Allow`. The production adapter
needs an HTTP library that lets it keep these rules.

ADR-0004 keeps HTTP in adapter targets, composes adapters only in `sitometrond`, supports Linux
with GCC and Windows with MSVC, and keeps dependency-owned types out of Sitometron-owned public
types. Its allowlist covers `sitometron_core` only. `docs/02_architecture.md` names
`sitometron_http` as a later adapter target, and `docs/09_dependency_policy.md` lets adapter
targets own their third-party dependencies. As ADR-0006 Section 9 requires for a logger, an adapter's
third-party dependency still needs its own dependency decision; this ADR is that decision for
`sitometron_http`.

A probe outside the repository built a loopback listener on Boost.Beast 1.91.0 and Boost.Asio
1.91.0 from the pinned baseline `40f3c709db80acf154ac4b17a1f83c564ebd022e` (Issue #90). It
sent 51 raw requests covering every refusal of ADR-0009 Section 1, the order of Section 2, and the
`405` of Section 6. All 51 statuses and error codes matched. The probe ran on Linux only. The probe
and the Beast 1.91.0 source showed:

- `beast::tcp_stream` closes the socket when its timer expires, so it cannot send a `408`. A read
  on a plain Asio socket cancelled with `cancellation_type::total` leaves the socket usable; Asio
  documents that cancellation type for socket reads and writes on POSIX and Windows.
- Beast refuses every version other than `HTTP/1.0` and `HTTP/1.1` with a parse error, including
  `HTTP/1.2`. RFC 9110 Section 2.5 expects a higher minor version to be processed as the highest
  minor version the server implements.
- Beast checks the version as soon as its eight characters arrive, before the request line ends.
- Beast refuses an empty line received before the request line. RFC 9112 Section 2.2 says a server
  should ignore at least one.
- Beast's header limit counts the request line and the header fields together.
- Beast parses an unregistered method token as `verb::unknown` and keeps its text.
- Beast replaces obsolete line folding with a space, which RFC 9112 Section 5.2 permits.
- When the last transfer coding is not `chunked`, Beast treats the request as having no body.
  RFC 9112 Section 6.3 requires `400` in that case.
- Beast accepts `chunked` repeated inside one `Transfer-Encoding` field, and a `Content-Length`
  after a transfer coding list whose last coding is not `chunked`.

The owner decided on 2026-10-07 and 2026-10-08 under Issue #90:

- the library is Boost.Beast with Boost.Asio; and
- a request whose version is `HTTP/1.x` with x greater than 1 is processed as `HTTP/1.1` by
  rewriting its minor version before the parser sees it.

## Decision

We will build `sitometron_http` on Boost.Beast for HTTP/1.1 parsing and serialization and on
Boost.Asio for sockets, timers, and C++20 coroutines.

### 1. Direct dependency allowlist of `sitometron_http`

Only these manifest ports, direct CMake targets, and headers are authorized for `sitometron_http`:

| Manifest port | Pinned-baseline version | Direct CMake target | Authorized headers |
|---|---:|---|---|
| `boost-beast` | 1.91.0 | `Boost::beast` | `<boost/beast/core/error.hpp>`, `<boost/beast/http/message.hpp>`, `<boost/beast/http/parser.hpp>`, `<boost/beast/http/string_body.hpp>`, `<boost/beast/http/write.hpp>`, `<boost/beast/http/error.hpp>`, `<boost/beast/http/field.hpp>`, `<boost/beast/http/verb.hpp>`, and `<boost/beast/http/status.hpp>` |
| `boost-asio` | 1.91.0 | `Boost::asio` | `<boost/asio/io_context.hpp>`, `<boost/asio/ip/tcp.hpp>`, `<boost/asio/buffer.hpp>`, `<boost/asio/error.hpp>`, `<boost/asio/cancellation_type.hpp>`, `<boost/asio/write.hpp>`, `<boost/asio/awaitable.hpp>`, `<boost/asio/co_spawn.hpp>`, `<boost/asio/detached.hpp>`, `<boost/asio/use_awaitable.hpp>`, `<boost/asio/as_tuple.hpp>`, and `<boost/asio/cancel_after.hpp>` |

The list restricts direct includes. Headers of these two ports that a listed header includes are
permitted as its implementation.

The `boost-asio` features `ssl` and `spawn` are not used. `boost-asio` enables `spawn` and
`deadline-timer` by default, and `boost-beast` depends on it with its default features, so
`boost-context` and `boost-date-time` are built. Through the exported `Boost::beast` and
`Boost::asio` targets, the compiled libraries of `boost-context`, `boost-date-time`, and
`boost-container` are on the link line of `sitometron_http`. Nothing from them is used: the adapter
does not include `<boost/asio/spawn.hpp>` or the other headers that need them.

- On Linux (`x64-linux`, static libraries), no object of them is linked into the program. The
  probe's binary, built through these targets, contains no symbol of any of the three.
- On Windows (`x64-windows`, DLLs), their import libraries are on the link line, and a DLL becomes a
  load-time dependency only if a symbol from it is used. The integration Issue confirms that the
  program imports none of the three.

WebSocket, TLS, and `beast::tcp_stream` are not authorized.

This allowlist covers the HTTP dependencies only. The JSON parsing of request bodies (ADR-0009
Section 8) is not decided here.

The rules of ADR-0004 that protect the core also apply to this target:

- Public headers of `sitometron_http` expose only Sitometron-owned types. No Beast or Asio type
  appears in them.
- The `NFR-005` checks keep rejecting every Beast or Asio include and target in `sitometron_core`.
- The baseline-resolved transitive ports are opaque prerequisites. `sitometron_http` source does
  not include or link them directly.

### 2. Which checks Beast makes and which the adapter makes

The adapter owns the bytes it reads. It passes them to `http::request_parser::put()` with eager
parsing off, so that parsing stops after the header section. Before route, method, and media type
are decided, the parser has no body limit.

Beast decides these outcomes of ADR-0009 Section 1 and Section 2:

- `431` `headers_too_large`, from its header limit;
- `413` `payload_too_large` for a chunked body over the body bound, from its body limit; and
- `400` `malformed_request` for a malformed method, request-target, version, field, or chunk, and
  for a malformed or conflicting `Content-Length`.

Beast also refuses some of the transfer coding cases below, but not all of them, so the adapter
checks every one of them itself.

The adapter decides every other outcome:

- `408` `request_timeout` for both time bounds of ADR-0009 Sections 1 and 2;
- `414` `target_too_long`;
- `400` `malformed_request` for a missing `Host` in `HTTP/1.1`, more than one `Host`, a request-target
  of no valid form, a transfer coding list (over every `Transfer-Encoding` field) whose last coding
  is not `chunked` or that names `chunked` more than once, `Transfer-Encoding` together with
  `Content-Length`, and any transfer coding in `HTTP/1.0` (RFC 9112 Sections 3.2, 6.1, and 6.3);
- `501` `not_implemented` for a transfer coding other than `chunked` before a final `chunked`;
- the request-target forms, including absolute-form, asterisk-form, and authority-form (ADR-0009
  Section 1);
- the query, route, method, and media type checks, with `Allow` on `405` (ADR-0008 Section 5 and
  ADR-0009 Sections 4 and 6); and
- `413` `payload_too_large` from `Content-Length`.

### 3. Bounds

The header bound of ADR-0009 Section 1 is measured over the request line and the header fields
together. A request that exceeds it before its header section is complete is `431`, even when its
target alone is shorter than the header bound. The target bound applies to a request whose header
section is complete, so a `414` is possible only for a target shorter than the header bound.

### 4. Time bounds

Each read is cancelled when the remaining time of its bound runs out, with per-operation
cancellation that leaves the socket open. The adapter then writes the `408`. The adapter does not
use `beast::tcp_stream` timeouts, because they close the socket.

### 5. The request line before parsing

The adapter skips empty lines received before the request line (RFC 9112 Section 2.2). Then it
passes no byte to the parser until the request line has ended at its first LF. When it has, the
adapter rewrites a version of the form `HTTP/1.x` with a digit x greater than 1 to `HTTP/1.1`, and passes
the bytes on. The response carries `HTTP/1.1`, as for every request. Every other version reaches
Beast unchanged: `HTTP/1.0` and `HTTP/1.1` are accepted as ADR-0009 Section 1 requires, and the
rest are `400`. While the adapter holds the request line, the header bound (`431`) and the time
bound (`408`) still apply. Named tests send such a request split after its version and after the CR
that ends its request line, and after a leading empty line.

### 6. `Expect: 100-continue`

Beast parses and serializes messages but writes nothing on its own, so the adapter answers
`Expect: 100-continue`. It sends `100 Continue` only after route, method, media type, and, when
`Content-Length` is present, the body bound have passed. If any of them fails, the final status is
sent instead and the body is not read. A chunked body is then bounded by the parser's body limit.

### 7. Closure and license

On `x64-linux`, the two ports add 47 ports to the 16 that the manifest resolves today, according to
a vcpkg dry run at the pinned baseline. A dry run for `x64-windows` cannot resolve on a Linux host.
In the port manifests of the closure, the only dependencies with a platform qualifier are
`boost-asio[spawn]` on `boost-context` and `boost-asio[ssl]` on `openssl`. Neither differs for
Windows desktop, and `ssl` is not enabled. The Windows closure is confirmed by the integration Issue,
which changes the manifest with Linux and Windows evidence as `docs/09_dependency_policy.md`
requires.

Every Boost library port in the closure uses the Boost Software License 1.0, which is compatible
with the project's Apache-2.0 license. The build helper port `boost-uninstall` and the vcpkg helper
ports use the MIT license and are build-time only.
Distribution keeps the Boost terms for the complete resolved closure, as ADR-0004 already requires.
The integration Issue records the closure in `docs/dependency_closure.md`.

## Consequences

- Good: Beast's own parser and tests cover the RFC 9112 framing and field rules. The adapter tests
  only the checks it adds.
- Good: every refusal of ADR-0009 can be sent by the adapter, and the order of ADR-0009 Section 2
  holds, as the probe showed for 51 requests.
- Good: Boost is already a reviewed dependency family, and Asio gives one socket and timer layer for
  Linux and Windows.
- Bad: the adapter writes its own connection handling and the checks listed in Section 2 of this
  ADR.
- Bad: the build gains 47 ports, and `boost-context` is compiled and on the link line although no
  object of it is used.
- Bad: the adapter holds the request line before parsing, only to rewrite a version no known client
  sends.
- Neutral: the bounds' values, connection handling, and the adapter's own tests belong to the
  production adapter Issue.
- Neutral: the manifest, CMake, CI, and closure documentation change in a separate integration
  Issue.
- Neutral: `sitometron_http` still needs a dependency decision for its JSON parser before the
  production adapter Issue starts.

## Options considered

- **cpp-httplib 0.50.1**: rejected because it never sends `408`, `431`, or `501`, and it answers
  `400` to a method outside its fixed set where ADR-0009 Section 6 requires `405`. Meeting ADR-0009
  would need changes to the library.
- **cpp-httplib with a superseding ADR that drops those refusals**: rejected because it reverses
  ADR-0009 Section 1 for the convenience of one library.
- **Keep the hand-written listener of the walking skeleton**: rejected because the project would
  own RFC 9112 framing and field syntax and their tests. ADR-0009 Section 1 defers that syntax to RFC
  9112 so that an HTTP library, not Sitometron code, implements it.
- **civetweb, Drogon, Crow, or libmicrohttpd**: rejected because they are broader than one loopback
  listener needs, libmicrohttpd uses the LGPL, and their built-in error responses are harder to keep
  from reaching the client.
- **`beast::tcp_stream` for time bounds**: rejected because it closes the socket when the time runs
  out, so no `408` can be sent.
- **Answer `HTTP/1.2` and later minor versions with `400`**: rejected because RFC 9110 Section 2.5
  expects them to be processed as `HTTP/1.1`, and ADR-0009 Section 1 does not narrow RFC 9112.
- **Use Beast on a POSIX socket loop without Asio**: rejected because the daemon must also build on
  Windows, and Asio already belongs to Beast's closure.

## References

- Issue #90
- Requirements: relies on `API-007` and `API-001` to `API-006`; `NFR-005` for the core boundary
- Contract Registry: `HTTP adapter dependency boundary and allowlist`, `External REST v1`
- Related ADR: ADR-0004, ADR-0006, ADR-0008, ADR-0009
- RFC 9110 and RFC 9112
- Pinned vcpkg builtin baseline: `40f3c709db80acf154ac4b17a1f83c564ebd022e`
