// ge - ReXGlue Recompiled Project
//
// Stand-ins outside Windows for the Windows-only native renderer and the GPU
// detection behind Render Scale "Auto" (see CMakeLists.txt): the classic
// renderer runs, and the display values come from SDL.

#include <SDL3/SDL.h>

#include <cmath>
#include <cstdint>
#include <string>

#include <rex/cvar.h>

#include "ge_gpu_detect.h"
#include "ge_init.h"  // PPCRegister
#include "ge_native_shaders.h"

// The native renderer's settings, so ge.toml and the menus find them; the
// classic renderer always runs here.
REXCVAR_DEFINE_BOOL(gpu_native, false, "GPU",
                    "Draw with the native renderer (Windows only; false: the classic Xenos renderer)");
REXCVAR_DEFINE_STRING(gpu_native_api, "auto", "GPU", "Native renderer API (Windows only)");
REXCVAR_DEFINE_BOOL(gpu_native_presenter, true, "GPU",
                    "Native renderer: show frames through the SDK presenter (Windows only)");

// Mid-function hooks the recompiled game calls (native/native_hooks.toml,
// native/probe_hooks.toml): nothing to do without the native renderer.
void ge_native_frame_wait(PPCRegister&, PPCRegister&, PPCRegister&) {}

namespace {
struct NpFn {
  uint32_t address;
  const char* name;
};
void ge_np_hit(uint32_t) {}
void ge_np_frame(uint32_t) {}
void ge_np_draw(uint32_t, uint32_t, uint32_t, bool) {}
void ge_np_compile(uint32_t, uint32_t, uint32_t, uint32_t, uint32_t) {}
void ge_np_create_shader(uint32_t, uint32_t, bool) {}
void ge_np_inplace_shader(uint32_t, uint32_t, uint32_t) {}
void ge_np_bind_shader(uint32_t, uint32_t, bool) {}
void ge_np_query(uint32_t, uint32_t, uint32_t) {}
void ge_np_target(uint32_t, uint32_t, uint32_t) {}
void ge_np_resolve(uint32_t, uint32_t, uint32_t, uint32_t, uint32_t, uint32_t, uint32_t) {}
void ge_np_set_texture(uint32_t, uint32_t, uint32_t, uint32_t) {}
void ge_np_tiling(uint32_t, uint32_t) {}
}  // namespace

#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wunused-variable"
#include "ge_native_probe_table.inc"
#pragma clang diagnostic pop

namespace ge::native_shaders {
void SetSpirv(bool) {}
}  // namespace ge::native_shaders

namespace ge {
namespace {

const SDL_DisplayMode* ModeOf(SDL_DisplayID display) {
  return display ? SDL_GetCurrentDisplayMode(display) : nullptr;
}

// The game opens on the primary display (no monitor choice outside Windows).
const SDL_DisplayMode* StartupMode() { return ModeOf(SDL_GetPrimaryDisplay()); }

int PixelHeight(const SDL_DisplayMode* mode) {
  if (!mode) return 0;
  const float density = mode->pixel_density > 0.0f ? mode->pixel_density : 1.0f;
  return static_cast<int>(std::lround(mode->h * density));
}

}  // namespace

GpuForScale DetectGpuForScale() { return {}; }  // unknown: Render Scale "Auto" stays at its default

int StartupDisplayHeight(int /*monitor_option*/) { return PixelHeight(StartupMode()); }

int WindowDisplayHeight(GeDisplayWindow window) {
  return PixelHeight(ModeOf(window ? SDL_GetDisplayForWindow(window) : SDL_GetPrimaryDisplay()));
}

double DisplayRefreshHz(GeDisplayWindow window) {
  const SDL_DisplayMode* mode = ModeOf(window ? SDL_GetDisplayForWindow(window) : 0);
  if (!mode) return 0.0;
  if (mode->refresh_rate_numerator > 0 && mode->refresh_rate_denominator > 0)
    return double(mode->refresh_rate_numerator) / double(mode->refresh_rate_denominator);
  return mode->refresh_rate;
}

std::string StartupDisplayMode(int /*monitor_option*/) {
  const SDL_DisplayMode* mode = StartupMode();
  if (!mode) return "unknown";
  const float density = mode->pixel_density > 0.0f ? mode->pixel_density : 1.0f;
  return std::to_string(std::lround(mode->w * density)) + "x" + std::to_string(PixelHeight(mode)) +
         " @ " + std::to_string(std::lround(mode->refresh_rate)) + " Hz";
}

bool NvidiaGpuClocks(unsigned&, unsigned&, int&) { return false; }

void SetNativeAnisotropy(int) {}
int NativeAnisotropy() { return -1; }

}  // namespace ge
