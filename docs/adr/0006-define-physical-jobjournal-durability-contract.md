# ADR-0006: Define the physical JobJournal durability contract

## Status

Proposed — 2026-09-26

## Context

Accepted ADR-0002 defines the logical JobJournal: the six-field envelope, the closed event kinds,
the global sequence as the sole durable ordering authority, and the rule that an accepted event is
appended and disk-synced before its state update, response, acknowledgment, or post-sync effect. It
assigns NDJSON encoding details, append/flush/disk-sync, Quill integration, filesystem behavior,
fault injection, and the production Journal adapter to Phase 0B.

Accepted ADR-0003 makes a Journal commit failure writer-local and sticky. The process allocates no
later normal sequence, applies nothing, and requires offline recovery and restart. Accepted ADR-0005
defines `JobJournalPort::Commit()` as one synchronous, non-throwing call per complete immutable
event that returns exactly `kCommitted`, `kDefiniteFailure`, or `kOutcomeUnknown`.

Phase 0A proved these rules against a logical fake only. The non-normative walking skeleton
(Issue #48) wrote a file-backed Journal end to end on Linux and recorded findings in
`spike/README.md`. Four of them shape this decision: no canonical C++ serializer exists (finding 6),
replay through the pure reducer is feasible but unimplemented (finding 7), one data sync per record
cost about 4–6 ms on WSL/ext4 (finding 8), and records of concurrent Jobs interleave under the global
sequence (finding 11).

Issue #51 owns this decision under Phase 0B Gate #50. The Gate's owner-accepted horizontal review
assigns findings H1–H6, H8, and the Journal-pruning part of H7 to this ADR.

This ADR must not change the logical envelope, the event kinds, the JSON Schemas, or the
`JobJournalPort` semantics.

## Decision

### 1. Record encoding

We will store the Journal as NDJSON. One physical record is one logical event, encoded as one JSON
object followed by exactly one LF byte (`0x0A`).

- The object is exactly the ADR-0002 envelope and validates against
  [`job-journal-event.schema.json`](../../schemas/core/v1/job-journal-event.schema.json) and the
  payload schema for its `event_type`. v1 adds no physical wrapper, checksum, or extra field.
- Encoding is UTF-8 without a byte-order mark. Envelope members appear in ADR-0002 order:
  `schema_version`, `sequence`, `event_type`, `recorded_at`, `job_id`, `payload`. Payload members
  appear in the order their schema declares them. The encoder emits no insignificant whitespace and
  escapes only what RFC 8259 requires, using lowercase `\u` hex escapes for control characters.
- `sequence` is a JSON integer in the full unsigned 64-bit range. Decoders must reject a record whose
  integers cannot be represented exactly; they must not round through binary floating point.
- The resolved allocation text inside `resources_committed` is carried as a JSON string. Decoding
  must return the exact original bytes, so the ADR-0002 SHA-256 digest still matches.
- A record, including its LF, is at most 1,048,576 bytes. A longer encoding is rejected before any
  byte is written.

There is one canonical serializer and one inverse parser, owned by the production Journal adapter
target outside `sitometron_core`. The parser accepts only what the serializer can emit. Round trip
is exact in both directions: every schema-valid event encodes to one byte sequence, and that byte
sequence decodes to an equal event.

### 2. File layout

- The Journal lives in one directory supplied by daemon configuration. The daemon has no default
  Journal path.
- The directory holds append-only segment files named `journal-<S>.ndjson`, where `<S>` is the first
  record's sequence as 20 zero-padded decimal digits. Segments sort lexically in sequence order.
- Exactly one segment, the one with the highest name, is active. The adapter rotates at a
  record boundary when the active segment would exceed its configured size limit. The default limit
  is 67,108,864 bytes. Earlier segments are sealed and never modified by the daemon.
- The daemon holds an exclusive advisory lock on `journal.lock` in the directory for its whole
  lifetime. It refuses to start when the lock is held.
- Access is owner-only. On POSIX the directory is created `0700` and files `0600`. On Windows the
  adapter uses an owner-only access control list; Phase 6 qualifies it.

### 3. Ordering and durability

For each `Commit()` call, the adapter performs these steps on the calling writer thread:

1. encode the complete record;
2. write every byte of the record, including its LF, to the active segment opened for append, with
   no user-space buffering carried across calls;
3. complete a data sync of that file: `fdatasync` on POSIX, `FlushFileBuffers` on Windows;
4. return `kCommitted`.

Creating a segment is durable before its first record is committed. The adapter syncs the new file
and then the directory that holds its entry:

- on POSIX, `fsync` on the directory opened read-only;
- on Windows, `FlushFileBuffers` on the directory opened with `FILE_FLAG_BACKUP_SEMANTICS`. The
  supported Journal file system on Windows is NTFS, and Phase 6 qualifies the behavior.

If either sync fails, or the platform or file system cannot perform it, the adapter writes no record
to that segment and the pending `Commit()` returns `kDefiniteFailure`. A crash after the directory
sync and before the first record leaves an empty highest segment, which startup accepts under
Section 5.

A record is committed if and only if its complete bytes and LF are durable. A complete record that
reached disk before a crash is committed even when its `Commit()` never returned. That is the case
`kOutcomeUnknown` already describes.

Group commit is not part of v1. `Commit()` is synchronous for one event, and batching events would
require changing ADR-0005. The cost of one data sync per event is accepted for v0.1; a later ADR that
amends ADR-0005 may introduce batching.

### 4. Commit result classification

- `kDefiniteFailure` covers every failure before the first byte of the record is written. This
  includes encoding failure, the size bound, a missing or unwritable active segment, and a write that
  fails with no bytes written, including out-of-space.
- `kOutcomeUnknown` covers every failure after the first byte is written. This includes a short write
  that cannot finish, any write error after partial output, and any data-sync failure.
- The adapter retries interrupted system calls and continues after partial writes until the record is
  complete or an error occurs. It never retries a failed data sync and never reports success after
  one.
- After the first non-`kCommitted` result, the adapter is poisoned. Every later `Commit()` returns
  `kDefiniteFailure` without I/O. ADR-0003 already forbids the writer from calling it again.
- Every exception and internal error is caught inside the adapter and classified by the rules above,
  as ADR-0005 requires.

### 5. Startup validation

Before the writer starts, the daemon reads every segment in order and validates the Journal. It
never modifies the Journal on its own.

- **Torn tail**: the last segment ends in bytes after the final LF. Those bytes were never
  committed. The daemon refuses to start and reports `journal_torn_tail` with the segment name and
  byte offset.
- **Corruption**: a record that is not valid JSON, fails schema validation, breaks sequence
  continuity, or is rejected during replay. Continuity means the first retained record's sequence
  equals its segment name, and each later record is exactly one greater than the previous one across
  segment boundaries. The daemon refuses to start and reports `journal_corrupt` with the location.
- **Capacity**: replay would create more resident Jobs than the configured maximum. The daemon
  refuses to start and reports `journal_capacity_exceeded`.
- **Sequence exhaustion**: the last replayed sequence is the maximum unsigned 64-bit value, so no
  next sequence exists. Following ADR-0003's exhaustion rule, the daemon refuses to start and reports
  `journal_sequence_exhausted`.

An empty segment is valid only as the highest segment, and only when its name equals the next
sequence: the last replayed sequence plus one, or one for a Journal with no records. It is the
expected result of a crash after a rotation made the segment durable and before its first record
committed. The daemon keeps it as the active segment and appends the next record to it. An empty
segment in any other position, or one whose name differs from the next sequence, is corruption.

An offline operator tool may remove a torn tail. It moves the removed bytes into a quarantine file
next to the segment and never touches a complete record. No tool repairs corruption in v1.

### 6. Replay and recovery

Replay rebuilds writer state by folding each record through the pure reducer.

- For each record, replay reconstructs the reducer input that produced it and requires the reducer to
  return an accepting disposition and an identical logical event. Any difference is corruption.
- Replay applies the declared updates. It dispatches no post-sync effect, arms no timer, sends no
  acknowledgment, and releases no response.
- After replay, the next sequence is the last replayed sequence plus one. An empty Journal starts at
  one.

ADR-0003 forbids reusing a sequence after a failed or unknown commit. That rule governs one writer
lifetime, and this ADR does not change it. Across a restart, the durable Journal is the only
authority for sequence identity. A sequence with no complete durable record was never committed:
ADR-0003 releases no response, acknowledgment, state change, or effect for it, so no party outside
the failed process has observed it. The next process may therefore allocate that number to a new
event. Bytes removed as a torn tail stay in the quarantine file as evidence and are never replayed.

A Job is unresolved after replay when it is non-terminal, its resources are not released, or its
cleanup status is not recorded. ADR-0002 states that the v0.1 snapshot is not a replayable launch
checkpoint, so the daemon cannot resume or conclude such a Job alone. When any Job is unresolved,
the daemon finishes startup with readiness false and admission closed, reports each unresolved Job,
and appends nothing.

The events and procedures that resolve Jobs left over from a previous process are deferred to the
Phase 2 design authority, which owns process containment. Issue #35 tracks that assignment until the
Phase 2 Gate exists.

### 7. Sequence

The physical record carries only the global sequence. Per-Job order is the order of that Job's
records under the global sequence. v1 adds no per-Job sequence.

### 8. Pruning and retention

- The daemon never deletes or rewrites Journal data. The default retention is everything.
- An offline operator operation may archive or delete a prefix of sealed segments while holding the
  Journal lock. A prefix is prunable only when every Job with a record in it has a terminal outcome,
  released resources, and a recorded cleanup status within the prefix, and has no record in any
  retained segment.
- After pruning, replay starts at the first retained segment and continuity is checked from its first
  sequence. Sequences are never reset or reused.
- A later event for a pruned Job reaches the reducer as an event for an absent Job and is decided by
  the existing ADR-0002 rules.

Online pruning and in-memory resident retirement are not decided here. The Gate #50 owner decision
on finding H7 owns them.

### 9. Logger boundary

The Journal is not a log. The Journal adapter and the diagnostic logger share no queue, thread,
file, or buffer. A logger failure never changes a commit result, and the Journal never waits for the
logger. The logger may record diagnostics about committed events after commit.

Adopting Quill or any other logger requires a dependency decision under ADR-0004. Before that
decision, a logger durability spike must show:

- bounded memory under a burst larger than its queue;
- behavior when the disk is full, with dropped messages counted and no unbounded blocking;
- what reaches disk after a crash signal;
- Linux GCC and Windows MSVC builds with the pinned toolchain; and
- a license compatible with Apache-2.0.

Logger output is not a stable contract in v0.1.

### 10. Thread placement

`Commit()` performs its I/O on the calling writer thread and returns only after the outcome is known.
The adapter owns no background commit thread. ADR-0003 bounds ingress by entry counts, not time, so
its FIFO and critical-reserve proofs are unchanged. The added latency lengthens each writer turn.

## Consequences

- Good: One serializer and parser pair gives a single byte-exact definition of a record, and replay
  can prove the reducer reproduces every committed record.
- Good: The commit result classes map directly onto observable I/O progress, so fault injection can
  target each class.
- Good: A crash can only leave a torn tail, which is detected, never silently accepted, and never
  mistaken for a committed record.
- Bad: One data sync per event limits throughput. At the measured 4–6 ms per sync, one 13-event Job
  spends about 60–80 ms in durability, and the writer commits roughly 150–250 events per second.
- Bad: A daemon that stops with unresolved Jobs cannot accept new work after restart until the
  Phase 2 authority defines resolution.
- Bad: v1 records carry no checksum, so a complete but bit-damaged record is detected only if it no
  longer parses or validates. Storage integrity below the filesystem is out of scope.
- Neutral: Segments add a directory and lock file to the operator's view of the Journal.

## Options considered

- **Length-prefixed binary framing**: rejected because NDJSON is readable with standard tools and the
  LF terminator is enough to detect a torn tail.
- **A physical wrapper with a per-record checksum**: rejected for v1 because it adds a second schema
  next to the logical envelope. A later ADR may add it with a schema version change.
- **Automatic torn-tail truncation at startup**: rejected because ADR-0002 requires offline recovery
  after a persistence failure, and silent repair hides the failure from the operator.
- **Group commit or a dedicated Journal thread**: rejected for v1 because `Commit()` is synchronous
  for one event, and changing that requires amending ADR-0005.
- **A per-Job sequence in the record**: rejected because the global sequence already orders each
  Job's records, and adding a field changes the logical schema.
- **Persisting failed sequences across restarts**: rejected because a failed sequence is never
  observed outside its process, and recording it needs a second durable write per commit with its
  own failure classification and startup reconciliation.
- **Failing unresolved Jobs automatically at startup**: rejected because the closed event set has no
  event for it and it would claim an outcome that was never observed.

## References

- Issue #51
- Gate #50
- Issue #48 and `spike/README.md` findings 6, 7, 8, and 11
- Requirements: `JOB-004`, `JRN-001`–`JRN-003`, new `JRN-004`–`JRN-007`, new `OPS-002`–`OPS-005`
- Contract Registry: `JobJournal envelope and event schemas`, new `Physical JobJournal record
  encoding and segment layout`, new `JobJournal startup validation, replay, and pruning`
- Related ADR: ADR-0002, ADR-0003, ADR-0004, ADR-0005
