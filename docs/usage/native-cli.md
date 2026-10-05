# Rose Agent desktop and native CLI

> **Recorded Linux verification:** all 15 registered suites pass. Real Kenari inference covers prompt/image proposals, repeated photo-aware chat, exact review/apply, separate native save and imported-model edit/Save As with independent reopen. Setup needs only Base URL/API key; model choices and advertised context metadata come from the provider. Earlier exact Petal 44 exports displayed in Rational Rose 2000e. Newer chat exports and a specifically repaired native model have Linux reopen/rendering evidence, not fresh native-Rose certification. Windows 8.1 x86 is a dedicated build target, not an already certified guest run.

`rose-cli` uses the same in-process Workspace as the desktop. It reads and writes native Petal models, not a replacement JSON file format. JSON is the inspection/session boundary. Build outputs are in `build/bin`; multi-configuration generators use the configuration subdirectory, and Windows appends `.exe`.

## Build and run

Requires C++20, CMake 3.24+, Qt 6.5+ Core/Concurrent/Network/Gui/Widgets/PrintSupport/Svg (default `ROSE_QT_MAJOR=6`) or explicitly selected Qt 5.15 (`ROSE_QT_MAJOR=5`). Tests require Qt Test; Linux credentials additionally require Qt DBus and a running Secret Service keyring. Use one matching compiler/Qt installation; separate build directories are required for different Qt majors. Installation assets and release deployment details are in the [README](../../README.md#install).

```sh
cmake -S . -B build -DROSE_QT_MAJOR=6 -DBUILD_TESTING=ON
cmake --build build --parallel
ctest --test-dir build --output-on-failure
build/bin/rose-agent
```

For a Qt 5.15 source build, use `-DROSE_QT_MAJOR=5` with a separate build directory and the matching `CMAKE_PREFIX_PATH` if Qt is not auto-discovered. The Windows 8.1 x86 release uses official Qt 5.15.18 source, MSVC 2019/v142 and Schannel TLS; choosing Qt 5 alone does not deploy the required compiler runtime/plugins. Modern releases bundle Qt 6.8.3. The selected build directory determines the `bin/` path in the examples.


## Desktop workflow

Launch `build/bin/rose-agent` in the graphical session, or the installed/portable launcher. The **Agent chat** dock is the primary AI workflow; the canvas, model browser and manual editor use the same Workspace.

1. **Provider…**: enter an OpenAI-compatible **Base URL** and **API key**, then **Connect / load models**. Kenari is prefilled as `https://kenari.id/v1`; other compatible providers are supported. Choose a discovered model and **Use model**. No manual endpoint suffix, credential reference, context window or technical budget is required. The dialog displays the provider's advertised context token count, or honestly reports “managed by provider; not advertised” when missing. Models explicitly reporting no tool calls, no chat endpoint or no image input are filtered; generic OpenAI model lists without capability metadata are accepted without inventing capabilities. Keys stay in the OS keyring, never QSettings or CLI JSON. Changing Base URL does not forward the old provider's key. A local loopback service can omit a key when it needs no authentication.
2. **New model…**: choose the destination, explicit allowed directories/encoding, and native Petal44 (default) or Petal50 profile. The verified Rose2000e installation accepts **Petal44** and rejects Petal50 as unsupported; select Petal50 only for a target release that supports it. New creates an unsaved in-memory model; it does not write a file. Alternatively, **Open .mdl…** imports an existing native model, including supported controlled units.
3. Type in the bottom composer, attach/remove an image, or use both. Image-only input is valid. Accepted image formats are decoded PNG, JPEG and WebP: at most four, 8 MiB each, 8192 pixels per side and 16 megapixels. Context/encoded transport limits can reject smaller inputs too; no hidden truncation occurs. Image paths in CLI sessions must be inside explicitly granted roots.
4. **Send** or press **Enter**; **Shift+Enter** inserts a newline. User messages and actual assistant responses appear in selectable, plain-text chat bubbles. **Stop** cancels unapproved work; a draft remains editable while the run is active. Technical progress is separate under **Activity…**. Follow-up turns retain the prior discussion and validated images, but inspect fresh live workspace state. The trusted software-engineering skill guides semantic members/relationships and native layout; tools remain inspect/propose-only, never approval, apply, save, shell or filesystem grants.
5. Open **Review details** on the proposal card. Inspect the complete diff, diagnostics and client-ID mappings, including names, owners, properties, endpoints, geometry and routes. The details table supports horizontal scrolling and full-value tooltips. **Apply** binds the displayed proposal ID, exact base revision and digest and commits only that transaction in memory. **Reject** discards it without changing live state. **Clear chat** resets idle conversation context, not the model or saved file.
6. Applying a new class diagram opens it in the canvas. **Save / Save As** is a separate user action and works offline. Undo/redo are also separate from saving. Existing-model edits follow the same import → inspect → proposal → explicit review → apply → save path. Dirty replacement/close requires the user's save/discard decision.

Credential reads/writes and model discovery do not block the GUI. Leave the API key blank to reuse the connected key for that Base URL. Closing cancels pending credential reads and catalog requests; closing after a key write was queued does not roll back that OS-store write. Linux uses Secret Service and Windows uses Credential Manager. Cancelling an agent run prevents unapproved work, but cannot undo an already approved atomic commit; use Undo after completion.

This is not a full Rational Rose replacement. Authored semantics/layout are class-focused; other imported families are preserved as native source, not advertised as fully editable. Native compatibility must be checked against the target Rose release. A successful internal reopen or HTTP fixture run is not proof of live AI inference or universal Rose compatibility.

### Manual existing-model workflow

No provider/key is required for offline editing. Use **File → Open**, explicitly select the source encoding, allowed directories and controlled-unit path mappings, then review diagnostics. Missing or read-only units do not become writable simply because they appear in the browser.

Select a class/member in the model browser. Use **Model → Rename** (F2), supported create/relation/ownership actions or editable typed properties; use **Diagram → Geometry / Edit route** for supported presentations. Manual actions stage a transaction and open **Review model transaction**, with the exact proposal/revision/digest, before/after values, diagnostics and generated IDs. **Apply transaction** changes only memory; **Reject transaction** does not mutate. Undo/redo remain independent of **Save / Save As**. Double-click a supported class diagram to inspect layout, then save explicitly and reopen the output. File export to PDF/SVG/PNG is a diagram image, not a replacement native model file.

Save As retains the root-relative controlled-unit layout and requires permitted, existing destination directories. External source changes, collisions, incompatible codecs and unsafe native mutations report errors rather than silently flattening, overwriting or converting the model.


## Inspect and roundtrip

```sh
build/bin/rose-cli inspect --file /models/example.mdl --allow-root /models
build/bin/rose-cli inspect --file /models/example.mdl --output /models/inspection.json --allow-root /models
build/bin/rose-cli roundtrip --file /models/example.mdl --allow-root /models
build/bin/rose-cli roundtrip --file /models/example.mdl --output /exports/example.mdl --allow-root /models --allow-root /exports
```

`inspect` emits a projection to stdout, or writes inspection JSON with `--output`. Inspection cannot overwrite its native input. Its output path is subject to the same canonical directory policy. `roundtrip` emits a save receipt to stdout. Without `--output`, it writes `example.roundtrip.mdl` beside `example.mdl` and refuses if that default destination already exists. It never overwrites the source by default. An explicit destination is an intentional Save As; existing targets and external changes are checked by Workspace storage.

Options:

- `--file` / `-f`: model to open; optional for a session.
- `--output` / `-o`: inspection JSON or roundtrip native destination; unavailable for sessions.
- `--allow-root directory`: repeatable allowed directory for models, dependencies and destinations. Canonical paths, including symlink targets, must stay within allowed roots. If omitted, the input's parent directory is allowed, or the current directory for a session without a file.
- `--path-variable NAME=directory`: repeatable controlled-unit path mapping. Duplicate names are rejected. A mapping does not bypass allowed roots.
- `--source-encoding codec`: explicit source encoding; default `ASCII`. Non-ASCII source is not guessed. Use the actual model's legacy encoding, for example `Windows-1252`, when supported by the format layer.
- `--read-only`: inspect without authorizing mutations or native saves.
- `--help`, `--version`: standard Qt command-line help/version.

Native file operations report typed errors, including unsupported format/encoding, unavailable units, access denial, unsafe rewrites, disk conflicts and storage failures. Unsupported constructs are preserved as source rather than silently normalized. A projection's diagnostics and unit resolution/read-only flags must be considered before editing.

## JSON-lines sessions

```sh
build/bin/rose-cli session --allow-root /models
build/bin/rose-cli session --file /models/example.mdl --allow-root /models
```

Each stdin line is exactly one JSON request. Each stdout line is either a correlated response or an asynchronous `agentEvent`; consumers must demultiplex them. Request shape is closed:

```json
{"id":"open-1","method":"open","params":{"file":"/models/example.mdl"}}
```

`id` is a nonempty string chosen by the caller and echoed in the response. Success has `ok:true` and a `result`; failure has `ok:false` and an `error`. If a workspace is open, the envelope also contains its current `revision` as a decimal string. Unparseable requests or invalid IDs receive `id:null`; malformed requests do not terminate the session. End-of-input exits; neither exit nor close implicitly saves.

| Method | Params | Success result |
| --- | --- | --- |
| `create` | `file`; optional `petalVersion` (`44` default or `50`) and string `encoding` (`ASCII` for authored models) | projection of a new, unsaved workspace |
| `open` | `file` | projection |
| `inspect` | query described below | projection |
| `propose` | decimal-string `baseRevision`, nonempty `commands` array | proposal |
| `apply` | exact proposal `id`, decimal-string `baseRevision`, lowercase SHA-256 `digest` | applied |
| `reject` | proposal `id` | rejected |
| `undo` / `redo` | `{}` | applied |
| `save` | `{}` or `destination` | saved |
| `close` | `{}` or boolean `discard` | closed |
| `access` | `directory` | explicit directory grant |

Open/create require no current workspace. Close refuses unsaved changes unless `discard:true` is explicit. Proposing stages a batch without changing live state. Applying requires the exact approval tuple returned for that proposal and checks that it is still current. Applying, rejecting, undoing and redoing never save.

### Human-approved rename

First open or inspect the actual model. Find the desired class in `elements`, taking its actual `id` and the returned `revision`. Substitute those observed values into the next request:

```json
{"id":"rename-review","method":"propose","params":{"baseRevision":"0","commands":[{"type":"renameElement","id":"CLASS_ID_FROM_INSPECTION","name":"Invoice"}]}}
```

The revision shown here is illustrative, not a default to assume. Read the returned proposal `diff`, `diagnostics` and `newIds`. Ask the human to approve that exact change. Only after approval send `apply`, copying the returned proposal `id`, `baseRevision` and `digest` unchanged. Do not manufacture a digest or reuse an approval after another transaction. To decline, send `reject` with the returned proposal ID instead.

After applying, inspect `elementsById` to confirm the live name and identity. Send `undo` to restore the previous name, then `redo`, or stage a fresh proposal at the new revision and approve it separately. Send `save` explicitly. Close, exit, and run a separate `inspect` process on the saved native file to verify persisted semantics. Approval alone does not authorize a disk write.

### Agent sessions

The desktop and CLI share explicit `RoseAgent/RoseAgent` provider settings and OS credential references. `agent.configure` stores only nonsecret settings; it does not accept an API key, token or password.

`ProviderConfig.contextWindowTokens` is optional provider-advertised metadata (`0` means unknown); it is persisted automatically by the model picker, not manually configured or estimated from bytes. `maxContextBytes` remains a request-memory/transport guard, not a token context window. The provider enforces its actual model token limit; Rose Agent does not silently truncate or pretend to have an exact tokenizer.

| Method | Params | Success result |
| --- | --- | --- |
| `agent.configure` | `kind` (`local`/`hosted`), full `endpoint`, `model`; optional `credentialReference`, `timeoutMs`, `maxRounds`, `maxToolCalls`, `maxContextBytes`, `maxResponseBytes` | `configured:true` |
| `agent.start` | `prompt` string; optional `images` file-path array, `selectedIds` stable element-ID array | decimal-string `runId` |
| `agent.cancel` | `{}` | current `runId` and status |
| `agent.apply` | exact `runId`, proposal `id`, `baseRevision`, `digest` | actual applied transaction |
| `agent.reject` | exact `runId`, `proposalId` | actual rejection |

For an unsaved Rose2000e-compatible workspace:

```json
{"id":"new","method":"create","params":{"file":"/models/order.mdl","petalVersion":44}}
{"id":"plan","method":"agent.start","params":{"prompt":"Design an order service with classes, members, relationships and a readable native class diagram."}}
```

Or, after creating/opening a workspace and configuring a vision/tool-capable provider:

```json
{"id":"photo","method":"agent.start","params":{"prompt":"","images":["/models/order-diagram.png"]}}
```

`agentEvent` includes `runId`, monotonic decimal-string `sequence`, `status`, plain-text `activity`, `transactionId`, and optional `assistantMessage` (`{"text":"actual provider response"}`), `proposal` and `error`. The obsolete top-level `text` field is removed. Activities are not assistant replies. Events can precede the `agent.start` response. At `Awaiting review`, show the proposal diff, diagnostics, mappings and approval tuple to the human; only then send `agent.apply` with the unchanged tuple and matching run ID, or `agent.reject`. Manual `apply`/`reject` cannot bypass an agent-owned review. A stale revision/session or superseded run cannot reuse that approval.

Inspection/input remains responsive while network or credential lookup is pending. EOF cancels unapproved runs and does not save; an already approved applying transaction finishes before exit. Stdout failure stops input and drops unprocessed queued requests, including saves; it does not undo an approved commit.

Subsequent `agent.start` calls retain bounded, in-memory historical user/assistant messages and validated image evidence. Prior tool calls, projections, credentials and system roles are not replayed; each turn gets a fresh workspace projection. Rejected/cancelled proposals are historical designs, not live model facts; applying does not imply saving. Changing workspace generation resets context. The desktop's idle **Clear chat**, or closing/reopening the CLI workspace, resets it explicitly. Budget overflow reports `contextBudgetExceeded` rather than silently discarding history.


## Queries, commands and values

All schemas are closed: unknown fields, wrong types and fields belonging to another discriminant are rejected. Optional fields are validated when present. Query `kind` selects one of:

- `modelTree`: no additional selection fields.
- `elementsById`: nonempty `elements` array of element-ID strings.
- `search`: string `search`.
- `diagramById`: diagram-ID string `diagram`.
- `neighborhood`: element-ID string `focus`; optional nonnegative integer `depth`.
- `diagnostics`: no additional selection fields.

Every query accepts nonnegative `offset` and `limit`; omitted values use Workspace Query defaults. Values outside safe JSON integer precision use `{"type":"integer","value":"9223372036854775807"}` and are bounded by the platform's signed size range. Diagram queries include presentations and related semantic records; tree and other queries return their requested records with pagination metadata.

Commands use `type` with lowerCamelCase names. Every listed payload field is required; an empty owner/unit string means unspecified where supported. IDs refer to inspected records or exact transaction-local `clientId` strings from creation commands.

| Type | Payload fields |
| --- | --- |
| `createElement` | `clientId`, `kind`, `name`, `owner`, `unit` |
| `renameElement` | `id`, `name` |
| `setProperty` | `id` (ObjectId), `key`, `value` |
| `setOwner` | `id`, `owner` |
| `copyElements` | nonempty `ids`, `owner`, `clientIds` mapping source element IDs to client-ID strings |
| `createRelation` | `clientId`, `kind`, `name`, `owner`, at least two `endpoints`, `properties` mapping keys to values |
| `reconnectRelation` | `id`, at least two `endpoints` |
| `createDiagram` | `clientId`, `kind`, `name`, `owner`, `unit` |
| `addPresentation` | `clientId`, `diagram`, `subject` (ObjectId), `geometry` |
| `setGeometry` | `id`, `geometry` |
| `setRoute` | `id`, `points` (at least two points) |
| `setMessageOrder` | `id`, signed integer `ordinal` |
| `removePresentation` | `id` |
| `deleteElement` | `id`, `acknowledgedDependents` array of dependent-ID strings |

Ordinary typed IDs are strings. ObjectId is a closed object with `kind` (`element`, `relation`, `diagram`, or `presentation`) and string `value`, for example `{"kind":"element","value":"CLASS_ID_FROM_INSPECTION"}`. Semantic IDs and client aliases must not contain whitespace or control characters. Unit IDs are canonical filesystem paths: spaces are valid, NUL is not, and specifying a unit does not authorize access to an unloaded file. Presentation IDs percent-encode their UTF-8 unit/diagram components and include the diagram-local decimal label; copy the whole inspected ID unchanged. They remain stable when reopening the same path, but relocation changes their unit-qualified identity. Model staging additionally validates identity, references, supported native object kinds and whole-batch invariants; a syntactically valid JSON command does not imply it can be applied safely.

Geometry is `{x,y,width,height}` with finite numeric values and positive extents. Coordinates are native diagram item centers. Route points are `{x,y}` with finite numbers.

Property values support string, boolean and finite number literals. Signed 64-bit integers beyond safe JSON precision use `{"type":"integer","value":"-9223372036854775808"}`. Local references use `{"type":"reference","value":"18446744073709551615"}`. Decimal strings are canonical: no leading zeros except `0`, no whitespace, no leading plus; unsigned values cannot be negative. Revisions and presentation local labels are always unsigned decimal strings. Hashes are exactly 64 lowercase hexadecimal characters. Null, arrays and arbitrary property-value objects are not supported.

## Output and agent tools

`result.type` discriminates `projection`, `proposal`, `applied`, `saved`, `closed`, and `rejected`. The shared library also serializes `accessGranted` for desktop directory grants. Errors have `type:"error"`, lowerCamelCase `code`, `message`, `file`, `offset`, `line` and `column`. Offsets/counts use tagged signed integers if safe JSON precision would otherwise be lost.

Projections include `schemaVersion`, decimal `revision`, `dirty`, `canUndo`, `canRedo`, native `path`, requested elements/relations/diagrams/presentations, `total`, units and diagnostics. Semantic and presentation properties include `propertyTypes` metadata; consumers must use it rather than guessing an editable type from displayed text. Proposal results include exact approval fields, a semantic diff, diagnostics and new-ID mappings. Save receipts include path, revision and per-unit checksums.

`rose::json::tools()` publishes the matching `inspect` and `submit_proposal` JSON function schemas for agent providers. The same command/query schema tables validate session and agent inputs. Neither tool grants approval, applies changes, or saves. `canonicalCommands()` serializes a deterministic, sorted-key, schema-versioned envelope; Workspace incorporates that input into proposal digests.

## Supported native edit profile

The semantic root requires exactly one explicit Petal44 or Petal50 header and a supported semantic object. Headerless controlled dependencies inherit the root profile; explicitly headed dependencies must match it. Standalone headerless roots and unsupported versions such as Petal47 return `unsupportedProfile`. Parser acceptance of other syntax is not model/edit support.

The class-focused writer supports creation of `Class_Category`, `Class`, `ClassAttribute`, `Operation` and `Parameter`, binary `Association` and class-owned `Inheritance_Relationship`, `ClassDiagram`, and native class/category/inheritance/binary-association presentations. Association appearances use native `AssociationViewNew` and nested `RoleView` records with persistent semantic role references. Class views carry native compartment flags and positioned labels; moves/resizes/reconnects maintain attachment endpoints and preserve interior bends. Sequence-message ordering and creation of other diagram/presentation kinds remain unsupported. Existing foreign syntax is retained rather than converted into a substitute model.

Authored diagram declarations are emitted deterministically with numeric local-label/dependency ordering so connector suppliers are already declared. Untouched imported source is not broadly reordered. The canvas preserves explicit stored route endpoints, including detached endpoints in an imported file; it does not hide bad native geometry by silently snapping an arrow to a box. New default routes use box-boundary attachments. Correcting explicit geometry requires a reviewed route edit.

Presentation `geometry.x`/`y` are native symbol centers, not top-left positions. Class bounds are `x ± width/2`, `y ± height/2`; custom route endpoints must be chosen from the actual endpoint appearances. The bundled skill spells out this coordinate contract and prefers generated border-attached routes when custom bends are unnecessary.

The New-model default is not a version conversion: opening/saving Petal 50 preserves Petal 50. Do not replace a header number to force Rose 2000e compatibility. An empty imported workspace and a populated model with declaration/geometry defects are different cases; changing a profile cannot invent missing classes or repair arbitrary native content.

Property edits require native scalar/type rules. Structured/opaque fields, arbitrary reference insertion and native identity mutation are rejected. Association `roles.0.quid` and `roles.1.quid` are read-only projection metadata: generated/native Role IDs persist through save/reopen and are written only inside native Role nodes.

An existing `quidu` binding is not retargeted by editing its displayed `type`/`result` string. Incompatible scalar edits are rejected before apply; valid explicit simple/qualified spellings and semantic target renames remain supported. Untouched legacy spellings are preserved.

Moves and copies are restricted to safe native ownership lists. Cross-unit moves/insertion, root/property-owned moves or deletion, ambiguous native lists and hidden identity/reference hazards are rejected. Source-preserving copies require matching encoding and newline style, unchanged descendant ownership and no unsupported owned diagrams/relationships; structurally changed copies require save/reopen first. Referenced `.mdl` models and their dependency closure are read-only; opening a model explicitly as the root does not make it read-only because a dependency cycle points back to it.

Save As preserves the source-root-relative dependency layout. Destination subdirectories must already exist inside granted roots. Dependencies outside that layout, target collisions, changed source files and existing destinations are rejected; files are not flattened or silently overwritten. Valid relative `file_name` spellings are preserved, and relocation patches offsets rediscovered from the current serialized document.

Native root classification uses source identity, not its destination suffix. Authorized destination-directory symlinks use canonical identities, and a cycle back to the already loaded root remains resolved even after Save As to a nonstandard suffix. New dependency reads still require recognized native extensions.

## Verification boundaries

The CLI consumer tests launch the actual executable, author native inputs, derive inspected IDs, stage and approve returned proposal tuples, exercise undo/redo and fresh reapproval, save and reopen in a separate process, and check preserved foreign extensions. They also cover malformed requests, read-only saves, rejected approvals, permitted-root enforcement and default roundtrip source preservation. A successful internal roundtrip is not proof of native Rose acceptance.

### Linux and provider evidence

- All **15 registered Linux suites** passed: `petal`, `projection`, `transactions`, `units`, `storage`, `recovery`, `controller`, `canvas`, `providers`, `permissions`, `agent_runs`, `credentials`, `json`, `cli`, `cli_agent`. The isolated credential suite covers unlock-prompt cancellation/dismissal, session cleanup and stale-result rejection without real user secrets.
- Actual desktop/CLI HTTP-fixture smoke exercised image/prompt review, Reject/Apply, exact digest validation, manual import/movement/cancellation, save/Save As and independent reopen. Exact attached bytes reached the transport. **These fixture requests are not live AI inference.**
- Real Kenari model discovery and inference separately exercised prompt-only authoring, empty-prompt photo reconstruction and imported-model rename while retaining members, relationships, geometry and source checksum. Provider-advertised context metadata was displayed/persisted; metadata-free discovery stayed unknown and changing Base URL did not forward the earlier provider key.
- Real photo-aware chat exercised image-only inference → Reject without mutation → historical-image-aware follow-up adding a member → full review → exact Apply in memory → separate Save → independent reopen. Reopen retained classes, members, ownership, association and native appearances. Actual Stop cancelled a pending run without live-revision or native-byte changes.
- Actual widget probes cover complete narrow wrapped text, resize/burst follow-tail, no jump while reading older messages and resumed following at the tail. Their text is fixture content, not an invented provider response. Conversation regressions cover fresh state after manual edits, rejected historical designs, workspace reset, image evidence and explicit context-budget failure.
- The final recorded real photo/history run retained both stored association endpoints on native-center class borders through independent reopen. This proves the exercised geometry contract, not every possible layout.

### Native Rose evidence

Earlier exact authored Petal 44 exports opened in Rational Rose 2000e with classes, members, binary association and inheritance visible. An exact edited Save As likewise displayed the renamed class while preserving the other content. That installation explicitly rejected Petal 50. The writer omits the observed version-incompatible `SubSystem.category` backlink for Petal 44 and retains it for Petal 50, where genuine newer source contains it.

A specifically repaired native model reordered five raw presentation spans into numeric/dependency order, preserving three classes, 16 members, two relationships, five appearances, IDs and explicit geometry/routes. Linux independent reopen and actual canvas rendering passed. The original explicit off-box endpoint remained literal; the repair was not an automatic geometry correction or general repair feature.

**Fresh native acceptance remains unverified for the repaired file and newer real-AI exports.** A permitted read-only screenshot attempt failed; display metadata reported 0 bits per pixel. No Windows input, install/configuration/license changes or simulated native acceptance replaced the missing proof. Earlier acceptance applies only to the observed files and Rose edition.

### Windows application boundary

The dedicated Windows 8.1 x86 configuration is Qt 5.15.18 + MSVC 2019/v142 + Schannel TLS. Qt's [official Windows platform table](https://doc.qt.io/archives/qt-5.15/windows.html) lists Windows 8.1 x86 with MSVC 2019; this is upstream support evidence, not an application guest test. Modern Windows x64 uses Qt 6.8.3 and targets Windows 10+.

No actual Rose Agent Windows 8.1 guest certification is claimed. A successful hosted build, a Linux test run and native Rose opening an exported file are three different checks. The first release stays prerelease pending the actual Windows 8.1 runtime matrix. [Windows 8.1 support ended January 10, 2023](https://learn.microsoft.com/en-us/lifecycle/products/windows-81).

## Troubleshooting

- **Key lookup fails or prompts remain pending:** run Linux in a session with D-Bus and an available/unlocked Secret Service. Windows uses the current user's Credential Manager. Cancel the pending lookup or retry after unlocking; no plaintext fallback is provided.
- **TLS/provider request fails:** check Base URL, credentials, system time and certificate trust. Legacy Schannel relies on OS TLS/trust facilities; an unsupported provider policy or stale roots can prevent access. Do not disable certificate verification.
- **Model/context metadata missing:** discovery can return models with incomplete capability data. Unknown context remains unknown; it is not a guessed budget. Oversized history reports `contextBudgetExceeded`; clear idle chat or submit a smaller request rather than expecting silent truncation.
- **Native open fails despite internal reopen:** check the actual target Rose edition/profile, encoding, declared supplier order and explicit endpoints. Internal parser/model success does not certify the native application. Do not force a version change by editing only the header.
- **Access/storage error:** grant only the required canonical roots and check dependencies, destination directories, existing targets and external changes. Path variables do not bypass permission policy.
- **Qt plugin/image-format error:** preserve the complete portable deployment and use matching Qt libraries/plugins when building. Linux packages need xcb/XWayland; the release must include image-format plugins for the documented image support.
