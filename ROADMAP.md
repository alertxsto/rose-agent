# Public roadmap

This roadmap separates implemented behavior, recorded verification and future acceptance gates. It is not a promise of 1:1 Rational Rose parity. Current scope is native Petal 44/50 class modeling; unsupported imported content is preserved, not silently converted.

## Completed product capabilities

- **Native storage:** source-preserving parser and class-focused semantic workspace, explicit source codecs, typed diagnostics, native `.mdl` Save/Save As, controlled-unit resolution, permitted-root enforcement, read-only referenced-model closure, dependency-cycle handling and conflict/checksum protection.
- **Transactions:** inspect → propose → exact human review → atomic apply; complete created-object details, stable semantic identities, scoped presentation identities, stale/digest rejection, reject, undo/redo and separate save.
- **Desktop and CLI:** real native Qt desktop, model browser/property editing, supported manual element/relation/diagram actions, native class canvas, geometry/routes and shared JSON-lines CLI workspace. Manual editing and saving work offline.
- **Class authoring:** packages, classes, attributes, operations, parameters, binary associations, inheritance, class diagrams, native compartments/labels and connector appearances. New models default to Petal 44; Petal 50 is explicit and imported versions are preserved.
- **AI assistance:** OpenAI-compatible Base URL/key setup, provider model discovery and advertised context metadata; asynchronous OS-only credential storage; prompt/photo input, bounded photo-aware conversation history, fresh live workspace state each turn, inspect/propose-only model tools, exact review/apply/reject and cancellation.
- **Chat usability:** selectable plain-text bubbles, attachments, Enter-send/Shift+Enter-newline, editable drafts, separate activity, idle Clear chat, wrapped responses and follow-tail that respects manual history scrolling.
- **Native declaration/layout fixes:** deterministic supplier-before-consumer ordering for authored presentations, literal explicit endpoints, native-center geometry and generated border attachments with interior bends preserved.

### Recorded proof, not generalized certification

Linux evidence includes **15 passing suites**, real Kenari prompt/photo/imported-model inference, repeated historical-image context, exact approval, separate native save and independent reopen. Actual desktop/widget probes additionally cover rendering, wrapping, scrolling and Stop without unapproved mutation. Fixture provider responses are not counted as live inference.

Earlier exact Petal 44 exports opened in Rational Rose 2000e with class members and association/inheritance visible; that release rejected Petal 50. Newer live-AI outputs and one specifically repaired native model have Linux reopen/rendering evidence only. Fresh native verification was blocked by a read-only screenshot failure with the display reporting 0 bits per pixel. Internal parse/rendering success is not native certification.

## P0 — Publish and prove the first release

**Target:** `v0.1.0` prerelease, with reproducible hosted builds and complete release assets.

- Publish bundled Linux x86-64 `.deb`, `.rpm` and portable TGZ, modern Windows x64 installer/ZIP, and dedicated Windows 8.1 x86 installer/ZIP, with `SHA256SUMS.txt`, `release-manifest.json`, public source archives and third-party notices.
- Use Qt 6.8.3 for modern releases; build official Qt 5.15.18 x86 with MSVC 2019/v142 and Schannel for the Windows 8.1 target. A Qt-major selection must preserve real AI, history, review and native-save behavior, not disable features for legacy compilation.
- Record actual clean installation/portable execution, model create/import/manual edit, real provider discovery/photo/history, review/reject/apply, separate save/reopen and Credential Manager behavior on Windows 8.1 32-bit. Hosted Windows compilation alone does not meet this gate.
- Record Linux package install/uninstall and relocated portable-launch checks on the declared glibc 2.35+ baseline, including plugin/TLS/image-format deployment.

**Acceptance:** released assets and matching checksums exist; workflow logs identify exact toolchain/source inputs; the legacy guest matrix has real runtime evidence. Keep the first release marked prerelease while Windows 8.1 native proof is missing. Windows 8.1 is end-of-life; no claim of restored OS security support is made.

## P1 — Strengthen native interoperability evidence

- With a legally available, explicitly authorized native Rose environment, check newest photo/chat exports and the specifically repaired model through native open → inspect semantics/layout → save → reopen.
- Build a public edition/profile matrix separating Rose 2000e/Petal 44 from newer Petal 50 releases, with exact file checksums and observed results.
- Expand authored regressions for connector supplier ordering, member compartments, ownership, role identity and layout without redistributing models whose rights are unknown.

**Acceptance:** each supported interoperability claim names the actual Rose release/profile and exercised operation; newer exports are not certified merely because older exports worked. Unknown/unsupported constructs stay preserved or produce typed errors rather than guessed rewrites.

## P2 — Improve the supported class-model editing workflow

- Prioritize clearer diagnostics/review for imported controlled units, read-only dependencies and safe Save As relocation.
- Expand manual class/member/property and route interactions based on reproducible user cases, retaining the same review/undo/separate-save contract.
- Extend provider capability/context handling only with real provider metadata and regressions for missing metadata, cancellation, image/history limits and stale state.

**Acceptance:** every added editing path persists supported semantics/layout through independent reopen; rejected/cancelled/stale proposals cannot mutate model or disk; credentials never migrate to plaintext configuration.

## P3 — Consider additional UML families only after native proof

Evaluate requested families one at a time, starting with concrete lawful source examples and target native editions. Sequence/state/activity or other authored diagrams are not current supported authoring promises.

**Acceptance before announcing a new family:** semantic mapping, identities/ownership/dependencies, inspect/reviewable mutations, undo/redo, source-preserving serialization, independent reopen, real desktop behavior and native Rose acceptance for a named release. Merely rendering a diagram or accepting parser syntax is insufficient.

## Boundaries that do not change

- AI/model output remains untrusted and cannot authorize approval, saving, shell execution or directory access.
- New/save/import remain native operations; JSON is an inspection/session boundary, not a replacement model format.
- No unsupported version is silently downgraded and no absent runtime proof is replaced with fixtures.
- Public source availability does not grant a project-wide open-source license; see [LICENSE](LICENSE).

See [README](README.md), [usage/protocol](docs/usage/native-cli.md), [compatibility evidence](docs/compatibility/m0-baseline.md) and [CHANGELOG](CHANGELOG.md).
