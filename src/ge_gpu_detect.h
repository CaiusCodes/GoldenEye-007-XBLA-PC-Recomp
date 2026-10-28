// ge - ReXGlue Recompiled Project
//
// Inputs for Render Scale "Auto" (see ge_gpu_detect.cpp).

#pragma once

#include <cstdint>
#include <string>

struct SDL_Window;

namespace ge {

struct GpuForScale {
  bool known = false;          // a hardware adapter was found
  bool integrated = false;     // shares system memory (handhelds, laptops)
  uint64_t video_memory_mb = 0;
  std::string name;
  std::string driver;          // user-mode driver version, a.b.c.d ("" if unknown)
};

GpuForScale DetectGpuForScale();
// Real display mode height (not affected by desktop display scaling).
int StartupDisplayHeight(int monitor_option);
int WindowDisplayHeight(SDL_Window* window);
// Exact refresh rate of the window's display (59.940 etc.), 0 if unknown.
double DisplayRefreshHz(SDL_Window* window);
// "2560x1440 @ 165 Hz" for the display the game opens on (log header).
std::string StartupDisplayMode(int monitor_option);
// NVIDIA only (NVML from the driver, loaded on first use): the first GPU's
// current memory and graphics clocks and performance state (0 = P0, fastest).
// False on other GPUs or when NVML is unavailable.
bool NvidiaGpuClocks(unsigned& memory_mhz, unsigned& graphics_mhz, int& pstate);

}  // namespace ge
