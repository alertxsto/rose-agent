# Petal syntax support

`core/PetalDocument` is a source-preserving syntax layer, not a complete Rose workspace or native compatibility certificate. Observed corpus includes Petal44,47 and50. Workspace/Model/ControlledUnits map supported Petal44/50 class semantics, scoped references and native storage; new authored models default to Petal44, with Petal50 explicit. The edit profile and runtime evidence are documented in [Rose Agent desktop and native CLI](../usage/native-cli.md). Headerless controlled units inherit the root profile; headed units must match it. Parser acceptance alone does not authorize editing other profiles.

## Current behavior

- Iterative parenthesis tree; immutable owned source bytes and byte spans. No recursive parser call stack.
- Object headers retain kind, positional quoted names and local `@unsigned-integer` labels. Ordered properties retain duplicate keys. Lists, values and tuples remain nested syntax nodes.
- Quoted strings recognize escaped quote/backslash; other backslashes remain literal. Pipe-prefixed multiline text consumes whole lines, including syntax-looking content.
- Name and primitive property replacements splice validated, nonoverlapping spans. Output is reparsed before return. Unaffected source, including unknown properties, whitespace and line endings, is not serialized again.
- References to names/properties belong to one document identity. Foreign edits return `InvalidCommand`; foreign raw property access is a programming-contract exception.
- Default text encoding is ASCII, deliberately rejecting non-ASCII input without a selected codec. A UTF-8 BOM supplies an explicit UTF-8 signal and is retained. Named Qt codecs must preserve ASCII syntax; unrepresentable edits are rejected. `charSet 0` is not guessed to mean UTF-8 or Windows-1252.
- Malformed syntax, including unclosed nodes, strings and invalid local label tokens, reports source offset, line and column. Parsing does not certify UML validity or resolve duplicate semantic identities.

Multiline property writing and profile-specific structured mutations still require model/profile serialization rules; successful syntax parsing alone is not a license to rewrite an unknown Rose field.

## Observed verification

Recorded Linux parser verification covers rename with escaped quotes and opaque properties, literal text, truncation location, foreign handles, malformed references, explicit Windows-1252/unrepresentable edits, 20,000 levels of nesting and overlapping patches. Later full-workspace verification has 15 registered suites; see the [usage evidence](../usage/native-cli.md#verification-boundaries). Development toolchain details are not installation requirements.

A separate compiled consumer loaded the public research files listed in [corpus provenance](../compatibility/corpus-provenance.md), renamed a real class, reparsed the output and checked that the source document stayed unchanged. That consumer did not use a native Rose process; filesystem storage, controlled units and the native editor were outside its scope. Earlier exact authored Petal 44 exports were separately observed in Rose 2000e; newer exports and the specifically repaired native model remain without fresh native certification. See the [compatibility matrix](../compatibility/m0-baseline.md).

## Editable profile versus syntax

New native workspaces default to **Petal 44**. **Petal 50** must be selected explicitly; imported versions remain unchanged on save. Standalone roots require a supported explicit 44/50 header. Headerless controlled dependencies inherit the root profile; explicitly headed dependencies must match it. Petal 47 parser acceptance in the research corpus does not enable a Petal 47 editing profile.

Authored semantics cover `Class_Category`, `Class`, `ClassAttribute`, `Operation`, `Parameter`, binary `Association`, class-owned `Inheritance_Relationship` and `ClassDiagram`, with supported class/category/association/inheritance appearances. Native association appearances use `AssociationViewNew` and nested `RoleView`. Unsupported families remain source-preserved, not silently turned into supported classes or substitute JSON.

Presentation geometry positions are native item centers. Explicit stored route endpoints are rendered literally; generated default routes attach to class borders. Authored suppliers precede connector consumers in deterministic numeric/dependency order without broadly reordering untouched imported content. Unsafe structured/identity rewrites are rejected.

Petal 44 acceptance was observed for earlier exact files in Rational Rose 2000e; that edition rejected Petal 50. Changing only the version header is not a validated downgrade. Internal parse/reopen and target-native open/save/reopen remain separate acceptance checks.
