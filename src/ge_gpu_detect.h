// ge - ReXGlue Recompiled Project
//
// Inputs for Render Scale "Auto" (see ge_gpu_detect.cpp).

#pragma once

#include <cstdint>
#include <string>

#if defined(_WIN32)
struct HWND__;
using GeDisplayWindow = HWND__*;
#else
struct SDL_Window;  // the game window is an SDL window elsewhere
using GeDisplayWindow = SDL_Window*;
#endif

namespace ge {

struct GpuForScale {
  bool known = false;          // a hardware adapter was found
  bool integrated = false;     // shares system memory (handhelds, laptops)
  uint64_t video_memory_mb = 0;
  std::string name;
  std::string driver;          // user-mode driver version, a.b.c.d ("" if unknown)
};

GpuForScale DetectGpuForScale();
// Real display mode height (not affected by Windows display scaling).
int StartupDisplayHeight(int monitor_option);
int WindowDisplayHeight(GeDisplayWindow window);
// Exact refresh rate of the window's display (59.940 etc.), 0 if unknown.
double DisplayRefreshHz(GeDisplayWindow window);
// "2560x1440 @ 165 Hz" for the display the game opens on (log header).
std::string StartupDisplayMode(int monitor_option);
// NVIDIA only (NVML from the driver, loaded on first use): the first GPU's
// current memory and graphics clocks and performance state (0 = P0, fastest).
// False on other GPUs or when NVML is unavailable.
bool NvidiaGpuClocks(unsigned& memory_mhz, unsigned& graphics_mhz, int& pstate);

}  // namespace ge
