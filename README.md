# Rose Agent

Rose Agent is a native desktop editor and AI assistant for Rational Rose Petal models. Describe a class model, attach a diagram photo, or open an existing `.mdl`; inspect the proposed semantic and layout changes, approve the exact transaction, and save the native model separately. The desktop (`rose-agent`) and JSON-lines CLI (`rose-cli`) share one authoritative workspace.

**Current scope is class-focused interoperability, not a 1:1 Rational Rose replacement.** Supported authoring covers packages, classes, attributes, operations, parameters, binary associations, inheritance and class-diagram appearances. Unsupported imported content is preserved as native source; that does not mean every diagram family is editable or compatible with every Rose version.

## Install

Download assets from [GitHub Releases](https://github.com/alertxsto/rose-agent/releases). The first release tag is **`v0.1.0`**, published as a **prerelease until actual Windows 8.1 native execution is proven**. A source checkout or a documented release target is not evidence that a published asset or a Windows guest run already exists; consult the release page and workflow results.

| Platform | Distribution | Notes |
| --- | --- | --- |
| Linux x86-64 | `.deb`, `.rpm`, portable `.tar.gz` | Bundled Qt 6.8.3; Ubuntu 22.04/glibc 2.35+ baseline. Desktop needs a graphical session and keyring access. |
| Windows 10+ x64 | `RoseAgent-0.1.0-windows-x64-setup.exe`, `RoseAgent-0.1.0-windows-x64-portable.zip` | Modern Qt 6.8.3 build; not the Windows 8.1 package. |
| Windows 8.1 32-bit | `RoseAgent-0.1.0-windows-x86-setup.exe`, `RoseAgent-0.1.0-windows-x86-portable.zip` | Dedicated Qt 5.15.18/MSVC 2019 build target; actual Windows 8.1 guest runtime is not yet certified. |
| Release metadata/source | `SHA256SUMS.txt`, `release-manifest.json`, public source ZIP/tar.gz | Checksums, build metadata and corresponding public source archives. |

For Debian/Ubuntu, install the downloaded `.deb` with `sudo apt install ./<downloaded-package>.deb`. For RPM distributions, use `sudo dnf install ./<downloaded-package>.rpm`. These are illustrative filenames: use the exact Linux asset name on the release page. For portable Linux distributions, extract the complete archive and run `./bin/rose-agent` or `./bin/rose-cli` from its extracted root. Do not move the launcher/executable away from its bundled libraries/plugins. Linux bundles need xcb/XWayland; native Wayland is available only when the corresponding Qt plugin is included. On Windows, run the installer or extract the entire portable ZIP and start `rose-agent.exe`; keep the CLI and Qt deployment files together.

Verify on Linux using `sha256sum -c SHA256SUMS.txt` in a directory containing the release downloads. For an individual asset, compare `sha256sum <downloaded-asset>` with its matching entry; full-manifest verification requires all listed files. On Windows, compare `Get-FileHash .\<downloaded-asset> -Algorithm SHA256` with the matching entry in `SHA256SUMS.txt`. A checksum detects corruption or substitution relative to that manifest; it is not a code-signing certificate.

### Windows 8.1 support boundary

The legacy target uses **Qt 5.15.18, x86, MSVC 2019/v142**, with Windows **Schannel TLS**, rather than an obsolete OpenSSL 1.1 deployment. Qt's [official Qt 5.15 Windows support table](https://doc.qt.io/archives/qt-5.15/windows.html) lists Windows 8.1 x86 and MSVC 2019. This is the upstream platform basis, not proof that this application has completed an actual Windows 8.1 run. Release/build results and guest verification are separate evidence.

[Windows 8.1 reached end of support on January 10, 2023](https://learn.microsoft.com/en-us/lifecycle/products/windows-81). A legacy app build cannot restore operating-system security updates, repair an outdated root certificate store, or guarantee that every provider still accepts that system's TLS capabilities. Prefer a supported OS whenever possible.

## First model and AI chat

1. Use **New** to choose a native destination, encoding and allowed directories, or **Open** an existing `.mdl`. New creates an unsaved in-memory model. New models default to **Petal 44**; choose **Petal 50** explicitly only for a compatible target Rose release. Opening and saving an imported model preserves its version; this is not a version converter.
2. In **Provider…**, enter an OpenAI-compatible **Base URL** and **API key**, then **Connect / load models**. Kenari (`https://kenari.id/v1`) is prefilled, not required. Choose a discovered model and **Use model**. Credentials are stored only in the OS credential store, scoped to the Base URL: Linux Secret Service or Windows Credential Manager. They are not written into model files, CLI JSON or ordinary application settings. A different Base URL does not receive the old provider's key.
3. Type a request, attach PNG/JPEG/WebP images, or send an image alone. **Enter** sends; **Shift+Enter** adds a newline. Historical discussion and validated images remain available to follow-up turns in the current workspace, while each turn inspects fresh model state. History is bounded and in memory, not an unlimited or persistent chat archive.
4. Open **Review details** and inspect the complete change set, diagnostics, names, ownership, properties, endpoints and routes. **Apply** authorizes only the exact run/proposal/revision/digest shown and changes memory. **Reject** does not change the model. The model cannot approve itself or call save/shell tools.
5. Use **Save / Save As** explicitly to write the native model. Apply, reject, undo/redo, chat completion and CLI EOF do not implicitly save. **Stop** cancels unapproved work; **Clear chat** resets idle conversation context, not the model.

The model picker uses provider-advertised context metadata. Missing metadata stays unknown; there is no invented token count or hidden truncation. Images are validated (maximum four, 8 MiB each, 8192 pixels per side and 16 megapixels); context/transport limits can reject smaller requests. See the [desktop and CLI guide](docs/usage/native-cli.md) for the exact limits, protocol and approval rules.

## Edit an existing model without AI

Manual editing and saving work offline; no API key is required.

1. **Open** the root `.mdl`; select its actual source encoding and explicit allowed roots/path-variable mappings. Read diagnostics for missing units and read-only dependencies before editing.
2. Select a class/member in the model browser or open a supported class diagram. Use **Model → Rename**, typed property editing, supported element/relation actions or **Diagram → Geometry / Edit route**. Manual edits also present a transaction review before applying.
3. Review, apply, optionally undo/redo, then use **Save** or **Save As**. Save As preserves the source-root-relative unit layout; required destination subdirectories must exist and remain within granted roots. Unsafe rewrites, external changes and collisions are rejected rather than silently overwritten.
4. Reopen the output and check it in the intended native Rose release before relying on interoperability. Do not change only a Petal header number to force a downgrade.

## Build from source

Requires a C++20 compiler, CMake3.24+, and one consistent compiler/Qt installation. The default build requires **Qt6.5+** with Core, Core5Compat, Concurrent, Network, Gui, Widgets, PrintSupport and Svg. Core5Compat supplies strict legacy Rose codecs on every platform, including Windows-1252 without relying on a particular Qt Core build's ICU configuration. Tests also require Qt Test; Linux credentials require Qt DBus and a running Secret Service implementation. Linux credential tests additionally use `dbus-run-session`.

```sh
git clone https://github.com/alertxsto/rose-agent.git
cd rose-agent
cmake -S . -B build -DROSE_QT_MAJOR=6 -DBUILD_TESTING=ON
cmake --build build --parallel
ctest --test-dir build --output-on-failure
build/bin/rose-agent
```

Use **`-DROSE_QT_MAJOR=5`** explicitly for the Qt 5.15 legacy build, pointing `CMAKE_PREFIX_PATH` at the matching Qt installation if needed. Qt 5 and Qt 6 use separate build directories; do not reuse an already configured directory across majors. On Windows choose the matching x86 or x64 compiler environment; multi-configuration output is typically `build/bin/Release/rose-agent.exe`. Selecting Qt 5 does not by itself produce a correct Windows 8.1 x86 deployment: use the dedicated release build/deployment configuration.

Linux packaging additionally needs `patchelf`, `binutils`, `dpkg-dev` and `rpm`, plus the Qt SVG/image-format plugins (including WebP) and matching official Qt source for redistribution notices. With the release Qt installation and source available:

```sh
cmake -S . -B build-package -DROSE_QT_MAJOR=6 -DROSE_ENABLE_PACKAGING=ON \
  -DROSE_QT_PREFIX="$QT_PREFIX" -DROSE_QT_SOURCE_DIR="$QT_SOURCE_DIR"
cmake --build build-package --parallel
cpack --config build-package/CPackConfig.cmake -G 'DEB;RPM;TGZ'
```

Set `QT_PREFIX` to the matching Qt installation and `QT_SOURCE_DIR` to official source containing `qtbase`, `qt5compat`, `qtsvg`, `qtimageformats` and, when deployed, `qtwayland`. An extraction root of official per-module source archives is supported; `qtbase/.cmake.conf` must match the exact deployed Qt version. These package configuration inputs are not application credential settings.

If the SDK deployment closure includes non-Qt shared libraries such as ICU, also provide `-DROSE_QT_EXTRA_LICENSE_DIR="$EXTRA_LICENSE_DIR"`. That directory must contain `Libraries.txt` identifying each exact library name/version, `SOURCE.txt`, `NOTICE.txt` and a `LICENSES/` directory with the actual license texts. Packaging rejects missing or unlisted dependency notices instead of silently shipping undocumented libraries.

The release workflow builds/tests/packages tagged source for **`v0.1.0`** and later `v*` tags, then publishes native packages, portable archives, checksums, metadata and public source archives. Manual workflow dispatch produces build artifacts unless publication is explicitly configured. Bundled Qt/runtime notices travel with the packages. A successful hosted runner build does not certify native Windows 8.1 execution. Read each release's actual status before claiming an asset was produced.

## Verified behavior and remaining gaps

Recorded Linux evidence includes **15 passing suites** and real Kenari prompt, photo and imported-model inference. The exercised photo/history flow covers Reject → image-aware follow-up → exact Apply → separate Save → independent CLI reopen. Actual widget probes cover wrapped chat text, scrolling/follow-tail and cancellation; HTTP fixture tests are distinguished from real provider inference.

Earlier exact Petal 44 exports were opened in Rational Rose 2000e with classes, members, association and inheritance visible; that installation rejected Petal 50. Newer live-AI output and a specifically repaired native model have Linux reopen/rendering evidence, **not fresh native-Rose certification**. A read-only screenshot attempt was blocked by an unusable display capture reporting 0 bits per pixel. No Windows input was used to manufacture missing proof. The Rose Agent Windows 8.1 x86 executable likewise requires its own actual guest evidence.

See [compatibility evidence](docs/compatibility/m0-baseline.md), [Petal syntax/edit profile](docs/format/petal-50.md), [research corpus provenance](docs/compatibility/corpus-provenance.md), [CHANGELOG](CHANGELOG.md) and the public [ROADMAP](ROADMAP.md).

## Troubleshooting

- **Provider connection/TLS errors:** check the Base URL, certificate trust, clock and provider compatibility. Never disable certificate verification as a workaround. On legacy Windows, Schannel uses the OS trust/TLS facilities; unsupported TLS policy or obsolete trust roots can prevent access even when the app launches.
- **No API key / locked keyring:** Linux needs a session D-Bus and an available, unlocked Secret Service (for example GNOME Keyring or KWallet with Secret Service support). Windows uses the current user's Credential Manager. The app reports failures; it does not fall back to plaintext secrets. Cancel a pending unlock request or reopen Provider settings after unlocking.
- **Empty model list or unsupported model:** check discovery/authentication. Models advertising incompatible chat, tool or image capabilities are filtered. Providers with missing metadata are not magically guaranteed to support those capabilities.
- **Unknown context / budget error:** unknown metadata is honest. `contextBudgetExceeded` rejects an oversized conversation rather than silently dropping history. Clear idle chat or start a new workspace context and send a smaller request.
- **Unsupported Petal version/encoding:** use the correct supported profile and explicit legacy codec. Parser acceptance, internal reopen and native Rose acceptance are different checks. Unsupported constructs are preserved, not automatically upgraded or downgraded.
- **Detached connectors:** explicit stored endpoints are displayed literally. Review an explicit route edit; the canvas does not hide an imported geometry defect by silently snapping it.
- **Missing Qt platform plugin:** keep the complete portable deployment intact. When building, use Qt libraries/plugins from the same installation; mixing distro packages or Qt majors is unsupported.

## Licensing and notices

The [project licensing notice](LICENSE) grants **no project-wide redistribution or modification license**: all rights remain with their respective authors. Public availability is not an open-source license; do not represent the project as MIT/GPL-licensed. Qt and other bundled third-party components have their own license obligations and notices. A Qt runtime license does not automatically license this project's code. Rational Rose, Qt and provider names belong to their respective owners; no affiliation or full compatibility certification is implied.
