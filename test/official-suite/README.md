# Official conformance tests

`PIN` identifies the vendored upstream snapshot. Its compatibility constraints
select draft-07 or 2020-12 cases without changing schemas or expected results.
External schemas are scoped to each group and loaded only from inline fixtures;
no network requests are made. Compilation errors fail every case in that group.

CTest labels separate `required`, `optional`, and `format` assertions. The
required run uses annotation-only format behavior. Format-assertion tagged cases
are run separately, including those in the upstream top-level files. Cases for
other dialects/configurations are counted as inapplicable, not passed.

Baseline at f15f156c69bfea115267b189c0129462f6511913 (GCC, std::regex):

| Dialect | Required | Optional | Format assertions |
|---|---:|---:|---:|
| draft-07 | 963/972 | 87/126 | 557/709 |
| 2020-12 (unsupported at baseline) | 1110/1319 | 128/176 | 617/789 |

All 95 pre-existing CTests passed, including their existing expected-failure
configuration. None of those tests or expectations was changed for this baseline.

## Corrected development base

Owner-directed base correction: local `master` is upstream
pboettch/json-schema-validator at d5199551f758ba067612979ae32ec35693640ccf.
The draft-2020-12 changes were rebased onto it, retaining its URI, default,
format-checking and test-harness fixes. All 108 upstream baseline CTests passed.
The earlier table is the historical baseline before this base correction. The corrected upstream draft-07 required baseline is 965/972 (seven failures), measured against the unchanged d5199551 static library with the same pinned compatibility/fixture runner and no new dialect APIs.

With PCRE2 10.49, required draft-07 passes 972/972 and required 2020-12 passes
1319/1319. With std::regex, draft-07 passes 972/972; 2020-12 passes 1310/1319.
Its nine required failures are the Unicode property-escape groups in pattern
and patternProperties. No network-dependent required test is omitted: external
schemas come from each group's inline fixtures.

Optional/format failures remain visible. In particular the existing upstream
optional cross-draft case requests 2019-09, outside the two supported dialects;
the implementation explicitly rejects it. Existing fixed-case exemptions have
been removed where the implementation now passes. No expected validity or
schema in the upstream data was changed.

The separate dialect regression also checks simultaneous `$ref` and `$dynamicRef`: both are applied through the existing applicator path, as required by [2020-12 schema references](https://json-schema.org/draft/2020-12/json-schema-core#section-8.2.3). The official required snapshot does not contain that combination.

The numeric regression (`issue-293-float-point-error`) also checks signed zero, exact integral floating-point values, adjacent fractional values and overflowing `multipleOf` quotients. Integer detection uses the finite floor boundary; zero detection uses floating-point classification. These preserve exact JSON Schema numeric decisions without epsilon comparisons or `-Wfloat-equal` diagnostics.
