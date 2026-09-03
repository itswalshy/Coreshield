#include "headroom_logic.h"

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <thread>

#ifdef _WIN32
#include <TlHelp32.h>
#include <Windows.h>
#include <dxgi1_4.h>
#include <wrl/client.h>
#endif

namespace {

struct Options {
  std::optional<std::uint32_t> pid;
  std::wstring process_name;
  std::filesystem::path output{"vram-headroom.csv"};
  std::uint32_t interval_ms{1000};
  double threshold_percent{10.0};
  double hysteresis_percent{3.0};
  bool help{false};
};

void usage(std::ostream &out) {
  out << "VRAM Headroom Bell 0.1\n\n"
      << "Records the WDDM local-memory budget and warns when headroom is "
         "low.\n\n"
      << "Usage:\n"
      << "  vram-headroom-bell --pid <id> [options]\n"
      << "  vram-headroom-bell --process <game.exe> [options]\n\n"
      << "Options:\n"
      << "  --output <file>        CSV destination (default: "
         "vram-headroom.csv)\n"
      << "  --interval-ms <ms>     Sample interval, 100-60000 (default: 1000)\n"
      << "  --threshold <percent>  Enter pressure state at/below this "
         "headroom\n"
      << "  --hysteresis <percent> Extra headroom required to clear warning\n"
      << "  --help                 Show this help\n";
}

std::optional<std::uint64_t> parse_u64(std::wstring_view value) {
  if (value.empty())
    return std::nullopt;
  std::uint64_t result = 0;
  for (const wchar_t ch : value) {
    if (ch < L'0' || ch > L'9')
      return std::nullopt;
    const auto digit = static_cast<std::uint64_t>(ch - L'0');
    if (result > (UINT64_MAX - digit) / 10)
      return std::nullopt;
    result = result * 10 + digit;
  }
  return result;
}

std::optional<double> parse_decimal(std::wstring_view value) {
  try {
    std::size_t used = 0;
    const double result = std::stod(std::wstring(value), &used);
    if (used != value.size())
      return std::nullopt;
    return result;
  } catch (const std::exception &) {
    return std::nullopt;
  }
}

std::optional<Options> parse_options(int argc, wchar_t **argv,
                                     std::ostream &errors) {
  Options options;
  for (int i = 1; i < argc; ++i) {
    const std::wstring_view arg(argv[i]);
    if (arg == L"--help" || arg == L"-h") {
      options.help = true;
      continue;
    }
    if (i + 1 >= argc) {
      errors << "Missing value for command-line option.\n";
      return std::nullopt;
    }
    const std::wstring_view value(argv[++i]);
    if (arg == L"--pid") {
      const auto parsed = parse_u64(value);
      if (!parsed || *parsed == 0 || *parsed > UINT32_MAX) {
        errors << "--pid must be a positive 32-bit integer.\n";
        return std::nullopt;
      }
      options.pid = static_cast<std::uint32_t>(*parsed);
    } else if (arg == L"--process") {
      options.process_name = value;
    } else if (arg == L"--output") {
      options.output = value;
    } else if (arg == L"--interval-ms") {
      const auto parsed = parse_u64(value);
      if (!parsed || *parsed < 100 || *parsed > 60000) {
        errors << "--interval-ms must be between 100 and 60000.\n";
        return std::nullopt;
      }
      options.interval_ms = static_cast<std::uint32_t>(*parsed);
    } else if (arg == L"--threshold" || arg == L"--hysteresis") {
      const auto parsed = parse_decimal(value);
      if (!parsed || *parsed < 0.0 || *parsed > 100.0) {
        errors << "Percent values must be between 0 and 100.\n";
        return std::nullopt;
      }
      (arg == L"--threshold" ? options.threshold_percent
                             : options.hysteresis_percent) = *parsed;
    } else {
      errors << "Unknown option. Use --help.\n";
      return std::nullopt;
    }
  }
  if (!options.help &&
      options.pid.has_value() == !options.process_name.empty()) {
    errors << "Specify exactly one of --pid or --process.\n";
    return std::nullopt;
  }
  if (options.threshold_percent + options.hysteresis_percent > 100.0) {
    errors << "Threshold plus hysteresis cannot exceed 100%.\n";
    return std::nullopt;
  }
  return options;
}

#ifdef _WIN32

using Microsoft::WRL::ComPtr;

std::optional<DWORD> find_process(std::wstring_view executable) {
  const HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
  if (snapshot == INVALID_HANDLE_VALUE)
    return std::nullopt;
  PROCESSENTRY32W entry{.dwSize = sizeof(entry)};
  std::optional<DWORD> result;
  if (Process32FirstW(snapshot, &entry)) {
    do {
      if (_wcsicmp(entry.szExeFile, std::wstring(executable).c_str()) == 0) {
        result = entry.th32ProcessID;
        break;
      }
    } while (Process32NextW(snapshot, &entry));
  }
  CloseHandle(snapshot);
  return result;
}

std::string iso8601_now() {
  const auto now = std::chrono::system_clock::now();
  const auto seconds = std::chrono::time_point_cast<std::chrono::seconds>(now);
  const auto millis =
      std::chrono::duration_cast<std::chrono::milliseconds>(now - seconds);
  const std::time_t value = std::chrono::system_clock::to_time_t(now);
  std::tm utc{};
  gmtime_s(&utc, &value);
  std::ostringstream out;
  out << std::put_time(&utc, "%Y-%m-%dT%H:%M:%S") << '.' << std::setfill('0')
      << std::setw(3) << millis.count() << 'Z';
  return out.str();
}

std::string narrow(const wchar_t *text) {
  if (!text)
    return {};
  const int length = static_cast<int>(wcslen(text));
  const int size = WideCharToMultiByte(CP_UTF8, 0, text, length, nullptr, 0,
                                       nullptr, nullptr);
  if (size <= 0)
    return {};
  std::string result(static_cast<std::size_t>(size), '\0');
  WideCharToMultiByte(CP_UTF8, 0, text, length, result.data(), size, nullptr,
                      nullptr);
  return result;
}

int run(const Options &options) {
  DWORD pid = options.pid.value_or(0);
  if (!options.process_name.empty()) {
    std::cout << "Waiting for the game process...\n";
    while (!(pid = find_process(options.process_name).value_or(0))) {
      std::this_thread::sleep_for(std::chrono::seconds(1));
    }
  }
  const HANDLE process = OpenProcess(SYNCHRONIZE, FALSE, pid);
  if (!process) {
    std::cerr << "Cannot monitor process " << pid << " (Win32 error "
              << GetLastError() << ").\n";
    return 2;
  }

  ComPtr<IDXGIFactory4> factory;
  HRESULT hr = CreateDXGIFactory1(IID_PPV_ARGS(&factory));
  if (FAILED(hr)) {
    std::cerr << "CreateDXGIFactory1 failed (HRESULT 0x" << std::hex << hr
              << ").\n";
    CloseHandle(process);
    return 3;
  }

  // Prefer the hardware adapter with the largest dedicated memory. This avoids
  // choosing Microsoft Basic Render Driver and is predictable on hybrid
  // laptops.
  ComPtr<IDXGIAdapter3> adapter;
  DXGI_ADAPTER_DESC2 adapter_desc{};
  SIZE_T largest_vram = 0;
  for (UINT index = 0;; ++index) {
    ComPtr<IDXGIAdapter1> candidate1;
    if (factory->EnumAdapters1(index, &candidate1) == DXGI_ERROR_NOT_FOUND)
      break;
    ComPtr<IDXGIAdapter3> candidate3;
    ComPtr<IDXGIAdapter2> candidate2;
    DXGI_ADAPTER_DESC2 desc{};
    if (SUCCEEDED(candidate1.As(&candidate3)) &&
        SUCCEEDED(candidate1.As(&candidate2)) &&
        SUCCEEDED(candidate2->GetDesc2(&desc)) &&
        !(desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) &&
        desc.DedicatedVideoMemory >= largest_vram) {
      largest_vram = desc.DedicatedVideoMemory;
      adapter = candidate3;
      adapter_desc = desc;
    }
  }
  if (!adapter) {
    std::cerr << "No WDDM hardware adapter exposing IDXGIAdapter3 was found.\n";
    CloseHandle(process);
    return 4;
  }

  const HANDLE budget_event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
  DWORD budget_cookie = 0;
  if (!budget_event ||
      FAILED(adapter->RegisterVideoMemoryBudgetChangeNotificationEvent(
          budget_event, &budget_cookie))) {
    std::cerr << "Could not register the DXGI budget notification.\n";
    if (budget_event)
      CloseHandle(budget_event);
    CloseHandle(process);
    return 5;
  }

  std::ofstream csv(options.output, std::ios::app);
  if (!csv) {
    std::cerr << "Cannot open output CSV.\n";
    adapter->UnregisterVideoMemoryBudgetChangeNotification(budget_cookie);
    CloseHandle(budget_event);
    CloseHandle(process);
    return 6;
  }
  if (std::filesystem::file_size(options.output) == 0) {
    csv << "timestamp_utc,pid,adapter,usage_bytes,budget_bytes,headroom_bytes,"
           "headroom_percent,pressure,budget_event\n";
  }
  std::cout << "Monitoring PID " << pid << " on "
            << narrow(adapter_desc.Description)
            << ". Press Ctrl+C or close the game to stop.\n";

  vhb::PressureState state = vhb::PressureState::healthy;
  const HANDLE handles[] = {process, budget_event};
  for (;;) {
    const DWORD wait =
        WaitForMultipleObjects(2, handles, FALSE, options.interval_ms);
    if (wait == WAIT_OBJECT_0)
      break;
    if (wait == WAIT_FAILED) {
      std::cerr << "WaitForMultipleObjects failed.\n";
      break;
    }
    const bool was_budget_event = wait == WAIT_OBJECT_0 + 1;
    DXGI_QUERY_VIDEO_MEMORY_INFO info{};
    hr = adapter->QueryVideoMemoryInfo(0, DXGI_MEMORY_SEGMENT_GROUP_LOCAL,
                                       &info);
    if (FAILED(hr))
      continue;
    const vhb::Sample sample{info.Budget, info.CurrentUsage};
    const auto previous = state;
    state = vhb::next_pressure_state(state, sample, options.threshold_percent,
                                     options.threshold_percent +
                                         options.hysteresis_percent);
    const double percent = vhb::headroom_percent(sample);
    csv << iso8601_now() << ',' << pid << ",\""
        << narrow(adapter_desc.Description) << "\"," << sample.usage_bytes
        << ',' << sample.budget_bytes << ',' << vhb::headroom_bytes(sample)
        << ',' << std::fixed << std::setprecision(3) << percent << ','
        << (state == vhb::PressureState::pressured ? 1 : 0) << ','
        << (was_budget_event ? 1 : 0) << '\n';
    csv.flush();
    if (state != previous) {
      if (state == vhb::PressureState::pressured) {
        std::cerr << "LOW VRAM HEADROOM: " << std::setprecision(1) << percent
                  << "%\n";
      } else {
        std::cout << "VRAM headroom recovered: " << std::setprecision(1)
                  << percent << "%\n";
      }
    }
  }
  adapter->UnregisterVideoMemoryBudgetChangeNotification(budget_cookie);
  CloseHandle(budget_event);
  CloseHandle(process);
  std::cout << "Game exited; monitoring stopped.\n";
  return 0;
}

#endif

} // namespace

#ifdef _WIN32
int wmain(int argc, wchar_t **argv) {
  const auto options = parse_options(argc, argv, std::cerr);
  if (!options) {
    usage(std::cerr);
    return 1;
  }
  if (options->help) {
    usage(std::cout);
    return 0;
  }
  return run(*options);
}
#else
int main() {
  std::cerr << "vram-headroom-bell requires Windows 10 or later (WDDM 2.x).\n";
  return 1;
}
#endif
