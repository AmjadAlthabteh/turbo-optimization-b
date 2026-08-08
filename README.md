# TurboBuild

TurboBuild is a C++ command-line tool for analyzing, building, testing,
benchmarking, profiling, comparing, and optimizing C and C++ projects.

The core rule is measurement-first optimization: TurboBuild records baseline and
candidate metrics and will not report an optimization as an improvement unless
the benchmark data shows a real measured improvement.

## Build

```powershell
cmake -S . -B build
cmake --build build
```

The executable is generated as `build/turbobuild.exe` on Windows.

## Commands

```powershell
turbobuild analyze --project path\to\project
turbobuild build --project path\to\project --config gcc-o2
turbobuild test --project path\to\project
turbobuild warnings --project path\to\project --strict
turbobuild sanitize --project path\to\project
turbobuild static-analysis --project path\to\project
turbobuild benchmark --project path\to\project --runs 100 --command ".\app.exe"
turbobuild benchmark --project path\to\project --warmups 5 --runs 100 --command ".\app.exe"
turbobuild optimize --project path\to\project --goal speed --benchmark-command ".\app.exe"
turbobuild optimize --project path\to\project --goal size --benchmark-command ".\app.exe"
turbobuild compare gcc clang --project path\to\project --benchmark-command ".\app.exe"
turbobuild profile --project path\to\project
turbobuild report --project path\to\project --format html
turbobuild doctor --project path\to\project
turbobuild init-ci --project path\to\project
```

## Safety

Unsafe semantic-changing flags are disabled unless explicitly requested:

- `--allow-ofast`
- `--allow-fast-math`
- `--allow-native`

TurboBuild creates isolated build directories under `.turbobuild/builds` by
default and stores measurements under `.turbobuild/results`.

## Architecture

The executable entry point is intentionally small:

- `src/main.cpp` delegates to the application runner.
- `src/turbobuild.h` exposes the public CLI boundary.
- `src/turbobuild.cpp` contains the current implementation behind that boundary.

See `docs/architecture.md` for the command flow and maintainer notes.
That document also includes the deeper target `.h/.cpp` module split for CLI,
process execution, project discovery, compiler probing, builds, benchmarks,
diagnostics, reports, and command orchestration.

## Project Readiness And CI

`turbobuild doctor` writes `.turbobuild/results/doctor.json` and summarizes
whether the target project has a supported build system, available compiler
configs, tests, benchmarks, and analysis tools.

`turbobuild init-ci` creates `.github/workflows/turbobuild.yml` with a practical
GitHub Actions workflow for building TurboBuild, running `doctor`, collecting
warning/static-analysis reports, and uploading the result artifacts. Existing
workflow files are not overwritten unless `--force` is supplied.

## Warning Analysis

`turbobuild warnings` builds syntax checks with:

- `-Wall`
- `-Wextra`
- `-Wpedantic`
- `-Wconversion`
- `-Wsign-conversion`
- `-Wshadow`
- `-Wformat=2`
- `-Wundef`
- `-Wdouble-promotion`
- `-Wnull-dereference`
- `-Wold-style-cast`
- `-Woverloaded-virtual`
- `-Wnon-virtual-dtor`

Use `--strict` to add `-Werror`.

The warning report is written to `.turbobuild/results/warnings.json` and
contains warning count, category, file, line, severity, possible fix, and whether
the issue may affect correctness or performance.

## Sanitizer Testing

`turbobuild sanitize` creates isolated sanitizer builds with `-O1`, `-g`, and
`-fno-omit-frame-pointer`. Sanitizers are probed individually and unsupported
sanitizers are skipped instead of combined incorrectly.

Supported sanitizer build intents:

- AddressSanitizer: `-fsanitize=address`
- UndefinedBehaviorSanitizer: `-fsanitize=undefined`
- ThreadSanitizer: `-fsanitize=thread`
- LeakSanitizer: `-fsanitize=leak`
- MemorySanitizer when supported: `-fsanitize=memory`

Reports are written as `.turbobuild/results/sanitize-*.json` and summarize
invalid reads and writes, use-after-free, double deletion, buffer overflow,
undefined behavior, invalid shifts, data races, leaks, stack traces, and source
location availability when the sanitizer output contains them.

## Static And Optimization Analysis

`turbobuild static-analysis` detects available external tools:

- `clang-tidy`
- `cppcheck`
- `include-what-you-use`
- `perf`
- Valgrind tools
- `gprof`
- `objdump`
- `llvm-objdump`
- `nm`
- `size`

It also runs source heuristics for container, memory, cache, branch, loop, SIMD,
function-call, algorithm, multithreading, and I/O optimization opportunities.
The report is written to `.turbobuild/results/static-analysis.json`.

TurboBuild does not automatically rewrite code for these findings. It records
opportunities such as `vector::reserve`, `string_view`, `span`, hot/cold data
splitting, SoA layouts, branch predictability changes, loop fusion, vectorization
reports, thread scaling, batching, buffering, and binary-size reductions for
measured follow-up experiments.

## Benchmarking And Profiling

The benchmark runner supports warmups and measured runs:

- minimum runtime
- maximum runtime
- mean runtime
- median runtime
- standard deviation
- p50
- p90
- p95
- p99
- p99.9
- throughput / operations per second

System metrics such as memory usage, allocation count, text-section size, CPU
utilization, context switches, and page faults are represented in the JSON schema
and are filled when a platform profiler can measure them. Sanitizer builds are
not treated as production performance builds.

`turbobuild profile` records profiler/tool availability and the target hardware
counter set, including cycles, instructions, IPC, branches, branch misses, cache
references, cache misses, page faults, and context switches. It falls back
gracefully when hardware PMUs are unavailable, including WSL and virtual
machines.
