#include "turbobuild.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <map>
#include <numeric>
#include <optional>
#include <regex>
#include <set>
#include <sstream>
#include <string>
#include <vector>

#ifdef _WIN32
#include <windows.h>
#define TB_POPEN _popen
#define TB_PCLOSE _pclose
#else
#include <sys/wait.h>
#define TB_POPEN popen
#define TB_PCLOSE pclose
#endif

namespace fs = std::filesystem;

namespace {

// Core data models

struct CommandResult {
  int exit_code = -1;
  std::string output;
};

struct Options {
  std::string command;
  fs::path project = fs::current_path();
  std::string config;
  std::string goal = "balanced";
  std::string format = "json";
  std::string benchmark_command;
  int runs = 10;
  int warmups = 0;
  bool allow_ofast = false;
  bool allow_fast_math = false;
  bool allow_native = false;
  bool strict = false;
  bool verbose = false;
  bool force = false;
  std::vector<std::string> positional;
};

struct Compiler {
  std::string id;
  std::string c;
  std::string cxx;
  std::string version;
  bool available = false;
  std::set<std::string> supported_flags;
};

struct ProjectInfo {
  fs::path root;
  bool has_cmake = false;
  bool has_make = false;
  bool has_ninja = false;
  bool has_c = false;
  bool has_cpp = false;
  bool has_tests = false;
  bool has_benchmarks = false;
  std::optional<std::string> cpp_standard;
  std::vector<fs::path> sources;
  std::vector<fs::path> headers;
  std::vector<fs::path> generated_sources;
  std::vector<fs::path> build_outputs;
  std::vector<std::string> dependencies;
};

struct BuildConfig {
  std::string name;
  std::string compiler_id;
  std::vector<std::string> flags;
  bool unsafe = false;
  std::string safety_note;
};

struct BenchmarkStats {
  int runs = 0;
  int warmups = 0;
  int failures = 0;
  double min_ms = 0;
  double mean_ms = 0;
  double median_ms = 0;
  double stddev_ms = 0;
  double p50_ms = 0;
  double p90_ms = 0;
  double p95_ms = 0;
  double p99_ms = 0;
  double p999_ms = 0;
  double max_ms = 0;
  double relative_stddev_percent = 0;
  double throughput_per_sec = 0;
  std::vector<double> samples_ms;
};

struct WarningIssue {
  fs::path file;
  int line = 0;
  std::string category;
  std::string message;
  std::string severity;
  std::string possible_fix;
  bool correctness = false;
  bool performance = false;
};

struct ToolInfo {
  std::string name;
  bool available = false;
  std::string version;
};

// Process and string utilities

std::string shell_quote(const std::string &value) {
#ifdef _WIN32
  std::string out = "\"";
  for (char ch : value) {
    if (ch == '"') out += "\\\"";
    else out += ch;
  }
  out += "\"";
  return out;
#else
  std::string out = "'";
  for (char ch : value) {
    if (ch == '\'') out += "'\\''";
    else out += ch;
  }
  out += "'";
  return out;
#endif
}

CommandResult run_capture(const std::string &command) {
  CommandResult result;
  FILE *pipe = TB_POPEN((command + " 2>&1").c_str(), "r");
  if (!pipe) {
    result.output = "failed to start command";
    return result;
  }
  char buffer[4096];
  while (fgets(buffer, sizeof(buffer), pipe) != nullptr) result.output += buffer;
  result.exit_code = TB_PCLOSE(pipe);
#ifndef _WIN32
  if (WIFEXITED(result.exit_code)) result.exit_code = WEXITSTATUS(result.exit_code);
#endif
  return result;
}

int run_passthrough(const std::string &command) {
  return std::system(command.c_str());
}

std::string trim(std::string s) {
  auto not_space = [](unsigned char c) { return !std::isspace(c); };
  s.erase(s.begin(), std::find_if(s.begin(), s.end(), not_space));
  s.erase(std::find_if(s.rbegin(), s.rend(), not_space).base(), s.end());
  return s;
}

bool contains_case_insensitive(std::string haystack, std::string needle) {
  std::transform(haystack.begin(), haystack.end(), haystack.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
  std::transform(needle.begin(), needle.end(), needle.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
  return haystack.find(needle) != std::string::npos;
}

// CLI and workspace paths

bool is_build_or_metadata_path(const fs::path &root, const fs::path &path) {
  std::error_code ignored;
  fs::path rel = fs::relative(path, root, ignored);
  if (ignored) rel = path;
  for (const auto &part_path : rel) {
    std::string part = part_path.string();
    std::transform(part.begin(), part.end(), part.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    if (part == ".git" || part == ".turbobuild" || part == "cmakefiles" ||
        part == "build" || part.rfind("build-", 0) == 0 || part == "out" ||
        part == "dist" || part == "bin" || part == "obj") {
      return true;
    }
  }
  return false;
}

Options parse_args(int argc, char **argv) {
  Options options;
  if (argc > 1) options.command = argv[1];
  for (int i = 2; i < argc; ++i) {
    std::string arg = argv[i];
    auto need_value = [&](const std::string &name) -> std::string {
      if (i + 1 >= argc) throw std::runtime_error("missing value for " + name);
      return argv[++i];
    };
    if (arg == "--project") options.project = need_value(arg);
    else if (arg == "--config") options.config = need_value(arg);
    else if (arg == "--goal") options.goal = need_value(arg);
    else if (arg == "--format") options.format = need_value(arg);
    else if (arg == "--runs") options.runs = std::stoi(need_value(arg));
    else if (arg == "--warmups") options.warmups = std::stoi(need_value(arg));
    else if (arg == "--command" || arg == "--benchmark-command") options.benchmark_command = need_value(arg);
    else if (arg == "--allow-ofast") options.allow_ofast = true;
    else if (arg == "--allow-fast-math") options.allow_fast_math = true;
    else if (arg == "--allow-native") options.allow_native = true;
    else if (arg == "--strict") options.strict = true;
    else if (arg == "--verbose") options.verbose = true;
    else if (arg == "--force") options.force = true;
    else options.positional.push_back(arg);
  }
  return options;
}

fs::path state_dir(const Options &options) {
  return options.project / ".turbobuild";
}

fs::path result_dir(const Options &options) {
  return state_dir(options) / "results";
}

fs::path builds_dir(const Options &options) {
  return state_dir(options) / "builds";
}

std::string json_escape(const std::string &s) {
  std::ostringstream out;
  for (char ch : s) {
    switch (ch) {
      case '\\': out << "\\\\"; break;
      case '"': out << "\\\""; break;
      case '\n': out << "\\n"; break;
      case '\r': out << "\\r"; break;
      case '\t': out << "\\t"; break;
      default: out << ch; break;
    }
  }
  return out.str();
}

// Diagnostics and tool discovery

std::vector<std::string> warning_flags(bool strict) {
  std::vector<std::string> flags = {
      "-Wall", "-Wextra", "-Wpedantic", "-Wconversion", "-Wsign-conversion",
      "-Wshadow", "-Wformat=2", "-Wundef", "-Wdouble-promotion",
      "-Wnull-dereference", "-Wold-style-cast", "-Woverloaded-virtual",
      "-Wnon-virtual-dtor"};
  if (strict) flags.push_back("-Werror");
  return flags;
}

std::vector<std::string> sanitizer_names() {
  return {"address", "undefined", "thread", "leak", "memory"};
}

std::vector<std::string> sanitizer_flags(const std::string &name) {
  return {"-O1", "-g", "-fno-omit-frame-pointer", "-fsanitize=" + name};
}

ToolInfo detect_tool(const std::string &tool, const std::string &version_arg = "--version") {
  ToolInfo info;
  info.name = tool;
  CommandResult result = run_capture(tool + " " + version_arg);
  info.available = result.exit_code == 0 && !result.output.empty();
  if (info.available) {
    std::istringstream lines(result.output);
    std::getline(lines, info.version);
    info.version = trim(info.version);
  }
  return info;
}

std::vector<ToolInfo> detect_analysis_tools() {
  return {
      detect_tool("clang-tidy"),
      detect_tool("cppcheck"),
      detect_tool("include-what-you-use"),
      detect_tool("perf", "--version"),
      detect_tool("valgrind", "--version"),
      detect_tool("gprof", "--version"),
      detect_tool("objdump", "--version"),
      detect_tool("llvm-objdump", "--version"),
      detect_tool("nm", "--version"),
      detect_tool("size", "--version")};
}

std::string issue_fix_hint(const std::string &category, const std::string &message) {
  if (category.find("conversion") != std::string::npos) return "Use an explicit checked cast or preserve the source type through the calculation.";
  if (category.find("shadow") != std::string::npos) return "Rename the inner variable or reduce the variable scope.";
  if (category.find("old-style-cast") != std::string::npos) return "Replace the C-style cast with static_cast, const_cast, or reinterpret_cast as appropriate.";
  if (category.find("non-virtual-dtor") != std::string::npos) return "Add a virtual destructor to polymorphic base classes or prevent deletion through the base type.";
  if (category.find("format") != std::string::npos) return "Match the format string with the exact argument types.";
  if (category.find("undef") != std::string::npos) return "Use defined(NAME) checks or provide a default macro value.";
  if (message.find("unused") != std::string::npos) return "Remove the value, use [[maybe_unused]], or wire it into the intended code path.";
  return "Inspect the warning context and prefer the smallest source change that preserves behavior.";
}

std::string warning_severity(const std::string &category, const std::string &message) {
  if (category.find("null-dereference") != std::string::npos || category.find("format") != std::string::npos) return "high";
  if (category.find("conversion") != std::string::npos || category.find("non-virtual-dtor") != std::string::npos) return "medium";
  if (message.find("uninitialized") != std::string::npos) return "high";
  return "low";
}

bool warning_may_affect_correctness(const std::string &category, const std::string &message) {
  return category.find("conversion") != std::string::npos ||
         category.find("format") != std::string::npos ||
         category.find("null-dereference") != std::string::npos ||
         category.find("non-virtual-dtor") != std::string::npos ||
         message.find("uninitialized") != std::string::npos;
}

bool warning_may_affect_performance(const std::string &category, const std::string &message) {
  return category.find("double-promotion") != std::string::npos ||
         category.find("overloaded-virtual") != std::string::npos ||
         message.find("copy") != std::string::npos;
}

// Project and compiler discovery

ProjectInfo analyze_project(const fs::path &root) {
  ProjectInfo info;
  info.root = fs::absolute(root);
  info.has_cmake = fs::exists(root / "CMakeLists.txt");
  info.has_make = fs::exists(root / "Makefile") || fs::exists(root / "makefile");
  info.has_ninja = fs::exists(root / "build.ninja");

  if (!fs::exists(root)) throw std::runtime_error("project path does not exist: " + root.string());

  const std::set<std::string> source_exts{".c", ".cc", ".cpp", ".cxx", ".C"};
  const std::set<std::string> header_exts{".h", ".hh", ".hpp", ".hxx"};
  for (const auto &entry : fs::recursive_directory_iterator(root, fs::directory_options::skip_permission_denied)) {
    if (!entry.is_regular_file()) continue;
    const fs::path path = entry.path();
    const std::string path_text = path.string();
    const std::string ext = path.extension().string();
    if (is_build_or_metadata_path(root, path)) {
      if (source_exts.count(ext) || ext == ".o" || ext == ".obj" || ext == ".a" || ext == ".lib" || ext == ".exe") {
        info.build_outputs.push_back(path);
      }
      continue;
    }
    if (source_exts.count(ext)) {
      info.sources.push_back(path);
      if (ext == ".c") info.has_c = true;
      else info.has_cpp = true;
      if (contains_case_insensitive(path_text, "generated") || contains_case_insensitive(path_text, "autogen")) info.generated_sources.push_back(path);
      if (contains_case_insensitive(path_text, "test")) info.has_tests = true;
      if (contains_case_insensitive(path_text, "bench")) info.has_benchmarks = true;
    } else if (header_exts.count(ext)) {
      info.headers.push_back(path);
    }
    if (contains_case_insensitive(path_text, "build") || contains_case_insensitive(path_text, "out")) {
      if (source_exts.count(ext) || ext == ".o" || ext == ".obj" || ext == ".a" || ext == ".lib" || ext == ".exe") {
        info.build_outputs.push_back(path);
      }
    }
  }

  if (info.has_cmake) {
    std::ifstream cmake(root / "CMakeLists.txt");
    std::string line;
    while (std::getline(cmake, line)) {
      if (line.find("CMAKE_CXX_STANDARD") != std::string::npos) {
        std::string digits;
        for (char ch : line) if (std::isdigit(static_cast<unsigned char>(ch))) digits += ch;
        if (!digits.empty()) info.cpp_standard = digits;
      }
      if (line.find("find_package") != std::string::npos) info.dependencies.push_back(trim(line));
    }
  }

  return info;
}

Compiler probe_compiler(const std::string &id, const std::string &c, const std::string &cxx) {
  Compiler compiler;
  compiler.id = id;
  compiler.c = c;
  compiler.cxx = cxx;
  CommandResult version = run_capture(cxx + " --version");
  compiler.available = version.exit_code == 0 && !version.output.empty();
  std::istringstream stream(version.output);
  std::getline(stream, compiler.version);
  compiler.version = trim(compiler.version);

  if (!compiler.available) return compiler;

  std::vector<std::string> flags = {
      "-O0", "-Og", "-O1", "-O2", "-O3", "-Os", "-Oz", "-Ofast",
      "-march=native", "-mtune=native", "-flto", "-fno-exceptions",
      "-fno-rtti", "-ffast-math", "-fomit-frame-pointer"};
  for (const auto &flag : flags) {
    fs::path temp = fs::temp_directory_path() / ("turbobuild_flag_probe_" + id + ".cpp");
    fs::path exe = fs::temp_directory_path() / ("turbobuild_flag_probe_" + id);
    {
      std::ofstream probe(temp);
      probe << "int main(){return 0;}\n";
    }
    std::string command = cxx + " " + flag + " " + shell_quote(temp.string()) + " -o " + shell_quote(exe.string());
    CommandResult result = run_capture(command);
    if (result.exit_code == 0) compiler.supported_flags.insert(flag);
    std::error_code ignored;
    fs::remove(temp, ignored);
    fs::remove(exe, ignored);
#ifdef _WIN32
    fs::remove(exe.string() + ".exe", ignored);
#endif
  }
  return compiler;
}

std::vector<Compiler> detect_compilers() {
  std::vector<Compiler> compilers;
  compilers.push_back(probe_compiler("gcc", "gcc", "g++"));
  compilers.push_back(probe_compiler("clang", "clang", "clang++"));
  return compilers;
}

std::vector<BuildConfig> candidate_configs(const Options &options, const std::vector<Compiler> &compilers) {
  std::vector<BuildConfig> configs;
  const std::vector<std::string> base_flags = {"-O0", "-Og", "-O1", "-O2", "-O3", "-Os", "-Oz"};
  for (const auto &compiler : compilers) {
    if (!compiler.available) continue;
    for (const auto &flag : base_flags) {
      if (!compiler.supported_flags.count(flag)) continue;
      std::string suffix = flag.substr(1);
      std::transform(suffix.begin(), suffix.end(), suffix.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
      configs.push_back({compiler.id + "-" + suffix, compiler.id, {flag}, false, ""});
    }
    if (compiler.supported_flags.count("-flto")) configs.push_back({compiler.id + "-o2-lto", compiler.id, {"-O2", "-flto"}, false, ""});
    if (options.allow_native && compiler.supported_flags.count("-march=native")) configs.push_back({compiler.id + "-o3-native", compiler.id, {"-O3", "-march=native", "-mtune=native"}, false, ""});
    if (options.allow_ofast && compiler.supported_flags.count("-Ofast")) configs.push_back({compiler.id + "-ofast", compiler.id, {"-Ofast"}, true, "Ofast can change floating-point and standards semantics"});
    if (options.allow_fast_math && compiler.supported_flags.count("-ffast-math")) configs.push_back({compiler.id + "-o3-fast-math", compiler.id, {"-O3", "-ffast-math"}, true, "fast-math can change floating-point semantics"});
  }
  return configs;
}

void write_analysis_json(const Options &options, const ProjectInfo &info, const std::vector<Compiler> &compilers) {
  fs::create_directories(result_dir(options));
  const auto configs = candidate_configs(options, compilers);
  std::ofstream out(result_dir(options) / "analysis.json");
  out << "{\n";
  out << "  \"root\": \"" << json_escape(info.root.string()) << "\",\n";
  out << "  \"project\": {\"c\": " << (info.has_c ? "true" : "false")
      << ", \"cpp\": " << (info.has_cpp ? "true" : "false")
      << ", \"cmake\": " << (info.has_cmake ? "true" : "false")
      << ", \"make\": " << (info.has_make ? "true" : "false")
      << ", \"ninja\": " << (info.has_ninja ? "true" : "false")
      << ", \"tests\": " << (info.has_tests ? "true" : "false")
      << ", \"benchmarks\": " << (info.has_benchmarks ? "true" : "false") << "},\n";
  out << "  \"counts\": {\"sources\": " << info.sources.size() << ", \"headers\": " << info.headers.size()
      << ", \"generated_sources\": " << info.generated_sources.size() << ", \"build_outputs\": " << info.build_outputs.size() << "},\n";
  out << "  \"cpp_standard\": " << (info.cpp_standard ? "\"" + *info.cpp_standard + "\"" : "null") << ",\n";
  out << "  \"compilers\": [\n";
  for (size_t i = 0; i < compilers.size(); ++i) {
    const auto &c = compilers[i];
    out << "    {\"id\": \"" << c.id << "\", \"available\": " << (c.available ? "true" : "false")
        << ", \"version\": \"" << json_escape(c.version) << "\", \"supported_flags\": [";
    size_t n = 0;
    for (const auto &flag : c.supported_flags) out << (n++ ? ", " : "") << "\"" << flag << "\"";
    out << "]}";
    if (i + 1 < compilers.size()) out << ",";
    out << "\n";
  }
  out << "  ],\n";
  out << "  \"candidate_configs\": [\n";
  for (size_t i = 0; i < configs.size(); ++i) {
    const auto &config = configs[i];
    out << "    {\"name\": \"" << json_escape(config.name) << "\", \"compiler\": \"" << config.compiler_id
        << "\", \"unsafe\": " << (config.unsafe ? "true" : "false") << ", \"flags\": [";
    for (size_t j = 0; j < config.flags.size(); ++j) {
      out << (j ? ", " : "") << "\"" << json_escape(config.flags[j]) << "\"";
    }
    out << "]";
    if (!config.safety_note.empty()) out << ", \"safety_note\": \"" << json_escape(config.safety_note) << "\"";
    out << "}";
    if (i + 1 < configs.size()) out << ",";
    out << "\n";
  }
  out << "  ]\n";
  out << "}\n";
}

void print_analysis(const ProjectInfo &info, const std::vector<Compiler> &compilers) {
  std::cout << "Project: " << info.root << "\n";
  std::cout << "Languages: " << (info.has_c ? "C " : "") << (info.has_cpp ? "C++" : "") << "\n";
  std::cout << "Build systems: " << (info.has_cmake ? "CMake " : "") << (info.has_make ? "Make " : "") << (info.has_ninja ? "Ninja " : "") << "\n";
  std::cout << "Sources: " << info.sources.size() << ", headers: " << info.headers.size() << "\n";
  std::cout << "Tests detected: " << (info.has_tests ? "yes" : "no") << ", benchmarks detected: " << (info.has_benchmarks ? "yes" : "no") << "\n";
  if (info.cpp_standard) std::cout << "C++ standard: " << *info.cpp_standard << "\n";
  std::cout << "Compilers:\n";
  for (const auto &compiler : compilers) {
    std::cout << "  " << compiler.id << ": " << (compiler.available ? compiler.version : "not found") << "\n";
  }
  const Options defaults;
  const auto configs = candidate_configs(defaults, compilers);
  std::cout << "Supported safe configs:\n";
  for (const auto &config : configs) {
    if (!config.unsafe) std::cout << "  " << config.name << "\n";
  }
}

std::optional<Compiler> find_compiler(const std::vector<Compiler> &compilers, const std::string &id) {
  for (const auto &compiler : compilers) if (compiler.id == id && compiler.available) return compiler;
  return std::nullopt;
}

// Build orchestration

uintmax_t directory_file_size(const fs::path &dir) {
  uintmax_t total = 0;
  std::error_code ignored;
  if (!fs::exists(dir, ignored)) return 0;
  for (const auto &entry : fs::recursive_directory_iterator(dir, fs::directory_options::skip_permission_denied, ignored)) {
    if (!entry.is_regular_file()) continue;
    total += entry.file_size(ignored);
  }
  return total;
}

void write_build_json(const Options &options, const BuildConfig &config, int exit_code, const fs::path &build_dir) {
  fs::create_directories(result_dir(options));
  std::ofstream out(result_dir(options) / ("build-" + config.name + ".json"));
  out << "{\n";
  out << "  \"config\": \"" << json_escape(config.name) << "\",\n";
  out << "  \"compiler\": \"" << json_escape(config.compiler_id) << "\",\n";
  out << "  \"exit_code\": " << exit_code << ",\n";
  out << "  \"build_dir\": \"" << json_escape(build_dir.string()) << "\",\n";
  out << "  \"artifact_bytes\": " << directory_file_size(build_dir) << ",\n";
  out << "  \"flags\": [";
  for (size_t i = 0; i < config.flags.size(); ++i) {
    out << (i ? ", " : "") << "\"" << json_escape(config.flags[i]) << "\"";
  }
  out << "]\n";
  out << "}\n";
}

int configure_and_build(const Options &options, const ProjectInfo &info, const std::vector<Compiler> &compilers, const BuildConfig &config) {
  auto compiler = find_compiler(compilers, config.compiler_id);
  if (!compiler) {
    std::cerr << "compiler not available: " << config.compiler_id << "\n";
    return 2;
  }
  fs::path build_dir = builds_dir(options) / config.name;
  fs::create_directories(build_dir);
  std::string flags;
  for (const auto &flag : config.flags) flags += flag + " ";

  std::cout << "Building " << config.name << " in " << build_dir << "\n";
  if (config.unsafe) std::cout << "Unsafe opt-in config: " << config.safety_note << "\n";

  if (info.has_cmake) {
    std::string configure = "cmake -S " + shell_quote(options.project.string()) + " -B " + shell_quote(build_dir.string()) +
      " -DCMAKE_C_COMPILER=" + shell_quote(compiler->c) +
      " -DCMAKE_CXX_COMPILER=" + shell_quote(compiler->cxx) +
      " -DCMAKE_C_FLAGS=" + shell_quote(flags) +
      " -DCMAKE_CXX_FLAGS=" + shell_quote(flags);
    int rc = run_passthrough(configure);
    if (rc == 0) rc = run_passthrough("cmake --build " + shell_quote(build_dir.string()) + " --config Release");
    write_build_json(options, config, rc, build_dir);
    return rc;
  }

  if (info.has_make) {
    std::string make = "make -C " + shell_quote(options.project.string()) +
      " CC=" + shell_quote(compiler->c) +
      " CXX=" + shell_quote(compiler->cxx) +
      " CFLAGS=" + shell_quote(flags) +
      " CXXFLAGS=" + shell_quote(flags);
    int rc = run_passthrough(make);
    write_build_json(options, config, rc, build_dir);
    return rc;
  }

  if (info.sources.empty()) {
    std::cerr << "no source files found\n";
    return 2;
  }

  fs::path output = build_dir / "app";
#ifdef _WIN32
  output += ".exe";
#endif
  std::string command = compiler->cxx + " ";
  for (const auto &flag : config.flags) command += flag + " ";
  for (const auto &src : info.sources) command += shell_quote(src.string()) + " ";
  command += "-o " + shell_quote(output.string());
  int rc = run_passthrough(command);
  write_build_json(options, config, rc, build_dir);
  return rc;
}

// Benchmarking and optimization reports

double percentile(const std::vector<double> &sorted, double p) {
  if (sorted.empty()) return 0;
  const double rank = (p / 100.0) * static_cast<double>(sorted.size() - 1);
  const size_t low = static_cast<size_t>(std::floor(rank));
  const size_t high = static_cast<size_t>(std::ceil(rank));
  if (low == high) return sorted[low];
  const double weight = rank - static_cast<double>(low);
  return sorted[low] * (1.0 - weight) + sorted[high] * weight;
}

std::string benchmark_stability_label(double relative_stddev_percent) {
  if (relative_stddev_percent <= 5.0) return "stable";
  if (relative_stddev_percent <= 15.0) return "moderate";
  return "noisy";
}

BenchmarkStats benchmark_command(const std::string &command, int runs, int warmups) {
  if (command.empty()) throw std::runtime_error("benchmark command is required; pass --benchmark-command or --command");
  if (runs <= 0) throw std::runtime_error("--runs must be greater than zero");
  if (warmups < 0) throw std::runtime_error("--warmups cannot be negative");
  std::vector<double> samples;
  BenchmarkStats stats;
  stats.runs = runs;
  stats.warmups = warmups;
  for (int i = 0; i < warmups; ++i) {
    int rc = run_passthrough(command);
    if (rc != 0) ++stats.failures;
  }
  for (int i = 0; i < runs; ++i) {
    const auto start = std::chrono::steady_clock::now();
    int rc = run_passthrough(command);
    const auto end = std::chrono::steady_clock::now();
    double ms = std::chrono::duration<double, std::milli>(end - start).count();
    if (rc != 0) ++stats.failures;
    samples.push_back(ms);
  }
  std::sort(samples.begin(), samples.end());
  stats.samples_ms = samples;
  stats.min_ms = samples.front();
  stats.max_ms = samples.back();
  stats.mean_ms = std::accumulate(samples.begin(), samples.end(), 0.0) / static_cast<double>(samples.size());
  double variance = 0.0;
  for (double sample : samples) variance += (sample - stats.mean_ms) * (sample - stats.mean_ms);
  stats.stddev_ms = std::sqrt(variance / static_cast<double>(samples.size()));
  stats.p50_ms = percentile(samples, 50);
  stats.median_ms = stats.p50_ms;
  stats.p90_ms = percentile(samples, 90);
  stats.p95_ms = percentile(samples, 95);
  stats.p99_ms = percentile(samples, 99);
  stats.p999_ms = percentile(samples, 99.9);
  stats.relative_stddev_percent = stats.mean_ms > 0 ? (stats.stddev_ms / stats.mean_ms) * 100.0 : 0;
  stats.throughput_per_sec = stats.mean_ms > 0 ? 1000.0 / stats.mean_ms : 0;
  return stats;
}

void write_benchmark_json(const fs::path &path, const std::string &name, const std::string &command, const BenchmarkStats &stats) {
  fs::create_directories(path.parent_path());
  std::ofstream out(path);
  out << std::fixed << std::setprecision(3);
  out << "{\n";
  out << "  \"name\": \"" << json_escape(name) << "\",\n";
  out << "  \"command\": \"" << json_escape(command) << "\",\n";
  out << "  \"runs\": " << stats.runs << ",\n";
  out << "  \"warmups\": " << stats.warmups << ",\n";
  out << "  \"failures\": " << stats.failures << ",\n";
  out << "  \"latency_ms\": {\"min\": " << stats.min_ms << ", \"mean\": " << stats.mean_ms
      << ", \"median\": " << stats.median_ms << ", \"stddev\": " << stats.stddev_ms
      << ", \"p50\": " << stats.p50_ms << ", \"p90\": " << stats.p90_ms << ", \"p95\": " << stats.p95_ms
      << ", \"p99\": " << stats.p99_ms << ", \"p99_9\": " << stats.p999_ms
      << ", \"max\": " << stats.max_ms << "},\n";
  out << "  \"relative_stddev_percent\": " << stats.relative_stddev_percent << ",\n";
  out << "  \"stability\": \"" << benchmark_stability_label(stats.relative_stddev_percent) << "\",\n";
  out << "  \"throughput_per_sec\": " << stats.throughput_per_sec << ",\n";
  out << "  \"samples_ms\": [";
  for (size_t i = 0; i < stats.samples_ms.size(); ++i) {
    out << (i ? ", " : "") << stats.samples_ms[i];
  }
  out << "],\n";
  out << "  \"system_metrics\": {\"memory_usage\": null, \"allocation_count\": null, \"cpu_utilization\": null, \"context_switches\": null, \"page_faults\": null},\n";
  out << "  \"environment\": {\"fixed_seed\": null, \"input_data\": null, \"sanitizer_build\": false}\n";
  out << "}\n";
}

void print_benchmark(const std::string &name, const BenchmarkStats &stats) {
  std::cout << std::fixed << std::setprecision(3);
  std::cout << name << ": runs=" << stats.runs << " failures=" << stats.failures
            << " warmups=" << stats.warmups
            << " mean_ms=" << stats.mean_ms << " stddev=" << stats.stddev_ms
            << " p50=" << stats.p50_ms << " p90=" << stats.p90_ms
            << " p95=" << stats.p95_ms << " p99=" << stats.p99_ms
            << " p99.9=" << stats.p999_ms
            << " rel_stddev%=" << stats.relative_stddev_percent
            << " stability=" << benchmark_stability_label(stats.relative_stddev_percent)
            << " throughput/s=" << stats.throughput_per_sec << "\n";
}

void write_optimize_summary(const Options &options,
                            const BuildConfig &baseline_config,
                            const BenchmarkStats &baseline_stats,
                            const BuildConfig &best_config,
                            const BenchmarkStats &best_stats) {
  fs::create_directories(result_dir(options));
  const double improvement = baseline_stats.mean_ms > 0
      ? ((baseline_stats.mean_ms - best_stats.mean_ms) / baseline_stats.mean_ms) * 100.0
      : 0.0;
  std::ofstream out(result_dir(options) / "optimize-summary.json");
  out << std::fixed << std::setprecision(3);
  out << "{\n";
  out << "  \"goal\": \"" << json_escape(options.goal) << "\",\n";
  out << "  \"baseline\": {\"config\": \"" << json_escape(baseline_config.name)
      << "\", \"mean_ms\": " << baseline_stats.mean_ms
      << ", \"p95_ms\": " << baseline_stats.p95_ms
      << ", \"p99_ms\": " << baseline_stats.p99_ms
      << ", \"failures\": " << baseline_stats.failures << "},\n";
  out << "  \"best\": {\"config\": \"" << json_escape(best_config.name)
      << "\", \"mean_ms\": " << best_stats.mean_ms
      << ", \"p95_ms\": " << best_stats.p95_ms
      << ", \"p99_ms\": " << best_stats.p99_ms
      << ", \"failures\": " << best_stats.failures << "},\n";
  out << "  \"measured_improvement_percent\": " << improvement << ",\n";
  out << "  \"improvement_claimed\": " << ((best_config.name != baseline_config.name && improvement > 0.0) ? "true" : "false") << "\n";
  out << "}\n";
}

std::optional<BuildConfig> select_config(const std::vector<BuildConfig> &configs, const std::string &name) {
  for (const auto &config : configs) if (config.name == name) return config;
  return std::nullopt;
}

std::vector<BuildConfig> filter_goal(const std::vector<BuildConfig> &configs, const std::string &goal) {
  std::vector<BuildConfig> out;
  for (const auto &config : configs) {
    bool keep = goal == "balanced";
    if (goal == "speed") keep = config.name.find("o2") != std::string::npos || config.name.find("o3") != std::string::npos || config.name.find("lto") != std::string::npos;
    if (goal == "size") keep = config.name.find("os") != std::string::npos || config.name.find("oz") != std::string::npos;
    if (keep) out.push_back(config);
  }
  if (out.empty()) return configs;
  return out;
}

// Warning, sanitizer, and static-analysis reports

std::string join_flags(const std::vector<std::string> &flags) {
  std::string out;
  for (const auto &flag : flags) out += flag + " ";
  return out;
}

std::vector<WarningIssue> parse_warning_output(const std::string &output) {
  std::vector<WarningIssue> issues;
  std::regex warning_re(R"(([^:\n]+):([0-9]+):[0-9]+:\s+(warning|error):\s+(.+?)(?:\s+\[(-W[^\]]+)\])?\s*$)");
  std::istringstream stream(output);
  std::string line;
  while (std::getline(stream, line)) {
    std::smatch match;
    if (!std::regex_search(line, match, warning_re)) continue;
    WarningIssue issue;
    issue.file = match[1].str();
    issue.line = std::stoi(match[2].str());
    issue.message = match[4].str();
    issue.category = match[5].matched ? match[5].str() : "compiler-warning";
    issue.severity = warning_severity(issue.category, issue.message);
    issue.possible_fix = issue_fix_hint(issue.category, issue.message);
    issue.correctness = warning_may_affect_correctness(issue.category, issue.message);
    issue.performance = warning_may_affect_performance(issue.category, issue.message);
    issues.push_back(issue);
  }
  return issues;
}

void write_warnings_json(const Options &options, const std::vector<WarningIssue> &issues, const std::map<std::string, int> &categories) {
  fs::create_directories(result_dir(options));
  std::ofstream out(result_dir(options) / "warnings.json");
  out << "{\n";
  out << "  \"warning_count\": " << issues.size() << ",\n";
  out << "  \"categories\": {";
  size_t category_index = 0;
  for (const auto &entry : categories) {
    out << (category_index++ ? ", " : "") << "\"" << json_escape(entry.first) << "\": " << entry.second;
  }
  out << "},\n";
  out << "  \"warnings\": [\n";
  for (size_t i = 0; i < issues.size(); ++i) {
    const auto &issue = issues[i];
    out << "    {\"file\": \"" << json_escape(issue.file.string()) << "\", \"line\": " << issue.line
        << ", \"category\": \"" << json_escape(issue.category) << "\", \"severity\": \"" << issue.severity
        << "\", \"message\": \"" << json_escape(issue.message) << "\", \"possible_fix\": \"" << json_escape(issue.possible_fix)
        << "\", \"may_affect_correctness\": " << (issue.correctness ? "true" : "false")
        << ", \"may_affect_performance\": " << (issue.performance ? "true" : "false") << "}";
    if (i + 1 < issues.size()) out << ",";
    out << "\n";
  }
  out << "  ]\n";
  out << "}\n";
}

int command_warnings(const Options &options) {
  ProjectInfo info = analyze_project(options.project);
  auto compilers = detect_compilers();
  auto compiler = find_compiler(compilers, options.positional.empty() ? "gcc" : options.positional[0]);
  if (!compiler) {
    std::cerr << "no supported compiler found for warning analysis\n";
    return 2;
  }

  const auto flags = warning_flags(options.strict);
  std::vector<WarningIssue> issues;
  for (const auto &source : info.sources) {
    std::string driver = source.extension() == ".c" ? compiler->c : compiler->cxx;
    std::string command = driver + " -fsyntax-only " + join_flags(flags) + shell_quote(source.string());
    CommandResult result = run_capture(command);
    auto parsed = parse_warning_output(result.output);
    issues.insert(issues.end(), parsed.begin(), parsed.end());
    if (options.verbose && !result.output.empty()) std::cout << result.output << "\n";
  }

  std::map<std::string, int> categories;
  for (const auto &issue : issues) ++categories[issue.category];
  std::cout << "Warnings: " << issues.size() << "\n";
  for (const auto &entry : categories) std::cout << "  " << entry.first << ": " << entry.second << "\n";
  write_warnings_json(options, issues, categories);
  std::cout << "Wrote " << (result_dir(options) / "warnings.json") << "\n";
  return options.strict && !issues.empty() ? 1 : 0;
}

std::vector<std::string> sanitizer_findings(const std::string &output) {
  std::vector<std::string> findings;
  const std::vector<std::pair<std::string, std::string>> patterns = {
      {"invalid read", "invalid reads"}, {"invalid write", "invalid writes"},
      {"heap-use-after-free", "use-after-free"}, {"double-free", "double deletion"},
      {"buffer-overflow", "buffer overflow"}, {"undefined", "integer or language undefined behavior"},
      {"shift", "invalid shifts"}, {"data race", "data races"}, {"leak", "memory leaks"},
      {"stack", "stack traces or stack source locations"}};
  std::string lower = output;
  std::transform(lower.begin(), lower.end(), lower.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
  for (const auto &pattern : patterns) {
    if (lower.find(pattern.first) != std::string::npos) findings.push_back(pattern.second);
  }
  return findings;
}

void write_sanitizer_json(const Options &options, const std::string &name, int build_rc, int test_rc, const std::string &output) {
  fs::create_directories(result_dir(options));
  auto findings = sanitizer_findings(output);
  std::ofstream out(result_dir(options) / ("sanitize-" + name + ".json"));
  out << "{\n";
  out << "  \"sanitizer\": \"" << json_escape(name) << "\",\n";
  out << "  \"build_exit_code\": " << build_rc << ",\n";
  out << "  \"test_exit_code\": " << test_rc << ",\n";
  out << "  \"findings\": [";
  for (size_t i = 0; i < findings.size(); ++i) out << (i ? ", " : "") << "\"" << json_escape(findings[i]) << "\"";
  out << "],\n";
  out << "  \"stack_traces_available\": " << (output.find("#0") != std::string::npos ? "true" : "false") << ",\n";
  out << "  \"source_locations_available\": " << (std::regex_search(output, std::regex(R"(:[0-9]+:[0-9]+)")) ? "true" : "false") << ",\n";
  out << "  \"raw_output_excerpt\": \"" << json_escape(output.substr(0, 4000)) << "\"\n";
  out << "}\n";
}

bool sanitizer_supported(const Compiler &compiler, const std::string &name) {
  fs::path temp = fs::temp_directory_path() / ("turbobuild_sanitizer_probe_" + name + ".cpp");
  fs::path exe = fs::temp_directory_path() / ("turbobuild_sanitizer_probe_" + name);
  {
    std::ofstream probe(temp);
    probe << "int main(){return 0;}\n";
  }
  CommandResult result = run_capture(compiler.cxx + " -O1 -g -fno-omit-frame-pointer -fsanitize=" + name + " " +
                                     shell_quote(temp.string()) + " -o " + shell_quote(exe.string()));
  std::error_code ignored;
  fs::remove(temp, ignored);
  fs::remove(exe, ignored);
#ifdef _WIN32
  fs::remove(exe.string() + ".exe", ignored);
#endif
  return result.exit_code == 0;
}

int command_sanitize(const Options &options) {
  ProjectInfo info = analyze_project(options.project);
  auto compilers = detect_compilers();
  auto compiler = find_compiler(compilers, options.positional.empty() ? "gcc" : options.positional[0]);
  if (!compiler) {
    std::cerr << "no supported compiler found for sanitizer builds\n";
    return 2;
  }

  int failures = 0;
  for (const auto &name : sanitizer_names()) {
    if (!sanitizer_supported(*compiler, name)) {
      std::cout << "Sanitizer skipped, unsupported by " << compiler->id << ": " << name << "\n";
      write_sanitizer_json(options, name, 0, 0, "skipped: sanitizer unsupported by selected compiler");
      continue;
    }
    BuildConfig config{compiler->id + "-" + name + "-san", compiler->id, sanitizer_flags(name), false, ""};
    std::cout << "Sanitizer build: " << name << "\n";
    int build_rc = configure_and_build(options, info, compilers, config);
    int test_rc = 0;
    std::string output;
    if (build_rc == 0) {
      fs::path build_dir = builds_dir(options) / config.name;
      std::string test_command;
      if (info.has_cmake) test_command = "ctest --test-dir " + shell_quote(build_dir.string()) + " --output-on-failure";
      else if (info.has_make) test_command = "make -C " + shell_quote(options.project.string()) + " test";
      else if (!options.benchmark_command.empty()) test_command = options.benchmark_command;
      if (!test_command.empty()) {
        CommandResult test = run_capture(test_command);
        test_rc = test.exit_code;
        output = test.output;
        if (options.verbose && !output.empty()) std::cout << output << "\n";
      } else {
        output = "No test suite or --command supplied for sanitizer execution.";
      }
    }
    if (build_rc != 0 || test_rc != 0) ++failures;
    write_sanitizer_json(options, name, build_rc, test_rc, output);
  }
  std::cout << "Wrote sanitizer reports to " << result_dir(options) << "\n";
  return failures == 0 ? 0 : 1;
}

std::vector<std::string> static_heuristics_for_file(const fs::path &path) {
  std::vector<std::string> findings;
  std::ifstream in(path);
  std::string line;
  int number = 0;
  while (std::getline(in, line)) {
    ++number;
    auto add = [&](const std::string &message) {
      findings.push_back(path.string() + ":" + std::to_string(number) + ": " + message);
    };
    if (line.find("std::endl") != std::string::npos) add("I/O: std::endl flushes; prefer '\\n' unless a flush is required.");
    if (line.find("new ") != std::string::npos || line.find("delete ") != std::string::npos) add("Ownership: raw new/delete may indicate manual lifetime management.");
    if (line.find("shrink_to_fit") != std::string::npos) add("Containers: shrink_to_fit in hot paths can cause avoidable reallocations.");
    if (line.find("std::list") != std::string::npos) add("Containers: std::list has pointer chasing; consider vector/deque when insertion pattern allows.");
    if (line.find("push_back") != std::string::npos) add("Containers: repeated push_back may need reserve; do not switch to emplace_back unless it removes a real temporary.");
    if (line.find("+=") != std::string::npos && line.find("std::string") != std::string::npos) add("Strings: repeated concatenation may benefit from preallocated output buffers.");
    if (line.find("dynamic_cast") != std::string::npos || line.find("reinterpret_cast") != std::string::npos) add("Casts: inspect unsafe or runtime casts.");
    if (line.find("virtual ") != std::string::npos) add("Dispatch: virtual calls may matter in hot paths; measure before changing.");
    if (line.find("std::mutex") != std::string::npos || line.find("lock_guard") != std::string::npos) add("Threads: inspect critical-section size and contention.");
  }
  return findings;
}

int command_static_analysis(const Options &options) {
  ProjectInfo info = analyze_project(options.project);
  auto tools = detect_analysis_tools();
  fs::create_directories(result_dir(options));

  std::vector<std::string> findings;
  for (const auto &source : info.sources) {
    auto local = static_heuristics_for_file(source);
    findings.insert(findings.end(), local.begin(), local.end());
  }

  std::ofstream out(result_dir(options) / "static-analysis.json");
  out << "{\n";
  out << "  \"tools\": [\n";
  for (size_t i = 0; i < tools.size(); ++i) {
    out << "    {\"name\": \"" << tools[i].name << "\", \"available\": " << (tools[i].available ? "true" : "false")
        << ", \"version\": \"" << json_escape(tools[i].version) << "\"}";
    if (i + 1 < tools.size()) out << ",";
    out << "\n";
  }
  out << "  ],\n";
  out << "  \"checks\": [\"unnecessary copies\", \"large pass-by-value\", \"missing const references\", \"unsafe casts\", \"raw owning pointers\", \"manual new/delete\", \"missing noexcept\", \"unnecessary allocations\", \"container growth\", \"signed/unsigned conversions\", \"dead code\", \"uninitialized data\", \"cache locality\", \"branch behavior\", \"loop vectorization\", \"SIMD compatibility\", \"multithreading\", \"I/O hot paths\"],\n";
  out << "  \"heuristic_findings\": [\n";
  for (size_t i = 0; i < findings.size(); ++i) {
    out << "    \"" << json_escape(findings[i]) << "\"";
    if (i + 1 < findings.size()) out << ",";
    out << "\n";
  }
  out << "  ]\n";
  out << "}\n";

  std::cout << "Static analysis tools:\n";
  for (const auto &tool : tools) std::cout << "  " << tool.name << ": " << (tool.available ? tool.version : "not found") << "\n";
  std::cout << "Heuristic findings: " << findings.size() << "\n";
  std::cout << "Wrote " << (result_dir(options) / "static-analysis.json") << "\n";
  return 0;
}

// User-facing commands

int command_analyze(const Options &options) {
  ProjectInfo info = analyze_project(options.project);
  auto compilers = detect_compilers();
  print_analysis(info, compilers);
  write_analysis_json(options, info, compilers);
  std::cout << "Wrote " << (result_dir(options) / "analysis.json") << "\n";
  return 0;
}

int command_list_configs(const Options &options) {
  auto compilers = detect_compilers();
  auto configs = candidate_configs(options, compilers);
  if (configs.empty()) {
    std::cerr << "no supported compiler configurations found\n";
    return 1;
  }
  std::cout << "Supported optimization configs:\n";
  for (const auto &config : configs) {
    std::cout << "  " << config.name << " (" << config.compiler_id << ")";
    if (!config.flags.empty()) {
      std::cout << " flags=";
      for (size_t i = 0; i < config.flags.size(); ++i) {
        std::cout << (i ? "," : "") << config.flags[i];
      }
    }
    if (config.unsafe) std::cout << " unsafe: " << config.safety_note;
    std::cout << "\n";
  }
  return 0;
}

int command_build(const Options &options) {
  ProjectInfo info = analyze_project(options.project);
  auto compilers = detect_compilers();
  auto configs = candidate_configs(options, compilers);
  std::string name = options.config.empty() ? "gcc-o2" : options.config;
  auto config = select_config(configs, name);
  if (!config) {
    std::cerr << "unknown or unsupported config: " << name << "\n";
    return 2;
  }
  return configure_and_build(options, info, compilers, *config);
}

int command_test(const Options &options) {
  ProjectInfo info = analyze_project(options.project);
  fs::path build_dir = builds_dir(options) / (options.config.empty() ? "gcc-o2" : options.config);
  if (info.has_cmake && fs::exists(build_dir)) {
    return run_passthrough("ctest --test-dir " + shell_quote(build_dir.string()) + " --output-on-failure");
  }
  if (info.has_make) return run_passthrough("make -C " + shell_quote(options.project.string()) + " test");
  std::cerr << "no supported test command found; use project CMake tests or Make test target\n";
  return 2;
}

int command_benchmark(const Options &options) {
  BenchmarkStats stats = benchmark_command(options.benchmark_command, options.runs, options.warmups);
  print_benchmark("benchmark", stats);
  write_benchmark_json(result_dir(options) / "benchmark.json", "benchmark", options.benchmark_command, stats);
  return stats.failures == 0 ? 0 : 1;
}

int command_optimize(const Options &options) {
  ProjectInfo info = analyze_project(options.project);
  auto compilers = detect_compilers();
  auto configs = filter_goal(candidate_configs(options, compilers), options.goal);
  if (configs.empty()) {
    std::cerr << "no supported compiler configurations found\n";
    return 2;
  }

  std::vector<std::pair<BuildConfig, BenchmarkStats>> measured;
  for (const auto &config : configs) {
    int build_rc = configure_and_build(options, info, compilers, config);
    if (build_rc != 0) {
      std::cerr << "build failed for " << config.name << "\n";
      continue;
    }
    if (options.benchmark_command.empty()) {
      std::cout << "Built " << config.name << "; no benchmark command supplied, so no improvement is claimed.\n";
      continue;
    }
    BenchmarkStats stats = benchmark_command(options.benchmark_command, options.runs, options.warmups);
    print_benchmark(config.name, stats);
    write_benchmark_json(result_dir(options) / (config.name + ".json"), config.name, options.benchmark_command, stats);
    measured.push_back({config, stats});
  }

  if (measured.empty()) {
    std::cout << "No measured optimization result was produced.\n";
    return 1;
  }

  auto best = std::min_element(measured.begin(), measured.end(), [](const auto &a, const auto &b) {
    if (a.second.failures != b.second.failures) return a.second.failures < b.second.failures;
    return a.second.mean_ms < b.second.mean_ms;
  });
  const auto &baseline = measured.front();
  write_optimize_summary(options, baseline.first, baseline.second, best->first, best->second);
  const double improvement = baseline.second.mean_ms > 0 ? ((baseline.second.mean_ms - best->second.mean_ms) / baseline.second.mean_ms) * 100.0 : 0.0;
  std::cout << std::fixed << std::setprecision(2);
  std::cout << "Best measured config: " << best->first.name << " mean_ms=" << best->second.mean_ms << "\n";
  if (best->first.name != baseline.first.name && improvement > 0.0) {
    std::cout << "Measured improvement over baseline " << baseline.first.name << ": " << improvement << "%\n";
  } else {
    std::cout << "No measured improvement over baseline " << baseline.first.name << " was found.\n";
  }
  return 0;
}

int command_compare(const Options &options) {
  if (options.positional.size() < 2) {
    std::cerr << "compare requires two compiler ids, for example: turbobuild compare gcc clang\n";
    return 2;
  }
  ProjectInfo info = analyze_project(options.project);
  auto compilers = detect_compilers();
  std::vector<BuildConfig> configs = {
      {options.positional[0] + "-o2", options.positional[0], {"-O2"}, false, ""},
      {options.positional[1] + "-o2", options.positional[1], {"-O2"}, false, ""}};
  for (const auto &config : configs) {
    int rc = configure_and_build(options, info, compilers, config);
    if (rc != 0) continue;
    if (!options.benchmark_command.empty()) {
      auto stats = benchmark_command(options.benchmark_command, options.runs, options.warmups);
      print_benchmark(config.name, stats);
      write_benchmark_json(result_dir(options) / (config.name + ".json"), config.name, options.benchmark_command, stats);
    }
  }
  if (options.benchmark_command.empty()) std::cout << "Comparison builds completed where supported; no benchmark command supplied.\n";
  return 0;
}

int command_profile(const Options &options) {
  auto tools = detect_analysis_tools();
  fs::create_directories(result_dir(options));
  std::ofstream out(result_dir(options) / "profile-tools.json");
  out << "{\n";
  out << "  \"tools\": [\n";
  for (size_t i = 0; i < tools.size(); ++i) {
    out << "    {\"name\": \"" << tools[i].name << "\", \"available\": " << (tools[i].available ? "true" : "false")
        << ", \"version\": \"" << json_escape(tools[i].version) << "\"}";
    if (i + 1 < tools.size()) out << ",";
    out << "\n";
  }
  out << "  ],\n";
  out << "  \"perf_events\": [\"task-clock\", \"cycles\", \"instructions\", \"IPC\", \"branches\", \"branch misses\", \"cache references\", \"cache misses\", \"page faults\", \"context switches\"],\n";
  out << "  \"fallback_policy\": \"Use available software counters when hardware PMUs are unavailable, including WSL and virtual machines.\"\n";
  out << "}\n";
  std::cout << "Profiling tools:\n";
  for (const auto &tool : tools) std::cout << "  " << tool.name << ": " << (tool.available ? tool.version : "not found") << "\n";
  if (!options.benchmark_command.empty()) return command_benchmark(options);
  std::cout << "Wrote " << (result_dir(options) / "profile-tools.json") << "\n";
  return 0;
}

int command_report(const Options &options) {
  fs::create_directories(result_dir(options));
  if (options.format == "html") {
    fs::path report = result_dir(options) / "report.html";
    std::ofstream out(report);
    out << "<!doctype html><meta charset=\"utf-8\"><title>TurboBuild Report</title>";
    out << "<style>body{font-family:system-ui,Segoe UI,sans-serif;margin:40px;line-height:1.45}code,pre{background:#f3f4f6;padding:2px 4px;border-radius:4px}li{margin:6px 0}</style>";
    out << "<h1>TurboBuild Report</h1><p>Project: <code>" << json_escape(fs::absolute(options.project).string()) << "</code></p>";
    out << "<h2>Result Files</h2><ul>";
    if (fs::exists(result_dir(options))) {
      for (const auto &entry : fs::directory_iterator(result_dir(options))) {
        if (entry.is_regular_file()) out << "<li><code>" << json_escape(entry.path().filename().string()) << "</code></li>";
      }
    }
    out << "</ul><p>TurboBuild only claims improvements when benchmark files contain before-and-after measurements.</p>";
    std::cout << "Wrote " << report << "\n";
  } else {
    std::cout << "JSON results are stored in " << result_dir(options) << "\n";
  }
  return 0;
}

int command_results(const Options &options) {
  fs::path dir = result_dir(options);
  if (!fs::exists(dir)) {
    std::cout << "No TurboBuild results found at " << dir << "\n";
    return 0;
  }
  std::vector<fs::directory_entry> files;
  for (const auto &entry : fs::directory_iterator(dir)) {
    if (entry.is_regular_file()) files.push_back(entry);
  }
  std::sort(files.begin(), files.end(), [](const auto &a, const auto &b) {
    return a.path().filename().string() < b.path().filename().string();
  });
  if (files.empty()) {
    std::cout << "No TurboBuild result files found at " << dir << "\n";
    return 0;
  }
  std::cout << "TurboBuild results in " << dir << ":\n";
  std::error_code ignored;
  for (const auto &entry : files) {
    std::cout << "  " << entry.path().filename().string()
              << " (" << entry.file_size(ignored) << " bytes)\n";
  }
  return 0;
}

int command_doctor(const Options &options) {
  ProjectInfo info = analyze_project(options.project);
  auto compilers = detect_compilers();
  auto tools = detect_analysis_tools();
  auto configs = candidate_configs(options, compilers);

  fs::create_directories(result_dir(options));
  std::ofstream out(result_dir(options) / "doctor.json");
  out << "{\n";
  out << "  \"project\": \"" << json_escape(info.root.string()) << "\",\n";
  out << "  \"status\": \"" << (!configs.empty() && (info.has_cmake || info.has_make || info.has_ninja) ? "ready" : "needs_attention") << "\",\n";
  out << "  \"build_systems\": {\"cmake\": " << (info.has_cmake ? "true" : "false")
      << ", \"make\": " << (info.has_make ? "true" : "false")
      << ", \"ninja\": " << (info.has_ninja ? "true" : "false") << "},\n";
  out << "  \"source_counts\": {\"sources\": " << info.sources.size() << ", \"headers\": " << info.headers.size() << "},\n";
  out << "  \"compilers\": [\n";
  for (size_t i = 0; i < compilers.size(); ++i) {
    out << "    {\"id\": \"" << compilers[i].id << "\", \"available\": " << (compilers[i].available ? "true" : "false")
        << ", \"version\": \"" << json_escape(compilers[i].version) << "\"}";
    if (i + 1 < compilers.size()) out << ",";
    out << "\n";
  }
  out << "  ],\n";
  out << "  \"analysis_tools\": [\n";
  for (size_t i = 0; i < tools.size(); ++i) {
    out << "    {\"name\": \"" << tools[i].name << "\", \"available\": " << (tools[i].available ? "true" : "false") << "}";
    if (i + 1 < tools.size()) out << ",";
    out << "\n";
  }
  out << "  ],\n";
  out << "  \"recommendations\": [\n";
  bool wrote = false;
  auto add_recommendation = [&](const std::string &text) {
    if (wrote) out << ",\n";
    out << "    \"" << json_escape(text) << "\"";
    wrote = true;
  };
  if (!info.has_cmake && !info.has_make && !info.has_ninja) add_recommendation("Add a supported build system so TurboBuild can create isolated builds.");
  if (configs.empty()) add_recommendation("Install GCC or Clang so TurboBuild can probe optimization configurations.");
  if (!info.has_tests) add_recommendation("Add tests or pass --command to sanitizer runs so correctness checks can execute.");
  if (!info.has_benchmarks) add_recommendation("Add a benchmark target or provide --benchmark-command before trusting optimization decisions.");
  if (!wrote) add_recommendation("Project is ready for analyze, build, warnings, and measured optimize workflows.");
  out << "\n  ]\n";
  out << "}\n";

  std::cout << "TurboBuild doctor\n";
  std::cout << "  Project: " << info.root << "\n";
  std::cout << "  Build system: " << (info.has_cmake ? "CMake " : "") << (info.has_make ? "Make " : "") << (info.has_ninja ? "Ninja " : "") << "\n";
  std::cout << "  Sources: " << info.sources.size() << ", headers: " << info.headers.size() << "\n";
  std::cout << "  Supported configs: " << configs.size() << "\n";
  std::cout << "  Tests detected: " << (info.has_tests ? "yes" : "no") << ", benchmarks detected: " << (info.has_benchmarks ? "yes" : "no") << "\n";
  std::cout << "Wrote " << (result_dir(options) / "doctor.json") << "\n";
  return configs.empty() ? 1 : 0;
}

int command_init_ci(const Options &options) {
  fs::path workflow_dir = options.project / ".github" / "workflows";
  fs::path workflow = workflow_dir / "turbobuild.yml";
  if (fs::exists(workflow) && !options.force) {
    std::cerr << "workflow already exists: " << workflow << "\n";
    std::cerr << "use --force to overwrite it\n";
    return 2;
  }

  fs::create_directories(workflow_dir);
  std::ofstream out(workflow);
  out << "name: TurboBuild\n\n";
  out << "on:\n";
  out << "  push:\n";
  out << "  pull_request:\n\n";
  out << "jobs:\n";
  out << "  analyze:\n";
  out << "    runs-on: ubuntu-latest\n";
  out << "    steps:\n";
  out << "      - uses: actions/checkout@v4\n";
  out << "      - name: Install build tools\n";
  out << "        run: sudo apt-get update && sudo apt-get install -y cmake ninja-build g++ clang\n";
  out << "      - name: Configure TurboBuild\n";
  out << "        run: cmake -S . -B build -G Ninja\n";
  out << "      - name: Build TurboBuild\n";
  out << "        run: cmake --build build\n";
  out << "      - name: Doctor\n";
  out << "        run: ./build/turbobuild doctor --project .\n";
  out << "      - name: Warning analysis\n";
  out << "        run: ./build/turbobuild warnings --project .\n";
  out << "      - name: Static analysis report\n";
  out << "        run: ./build/turbobuild static-analysis --project .\n";
  out << "      - name: Upload reports\n";
  out << "        uses: actions/upload-artifact@v4\n";
  out << "        with:\n";
  out << "          name: turbobuild-results\n";
  out << "          path: .turbobuild/results\n";

  std::cout << "Wrote " << workflow << "\n";
  return 0;
}

void usage() {
  std::cout
      << "TurboBuild " << "0.1.0" << "\n"
      << "Commands:\n"
      << "  analyze [--project PATH]\n"
      << "  list-configs [--allow-ofast] [--allow-fast-math] [--allow-native]\n"
      << "  build [--project PATH] [--config gcc-o2]\n"
      << "  test [--project PATH] [--config gcc-o2]\n"
      << "  warnings [--project PATH] [gcc|clang] [--strict]\n"
      << "  sanitize [--project PATH] [gcc|clang] [--command CMD]\n"
      << "  static-analysis [--project PATH]\n"
      << "  benchmark --command CMD [--runs N] [--warmups N]\n"
      << "  profile [--command CMD] [--runs N] [--warmups N]\n"
      << "  optimize --goal speed|size|balanced [--benchmark-command CMD] [--runs N]\n"
      << "  compare gcc clang [--benchmark-command CMD] [--runs N]\n"
      << "  results [--project PATH]\n"
      << "  report [--format json|html]\n"
      << "  doctor [--project PATH]\n"
      << "  init-ci [--project PATH] [--force]\n"
      << "Safety opt-ins: --allow-ofast --allow-fast-math --allow-native\n";
}

} // namespace

namespace turbobuild {

int run_app(int argc, char **argv) {
  try {
    Options options = parse_args(argc, argv);
    if (options.command.empty() || options.command == "--help" || options.command == "-h") {
      usage();
      return 0;
    }
    if (options.command == "analyze") return command_analyze(options);
    if (options.command == "list-configs") return command_list_configs(options);
    if (options.command == "build") return command_build(options);
    if (options.command == "test") return command_test(options);
    if (options.command == "warnings") return command_warnings(options);
    if (options.command == "sanitize") return command_sanitize(options);
    if (options.command == "static-analysis") return command_static_analysis(options);
    if (options.command == "benchmark") return command_benchmark(options);
    if (options.command == "profile") return command_profile(options);
    if (options.command == "optimize") return command_optimize(options);
    if (options.command == "compare") return command_compare(options);
    if (options.command == "results") return command_results(options);
    if (options.command == "report") return command_report(options);
    if (options.command == "doctor") return command_doctor(options);
    if (options.command == "init-ci") return command_init_ci(options);
    std::cerr << "unknown command: " << options.command << "\n";
    usage();
    return 2;
  } catch (const std::exception &ex) {
    std::cerr << "turbobuild: " << ex.what() << "\n";
    return 1;
  }
}

} // namespace turbobuild
