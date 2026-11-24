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
