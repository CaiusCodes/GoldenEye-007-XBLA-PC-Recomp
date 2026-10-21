// ge - ReXGlue Recompiled Project
//
// Inputs for Render Scale "Auto" (ge_native_menu.cpp), read without running
// anything on the GPU:
// - the primary GPU (the first hardware adapter, which is also the one the
//   Direct3D 12 renderer takes): its name, whether it is integrated (shares
//   system memory: ROG Ally, Steam Deck-class, laptops; a throwaway D3D12
//   device's UMA flag) and its dedicated video memory;
// - a monitor's real display mode height, from EnumDisplaySettings, which is
//   not affected by Windows display scaling (a 4K screen at 150% is 2160).
// DXGI and D3D12 are loaded at run time; anything that fails just reports
// "unknown" (treated as a dedicated GPU with enough memory).

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <d3d12.h>
#include <dxgi1_4.h>

#include <cstdint>
#include <string>
#include <vector>

#include "ge_gpu_detect.h"

namespace ge {

GpuForScale DetectGpuForScale() {
  GpuForScale out;
  HMODULE dxgi = ::LoadLibraryW(L"dxgi.dll");
  HMODULE d3d12 = ::LoadLibraryW(L"d3d12.dll");
  if (!dxgi || !d3d12) {
    if (dxgi) ::FreeLibrary(dxgi);
    if (d3d12) ::FreeLibrary(d3d12);
    return out;
  }
  using CreateFactory1Fn = HRESULT(WINAPI*)(REFIID, void**);
  auto create_factory1 =
      reinterpret_cast<CreateFactory1Fn>(::GetProcAddress(dxgi, "CreateDXGIFactory1"));
  auto create_device =
      reinterpret_cast<PFN_D3D12_CREATE_DEVICE>(::GetProcAddress(d3d12, "D3D12CreateDevice"));
  IDXGIFactory1* factory = nullptr;
  if (create_factory1 && create_device &&
      SUCCEEDED(create_factory1(__uuidof(IDXGIFactory1), reinterpret_cast<void**>(&factory)))) {
    IDXGIAdapter1* adapter = nullptr;
    for (UINT i = 0; factory->EnumAdapters1(i, &adapter) == S_OK; ++i) {
      DXGI_ADAPTER_DESC1 desc;
      const bool hardware =
          SUCCEEDED(adapter->GetDesc1(&desc)) && !(desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE);
      if (hardware) {
        out.known = true;
        out.video_memory_mb = uint64_t(desc.DedicatedVideoMemory) / (1024 * 1024);
        char name[128] = {};
        WideCharToMultiByte(CP_UTF8, 0, desc.Description, -1, name, sizeof(name) - 1, nullptr,
                            nullptr);
        out.name = name;
        LARGE_INTEGER umd{};
        if (SUCCEEDED(adapter->CheckInterfaceSupport(__uuidof(IDXGIDevice), &umd))) {
          out.driver = std::to_string(HIWORD(umd.HighPart)) + "." +
                       std::to_string(LOWORD(umd.HighPart)) + "." +
                       std::to_string(HIWORD(umd.LowPart)) + "." +
                       std::to_string(LOWORD(umd.LowPart));
        }
        ID3D12Device* device = nullptr;
        if (SUCCEEDED(create_device(adapter, D3D_FEATURE_LEVEL_11_0, __uuidof(ID3D12Device),
                                    reinterpret_cast<void**>(&device)))) {
          D3D12_FEATURE_DATA_ARCHITECTURE arch = {};
          if (SUCCEEDED(device->CheckFeatureSupport(D3D12_FEATURE_ARCHITECTURE, &arch,
                                                    sizeof(arch)))) {
            out.integrated = arch.UMA != FALSE;
          }
          device->Release();
        }
        adapter->Release();
        break;
      }
      adapter->Release();
    }
    factory->Release();
  }
  ::FreeLibrary(d3d12);
  ::FreeLibrary(dxgi);
  return out;
}

namespace {
int ModeHeight(HMONITOR monitor) {
  MONITORINFOEXW mi{};
  mi.cbSize = sizeof(mi);
  if (!monitor || !GetMonitorInfoW(monitor, &mi)) return 0;
  DEVMODEW mode{};
  mode.dmSize = sizeof(mode);
  if (EnumDisplaySettingsW(mi.szDevice, ENUM_CURRENT_SETTINGS, &mode)) {
    return int(mode.dmPelsHeight);
  }
  return mi.rcMonitor.bottom - mi.rcMonitor.top;
}
}  // namespace

namespace {
// The display the game shows on, before its window exists: the SDK's `monitor`
// option (0 = default, 1 = primary, 2.. = by enumeration order) or the primary.
HMONITOR StartupMonitor(int monitor_option) {
  HMONITOR monitor = MonitorFromPoint(POINT{0, 0}, MONITOR_DEFAULTTOPRIMARY);
  if (monitor_option > 1) {
    std::vector<HMONITOR> all;
    EnumDisplayMonitors(
        nullptr, nullptr,
        [](HMONITOR m, HDC, LPRECT, LPARAM p) -> BOOL {
          reinterpret_cast<std::vector<HMONITOR>*>(p)->push_back(m);
          return TRUE;
        },
        reinterpret_cast<LPARAM>(&all));
    if (monitor_option <= int(all.size())) monitor = all[monitor_option - 1];
  }
  return monitor;
}
}  // namespace

int StartupDisplayHeight(int monitor_option) { return ModeHeight(StartupMonitor(monitor_option)); }

std::string StartupDisplayMode(int monitor_option) {
  MONITORINFOEXW mi{};
  mi.cbSize = sizeof(mi);
  if (!GetMonitorInfoW(StartupMonitor(monitor_option), &mi)) return "unknown";
  DEVMODEW mode{};
  mode.dmSize = sizeof(mode);
  if (!EnumDisplaySettingsW(mi.szDevice, ENUM_CURRENT_SETTINGS, &mode)) return "unknown";
  return std::to_string(mode.dmPelsWidth) + "x" + std::to_string(mode.dmPelsHeight) + " @ " +
         std::to_string(mode.dmDisplayFrequency) + " Hz";
}

// The display a window (the game's) is on.
int WindowDisplayHeight(HWND window) {
  return ModeHeight(MonitorFromWindow(window, MONITOR_DEFAULTTOPRIMARY));
}

}  // namespace ge
