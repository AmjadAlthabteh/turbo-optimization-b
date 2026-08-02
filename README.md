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
turbobuild benchmark --project path\to\project --runs 100 --command ".\app.exe"
turbobuild optimize --project path\to\project --goal speed --benchmark-command ".\app.exe"
turbobuild optimize --project path\to\project --goal size --benchmark-command ".\app.exe"
turbobuild compare gcc clang --project path\to\project --benchmark-command ".\app.exe"
turbobuild profile --project path\to\project
turbobuild report --project path\to\project --format html
```

## Safety

Unsafe semantic-changing flags are disabled unless explicitly requested:

- `--allow-ofast`
- `--allow-fast-math`
- `--allow-native`

TurboBuild creates isolated build directories under `.turbobuild/builds` by
default and stores measurements under `.turbobuild/results`.
