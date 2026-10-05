# Research corpus and oracle status

Research source: Eclipse ATL [MDL2GMF archive](https://eclipse.dev/atl/atltransformations/MDL2GMF/org.eclipse.m2m.atl.mdl2gmf.zip), downloaded outside the repository. Sample rights have not been established for redistribution; no downloaded model is vendored. Permanent test input is authored inline in `tests/tst_petal.cpp`.

| Archive sample | Bytes | Observed format | Parser consumer result | SHA-256 |
|---|---:|---|---|---|
| `rose.mdl` | 142059 | Petal 47, writer Rose 8.0.0303.1400 | 1598 nodes, 3 Class objects; class rename/reparse and unchanged source passed | `83ff9aa4d01de459866e67f644349b81498baa7944d08013090047a38eef4d2f` |
| `rose2.mdl` | 17557 | Petal 50, writer Rose 2006.0.0.060314 | 183 nodes, 3 Class objects; class rename/reparse and unchanged source passed | `8f558fc4919645e59a7d4426494b13a86903fcecea69c58458356ccd3622ba12` |
| `QVT.mdl` | 1208080 | Petal 50 | 10167 nodes, 149 Class objects; class rename/reparse and unchanged source passed | `20c06885542047d5d1a441fdc72cb84685abd5be82767a68b758b5777485b476` |
| `Topcased-mm_V24_V2toV3-last.mdl` | 3022946 | Petal-looking content with 156 literal `=09` markers and 958 quoted-printable soft line breaks | Rejected as malformed input; not silently repaired | Not used as an accepted native fixture |
| `Sample.mdl` | — | XML rather than Petal | Not a native-format acceptance fixture | — |

For diagnosis only, Python `quopri.decodestring` produced a separate 3019630-byte Topcased research copy. With explicitly selected Windows-1252, that copy parsed into 23129 nodes and 329 classes; rename/reparse and unchanged-source checks passed. Its SHA-256 is `b8227a13ce03acd5a84dd6b3779b7804558a5636c08dfd523451cb0e4115deff`. This is not proof of the original file's intended encoding, and the application does not perform this transformation.

## Syntax references

- [Grammar and API for Rational Rose Petal files](https://silo.tips/download/grammar-and-api-for-rational-rose-petal-files), M. Dahm, 2001, mirrored empirical grammar. Incomplete evidence, not a normative complete specification.
- IBM [supported Rose Petal versions](https://www.ibm.com/docs/en/rational-soft-arch/9.6.1?topic=migration-supported-rose-petal-versions) and [Rose import artifacts](https://www.ibm.com/docs/en/rational-soft-arch/9.6.1?topic=migration-rose-import-artifacts).
- [Umbrello Rose import notes](https://uml.sourceforge.io/roseimport.php), controlled-unit/path evidence, not a parity oracle.

## Native acceptance and rights boundary

The research consumers above establish syntax-only results for those samples, not native Rose acceptance, editing support for Petal 47, or redistribution permission. Do not vendor the downloaded models or infer their license from public availability.

Separate product verification later opened earlier exact authored Petal 44 exports in Rational Rose 2000e with class members and association/inheritance visible. That installation rejected Petal 50. Newer real-AI exports and a specifically repaired native model have Linux reopen/rendering evidence only; fresh native acceptance remains unverified after a read-only screenshot attempt failed with the display reporting 0 bits per pixel.

The dedicated Windows 8.1 x86 build target also lacks actual application guest certification. Native Rose reading an exported file does not prove the Rose Agent Windows executable works. See the [current compatibility matrix](m0-baseline.md), [usage verification boundaries](../usage/native-cli.md#verification-boundaries) and [public roadmap](../../ROADMAP.md).
