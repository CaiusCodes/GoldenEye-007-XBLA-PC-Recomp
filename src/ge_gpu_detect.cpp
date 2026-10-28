// ge - ReXGlue Recompiled Project
//
// Inputs for Render Scale "Auto" (ge_native_menu.cpp), read without running
// anything on the GPU:
// - the primary GPU: its name, whether it is integrated (shares system memory:
//   ROG Ally, Steam Deck-class, laptops) and its dedicated video memory. On
//   Windows the first hardware adapter (also the one the Direct3D 12 renderer
//   takes) with a throwaway D3D12 device's UMA flag; elsewhere a throwaway
//   Vulkan instance (discrete preferred over integrated);
// - a monitor's real display mode height, not affected by desktop display
//   scaling (a 4K screen at 150% is 2160).
// Everything is loaded at run time; anything that fails just reports
// "unknown" (treated as a dedicated GPU with enough memory).

#include <rex/platform.h>

#if REX_PLATFORM_WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <d3d12.h>
#include <dxgi1_4.h>
#else
#include <dlfcn.h>
#define VK_NO_PROTOTYPES
#include <vulkan/vulkan.h>
#endif

#include <SDL3/SDL.h>
#include <SDL3/SDL_vulkan.h>

#include <cmath>
#include <cstdint>
#include <string>
#include <vector>

#include "ge_gpu_detect.h"

namespace ge {

#if REX_PLATFORM_WIN32

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

HWND WindowHwnd(SDL_Window* window) {
  if (!window) return nullptr;
  return static_cast<HWND>(SDL_GetPointerProperty(SDL_GetWindowProperties(window),
                                                  SDL_PROP_WINDOW_WIN32_HWND_POINTER, nullptr));
}

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

// Exact refresh rate of the display a window is on (for example 59.940), from
// the display configuration's rational refresh; 0 if unknown.
double DisplayRefreshHz(SDL_Window* sdl_window) {
  HWND window = WindowHwnd(sdl_window);
  MONITORINFOEXW mi{};
  mi.cbSize = sizeof(mi);
  if (!GetMonitorInfoW(MonitorFromWindow(window, MONITOR_DEFAULTTOPRIMARY), &mi)) return 0.0;
  UINT32 path_count = 0, mode_count = 0;
  if (GetDisplayConfigBufferSizes(QDC_ONLY_ACTIVE_PATHS, &path_count, &mode_count) !=
      ERROR_SUCCESS) {
    return 0.0;
  }
  std::vector<DISPLAYCONFIG_PATH_INFO> paths(path_count);
  std::vector<DISPLAYCONFIG_MODE_INFO> modes(mode_count);
  if (QueryDisplayConfig(QDC_ONLY_ACTIVE_PATHS, &path_count, paths.data(), &mode_count,
                         modes.data(), nullptr) != ERROR_SUCCESS) {
    return 0.0;
  }
  for (UINT32 i = 0; i < path_count; ++i) {
    DISPLAYCONFIG_SOURCE_DEVICE_NAME source{};
    source.header.type = DISPLAYCONFIG_DEVICE_INFO_GET_SOURCE_NAME;
    source.header.size = sizeof(source);
    source.header.adapterId = paths[i].sourceInfo.adapterId;
    source.header.id = paths[i].sourceInfo.id;
    if (DisplayConfigGetDeviceInfo(&source.header) != ERROR_SUCCESS) continue;
    if (wcscmp(source.viewGdiDeviceName, mi.szDevice) != 0) continue;
    const DISPLAYCONFIG_RATIONAL r = paths[i].targetInfo.refreshRate;
    return r.Denominator ? double(r.Numerator) / double(r.Denominator) : 0.0;
  }
  return 0.0;
}

// The display a window (the game's) is on.
int WindowDisplayHeight(SDL_Window* window) {
  return ModeHeight(MonitorFromWindow(WindowHwnd(window), MONITOR_DEFAULTTOPRIMARY));
}

#else  // !REX_PLATFORM_WIN32

namespace {

std::string DriverVersionString(uint32_t vendor_id, uint32_t v) {
  if (vendor_id == 0x10DE) {  // NVIDIA: 10.8.8.6 bit layout
    return std::to_string(v >> 22) + "." + std::to_string((v >> 14) & 0xFF) + "." +
           std::to_string((v >> 6) & 0xFF) + "." + std::to_string(v & 0x3F);
  }
  return std::to_string(VK_VERSION_MAJOR(v)) + "." + std::to_string(VK_VERSION_MINOR(v)) + "." +
         std::to_string(VK_VERSION_PATCH(v));
}

int TypeRank(VkPhysicalDeviceType t) {
  switch (t) {
    case VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU: return 3;
    case VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU: return 2;
    default: return 1;
  }
}

}  // namespace

GpuForScale DetectGpuForScale() {
  GpuForScale out;
  if (!SDL_Vulkan_LoadLibrary(nullptr)) return out;
  auto get_instance_proc =
      reinterpret_cast<PFN_vkGetInstanceProcAddr>(SDL_Vulkan_GetVkGetInstanceProcAddr());
  if (get_instance_proc) {
    auto create_instance =
        reinterpret_cast<PFN_vkCreateInstance>(get_instance_proc(nullptr, "vkCreateInstance"));
    VkInstance instance = VK_NULL_HANDLE;
    VkApplicationInfo app{VK_STRUCTURE_TYPE_APPLICATION_INFO};
    app.apiVersion = VK_API_VERSION_1_0;
    VkInstanceCreateInfo info{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
    info.pApplicationInfo = &app;
    if (create_instance && create_instance(&info, nullptr, &instance) == VK_SUCCESS) {
      auto destroy_instance = reinterpret_cast<PFN_vkDestroyInstance>(
          get_instance_proc(instance, "vkDestroyInstance"));
      auto enumerate = reinterpret_cast<PFN_vkEnumeratePhysicalDevices>(
          get_instance_proc(instance, "vkEnumeratePhysicalDevices"));
      auto get_props = reinterpret_cast<PFN_vkGetPhysicalDeviceProperties>(
          get_instance_proc(instance, "vkGetPhysicalDeviceProperties"));
      auto get_mem = reinterpret_cast<PFN_vkGetPhysicalDeviceMemoryProperties>(
          get_instance_proc(instance, "vkGetPhysicalDeviceMemoryProperties"));
      uint32_t count = 0;
      std::vector<VkPhysicalDevice> devices;
      if (enumerate && get_props && get_mem && enumerate(instance, &count, nullptr) == VK_SUCCESS &&
          count) {
        devices.resize(count);
        if (enumerate(instance, &count, devices.data()) < 0) devices.clear();
        devices.resize(devices.empty() ? 0 : count);
      }
      int best_rank = 0;
      for (VkPhysicalDevice device : devices) {
        VkPhysicalDeviceProperties props;
        get_props(device, &props);
        if (props.deviceType == VK_PHYSICAL_DEVICE_TYPE_CPU) continue;
        const int rank = TypeRank(props.deviceType);
        if (rank <= best_rank) continue;
        best_rank = rank;
        VkPhysicalDeviceMemoryProperties mem;
        get_mem(device, &mem);
        uint64_t bytes = 0;
        for (uint32_t h = 0; h < mem.memoryHeapCount; ++h) {
          if (mem.memoryHeaps[h].flags & VK_MEMORY_HEAP_DEVICE_LOCAL_BIT) {
            bytes += mem.memoryHeaps[h].size;
          }
        }
        out.known = true;
        out.integrated = props.deviceType == VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU;
        out.video_memory_mb = bytes / (1024 * 1024);
        out.name = props.deviceName;
        out.driver = DriverVersionString(props.vendorID, props.driverVersion);
      }
      if (destroy_instance) destroy_instance(instance, nullptr);
    }
  }
  SDL_Vulkan_UnloadLibrary();
  return out;
}

namespace {

const SDL_DisplayMode* CurrentMode(SDL_DisplayID id) {
  return id ? SDL_GetCurrentDisplayMode(id) : nullptr;
}

// The display the game shows on, before its window exists: the `monitor`
// option (1-based index into SDL's list; 0 or invalid = the primary display).
SDL_DisplayID StartupDisplay(int monitor_option) {
  if (!SDL_WasInit(SDL_INIT_VIDEO)) return 0;
  SDL_DisplayID id = 0;
  if (monitor_option > 0) {
    int n = 0;
    SDL_DisplayID* list = SDL_GetDisplays(&n);
    if (list) {
      if (monitor_option <= n) id = list[monitor_option - 1];
      SDL_free(list);
    }
  }
  return id ? id : SDL_GetPrimaryDisplay();
}

SDL_DisplayID WindowDisplay(SDL_Window* window) {
  if (!window || !SDL_WasInit(SDL_INIT_VIDEO)) return 0;
  return SDL_GetDisplayForWindow(window);
}

// SDL_DisplayMode is in desktop coordinates; pixel_density turns it into the
// real mode (a 4K display at 150% scale reports 2560x1440 with density 1.5).
int ModePixels(int size, float density) {
  return density > 0.f ? int(std::lround(size * density)) : size;
}

}  // namespace

int StartupDisplayHeight(int monitor_option) {
  const SDL_DisplayMode* mode = CurrentMode(StartupDisplay(monitor_option));
  return mode ? ModePixels(mode->h, mode->pixel_density) : 0;
}

std::string StartupDisplayMode(int monitor_option) {
  const SDL_DisplayMode* mode = CurrentMode(StartupDisplay(monitor_option));
  if (!mode) return "unknown";
  return std::to_string(ModePixels(mode->w, mode->pixel_density)) + "x" +
         std::to_string(ModePixels(mode->h, mode->pixel_density)) + " @ " +
         std::to_string(int(mode->refresh_rate + 0.5f)) + " Hz";
}

double DisplayRefreshHz(SDL_Window* window) {
  const SDL_DisplayMode* mode = CurrentMode(WindowDisplay(window));
  if (!mode) return 0.0;
  if (mode->refresh_rate_denominator) {
    return double(mode->refresh_rate_numerator) / double(mode->refresh_rate_denominator);
  }
  return mode->refresh_rate;
}

int WindowDisplayHeight(SDL_Window* window) {
  const SDL_DisplayMode* mode = CurrentMode(WindowDisplay(window));
  return mode ? ModePixels(mode->h, mode->pixel_density) : 0;
}

#endif  // REX_PLATFORM_WIN32

bool NvidiaGpuClocks(unsigned& memory_mhz, unsigned& graphics_mhz, int& pstate) {
#if REX_PLATFORM_WIN32
#define GE_NVML_CALL __cdecl
#else
#define GE_NVML_CALL
#endif
  using NvmlInit = int(GE_NVML_CALL*)();
  using NvmlHandleByIndex = int(GE_NVML_CALL*)(unsigned, void**);
  using NvmlClockInfo = int(GE_NVML_CALL*)(void*, int, unsigned*);
  using NvmlPState = int(GE_NVML_CALL*)(void*, int*);
#undef GE_NVML_CALL
  static bool tried = false;
  static void* device = nullptr;
  static NvmlClockInfo clock_info = nullptr;
  static NvmlPState performance_state = nullptr;
  if (!tried) {
    tried = true;
#if REX_PLATFORM_WIN32
    HMODULE nvml = LoadLibraryExW(L"nvml.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
    if (!nvml) return false;
    auto sym = [nvml](const char* n) { return reinterpret_cast<void*>(GetProcAddress(nvml, n)); };
#else
    void* nvml = dlopen("libnvidia-ml.so.1", RTLD_NOW);
    if (!nvml) return false;
    auto sym = [nvml](const char* n) { return dlsym(nvml, n); };
#endif
    auto init = reinterpret_cast<NvmlInit>(sym("nvmlInit_v2"));
    auto by_index = reinterpret_cast<NvmlHandleByIndex>(sym("nvmlDeviceGetHandleByIndex_v2"));
    clock_info = reinterpret_cast<NvmlClockInfo>(sym("nvmlDeviceGetClockInfo"));
    performance_state = reinterpret_cast<NvmlPState>(sym("nvmlDeviceGetPerformanceState"));
    if (!init || !by_index || !clock_info || !performance_state || init() != 0 ||
        by_index(0, &device) != 0) {
      device = nullptr;
    }
  }
  if (!device) return false;
  constexpr int kClockGraphics = 0, kClockMemory = 2;  // nvmlClockType_t
  return clock_info(device, kClockMemory, &memory_mhz) == 0 &&
         clock_info(device, kClockGraphics, &graphics_mhz) == 0 &&
         performance_state(device, &pstate) == 0;
}

}  // namespace ge
