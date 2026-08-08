# TurboBuild Architecture

TurboBuild is organized as a small CLI shell around a C++ implementation unit.
The current split keeps the executable entry point stable while giving the
application logic a named interface that can be decomposed further as the tool
grows.

## Source Layout

- `src/main.cpp` is intentionally tiny. It only delegates to the application
  runner.
- `src/turbobuild.h` is the public boundary for the executable.
- `src/turbobuild.cpp` owns command parsing, project inspection, compiler
  probing, build orchestration, benchmarking, diagnostics, report generation,
  and command dispatch.

## Command Flow

1. `main` calls `turbobuild::run_app`.
2. `parse_args` normalizes command-line input into `Options`.
3. Command dispatch calls a focused `command_*` function.
4. Shared helpers handle process execution, JSON escaping, project scanning,
   compiler detection, report paths, and benchmark statistics.
5. Commands write machine-readable results under `.turbobuild/results`.

## Maintainer Notes

- Keep `src/main.cpp` free of feature logic.
- Add new user-facing commands as `command_*` functions and wire them in
  `run_app`.
- Prefer shared helpers for process execution, paths, JSON output, and compiler
  probing instead of duplicating shell and filesystem code.
- Split `src/turbobuild.cpp` into domain files when a section starts changing
  independently, for example `benchmark.cpp`, `project.cpp`, or `reports.cpp`.
- Every optimization claim should remain measurement-backed: build, test, run,
  then report the result.

## Practical Workflows

- `doctor` checks whether a project has a supported build system, compiler
  configurations, tests, benchmark hints, and optional analysis tools.
- `init-ci` creates a GitHub Actions workflow that builds TurboBuild, runs
  readiness checks, gathers warning/static-analysis reports, and uploads the
  generated artifacts.
