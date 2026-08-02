# TurboBuild Optimization Roadmap

This is the implementation plan for the deeper optimization engine after the
level-one CLI foundation.

## Measurement Contract

TurboBuild must treat every optimization as unproven until it has:

- a baseline build artifact
- a candidate build artifact
- passing correctness tests for both
- benchmark samples for both
- recorded latency, throughput, failure, and artifact-size metrics
- a report that shows the measured before-and-after result

No optimization should be described as faster, smaller, or safer unless the data
supports that claim.

## Next Native Layers

1. Process runner with native CPU time, peak RSS, exit status, stdout, and stderr
   capture.
2. Benchmark harness with warmups, repetitions, outlier handling, confidence
   intervals, p50, p95, p99, and p99.9.
3. CMake preset generation for isolated build directories.
4. Make and direct compiler build graph discovery.
5. Test discovery for CTest, GoogleTest, Catch2, doctest, and Make `test`.
6. Compiler flag compatibility cache per compiler version.
7. PGO workflow: instrument, train, rebuild, measure.
8. LTO workflow with linker detection.
9. Sanitizer workflows for ASan, UBSan, TSan, and safe debug builds.
10. HTML reports with comparison tables and explicit improvement claims.

## Unsafe Flag Policy

These flags remain opt-in and must be clearly marked in reports:

- `-Ofast`
- `-ffast-math`
- `-march=native`
- `-fno-exceptions`
- `-fno-rtti`

TurboBuild can recommend testing these flags only when the project and user
explicitly allow the semantic or portability tradeoff.
