# Changelog

## 0.1.0 — initial prerelease

Release tag: `v0.1.0`. Publication/build results are available from [GitHub Releases](https://github.com/alertxsto/rose-agent/releases). The initial release remains prerelease until actual Windows 8.1 x86 native runtime evidence is recorded; a configured release target is not a successful guest run.

### Native editor and storage

- Integrated the native Qt `rose-agent` desktop and `rose-cli` around one authoritative workspace with manual editing, diagnostics and separate native Save/Save As.
- Added a C++20 source-preserving Petal parser with iterative nesting, document-scoped edit handles, explicit source codec handling, byte-span rewriting and rejection of malformed labels, unrepresentable edits and overlapping patches.
- Implemented class-focused Petal 44/50 semantics, atomic reviewed transactions, undo/redo, scoped presentation identities and source-preserving serialization. Unsafe/unsupported mutations return typed errors.
- Added controlled-unit resolution, canonical permitted roots, cycle/deduplication handling, referenced-model read-only closure, source-root-relative Save As and checksum/conflict protection. Relocation preserves source writability and unit structure rather than comparing basenames or flattening files.
- Made Petal 44 the new-model default, with explicit Petal 50 authoring. Imported model versions are preserved, not silently converted. Petal 44 omits the version-incompatible `SubSystem.category` backlink retained by the Petal 50 writer.
- Added native class compartments/labels, binary `AssociationViewNew`/`RoleView`, inheritance appearances and bend-preserving attachment updates. Supported diagram/neighborhood projections expose members/dependents; newly created and nonempty imported class diagrams open in the desktop.
- Fixed authored diagram declaration dependency/numeric ordering and insertion before connector consumers. Explicit imported/edited route endpoints remain literal; generated defaults attach to symbol borders. Geometry coordinates are native centers.
- Fixed copy ownership persistence, per-copy patch isolation, bound typed-reference refresh, opaque Role deletion protection, inheritance endpoint coherence and controlled-root deletion guards.
- Corrected signed x86 narrowing of Win32 `WriteFile` counts so nonempty native staging/journal/rollback writes do not request 4 GiB. Added exact-byte boundary coverage and genuine Windows directory-symlink fixtures without weakening pinned paths, conflict guards or durability.

### AI and review

- Added asynchronous OpenAI-compatible inference with a bundled trusted software-engineering skill, prompt and decoded PNG/JPEG/WebP image input, and inspect/propose-only tools. Provider/model output remains untrusted.
- Simplified setup to Base URL + API key, live model discovery and Use model. Kenari is prefilled but optional. Provider-advertised context metadata is persisted; missing metadata stays unknown without manual token estimates.
- Kept keys asynchronous, OS-only and scoped to the Base URL, using Secret Service on Linux and Credential Manager on Windows. Cancelled lookups dismiss unlock prompts, close sessions and reject stale results.
- Bound Apply/Reject to the displayed run/proposal/revision/digest and exposed full created-object review details: names, ownership, typed properties, endpoints, memberships, geometry and routes. Approval commits only the exact in-memory transaction; saving stays separate.
- Added Agent chat with selectable plain-text bubbles, image attachment/removal, Enter-send/Shift+Enter-newline, Send/Stop, editable drafts, idle Clear chat and a separate activity log.
- Added bounded, workspace-generation-scoped user/assistant/image history. Each turn receives fresh workspace state; old tools, projections, secrets and system roles are not replayed. Rejected designs remain historical, not live facts. Overflow is explicit rather than hidden truncation.
- Fixed wrapped-message height-for-width behavior and follow-tail during resize/bursts while preserving deliberate history scrolling. Stop cancels unapproved work without model/disk mutation.
- Separated event `activity` from optional `assistantMessage.text`; removed obsolete top-level `text`. CLI input remains live during network I/O, EOF does not save, and failed stdout discards unprocessed requests.
- Corrected the chat inspect function schema's required object root, enabling actual provider tool calls without provider-specific fake responses.
- Registered the Qt5 request-ID signal alias for correct queued correlation. Windows metadata-only history uses protected current-user DACLs and atomic replacement instead of `_wchmod` on an exclusively opened temporary file; readers close before replacement. History persistence diagnostics retain the primary typed run failure rather than replacing it with a secondary configuration error.

### Release distribution

- Added explicit default Qt 6.5+ and legacy Qt 5.15 selection through `ROSE_QT_MAJOR=6/5`, preserving the same application semantics.
- Modern release configuration uses Qt 6.8.3. Linux packages target x86-64/glibc 2.35+ and bundle the runtime; Windows x64 enforces Windows 10 version 1809/build 17763 or newer.
- Dedicated Windows 8.1 32-bit configuration builds official Qt 5.15.18 source with MSVC 2019/v142 and Schannel TLS, without an obsolete OpenSSL 1.1 runtime dependency.
- Release workflow targets version tags, native `.deb`/`.rpm`/`.exe` packages, portable TGZ/ZIP, `SHA256SUMS.txt`, release metadata, public source archives and third-party runtime notices. Manual dispatch produces build artifacts unless publication is configured.
- Added public introduction, installation/build/usage/compatibility documentation and prioritized roadmap. The project licensing notice grants no project-wide redistribution or modification license; public availability is not an open-source license.

### Recorded verification and limits

- Recorded Linux verification: **15/15 suites**, real Kenari prompt/photo/imported-model inference, photo rejection followed by historical-image-aware refinement, exact Apply, separate native Save and independent CLI reopen. Actual widget checks cover full wrapped text, resize/burst following, preserved history scroll, resumed following and Stop.
- Verified the pinned Qt 6.8.3 SDK independently: 15/15 suites, actual bundled-runtime deployment relocated away from its original directory, native review/apply/separate-save/reopen, real canvas rendering, PNG/JPEG/WebP decoding and certificate-validated HTTPS. Borrowed byte comparisons remain nonallocating and unambiguous on Qt 5/6.
- Earlier exact Petal 44 exports opened in Rational Rose 2000e with classes/members/association/inheritance visible; that installation rejected Petal 50. This is evidence for those exact files and edition, not universal compatibility.
- A specifically repaired native model preserved semantics, IDs and explicit geometry while correcting supplier declaration order; Linux independent reopen/rendering passed. Fresh native verification of this repair and newer live-AI outputs remains unverified: a permitted read-only screenshot attempt failed with display metadata reporting 0 bits per pixel. No Windows input was used to invent proof.
- Windows release build success and actual Windows 8.1 guest execution are distinct gates. No actual Windows 8.1 app certification is claimed. See [compatibility evidence](docs/compatibility/m0-baseline.md) and [public roadmap](ROADMAP.md).
