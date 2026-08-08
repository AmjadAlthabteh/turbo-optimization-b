# TurboBuild Architecture

TurboBuild is a measurement-first C and C++ optimization CLI. It scans a target
project, detects compilers and build systems, runs correctness checks,
benchmarks candidate builds, and writes reports under `.turbobuild/results`.

The current codebase is intentionally simple: a tiny executable entry point, one
public application boundary, and one implementation file with clearly marked
sections. This keeps the tool easy to ship now while giving maintainers a clear
path to split the implementation into focused `.h` and `.cpp` modules.

## High-Level Layers

```text
User terminal
  |
  v
src/main.cpp
  |
  v
turbobuild::run_app(argc, argv)
  |
  v
Command dispatcher
  |
  +--> Project discovery
  +--> Compiler/tool discovery
  +--> Build orchestration
  +--> Diagnostics
  +--> Benchmarking
  +--> Reporting
  +--> CI workflow generation
```

## Source Layout Today

- `src/main.cpp`
  - The executable entry point.
  - Calls `turbobuild::run_app`.
  - Should not contain business logic, parsing logic, build logic, or reporting
    logic.

- `src/turbobuild.h`
  - The public boundary for the CLI application.
  - Exposes `turbobuild::run_app(int argc, char **argv)`.
  - Keeps `main.cpp` independent from implementation details.

- `src/turbobuild.cpp`
  - The current implementation unit.
  - Contains the command parser, command dispatcher, project scanner, compiler
    detector, benchmark runner, diagnostics, report writers, and workflow
    commands.
  - Uses section comments so maintainers can navigate it until the code is split
    into domain files.

- `docs/architecture.md`
  - Maintainer-facing architecture notes.

- `docs/optimization-roadmap.md`
  - Future optimization-engine roadmap.

- `docs/site/`
  - Static product webpage explaining what TurboBuild does and how teams use it.

## Core Data Models

These structs are the main internal contracts. They are currently private to
`src/turbobuild.cpp`, but they define the natural module boundaries.

- `Options`
  - Parsed command-line input.
  - Holds command name, project path, config, optimization goal, report format,
    benchmark command, run counts, safety opt-ins, strict mode, verbosity, and
    positional arguments.

- `ProjectInfo`
  - Result of scanning a target project.
  - Tracks root path, build systems, C/C++ source presence, tests, benchmarks,
    C++ standard, source files, headers, generated files, build outputs, and
    dependencies.

- `Compiler`
  - Represents a detected compiler.
  - Stores compiler id, C driver, C++ driver, version string, availability, and
    supported optimization flags.

- `BuildConfig`
  - A candidate build configuration.
  - Stores config name, compiler id, flags, unsafe status, and safety notes.

- `BenchmarkStats`
  - Runtime measurement summary.
  - Stores warmups, runs, failures, min, mean, median, standard deviation,
    p50/p90/p95/p99/p99.9, max, and throughput.

- `WarningIssue`
  - Parsed compiler warning or error.
  - Stores source location, warning category, severity, message, suggested fix,
    and whether it may affect correctness or performance.

- `ToolInfo`
  - Detected external tool state.
  - Used for static-analysis and profiling reports.

## Command Flow

1. The user runs a command such as:

   ```powershell
   turbobuild optimize --project . --goal speed --benchmark-command ".\app.exe"
   ```

2. `main.cpp` delegates to:

   ```cpp
   turbobuild::run_app(argc, argv)
   ```

3. `parse_args` converts raw CLI arguments into `Options`.

4. `run_app` dispatches to the matching `command_*` function.

5. The command function coordinates lower-level helpers.

6. Helpers execute tools, scan files, run builds, collect metrics, and write
   reports.

7. The command returns an exit code that can be used by shells or CI.

## Command Responsibilities

- `doctor`
  - Checks whether a project is ready for deeper optimization.
  - Detects build system support, compiler configs, tests, benchmarks, and
    optional tools.
  - Writes `.turbobuild/results/doctor.json`.

- `analyze`
  - Scans the project structure.
  - Detects sources, headers, generated files, build outputs, build systems,
    tests, benchmarks, C++ standard, compilers, and safe candidate configs.
  - Writes `.turbobuild/results/analysis.json`.

- `build`
  - Selects a named `BuildConfig`.
  - Builds the target project using CMake, Make, or direct compiler fallback.
  - Writes `.turbobuild/results/build-<config>.json`.

- `test`
  - Runs CTest or Make test when available.
  - Uses the isolated TurboBuild build directory.

- `warnings`
  - Runs syntax-only compiler checks with strict warning flags.
  - Parses warning output into structured `WarningIssue` records.
  - Writes `.turbobuild/results/warnings.json`.

- `sanitize`
  - Probes sanitizer support per compiler.
  - Builds sanitizer configs independently.
  - Runs tests or a supplied command when available.
  - Writes `.turbobuild/results/sanitize-*.json`.

- `static-analysis`
  - Detects analysis/profiling tools such as `clang-tidy`, `cppcheck`, `perf`,
    `valgrind`, `gprof`, `objdump`, `nm`, and `size`.
  - Runs local source heuristics for common C++ performance and maintainability
    issues.
  - Writes `.turbobuild/results/static-analysis.json`.

- `benchmark`
  - Runs a supplied command with warmups and repeated measured samples.
  - Computes latency and throughput metrics.
  - Writes `.turbobuild/results/benchmark.json`.

- `optimize`
  - Builds safe candidate configurations for a goal: `speed`, `size`, or
    `balanced`.
  - Benchmarks each successful build when a benchmark command is supplied.
  - Reports the best measured config only when data supports the claim.
  - Writes candidate benchmark files and an optimization summary.

- `compare`
  - Builds two compiler configs, commonly GCC and Clang.
  - Optionally benchmarks both with the same benchmark command.

- `profile`
  - Reports profiler/tool availability and the intended hardware/software
    counter set.
  - Can fall back to benchmark behavior when a benchmark command is supplied.

- `report`
  - Summarizes generated result files.
  - Can write a simple HTML report.

- `init-ci`
  - Generates a practical GitHub Actions workflow.
  - Builds TurboBuild, runs readiness checks, runs warning/static-analysis
    reports, and uploads `.turbobuild/results`.
  - Does not overwrite an existing workflow unless `--force` is supplied.

## Data Flow

```text
CLI args
  |
  v
Options
  |
  +--> ProjectInfo from analyze_project(project)
  |
  +--> Compiler list from detect_compilers()
  |
  +--> BuildConfig list from candidate_configs(options, compilers)
  |
  +--> command-specific execution
          |
          +--> process output
          +--> warning issues
          +--> sanitizer findings
          +--> benchmark stats
          +--> build metadata
          |
          v
      JSON or HTML reports in .turbobuild/results
```

## Filesystem Contract

TurboBuild writes target-project state into `.turbobuild`:

```text
.turbobuild/
  builds/
    gcc-o2/
    gcc-o3/
    clang-o2/
  results/
    analysis.json
    doctor.json
    warnings.json
    static-analysis.json
    benchmark.json
    build-gcc-o2.json
    optimize-summary.json
```

Build outputs and result files are intentionally isolated from the target
project source tree.

## Safety Model

TurboBuild separates safe default optimization work from semantic or portability
tradeoffs.

Safe by default:

- `-O0`
- `-Og`
- `-O1`
- `-O2`
- `-O3`
- `-Os`
- `-Oz`
- `-flto` when supported

Explicit opt-in only:

- `-Ofast`
- `-ffast-math`
- `-march=native`

The safety rule is simple: do not claim an optimization is better unless the
build passed and measurement data proves it.

## Recommended Future `.h/.cpp` Split

When `src/turbobuild.cpp` becomes too large to maintain comfortably, split it by
domain instead of by arbitrary helper type.

Recommended target layout:

```text
src/
  main.cpp
  turbobuild.h
  app.cpp
  cli.h
  cli.cpp
  process.h
  process.cpp
  project.h
  project.cpp
  compiler.h
  compiler.cpp
  build.h
  build.cpp
  benchmark.h
  benchmark.cpp
  diagnostics.h
  diagnostics.cpp
  reports.h
  reports.cpp
  commands.h
  commands.cpp
```

Suggested responsibilities:

- `app.cpp`
  - Owns `turbobuild::run_app`.
  - Handles top-level exception boundaries and command dispatch.

- `cli.h/.cpp`
  - Owns `Options`, `parse_args`, and `usage`.
  - Should not know how builds or benchmarks work.

- `process.h/.cpp`
  - Owns `CommandResult`, `shell_quote`, `run_capture`, and `run_passthrough`.
  - Centralizes shell/process behavior.

- `project.h/.cpp`
  - Owns `ProjectInfo`, `analyze_project`, path filtering, source/header
    discovery, and build-output detection.

- `compiler.h/.cpp`
  - Owns `Compiler`, compiler probing, flag probing, tool detection, supported
    flag caching, and compiler selection.

- `build.h/.cpp`
  - Owns `BuildConfig`, candidate config generation, goal filtering,
    `configure_and_build`, and build metadata.

- `benchmark.h/.cpp`
  - Owns `BenchmarkStats`, percentile math, benchmark command execution, and
    benchmark printing.

- `diagnostics.h/.cpp`
  - Owns warning flags, sanitizer names/flags, warning parsing, sanitizer
    finding parsing, and static-analysis heuristics.

- `reports.h/.cpp`
  - Owns JSON escaping and all report writers.
  - Keeps output schemas centralized.

- `commands.h/.cpp`
  - Owns `command_*` functions.
  - Coordinates modules without hiding major behavior inside helpers.

## Dependency Direction

Keep dependencies one-way:

```text
commands
  -> cli
  -> project
  -> compiler
  -> build
  -> benchmark
  -> diagnostics
  -> reports
  -> process
```

Avoid circular dependencies. For example, `benchmark` should not call
`command_optimize`, and `reports` should not run compilers.

## Adding A New Command

Use this checklist:

1. Add any new option parsing to `Options` and `parse_args`.
2. Add a focused `command_<name>` function.
3. Reuse existing helpers for paths, process execution, JSON escaping, compiler
   detection, and report output.
4. Wire the command in `run_app`.
5. Add help text in `usage`.
6. Document the command in `README.md`.
7. If the command writes output, write it under `.turbobuild/results`.
8. Build and run at least the help path and one command path.

## Maintainer Rules

- Keep `src/main.cpp` tiny.
- Keep user-facing commands deterministic and scriptable.
- Keep `.turbobuild/results` machine-readable.
- Do not silently enable unsafe compiler flags.
- Do not report speedups without benchmark data.
- Do not mix UI/product website code with CLI implementation code.
- Prefer small focused helpers over large hidden abstractions.
- Split modules when ownership becomes clear, not just because a file is long.

## Current Architecture Status

The project has completed the first architecture cleanup:

- `main.cpp` is no longer a large implementation file.
- The CLI has a public application boundary in `turbobuild.h`.
- The implementation has navigable sections.
- New real-world workflows exist through `doctor` and `init-ci`.

The next architecture improvement should be extracting the strongest natural
boundaries first:

1. `process.h/.cpp`
2. `cli.h/.cpp`
3. `project.h/.cpp`
4. `compiler.h/.cpp`
5. `reports.h/.cpp`

Those modules have clear responsibilities and low risk compared with splitting
all command behavior at once.
