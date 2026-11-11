// ge - native renderer (milestone 1, checkpoint 4).
//
// With gpu_native = true the SDK loads no GPU plugin: there is no Xenos
// command processor, so nothing consumes the D3D ring buffer. The guest's own
// D3D library keeps running (device struct, shadows and bookkeeping stay
// valid; tested: the title runs at a steady 60 fps this way), and the native
// Plume (D3D12) backend draws from the D3D calls instead. Every hook here runs
// the original guest function (__imp__sub_*) too, so with gpu_native off
// nothing changes and Xenos stays the default.
//
// Shaders come from a local cache of XenosRecomp output (<exe>\native_shaders\
// <hash>.vs|ps.final.dxil, hash = FNV-1a of the shader container as the probe
// computes it), translated from the player's own files; nothing is shipped.
//
// What is drawn so far: indexed draws, DrawVerticesUP and Begin/EndVertices,
// with the guest's vertex declarations, streams, indices and constants; one
// scene target (resolves copy it into host textures that SetTexture binds);
// textures that weren't resolved sample white. Render states, guest textures
// and render-target switching come next.

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <algorithm>
#include <bit>
#include <condition_variable>
#include <deque>
#include <mutex>
#include <thread>
#include <cmath>
#include <atomic>
#include <condition_variable>
#include <cstdio>
#include <cstring>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

#include "ge_init.h"  // PPCContext, generated function declarations
#include "ge_native_shaders.h"
#include "ge_native_vk_out.h"
#include <rex/ui/presenter_graphics_system.h>
namespace ge {
float ActiveRenderScale();  // ge_native_menu.cpp
bool BackdropActiveNow();  // ge_native_menu.cpp: the desk goes behind this frame
void PersistSettings();
[[noreturn]] void RestartSelf();  // ge_hooks.cpp
// Texture Filtering (anisotropic_override): set by the setting, which the
// native renderer registers itself (ge_app.h); sampler_slot reads it for every
// new sampler, so a change applies at once.
std::atomic<int> g_native_aniso{3};
void SetNativeAnisotropy(int value) { g_native_aniso.store(value, std::memory_order_relaxed); }
int NativeAnisotropy() { return g_native_aniso.load(std::memory_order_relaxed); }
}  // namespace ge
#include <rex/cvar.h>
#include <rex/hook.h>
#include <rex/logging/api.h>
#include <rex/runtime.h>
#include <rex/system/interfaces/graphics.h>
#include <rex/system/kernel_state.h>
#include <rex/ui/d3d12/d3d12_presenter.h>
#include <rex/ui/d3d12/d3d12_provider.h>
#include <rex/ui/image_decode.h>
#include <rex/filesystem.h>
#include <rex/system/xmemory.h>

#include "plume_render_interface.h"
#include "plume_d3d12.h"
#include "plume_vulkan.h"

#include <d3dcompiler.h>
#include <dxgi1_4.h>
#include <psapi.h>
#include <filesystem>
#include <fstream>
#include <iterator>

#define XXH_INLINE_ALL
#include <xxhash.h>

REXCVAR_DEFINE_BOOL(gpu_native, true, "GPU",
                    "Draw with the native renderer (false: the classic Xenos renderer of 0.9.6c and before)");
REXCVAR_DECLARE(bool, gpu_native_translate);  // ge_native_shaders.cpp
REXCVAR_DECLARE(bool, ge_hide_button_icons);  // ge_native_menu.cpp
REXCVAR_DEFINE_STRING(gpu_native_skip_ps, "", "GPU",
                      "Native renderer test: skip draws whose pixel shader has this cache hash (16 hex digits)");
REXCVAR_DEFINE_STRING(gpu_native_hide, "", "GPU",
                      "Native renderer test: hide draws ""a-b"" of each frame, or triangles ""d:a-b"" of draw d");
REXCVAR_DEFINE_STRING(gpu_native_api, "auto", "GPU",
                      "Native renderer API: auto (Direct3D 12, Vulkan if that cannot start), d3d12 or vulkan");
REXCVAR_DEFINE_STRING(gpu_native_adapter, "", "GPU",
                      "Native renderer on Vulkan: part of the graphics card's name to use (empty = the default)");
REXCVAR_DEFINE_BOOL(gpu_native_presenter, true, "GPU",
                    "Native renderer: show frames through the SDK presenter (ImGui overlays, FSR, window "
                    "scaling) instead of a swap chain of its own");
REXCVAR_DEFINE_BOOL(gpu_native_debug_layer, false, "GPU",
                    "Native renderer test: enable the D3D12 debug layer and log its messages");
REXCVAR_DEFINE_INT32(gpu_native_test_remove, 0, "GPU",
                     "Native renderer test: remove the D3D12 device (as a driver reset does) after this many frames");
REXCVAR_DEFINE_BOOL(gpu_native_dred, true, "GPU",
                    "Native renderer: D3D12 breadcrumbs and page-fault data, logged if the graphics device is lost");
REXCVAR_DEFINE_INT32(gpu_native_trace_frame, 0, "GPU",
                     "Native renderer test: log every draw, clear, resolve and target change of this frame (0 = off)");
REXCVAR_DEFINE_BOOL(gpu_native_dump_textures, false, "GPU",
                    "Native renderer test: save every decoded texture as a BMP in native_textures (game content: keep local)");
REXCVAR_DEFINE_BOOL(gpu_native_half_pixel, true, "GPU",
                    "Native renderer: shift geometry half a pixel to match the 360's pixel centres");
REXCVAR_DEFINE_BOOL(gpu_native_all_mips, false, "GPU",
                    "Native renderer test: build a full mip chain for every texture");
REXCVAR_DEFINE_INT32(gpu_native_msaa, 4, "GPU",
                     "Native renderer: scene MSAA samples, 1 (off, the lightest), 2 or 4 (as the Xbox 360, the default); "
                     "the Video page's Anti-Aliasing row, applied at once");
REXCVAR_DEFINE_BOOL(gpu_native_log_creation, false, "GPU",
                    "Native renderer test: log every pipeline and texture created");
REXCVAR_DEFINE_BOOL(gpu_native_bc_textures, true, "GPU",
                    "Native renderer: keep DXT textures compressed (BC1-3) with the game's own mips");
REXCVAR_DEFINE_BOOL(gpu_native_depth_copy, true, "GPU",
                    "Native renderer: copy the scene depth for depth resolves (off: the last clear value)");
REXCVAR_DEFINE_BOOL(gpu_native_depth_copy_intel, false, "GPU",
                    "Native renderer: copy the scene depth on Intel integrated graphics too (it hung an HD 620)");
REXCVAR_DEFINE_BOOL(gpu_native_skip_resolves, false, "GPU",
                    "Native renderer test: ignore resolves (wrong picture; for timing only)");
REXCVAR_DEFINE_INT32(gpu_native_profile_frame, 0, "GPU",
                     "Native renderer test: GPU-time every draw, clear and resolve of this frame and log the costliest");
REXCVAR_DEFINE_DOUBLE(gpu_native_long_frame_ms, 25.0, "GPU",
                      "Native renderer test: with gpu_native_frame_stats, log frames longer than this");
// Measured against the classic renderer (Dam, same frame and settings): the
// native renderer's textures came out ~0.5 mip level sharper (7% more fine
// detail, a more jagged PP7 highlight); +0.5 matches classic's detail.
REXCVAR_DEFINE_DOUBLE(gpu_native_lod_bias, 0.5, "GPU",
                      "Native renderer: added to every texture's mip LOD bias (0.5 = classic's sharpness; 0 = sharper)");
REXCVAR_DEFINE_STRING(gpu_native_fxaa, "none", "GPU",
                      "Native renderer: FXAA on top of the MSAA, as the classic renderer's swap_post_effect: none, "
                      "fxaa or fxaa_extreme (the Video page's FXAA row)");
REXCVAR_DEFINE_BOOL(gpu_native_warm_modes, true, "GPU",
                    "Native renderer: draw two hidden frames in the other graphics mode as a level starts, so the "
                    "first switch (F / RB) doesn't stutter");
REXCVAR_DEFINE_BOOL(gpu_native_warm_shaders, true, "GPU",
                    "Native renderer: translate a level's shaders while it loads, not at their first draw");
REXCVAR_DEFINE_BOOL(gpu_native_stage_textures, true, "GPU",
                    "Native renderer: start the texture cache afresh on each new stage (level or front end)");
REXCVAR_DEFINE_BOOL(gpu_native_async_textures, true, "GPU",
                    "Native renderer: prepare new textures past ~1 ms a frame on a worker thread (in level)");
REXCVAR_DEFINE_BOOL(gpu_native_pace, true, "GPU",
                    "Native renderer: present each frame at a fixed moment after the frame's usual arrival");
REXCVAR_DEFINE_INT32(gpu_native_pace_margin_us, 2000, "GPU",
                     "Native renderer: how long after a frame's usual arrival it is presented (paced presents)");
REXCVAR_DEFINE_INT32(gpu_native_frame_stats, 0, "GPU",
                     "Log guest frame times (Swap to Swap) every this many frames, native or Xenos (0 = off)");
REXCVAR_DEFINE_INT32(gpu_native_debug_clear, 0, "GPU",
                     "Native renderer test: clear to this 0xRRGGBB instead of the game's colour (0 = off)");

HWND g_game_hwnd_for_resize();  // ge_hooks.cpp

namespace plume {
std::unique_ptr<RenderInterface> CreateD3D12Interface();
std::unique_ptr<RenderInterface> CreateVulkanInterface();
}

using namespace plume;

namespace {

// --- guest memory -----------------------------------------------------------

inline uint8_t* gmem() { return rex::system::kernel_state()->memory()->virtual_membase(); }
inline uint32_t g32(uint32_t a) {
  uint32_t v;
  std::memcpy(&v, gmem() + a, 4);
  return __builtin_bswap32(v);
}
inline uint16_t g16(uint32_t a) {
  uint16_t v;
  std::memcpy(&v, gmem() + a, 2);
  return __builtin_bswap16(v);
}
// Physical memory at 0xE0000000+ is seen 4 KB further on than the raw guest
// address reads (see ge_native_probe.cpp); shader microcode pointers need it.
inline uint32_t gpu_view(uint32_t a) { return a >= 0xE0000000u ? a + 0x1000 : a; }

uint64_t fnv1a(const uint8_t* p, size_t n) {
  uint64_t h = 0xCBF29CE484222325ull;
  for (size_t i = 0; i < n; ++i) { h ^= p[i]; h *= 0x100000001B3ull; }
  return h;
}

void create_scene_targets();
RenderSampleCounts pick_msaa();

// Device fields (offsets into the guest D3D device; see native/d3d_functions.toml).
constexpr uint32_t kDevVsConsts = 1920;     // 256 float4, big-endian
constexpr uint32_t kDevPsConsts = 6016;     // 256 float4
constexpr uint32_t kDevVsBools = 2528 * 4;  // dword index 2528
constexpr uint32_t kDevPsBools = 2532 * 4;
constexpr uint32_t kDevVertexDecl = 11812;
constexpr uint32_t kDevPixelShader = 12684;
constexpr uint32_t kDevVertexShader = 12688;
constexpr uint32_t kDevViewport = 12640;    // X, Y, W, H, MinZ, MaxZ (floats)

// --- native device ----------------------------------------------------------

constexpr uint32_t kTextureSlots = 1024;
constexpr uint32_t kSamplerSlots = 64;  // sampler descriptors (set 3); 0 = the default sampler
constexpr uint32_t kFrames = 3;  // frames in flight: a GPU clock stall doesn't block the game thread
constexpr uint64_t kUploadSize = 288ull << 20;  // system memory; a level load fills most of a 96 MB third
constexpr uint64_t kSlotUpload = kUploadSize / kFrames;
constexpr uint64_t kSlotConsts = 8ull << 20;        // shader constants per frame (~8 KB a draw; Dam ~5.3 MB)
// The game draws for a 1280x720 EDRAM frame (guest pixels); the scene targets
// are that times the Render Scale (ge::ActiveRenderScale, set at device start).
constexpr uint32_t kGuestW = 1280, kGuestH = 720;
// The native renderer's API this run: Vulkan when gpu_native_api says so, or
// when Direct3D 12 could not start on auto.
bool g_vulkan = false;
float g_scene_scale = 1.0f;  // 1, 1.5 (1920x1080), 2 or 3
uint32_t g_scene_w = kGuestW, g_scene_h = kGuestH;
constexpr RenderFormat kSceneFormat = RenderFormat::B8G8R8A8_UNORM;

// A vertex input. For declarations: stream = D3D stream, type = D3DDECLTYPE.
// For a shader's own vertex fetches: stream = vertex fetch constant slot, type
// = Xenos format | signed << 8 | integer << 9 (the same bits D3DDECLTYPE uses).
constexpr uint16_t kIdentitySwizzle = 0x688;  // vfetch dst swizzle x,y,z,w (3 bits each)

// Vulkan matches vertex inputs by location: XenosRecomp's [[vk::location]] for
// a D3D declaration usage (USAGE_LOCATIONS in shader_recompiler.cpp).
uint32_t vk_location(uint32_t usage, uint32_t index) {
  switch (usage) {
    case 0: return index == 1 ? 15 : 0;                        // Position
    case 1: return 10;                                         // BlendWeight
    case 2: return 9;                                          // BlendIndices
    case 3: return 1;                                          // Normal
    case 5: return index < 4 ? 4 + index : 12 + (index - 4);   // TexCoord
    case 6: return 2;                                          // Tangent
    case 7: return 3;                                          // Binormal
    case 10: return index == 1 ? 11 : 8;                       // Color
    default: return 0;
  }
}

struct Element {
  uint16_t stream, offset;
  uint32_t type;
  uint8_t usage, index;
  int8_t exp = 0;  // vfetch exponent adjust: values scale by 2^exp
  // Destination swizzle D3D patched into the live vfetch (e.g. BGRA for a
  // D3DCOLOR) where the shader was compiled from an identity template:
  // applied while decoding, since the translated shader doesn't know it.
  uint16_t swizzle = kIdentitySwizzle;
};

struct HostShader {
  std::unique_ptr<RenderShader> shader;
  std::vector<Element> inputs;                  // vertex shaders: from the microcode
  std::vector<uint8_t> container;               // vertex shaders: for re-parsing live microcode
  uint32_t cb_bytes = 4096;                     // constant-buffer bytes the shader reads (<hash>.cbsize)
  std::unordered_map<uint16_t, uint32_t> strides;  // fetch slot -> stride in bytes
  uint64_t hash = 0;
  bool missing = false;
};

struct TextureJob;
struct ResolvedTexture {
  std::unique_ptr<RenderTexture> texture;
  uint32_t slot = 0;
  std::shared_ptr<TextureJob> job;  // being prepared on the texture worker
  // Textures outside the CAFF heap can be rewritten by the game (classic Dam's
  // 4x4 sky colour ramp): their bytes are re-hashed once a frame when used.
  bool dynamic = false;
  uint32_t addr = 0, span = 0;
  uint64_t content = 0, checked_frame = 0;
  // A / B / Start button picture: uploaded blank while no controller is
  // connected (ge_hide_button_icons), as the Xenos texture cache does it.
  bool icon = false, icon_hidden = false;
  // Full-size depth resolve not copied yet (copy_pending_depth), and the last
  // frame a draw sampled this depth texture (0: never).
  bool depth_pending = false;
  uint64_t depth_used_frame = 0;
};

// A piece of a pooled GPU buffer (vertex or index cache entries). Each committed
// buffer costs 64 KB at least and ~0.5 ms to create: Dam's level load made 500
// of them in one frame.
struct GpuAlloc {
  RenderBuffer* buffer = nullptr;
  uint64_t offset = 0;
  uint32_t size_class = 0;  // capacity = class_bytes(size_class)
  explicit operator bool() const { return buffer != nullptr; }
};

// Size classes: four per doubling from 256 bytes (at most ~19% rounding,
// against up to 2x with powers of two).
constexpr uint32_t kSizeClasses = 128;
inline uint64_t class_bytes(uint32_t c) {
  const uint64_t base = 256ull << (c / 4);
  return base + (base / 4) * (c % 4);
}

struct BufferPool {
  uint64_t chunk = 32ull << 20;
  RenderBufferFlags flags = RenderBufferFlag::NONE;
  std::vector<std::unique_ptr<RenderBuffer>> chunks;
  uint64_t bump = UINT64_MAX;  // into chunks.back(); MAX = no chunk yet
  std::vector<GpuAlloc> free_lists[kSizeClasses];
};

struct Native {
  std::unique_ptr<RenderInterface> api;
  std::unique_ptr<RenderDevice> device;
  std::unique_ptr<RenderCommandQueue> queue;
  std::unique_ptr<RenderSwapChain> swapchain;
  // Two frames in flight: frame N records into slot N % 2 while the GPU may
  // still run frame N-1. A slot's list, fence and half of the upload ring are
  // reused only after waiting for its fence.
  std::unique_ptr<RenderCommandList> cmds[kFrames];
  std::unique_ptr<RenderCommandFence> fences[kFrames];
  RenderCommandList* cmd = nullptr;
  RenderCommandFence* fence = nullptr;
  uint32_t slot = 0;
  bool pending[kFrames] = {};  // a submission signalling that slot's fence not waited for yet (Plume's
                         // wait blocks on an event: never wait without one, or twice)
  struct Retired { uint64_t frame; BufferPool* pool; GpuAlloc alloc; };
  std::vector<Retired> retired;  // returned to their pool 2 frames later
  struct RetiredTexture { uint64_t frame; std::unique_ptr<RenderTexture> texture; uint32_t slot; };
  std::vector<RetiredTexture> retired_textures;  // re-uploaded dynamic textures: freed kFrames frames later
  struct RetiredSet { uint64_t frame; std::unique_ptr<RenderDescriptorSet> set; };
  std::vector<RetiredSet> retired_sets;  // replaced depth-copy sets: a frame in flight may still use them
  std::vector<uint32_t> free_slots;
  BufferPool vertex_pool, index_pool;
  std::unique_ptr<RenderCommandSemaphore> acquire;
  std::vector<std::unique_ptr<RenderCommandSemaphore>> release;
  std::vector<std::unique_ptr<RenderFramebuffer>> swap_framebuffers;

  // Presenter output (gpu_native_presenter): finished frames go to the SDK
  // presenter, which has a D3D12 device of its own (window, ImGui overlays,
  // FSR), through textures and fences shared between the two devices. Ours:
  // copy frame k into tex[k], signal `ready`. The presenter's queue: wait for
  // `ready`, convert tex[k] into its guest output, signal `done`; tex[k] is
  // written again only after that.
  struct Out {
    rex::ui::Presenter* presenter = nullptr;
    ID3D12Device* pdev = nullptr;  // the presenter's device and direct queue
    ID3D12CommandQueue* pqueue = nullptr;
    ID3D12Resource* tex[3] = {};   // shared frames, opened by our device ...
    ID3D12Resource* ptex[3] = {};  // ... and by the presenter's
    ID3D12Fence* ready = nullptr;  // created by ours
    ID3D12Fence* ready_p = nullptr;
    ID3D12Fence* done_p = nullptr;  // created by the presenter's
    ID3D12Fence* done = nullptr;
    uint64_t ready_value = 0, done_value = 0;
    uint64_t tex_ready[3] = {}, tex_done[3] = {}, list_done[3] = {};
    ID3D12CommandAllocator* alloc[3] = {};
    ID3D12GraphicsCommandList* list = nullptr;
    ID3D12RootSignature* root = nullptr;
    ID3D12PipelineState* pso = nullptr;
    ID3D12DescriptorHeap* heap = nullptr;
    UINT heap_step = 0;
    uint32_t next = 0, next_list = 0;
    // Menu backdrop pictures (16:9 and 21:9), loaded on first use.
    ID3D12Resource* backdrop[2] = {};
    ID3D12Resource* backdrop_upload[2] = {};
    uint32_t backdrop_w[2] = {}, backdrop_h[2] = {};
    bool backdrop_tried[2] = {};
    // The game's gamma ramp as a 256 x 1 texture (one upload buffer per list).
    ID3D12Resource* ramp = nullptr;
    ID3D12Resource* ramp_upload[3] = {};
    uint32_t ramp_version = 0;
    bool ramp_ready = false;
    HANDLE event = nullptr;
    bool vk = false;  // Vulkan: frames go through ge_native_vk_out.cpp
    uint64_t vk_handoff[kFrames] = {};  // per frame slot: its hand-off's number (ge::native_vk::WaitPresent)
  } out;
  // D3DDevice_SetGammaRamp: the display gamma ramp the Xenos path applies to
  // every frame on the way out (its 256-entry table, for 8_8_8_8 front buffers).
  uint16_t gamma_ramp[256 * 3] = {};  // R, G, B; 16-bit values
  uint32_t gamma_ramp_version = 0;

  std::unique_ptr<RenderPipelineLayout> layout;
  std::unique_ptr<RenderDescriptorSet> textures;
  std::unique_ptr<RenderDescriptorSet> samplers;
  std::unique_ptr<RenderSampler> default_sampler;
  std::unique_ptr<RenderTexture> white;
  std::unique_ptr<RenderBuffer> upload;
  // Shader constants: written to a per-slot upload range, copied in one go into
  // a GPU-local buffer by a short list submitted just before the frame's list
  // (the draws' root CBVs point at the GPU-local copy, not at system memory).
  std::unique_ptr<RenderBuffer> const_upload;
  uint8_t* const_ptr = nullptr;
  std::unique_ptr<RenderBuffer> const_gpu[kFrames];
  std::unique_ptr<RenderCommandList> const_lists[kFrames];
  uint64_t const_used = 0;
  std::unordered_map<uint64_t, uint64_t> const_dedup;  // block hash -> offset, this submission
  std::unique_ptr<RenderQueryPool> timestamps;  // GPU time per frame: [0] frame start, [1] end
  // One-frame profile (gpu_native_profile_frame): a timestamp after every
  // operation; the time since the previous one is that operation's.
  std::unique_ptr<RenderQueryPool> prof_pool;
  std::vector<std::string> prof_labels;
  bool prof_active = false;
  double gpu_ms_sum = 0, gpu_ms_max = 0, copy_ms_sum = 0;
  uint32_t gpu_frames = 0;
  uint8_t* upload_ptr = nullptr;
  uint64_t upload_used = 0;  // within the current slot's half

  RenderMultisampling msaa;  // scene sample count (gpu_native_msaa)
  // Render targets. The game's scene target (bound with a depth surface) is
  // the MSAA scene above; a target bound without depth (the post-process pass
  // into 400076A0) gets a single-sample colour target of its own, per guest
  // surface. bind_target() makes the bound one match what the game has set.
  struct ColorTarget {
    std::unique_ptr<RenderTexture> color;
    std::unique_ptr<RenderFramebuffer> fb;
  };
  std::unordered_map<uint32_t, ColorTarget> color_targets;
  uint32_t guest_rt = 0, guest_depth = 0, front_buffer = 0, scene_surface = 0;
  RenderFramebuffer* bound_fb = nullptr;
  RenderTexture* bound_color = nullptr;
  bool bound_scene = true;
  uint32_t last_resolve_dest = 0;
  bool draws_since_resolve = true;
  // Scene depth since its last clear: the clear value, and whether a draw has
  // written depth since (then a depth resolve would need the real contents).
  float depth_clear = 1.0f;
  bool depth_written = true;
  // Bumped by every depth-writing draw and depth clear: a full depth resolve
  // whose destination already holds this generation is skipped (the game
  // resolves the same depth more than once a frame).
  uint64_t depth_gen = 1;
  uint32_t depth_copies = 0;  // full depth copies, for the 600-frame log line
  std::vector<uint32_t> depth_pending;  // destinations with ResolvedTexture::depth_pending
  // Depth resolve pass (native_depth_resolve): sample 0 of the scene depth
  // into an R32F texture per destination.
  std::unique_ptr<RenderPipelineLayout> depth_layout;
  std::unique_ptr<RenderShader> depth_shader;
  std::unique_ptr<RenderPipeline> depth_pipe;
  std::unique_ptr<RenderTextureView> depth_view;  // R32_FLOAT view of the typeless scene depth
  bool depth_pass_failed = false;
  std::unordered_map<uint32_t, std::unique_ptr<RenderDescriptorSet>> depth_sets;
  std::unique_ptr<RenderTexture> scene;
  std::unique_ptr<RenderTexture> scene_depth;
  std::unique_ptr<RenderFramebuffer> scene_fb;

  std::unordered_map<uint32_t, HostShader> shaders;            // guest shader object
  std::unordered_map<uint32_t, uint32_t> created_blobs;        // object -> container (boot shaders)
  // Shaders a level registered in place (warm_registered_shaders): object ->
  // vertex?, the hash its bytes had last frame and the frame it was registered.
  struct WarmEntry { bool vertex; uint64_t last_hash; uint64_t frame; };
  std::unordered_map<uint32_t, WarmEntry> warm;
  uint32_t warm_requested = 0;
  std::unordered_map<uint64_t, std::unique_ptr<RenderPipeline>> pipelines;
  std::unordered_map<uint32_t, ResolvedTexture> resolved;      // guest texture -> host copy
  std::unordered_map<uint64_t, ResolvedTexture> textures_by_fetch;  // fetch constant hash -> upload
  uint32_t textures_stage = 0;  // the stage textures_by_fetch was filled in (begin_frame)
  std::vector<std::unique_ptr<RenderSampler>> sampler_objects;
  std::unordered_map<uint64_t, uint32_t> sampler_slots;        // filter/clamp/mip key -> sampler slot
  // Decoded vertex data kept on the GPU between frames (see cached_vertices).
  struct VertexCacheEntry {
    GpuAlloc buffer;
    uint64_t capacity = 0, content = 0, last_frame = 0;
    uint32_t misses = 0;  // content changes seen: past a few, decoded into the ring every time
  };
  std::unordered_map<uint64_t, VertexCacheEntry> vertex_cache;
  // Converted 32-bit index lists of indexed draws, the same way (index fetch from
  // the upload heap, i.e. system memory, made Dam's big draws slow on the GPU).
  struct IndexCacheEntry {
    GpuAlloc buffer;
    uint64_t capacity = 0, content = 0, last_frame = 0;
    uint32_t misses = 0, count = 0, vcount = 0;
    int64_t lo = 0;
    RenderPrimitiveTopology topo = RenderPrimitiveTopology::TRIANGLE_LIST;
  };
  std::unordered_map<uint64_t, IndexCacheEntry> index_cache;
  uint32_t vcache_hits = 0, vcache_misses = 0, last_vcache_hits = 0, last_vcache_misses = 0;
  uint32_t next_slot = 1;

  bool failed = false;
  bool recording = false;
  bool layout_bound = false;  // root signature + descriptor tables set on the current list
  uint64_t frames = 0;
  uint32_t draws = 0, skipped = 0;
  uint32_t skip_reason[8] = {};  // shader, decl, prim, range, stream, pipeline, upload
  // Creation work on the game thread this frame (and the last presented one).
  double pipe_ms = 0, tex_ms = 0, last_pipe_ms = 0, last_tex_ms = 0;
  double sect[5] = {}, last_sect[5] = {};
  double present_ms[4] = {};  // last present: acquire, submit, present, fence wait
  double tex_decode_ms = 0, tex_mip_ms = 0, last_tex_decode_ms = 0, last_tex_mip_ms = 0;
  double vdecode_ms = 0, last_vdecode_ms = 0;
  double flush_ms = 0, alloc_ms = 0, hash_ms = 0, last_flush_ms = 0, last_alloc_ms = 0, last_hash_ms = 0;
  uint32_t flushes = 0, last_flushes = 0;  // draw path: parse, indices, decode, state+submit, total
  uint32_t pipe_n = 0, tex_n = 0, last_pipe_n = 0, last_tex_n = 0;
  uint32_t tex_requests = 0;  // new textures asked for this frame (many = a level load)
  // Present pacing and its statistics.
  double pace_period = 1000.0 / 60.0, pace_next = 0, pace_last_arrival = 0, pace_waited_ms = 0;
  double pace_last_present = 0, pace_wait_sum = 0;
  std::vector<double> present_intervals;
  uint32_t last_counter = 0, double_steps = 0;
};

Native g;

inline bool tracing() {
  const int32_t f = REXCVAR_GET(gpu_native_trace_frame);
  return f > 0 && g.frames + 1 == uint64_t(f);
}
#define NTRACE(...) do { if (tracing()) REXKRNL_INFO("GENATIVE trace " __VA_ARGS__); } while (0)

struct Stream {
  uint32_t vb = 0, offset = 0, stride = 0;
};
Stream g_streams[16];
uint32_t g_textures[32];                                   // guest texture per sampler
std::atomic<uint32_t> g_clear_color{0xFF000000u};
uint32_t g_spec_bits = 2;
uint32_t g_vs_cb_bytes = 4096, g_ps_cb_bytes = 3584;  // what the current draw's shaders read  // shared c17.y: bit 0 R11G11B10 normals, bit 1 alpha test
uint32_t g_up_prim = 0, g_up_count = 0, g_up_stride = 0, g_up_data = 0;  // Begin/EndVertices

std::wstring exe_dir() {
  wchar_t path[MAX_PATH];
  const DWORD n = GetModuleFileNameW(nullptr, path, MAX_PATH);
  std::wstring s(path, path + n);
  const size_t slash = s.find_last_of(L"\\/");
  return slash == std::wstring::npos ? L"." : s.substr(0, slash);
}

void flush_mid_frame();

// Makes room for `size` more bytes of upload data, submitting the frame so far
// and waiting for it if the ring is too full. Only call it where no command
// state for the current draw has been recorded yet (the start of a draw, or a
// texture upload done before the draw's own allocations).
void ensure_room(uint64_t size) {
  if (g.upload_used + size + 4096 > kSlotUpload || g.const_used + 16384 > kSlotConsts) flush_mid_frame();
}

uint64_t const_alloc(uint64_t size) {
  const uint64_t off = (g.const_used + 255) & ~255ull;
  if (off + size > kSlotConsts) return UINT64_MAX;
  g.const_used = off + size;
  return off;
}

// Submits the current slot's list, preceded by the copy of this submission's
// constants into the GPU-local constant buffer.
void submit_frame_lists(RenderCommandSemaphore** waits, uint32_t wait_count, RenderCommandSemaphore** signals,
                        uint32_t signal_count) {
  const RenderCommandList* lists[2];
  uint32_t n = 0;
  if (g.const_used) {
    RenderCommandList* c = g.const_lists[g.slot].get();
    RenderBuffer* dst = g.const_gpu[g.slot].get();
    c->begin();
    if (g.timestamps) c->writeTimestamp(g.timestamps.get(), 2 * kFrames + g.slot);
    c->barriers(RenderBarrierStage::COPY, RenderBufferBarrier(dst, RenderBufferAccess::WRITE));
    c->copyBufferRegion(RenderBufferReference(dst, 0), RenderBufferReference(g.const_upload.get(), g.slot * kSlotConsts),
                        g.const_used);
    c->barriers(RenderBarrierStage::GRAPHICS, RenderBufferBarrier(dst, RenderBufferAccess::READ));
    c->end();
    lists[n++] = c;
  }
  lists[n++] = g.cmd;
  g.queue->executeCommandLists(lists, n, waits, wait_count, signals, signal_count, g.fence);
}

double now_ms() {
  static LARGE_INTEGER freq{};
  if (!freq.QuadPart) QueryPerformanceFrequency(&freq);
  LARGE_INTEGER t;
  QueryPerformanceCounter(&t);
  return double(t.QuadPart) * 1000.0 / double(freq.QuadPart);
}

GpuAlloc pool_alloc_impl(BufferPool& pool, uint64_t size);
GpuAlloc pool_alloc(BufferPool& pool, uint64_t size) {
  const double t0 = now_ms();
  const GpuAlloc a = pool_alloc_impl(pool, size);
  g.alloc_ms += now_ms() - t0;
  return a;
}

GpuAlloc pool_alloc_impl(BufferPool& pool, uint64_t size) {
  uint32_t cls = 0;
  while (cls + 1 < kSizeClasses && class_bytes(cls) < size) ++cls;
  if (!pool.free_lists[cls].empty()) {
    const GpuAlloc a = pool.free_lists[cls].back();
    pool.free_lists[cls].pop_back();
    return a;
  }
  const uint64_t bytes = class_bytes(cls);
  if (bytes > pool.chunk) {  // larger than a chunk: a buffer of its own
    pool.chunks.insert(pool.chunks.begin(), g.device->createBuffer(RenderBufferDesc::DefaultBuffer(bytes, pool.flags)));
    return GpuAlloc{pool.chunks.front().get(), 0, cls};
  }
  if (pool.bump == UINT64_MAX || pool.bump + bytes > pool.chunk) {
    pool.chunks.push_back(g.device->createBuffer(RenderBufferDesc::DefaultBuffer(pool.chunk, pool.flags)));
    pool.bump = 0;
  }
  const GpuAlloc a{pool.chunks.back().get(), pool.bump, cls};
  pool.bump += bytes;
  return a;
}

// Back to its pool's free list once no frame in flight can use it.
// Converted vertex and index data kept on the GPU (see end_frame) up to this.
constexpr uint64_t kGeometryBudget = 256ull << 20;

void retire(BufferPool& pool, GpuAlloc& a) {
  if (!a) return;
  g.retired.push_back({g.frames, &pool, a});
  a = GpuAlloc{};
}

uint64_t upload_alloc(uint64_t size, uint64_t align = 256) {
  uint64_t off = (g.upload_used + align - 1) & ~(align - 1);
  if (off + size > kSlotUpload) return UINT64_MAX;
  g.upload_used = off + size;
  return g.slot * kSlotUpload + off;
}


// D3D12 debug layer (gpu_native_debug_layer): messages are copied to the game log.
void drain_debug_messages(const char* where) {
  if (!REXCVAR_GET(gpu_native_debug_layer) || !g.device) return;
  auto* dev = static_cast<D3D12Device*>(g.device.get())->d3d;
  ID3D12InfoQueue* q = nullptr;
  if (FAILED(dev->QueryInterface(IID_PPV_ARGS(&q)))) return;
  const UINT64 n = q->GetNumStoredMessages();
  for (UINT64 i = 0; i < n && i < 20; ++i) {
    SIZE_T len = 0;
    q->GetMessage(i, nullptr, &len);
    std::vector<uint8_t> buf(len);
    auto* m = reinterpret_cast<D3D12_MESSAGE*>(buf.data());
    if (SUCCEEDED(q->GetMessage(i, m, &len)) && m->Severity <= D3D12_MESSAGE_SEVERITY_WARNING)
      REXKRNL_WARN("GENATIVE d3d12 ({}): {}", where, std::string(m->pDescription, m->DescriptionByteLength));
  }
  q->ClearStoredMessages();
  q->Release();
  rex::FlushLogging();
}

void create_swap_framebuffers() {
  g.swap_framebuffers.clear();
  for (uint32_t i = 0; i < g.swapchain->getTextureCount(); i++) {
    const RenderTexture* color = g.swapchain->getTexture(i);
    g.swap_framebuffers.push_back(g.device->createFramebuffer(RenderFramebufferDesc(&color, 1)));
  }
}

rex::ui::d3d12::D3D12Provider* sdk_provider() {
  auto* ks = rex::system::kernel_state();
  auto* rt = ks ? ks->emulator() : nullptr;
  auto* gs = rt ? rt->graphics_system() : nullptr;
  if (!gs || !gs->presenter() || !gs->provider()) return nullptr;
  // ge_app.h installs the SDK's PresenterGraphicsSystem in native mode: D3D12.
  return static_cast<rex::ui::d3d12::D3D12Provider*>(gs->provider());
}

// The presenter's adapter by name, so Plume opens its device on the same GPU
// (shared textures only work within one adapter).
std::string sdk_adapter_name() {
  auto* provider = REXCVAR_GET(gpu_native_presenter) ? sdk_provider() : nullptr;
  if (!provider) return {};
  IDXGIFactory4* factory = nullptr;
  if (FAILED(CreateDXGIFactory1(IID_PPV_ARGS(&factory)))) return {};
  std::string name;
  IDXGIAdapter1* adapter = nullptr;
  if (SUCCEEDED(factory->EnumAdapterByLuid(provider->GetDevice()->GetAdapterLuid(), IID_PPV_ARGS(&adapter)))) {
    DXGI_ADAPTER_DESC1 desc{};
    if (SUCCEEDED(adapter->GetDesc1(&desc))) {
      char utf8[256] = {};
      WideCharToMultiByte(CP_UTF8, 0, desc.Description, -1, utf8, sizeof(utf8) - 1, nullptr, nullptr);
      name = utf8;
    }
    adapter->Release();
  }
  factory->Release();
  return name;
}

// The output pass on the presenter's device: converts a shared B8G8R8A8 frame
// into its R10G10B10A2 guest output, with the port's own touches that the
// Xenos path applies after its swap: the menu backdrop (the desk behind the
// front-end folder, the boot cover) and the colour grade (postfx_*). Per pixel
// the same as the SDK's ge_backdrop.cs.hlsl and ge_grade.cs.hlsl, in one pass.
constexpr char kOutputShader[] = R"(
#ifdef __spirv__
// Vulkan (ge_native_vk_out.cpp): push constants and bindings 0-4.
struct PushConsts {
  uint2 size;
  float uv_scale_x, uv_scale_y, uv_offset_x, uv_offset_y;
  float key_r, key_g, key_b, key_tolerance;
  uint layout_4x3;
  uint mode;
  float brightness, contrast, saturation, vibrance, temperature, gamma_;
  float tint_r, tint_g, tint_b, tint_strength;
};
[[vk::push_constant]] ConstantBuffer<PushConsts> pc;
#define size pc.size
#define uv_scale_x pc.uv_scale_x
#define uv_scale_y pc.uv_scale_y
#define uv_offset_x pc.uv_offset_x
#define uv_offset_y pc.uv_offset_y
#define key_r pc.key_r
#define key_g pc.key_g
#define key_b pc.key_b
#define key_tolerance pc.key_tolerance
#define layout_4x3 pc.layout_4x3
#define mode pc.mode
#define brightness pc.brightness
#define contrast pc.contrast
#define saturation pc.saturation
#define vibrance pc.vibrance
#define temperature pc.temperature
#define gamma_ pc.gamma_
#define tint_r pc.tint_r
#define tint_g pc.tint_g
#define tint_b pc.tint_b
#define tint_strength pc.tint_strength
[[vk::binding(0)]] Texture2D<float4> src;
[[vk::binding(1)]] Texture2D<float4> image;
[[vk::binding(2)]] Texture2D<float4> ramp;
[[vk::binding(3)]] SamplerState image_sampler;
[[vk::binding(4)]] [[vk::image_format("rgb10a2")]] RWTexture2D<float4> dst;
#else
cbuffer push_consts : register(b0) {
  uint2 size;
  float uv_scale_x, uv_scale_y, uv_offset_x, uv_offset_y;
  float key_r, key_g, key_b, key_tolerance;
  uint layout_4x3;
  uint mode;  // 1 backdrop, 2 boot cover (all black), 4 colour grade, 8 gamma ramp, 16 FXAA, 32 FXAA extreme
  float brightness, contrast, saturation, vibrance, temperature, gamma_;
  float tint_r, tint_g, tint_b, tint_strength;
};
Texture2D<float4> src : register(t0);
Texture2D<float4> image : register(t1);
Texture2D<float4> ramp : register(t2);  // the game's gamma ramp: 256 x 1, R/G/B tables
SamplerState image_sampler : register(s0);
RWTexture2D<float4> dst : register(u0);
#endif

static const float kCoverLeft = 28.0;
static const float kCoverTop = 22.0;
static const float kSpineEnd = 148.0;
static const float kBoardTop = 28.0;
static const float kBoardRight = 1065.0;
static const float kChamferX = 1016.0;
static const float kFolderBottom = 100000.0;
static const float kCorner = 22.0;
static const float2 kShadowShift = float2(5.0, 7.0);
static const float kShadowReach = 22.0;
static const float kShadowStrength = 0.55;
static const float kFar = 100000.0;

float sd_round_box(float2 p, float2 lo, float2 hi, float r) {
  float2 centre = 0.5 * (lo + hi);
  float2 half_size = 0.5 * (hi - lo) - r;
  float2 q = abs(p - centre) - half_size;
  return length(max(q, 0.0)) + min(max(q.x, q.y), 0.0) - r;
}

float3 backdrop(float3 c, uint2 id) {
  float2 pos = (float2(id) + 0.5) / float2(size);
  bool key = c.g <= key_tolerance && abs(c.r - c.b) <= key_tolerance && c.r >= key_r && c.r <= key_b;
  float hi = max(c.r, max(c.g, c.b));
  float lo = min(c.r, min(c.g, c.b));
  bool clip = hi > 0.25 && hi - lo < 0.08;
  bool crosshair = c.r > 0.6 && c.g < c.r * 0.3 && c.b < c.r * 0.3;
  float2 p = pos * float2(1280.0, 720.0);
  // A 4:3 front end is the middle 960 units of the 16:9 one, stretched to the
  // frame: map back so the folder outline measured for 16:9 trims it the same.
  [branch] if (layout_4x3 != 0u) p.x = 160.0 + p.x * 0.75;
  float px_per_unit = float(size.y) / 720.0;
  // Only where the main menu's (left) or a briefing's (right) photo clip is.
  bool clip_area = (p.x > 320.0 && p.x < 370.0) || (p.x > 744.0 && p.x < 794.0);
  bool keep_clip = clip && clip_area && p.y < 40.0;
  bool left_zone = p.x < kSpineEnd;
  float edge_d;
  [branch] if (left_zone) {
    edge_d = sd_round_box(p, float2(kCoverLeft, kCoverTop), float2(kFar, kFolderBottom), kCorner);
  } else {
    edge_d = sd_round_box(p, float2(-kFar, -kFar), float2(kBoardRight, kFolderBottom), kCorner);
    edge_d = max(edge_d, (p.x - p.y - kChamferX) * 0.7071);
  }
  bool soft_edge = !crosshair && !keep_clip && edge_d * px_per_unit > -2.5;
  bool top_left = p.x > kSpineEnd && p.x < 232.0;
  float top_edge = top_left ? 27.5 : kBoardTop;
  bool top_cut = !left_zone && p.y < top_edge && !crosshair && !keep_clip;
  [branch] if (!key && !top_cut && !soft_edge) {
    return c;
  }
  float2 q = p - kShadowShift;
  float dist = min(sd_round_box(q, float2(kCoverLeft, kCoverTop), float2(232.0, kFolderBottom), kCorner),
                   sd_round_box(q, float2(150.0, kBoardTop), float2(kBoardRight, kFolderBottom), kCorner));
  float shadow = 1.0 - kShadowStrength * (1.0 - smoothstep(0.0, kShadowReach, dist)) *
                           (1.0 - smoothstep(704.0, 719.0, p.y));
  float alpha = 1.0;
  float3 base = c;
  [branch] if (!key) {
    [branch] if (top_cut) {
      [branch] if (top_left) {
        float n = dot(c, float3(0.3, 0.59, 0.11));
        alpha = p.y < 20.0 ? 1.0 : smoothstep(0.11, 0.16, n);
      }
    } else {
      alpha = saturate(edge_d * px_per_unit + 0.5);
      float rim = saturate(1.0 + edge_d * px_per_unit * 0.5);
      base = c * (1.0 - 0.3 * rim);
    }
  }
  float2 uv = pos * float2(uv_scale_x, uv_scale_y) + float2(uv_offset_x, uv_offset_y);
  float3 desk = image.SampleLevel(image_sampler, uv, 0.0).rgb * shadow;
  return lerp(base, desk, alpha);
}

float3 grade(float3 c) {
  c.r += temperature * 0.10;
  c.b -= temperature * 0.10;
  c += brightness;
  c = (c - 0.5) * contrast + 0.5;
  float3 tint = float3(tint_r, tint_g, tint_b);
  c = lerp(c, c * tint, tint_strength);
  float luma = dot(max(c, 0.0), float3(0.2126, 0.7152, 0.0722));
  c = lerp(float3(luma, luma, luma), c, saturation);
  float mx = max(c.r, max(c.g, c.b));
  float mn = min(c.r, min(c.g, c.b));
  c = lerp(float3(luma, luma, luma), c, 1.0 + vibrance * (1.0 - saturate(mx - mn)));
  c = pow(max(c, 0.0001), 1.0 / gamma_);
  return saturate(c);
}

// FXAA as the classic renderer's (the SDK's swap FXAA: 16 = normal, 32 =
// extreme), on the frame before the gamma ramp.
float fxaa_luma(float2 uv) { return dot(src.SampleLevel(image_sampler, uv, 0.0).rgb, float3(0.299, 0.587, 0.114)); }

float3 fxaa(uint2 id, bool extreme) {
  float edge = extreme ? 0.063 : 0.166, edge_min = extreme ? 0.0312 : 0.0833, span_max = extreme ? 12.0 : 8.0;
  float2 texel = 1.0 / float2(size);
  float2 uv = (float2(id) + 0.5) * texel;
  float3 rgbm = src.SampleLevel(image_sampler, uv, 0.0).rgb;
  float luma_m = dot(rgbm, float3(0.299, 0.587, 0.114));
  float luma_nw = fxaa_luma(uv + float2(-texel.x, -texel.y));
  float luma_ne = fxaa_luma(uv + float2(texel.x, -texel.y));
  float luma_sw = fxaa_luma(uv + float2(-texel.x, texel.y));
  float luma_se = fxaa_luma(uv + float2(texel.x, texel.y));
  float luma_min = min(luma_m, min(min(luma_nw, luma_ne), min(luma_sw, luma_se)));
  float luma_max = max(luma_m, max(max(luma_nw, luma_ne), max(luma_sw, luma_se)));
  [branch] if (luma_max - luma_min < max(edge_min, luma_max * edge)) return rgbm;
  float2 dir;
  dir.x = -((luma_nw + luma_ne) - (luma_sw + luma_se));
  dir.y = ((luma_nw + luma_sw) - (luma_ne + luma_se));
  float reduce = max((luma_nw + luma_ne + luma_sw + luma_se) * (0.25 * 0.125), 1.0 / 128.0);
  float rcp_min = 1.0 / (min(abs(dir.x), abs(dir.y)) + reduce);
  dir = clamp(dir * rcp_min, -span_max, span_max) * texel;
  float3 rgb_a = 0.5 * (src.SampleLevel(image_sampler, uv + dir * (1.0 / 3.0 - 0.5), 0.0).rgb +
                        src.SampleLevel(image_sampler, uv + dir * (2.0 / 3.0 - 0.5), 0.0).rgb);
  float3 rgb_b = rgb_a * 0.5 + 0.25 * (src.SampleLevel(image_sampler, uv + dir * -0.5, 0.0).rgb +
                                       src.SampleLevel(image_sampler, uv + dir * 0.5, 0.0).rgb);
  float luma_b = dot(rgb_b, float3(0.299, 0.587, 0.114));
  return (luma_b < luma_min || luma_b > luma_max) ? rgb_a : rgb_b;
}

[numthreads(8, 8, 1)] void main(uint3 id : SV_DispatchThreadID) {
  [branch] if (id.x >= size.x || id.y >= size.y) return;
  [branch] if (mode & 2u) {
    dst[id.xy] = float4(0.0, 0.0, 0.0, 1.0);
    return;
  }
  float3 c = (mode & 48u) ? fxaa(id.xy, (mode & 32u) != 0u) : src[id.xy].rgb;
  // The game's display gamma ramp, as the Xenos path applies its 256-entry
  // table before the port's own passes (the desk picture is not ramped).
  [branch] if (mode & 8u) {
    uint3 i = uint3(round(saturate(c) * 255.0));
    c = float3(ramp.Load(int3(i.r, 0, 0)).r, ramp.Load(int3(i.g, 0, 0)).g, ramp.Load(int3(i.b, 0, 0)).b);
  }
  // The Xenos path keys the backdrop on its 10-bit output: same precision here.
  c = round(c * 1023.0) / 1023.0;
  [branch] if (mode & 1u) c = backdrop(c, id.xy);
  [branch] if (mode & 4u) c = grade(c);
  dst[id.xy] = float4(c, 1.0);
}
)";

bool share_handle(ID3D12Device* from, ID3D12DeviceChild* object, ID3D12Device* to, REFIID iid, void** opened) {
  HANDLE h = nullptr;
  if (FAILED(from->CreateSharedHandle(object, nullptr, GENERIC_ALL, nullptr, &h))) return false;
  const bool ok = SUCCEEDED(to->OpenSharedHandle(h, iid, opened));
  CloseHandle(h);
  return ok;
}

struct OutputConstants {  // kOutputShader's push_consts
  uint32_t size[2];
  float uv_scale[2], uv_offset[2];
  float key[3], key_tolerance;
  uint32_t layout_4x3, mode;
  float brightness, contrast, saturation, vibrance, temperature, gamma;
  float tint_r, tint_g, tint_b, tint_strength;
};

bool setup_presenter_output() {
  rex::ui::d3d12::D3D12Provider* provider = REXCVAR_GET(gpu_native_presenter) ? sdk_provider() : nullptr;
  if (!provider) return false;
  Native::Out& o = g.out;
  ID3D12Device* dev = static_cast<D3D12Device*>(g.device.get())->d3d;
  o.pdev = provider->GetDevice();
  o.pqueue = provider->GetDirectQueue();
  const LUID a = dev->GetAdapterLuid(), b = o.pdev->GetAdapterLuid();
  if (a.LowPart != b.LowPart || a.HighPart != b.HighPart) {
    REXKRNL_WARN("GENATIVE the presenter is on another GPU: using a swap chain of our own");
    return false;
  }
  D3D12_HEAP_PROPERTIES heap{};
  heap.Type = D3D12_HEAP_TYPE_DEFAULT;
  D3D12_RESOURCE_DESC rd{};
  rd.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
  rd.Width = g_scene_w;
  rd.Height = g_scene_h;
  rd.DepthOrArraySize = 1;
  rd.MipLevels = 1;
  rd.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
  rd.SampleDesc.Count = 1;
  for (int i = 0; i < 3; ++i) {
    if (FAILED(dev->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_SHARED, &rd, D3D12_RESOURCE_STATE_COMMON, nullptr,
                                            IID_PPV_ARGS(&o.tex[i]))) ||
        !share_handle(dev, o.tex[i], o.pdev, IID_PPV_ARGS(&o.ptex[i])))
      return false;
  }
  if (FAILED(dev->CreateFence(0, D3D12_FENCE_FLAG_SHARED, IID_PPV_ARGS(&o.ready))) ||
      !share_handle(dev, o.ready, o.pdev, IID_PPV_ARGS(&o.ready_p)) ||
      FAILED(o.pdev->CreateFence(0, D3D12_FENCE_FLAG_SHARED, IID_PPV_ARGS(&o.done_p))) ||
      !share_handle(o.pdev, o.done_p, dev, IID_PPV_ARGS(&o.done)))
    return false;

  ID3DBlob* code = nullptr;
  ID3DBlob* errors = nullptr;
  if (FAILED(D3DCompile(kOutputShader, sizeof(kOutputShader) - 1, "ge_native_output", nullptr, nullptr, "main",
                        "cs_5_0", D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &code, &errors))) {
    REXKRNL_ERROR("GENATIVE output shader: {}",
                  errors ? std::string(static_cast<const char*>(errors->GetBufferPointer()), errors->GetBufferSize()) : "");
    if (errors) errors->Release();
    return false;
  }
  D3D12_DESCRIPTOR_RANGE ranges[2] = {};
  ranges[0].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
  ranges[0].NumDescriptors = 3;
  ranges[1].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_UAV;
  ranges[1].NumDescriptors = 1;
  ranges[1].OffsetInDescriptorsFromTableStart = 3;
  D3D12_ROOT_PARAMETER params[2] = {};
  params[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
  params[0].Constants.Num32BitValues = sizeof(OutputConstants) / 4;
  params[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
  params[1].DescriptorTable.NumDescriptorRanges = 2;
  params[1].DescriptorTable.pDescriptorRanges = ranges;
  D3D12_STATIC_SAMPLER_DESC sampler{};
  sampler.Filter = D3D12_FILTER_MIN_MAG_MIP_LINEAR;
  sampler.AddressU = sampler.AddressV = sampler.AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
  sampler.MaxLOD = D3D12_FLOAT32_MAX;
  D3D12_ROOT_SIGNATURE_DESC rs{};
  rs.NumParameters = 2;
  rs.pParameters = params;
  rs.NumStaticSamplers = 1;
  rs.pStaticSamplers = &sampler;
  ID3DBlob* rs_blob = nullptr;
  bool ok = SUCCEEDED(D3D12SerializeRootSignature(&rs, D3D_ROOT_SIGNATURE_VERSION_1, &rs_blob, nullptr)) &&
            SUCCEEDED(o.pdev->CreateRootSignature(0, rs_blob->GetBufferPointer(), rs_blob->GetBufferSize(),
                                                  IID_PPV_ARGS(&o.root)));
  if (rs_blob) rs_blob->Release();
  if (ok) {
    D3D12_COMPUTE_PIPELINE_STATE_DESC pd{};
    pd.pRootSignature = o.root;
    pd.CS = {code->GetBufferPointer(), code->GetBufferSize()};
    ok = SUCCEEDED(o.pdev->CreateComputePipelineState(&pd, IID_PPV_ARGS(&o.pso)));
  }
  code->Release();
  D3D12_DESCRIPTOR_HEAP_DESC hd{};
  hd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
  hd.NumDescriptors = 12;  // frame, backdrop, gamma ramp and output per command list
  hd.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
  ok = ok && SUCCEEDED(o.pdev->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&o.heap)));
  o.heap_step = o.pdev->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
  for (int i = 0; ok && i < 3; ++i)
    ok = SUCCEEDED(o.pdev->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&o.alloc[i])));
  ok = ok && SUCCEEDED(o.pdev->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, o.alloc[0], nullptr,
                                                 IID_PPV_ARGS(&o.list))) &&
       SUCCEEDED(o.list->Close());
  o.event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
  if (!ok || !o.event) return false;
  o.presenter = static_cast<rex::system::IGraphicsSystem*>(rex::system::kernel_state()->emulator()->graphics_system())
                    ->presenter();
  return true;
}

D3D12_RESOURCE_BARRIER transition(ID3D12Resource* r, D3D12_RESOURCE_STATES from, D3D12_RESOURCE_STATES to) {
  D3D12_RESOURCE_BARRIER b{};
  b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
  b.Transition.pResource = r;
  b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
  b.Transition.StateBefore = from;
  b.Transition.StateAfter = to;
  return b;
}

// The menu backdrop picture of one shape (0 = 16:9, 1 = 21:9), as the Xenos
// path loads it: from <exe>\resources, else beside the exe. The upload is
// recorded into `list`; the upload buffer is kept for the rest of the run.
bool ensure_backdrop(int kind, ID3D12GraphicsCommandList* list) {
  Native::Out& o = g.out;
  if (o.backdrop[kind]) return true;
  if (o.backdrop_tried[kind]) return false;
  o.backdrop_tried[kind] = true;
  const std::filesystem::path exe = rex::filesystem::GetExecutableFolder();
  std::vector<uint8_t> bytes;
  for (const std::filesystem::path& dir : {exe / "resources", exe}) {
    for (const char* name : {kind ? "menu-backdrop-219.png" : "menu-backdrop-169.png", "menu-backdrop.png"}) {
      std::ifstream file(dir / name, std::ios::binary);
      if (file) {
        bytes.assign(std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>());
        break;
      }
    }
    if (!bytes.empty()) break;
  }
  int w = 0, h = 0;
  std::vector<uint8_t> pixels = bytes.empty() ? std::vector<uint8_t>() : rex::ui::DecodeImageRGBA(bytes.data(), bytes.size(), w, h);
  if (pixels.empty() || w <= 0 || h <= 0 || w > 8192 || h > 8192) return false;
  D3D12_HEAP_PROPERTIES heap{};
  heap.Type = D3D12_HEAP_TYPE_DEFAULT;
  D3D12_RESOURCE_DESC td{};
  td.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
  td.Width = UINT64(w);
  td.Height = UINT(h);
  td.DepthOrArraySize = 1;
  td.MipLevels = 1;
  td.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
  td.SampleDesc.Count = 1;
  D3D12_PLACED_SUBRESOURCE_FOOTPRINT fp{};
  UINT64 upload_size = 0;
  o.pdev->GetCopyableFootprints(&td, 0, 1, 0, &fp, nullptr, nullptr, &upload_size);
  D3D12_HEAP_PROPERTIES upload_heap{};
  upload_heap.Type = D3D12_HEAP_TYPE_UPLOAD;
  D3D12_RESOURCE_DESC bd{};
  bd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
  bd.Width = upload_size;
  bd.Height = 1;
  bd.DepthOrArraySize = 1;
  bd.MipLevels = 1;
  bd.SampleDesc.Count = 1;
  bd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
  uint8_t* mapped = nullptr;
  if (FAILED(o.pdev->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &td, D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
                                             IID_PPV_ARGS(&o.backdrop[kind]))) ||
      FAILED(o.pdev->CreateCommittedResource(&upload_heap, D3D12_HEAP_FLAG_NONE, &bd, D3D12_RESOURCE_STATE_GENERIC_READ,
                                             nullptr, IID_PPV_ARGS(&o.backdrop_upload[kind]))) ||
      FAILED(o.backdrop_upload[kind]->Map(0, nullptr, reinterpret_cast<void**>(&mapped)))) {
    if (o.backdrop[kind]) o.backdrop[kind]->Release();
    if (o.backdrop_upload[kind]) o.backdrop_upload[kind]->Release();
    o.backdrop[kind] = o.backdrop_upload[kind] = nullptr;
    return false;
  }
  for (int y = 0; y < h; ++y)
    std::memcpy(mapped + fp.Offset + size_t(y) * fp.Footprint.RowPitch, pixels.data() + size_t(y) * w * 4, size_t(w) * 4);
  o.backdrop_upload[kind]->Unmap(0, nullptr);
  D3D12_TEXTURE_COPY_LOCATION to{}, from{};
  to.pResource = o.backdrop[kind];
  to.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
  from.pResource = o.backdrop_upload[kind];
  from.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
  from.PlacedFootprint = fp;
  list->CopyTextureRegion(&to, 0, 0, 0, &from, nullptr);
  D3D12_RESOURCE_BARRIER b = transition(o.backdrop[kind], D3D12_RESOURCE_STATE_COPY_DEST,
                                        D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
  list->ResourceBarrier(1, &b);
  o.backdrop_w[kind] = uint32_t(w);
  o.backdrop_h[kind] = uint32_t(h);
  rex::cvar::SetFlagByName("ge_backdrop_ready", "true");
  REXKRNL_INFO("GENATIVE menu backdrop picture {}x{} loaded", w, h);
  return true;
}

float cvar_float(const char* name, float def) {
  const std::string v = rex::cvar::GetFlagByName(name);
  return v.empty() ? def : float(std::atof(v.c_str()));
}

// The output pass's settings for this frame, as the Xenos swap sets up its
// backdrop and grade passes. Returns the backdrop picture to bind (or none).
// The backdrop picture of a shape (0 = 16:9, 1 = 21:9) for the API in use:
// made ready and its size given, or false.
using BackdropFn = std::function<bool(int kind, uint32_t& width, uint32_t& height)>;
int fill_output_constants(OutputConstants& c, const BackdropFn& backdrop);

ID3D12Resource* output_constants(OutputConstants& c, ID3D12GraphicsCommandList* list) {
  Native::Out& o = g.out;
  const int kind = fill_output_constants(c, [&](int k, uint32_t& w, uint32_t& h) {
    if (!ensure_backdrop(k, list)) return false;
    w = o.backdrop_w[k];
    h = o.backdrop_h[k];
    return true;
  });
  return kind >= 0 ? o.backdrop[kind] : nullptr;
}

int fill_output_constants(OutputConstants& c, const BackdropFn& backdrop) {
  int used = -1;
  c = {};
  c.size[0] = g_scene_w;
  c.size[1] = g_scene_h;
  if (rex::cvar::GetFlagByName("ge_boot_cover") == "true") {
    c.mode |= 2;
  } else if (ge::BackdropActiveNow()) {
    const float window = cvar_float("ge_backdrop_window_aspect", 0.0f);
    int kind = window >= 2.05f ? 1 : 0;
    uint32_t bw = 0, bh = 0;
    if (!backdrop(kind, bw, bh)) kind = 1 - kind;
    if (backdrop(kind, bw, bh)) {
      used = kind;
      c.mode |= 1;
      float shown = float(g_scene_w) / float(g_scene_h);
      const float ax = cvar_float("present_aspect_x", 0.0f), ay = cvar_float("present_aspect_y", 0.0f);
      if (ax > 0.0f && ay > 0.0f) shown = ax / ay;
      const float win = window > 0.1f && window < 10.0f ? window : shown;
      float out_scale[2] = {1.0f, 1.0f};
      if (win > shown) out_scale[0] = shown / win; else out_scale[1] = win / shown;
      const float image = float(bw) / float(std::max(1u, bh));
      float img_scale[2] = {1.0f, 1.0f};
      if (win > image) img_scale[1] = image / win; else img_scale[0] = win / image;
      for (int i = 0; i < 2; ++i) {
        c.uv_scale[i] = out_scale[i] * img_scale[i];
        c.uv_offset[i] = (1.0f - out_scale[i]) * 0.5f * img_scale[i] + (1.0f - img_scale[i]) * 0.5f;
      }
      // The game paints RGB (16, 0, 16) behind its folder: a dark (k, 0, k).
      c.key[0] = 0.5f / 1023.0f;
      c.key[1] = 0.0f;
      c.key[2] = 96.0f / 1023.0f;
      c.key_tolerance = 0.5f / 1023.0f;
      // The game lays the front end out for the Screen Ratio it was given, which is the
      // presented shape, not the window's: 4:3 in a wider window or fullscreen kept the
      // 16:9 folder outline, and the desk covered the 4:3 folder's right side.
      c.layout_4x3 = shown < 1.55f ? 1u : 0u;
    }
  }
  // FXAA on top of the scene's MSAA, as the classic renderer's Anti-Aliasing
  // setting: not on the front end's desk (its colour key needs exact pixels).
  if (!(c.mode & 3u)) {
    const std::string fxaa = REXCVAR_GET(gpu_native_fxaa);
    if (fxaa == "fxaa") c.mode |= 16;
    else if (fxaa == "fxaa_extreme") c.mode |= 32;
  }
  if (rex::cvar::GetFlagByName("postfx_enabled") == "true") {
    c.mode |= 4;
    c.brightness = cvar_float("postfx_brightness", 0.0f);
    c.contrast = cvar_float("postfx_contrast", 1.0f);
    c.saturation = cvar_float("postfx_saturation", 1.0f);
    c.vibrance = cvar_float("postfx_vibrance", 0.0f);
    c.temperature = cvar_float("postfx_temperature", 0.0f);
    c.gamma = cvar_float("postfx_gamma", 1.0f);
    c.tint_r = cvar_float("postfx_tint_r", 1.0f);
    c.tint_g = cvar_float("postfx_tint_g", 1.0f);
    c.tint_b = cvar_float("postfx_tint_b", 1.0f);
    c.tint_strength = cvar_float("postfx_tint", 0.0f);
  }
  return used;
}

// The 360's D3D adjusts every gamma ramp for the display it is told about
// (VdGetCurrentDisplayGamma; the SDK reports a BT.709 HDTV) before writing it
// to the GPU: sRGB-encoded in, BT.709-encoded out. Under Xenos this is what
// turns GoldenEye's identity ramp into the darker table it shows with (entry
// 64 -> 194 of 1023); the native renderer applies the same.
uint16_t for_display(uint16_t value) {
  const double v = value / 65535.0;
  const double linear = v <= 0.04045 ? v / 12.92 : std::pow((v + 0.055) / 1.055, 2.4);
  const double out = linear < 0.018 ? 4.5 * linear : 1.099 * std::pow(linear, 0.45) - 0.099;
  return uint16_t(std::clamp(out, 0.0, 1.0) * 65535.0 + 0.5);
}

// Uploads the game's gamma ramp when it changed (recorded into `list`; list
// slot `a` owns the upload buffer). Returns whether the ramp texture exists.
bool update_gamma_ramp(ID3D12GraphicsCommandList* list, uint32_t a) {
  Native::Out& o = g.out;
  if (!g.gamma_ramp_version) return false;  // never set: identity, nothing to do
  if (!o.ramp) {
    D3D12_HEAP_PROPERTIES heap{};
    heap.Type = D3D12_HEAP_TYPE_DEFAULT;
    D3D12_RESOURCE_DESC td{};
    td.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    td.Width = 256;
    td.Height = 1;
    td.DepthOrArraySize = 1;
    td.MipLevels = 1;
    td.Format = DXGI_FORMAT_R16G16B16A16_UNORM;
    td.SampleDesc.Count = 1;
    if (FAILED(o.pdev->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &td, D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
                                               IID_PPV_ARGS(&o.ramp))))
      return false;
    o.ramp_version = 0;
  }
  if (o.ramp_version != g.gamma_ramp_version) {
    if (!o.ramp_upload[a]) {
      D3D12_HEAP_PROPERTIES heap{};
      heap.Type = D3D12_HEAP_TYPE_UPLOAD;
      D3D12_RESOURCE_DESC bd{};
      bd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
      bd.Width = 256 * 8;
      bd.Height = 1;
      bd.DepthOrArraySize = 1;
      bd.MipLevels = 1;
      bd.SampleDesc.Count = 1;
      bd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
      if (FAILED(o.pdev->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &bd, D3D12_RESOURCE_STATE_GENERIC_READ,
                                                 nullptr, IID_PPV_ARGS(&o.ramp_upload[a]))))
        return o.ramp_ready;
    }
    uint16_t* mapped = nullptr;
    if (FAILED(o.ramp_upload[a]->Map(0, nullptr, reinterpret_cast<void**>(&mapped)))) return o.ramp_ready;
    for (int i = 0; i < 256; ++i) {
      mapped[i * 4 + 0] = for_display(g.gamma_ramp[i]);
      mapped[i * 4 + 1] = for_display(g.gamma_ramp[256 + i]);
      mapped[i * 4 + 2] = for_display(g.gamma_ramp[512 + i]);
      mapped[i * 4 + 3] = 0xFFFF;
    }
    o.ramp_upload[a]->Unmap(0, nullptr);
    if (o.ramp_ready) {
      D3D12_RESOURCE_BARRIER b = transition(o.ramp, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COPY_DEST);
      list->ResourceBarrier(1, &b);
    }
    D3D12_TEXTURE_COPY_LOCATION to{}, from{};
    to.pResource = o.ramp;
    to.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    from.pResource = o.ramp_upload[a];
    from.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    from.PlacedFootprint.Footprint = {DXGI_FORMAT_R16G16B16A16_UNORM, 256, 1, 1, 256 * 8};
    list->CopyTextureRegion(&to, 0, 0, 0, &from, nullptr);
    D3D12_RESOURCE_BARRIER b = transition(o.ramp, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    list->ResourceBarrier(1, &b);
    o.ramp_version = g.gamma_ramp_version;
    o.ramp_ready = true;
  }
  return o.ramp_ready;
}

// Hands shared frame k to the presenter: its queue waits for our copy, then
// converts the frame into the guest output it paints (with FSR, the overlays).
void present_to_presenter(uint32_t k) {
  Native::Out& o = g.out;
  o.presenter->RefreshGuestOutput(g_scene_w, g_scene_h, g_scene_w, g_scene_h,
                                  [k, &o](rex::ui::Presenter::GuestOutputRefreshContext& context) {
    auto& c = static_cast<rex::ui::d3d12::D3D12Presenter::D3D12GuestOutputRefreshContext&>(context);
    context.SetIs8bpc(true);
    const uint32_t a = o.next_list;
    o.next_list = (a + 1) % 3;
    if (o.list_done[a] && o.done_p->GetCompletedValue() < o.list_done[a]) {
      o.done_p->SetEventOnCompletion(o.list_done[a], o.event);
      WaitForSingleObject(o.event, INFINITE);
    }
    o.alloc[a]->Reset();
    o.list->Reset(o.alloc[a], o.pso);
    ID3D12Resource* dst = c.resource_uav_capable();
    OutputConstants constants;
    ID3D12Resource* picture = output_constants(constants, o.list);
    D3D12_CPU_DESCRIPTOR_HANDLE cpu = o.heap->GetCPUDescriptorHandleForHeapStart();
    cpu.ptr += SIZE_T(a) * 4 * o.heap_step;
    D3D12_SHADER_RESOURCE_VIEW_DESC sv{};
    sv.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    sv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    sv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    sv.Texture2D.MipLevels = 1;
    o.pdev->CreateShaderResourceView(o.ptex[k], &sv, cpu);
    cpu.ptr += o.heap_step;
    sv.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    o.pdev->CreateShaderResourceView(picture, &sv, cpu);
    cpu.ptr += o.heap_step;
    sv.Format = DXGI_FORMAT_R16G16B16A16_UNORM;
    o.pdev->CreateShaderResourceView(update_gamma_ramp(o.list, a) ? o.ramp : nullptr, &sv, cpu);
    if (o.ramp_ready) constants.mode |= 8;
    cpu.ptr += o.heap_step;
    D3D12_UNORDERED_ACCESS_VIEW_DESC uv{};
    uv.Format = rex::ui::d3d12::D3D12Presenter::kGuestOutputFormat;
    uv.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
    o.pdev->CreateUnorderedAccessView(dst, nullptr, &uv, cpu);
    constexpr auto kOutState = rex::ui::d3d12::D3D12Presenter::kGuestOutputInternalState;
    D3D12_RESOURCE_BARRIER in[2] = {
        transition(o.ptex[k], D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE),
        transition(dst, kOutState, D3D12_RESOURCE_STATE_UNORDERED_ACCESS)};
    o.list->ResourceBarrier(2, in);
    o.list->SetComputeRootSignature(o.root);
    o.list->SetDescriptorHeaps(1, &o.heap);
    D3D12_GPU_DESCRIPTOR_HANDLE gpu = o.heap->GetGPUDescriptorHandleForHeapStart();
    gpu.ptr += UINT64(a) * 4 * o.heap_step;
    o.list->SetComputeRoot32BitConstants(0, sizeof(constants) / 4, &constants, 0);
    o.list->SetComputeRootDescriptorTable(1, gpu);
    o.list->Dispatch((g_scene_w + 7) / 8, (g_scene_h + 7) / 8, 1);
    D3D12_RESOURCE_BARRIER back[2] = {
        transition(o.ptex[k], D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COMMON),
        transition(dst, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, kOutState)};
    o.list->ResourceBarrier(2, back);
    o.list->Close();
    o.pqueue->Wait(o.ready_p, o.tex_ready[k]);
    ID3D12CommandList* lists[] = {o.list};
    o.pqueue->ExecuteCommandLists(1, lists);
    o.pqueue->Signal(o.done_p, ++o.done_value);
    o.list_done[a] = o.tex_done[k] = o.done_value;
    return true;
  });
}

void prof_mark(std::string label);

// Vulkan: frames to the SDK's Vulkan presenter (ge_native_vk_out.cpp).
bool setup_presenter_output_vk() {
  auto* gs = static_cast<rex::system::IGraphicsSystem*>(rex::system::kernel_state()->emulator()->graphics_system());
  rex::ui::Presenter* presenter = gs ? gs->presenter() : nullptr;
  auto* pgs = dynamic_cast<rex::ui::PresenterGraphicsSystem*>(gs);
  if (!REXCVAR_GET(gpu_native_presenter) || !presenter || !pgs ||
      pgs->api() != rex::ui::PresenterGraphicsSystem::Api::kVulkan)
    return false;
  std::string source = kOutputShader;
  std::vector<uint8_t> spirv;
  std::string errors;
  if (!ge::native_shaders::CompileDxil(source, L"cs_6_0", spirv, errors)) {
    REXKRNL_WARN("GENATIVE Vulkan output pass: {}", errors.substr(0, 400));
    return false;
  }
  auto* dev = static_cast<VulkanDevice*>(g.device.get());
  ge::native_vk::Renderer r;
  r.instance = static_cast<VulkanInterface*>(g.api.get())->instance;
  r.physical_device = dev->physicalDevice;
  r.device = dev->vk;
  r.get_instance_proc_addr = reinterpret_cast<void*>(vkGetInstanceProcAddr);
  r.queue_family = static_cast<VulkanCommandQueue*>(g.queue.get())->familyIndex;
  if (!ge::native_vk::Setup(r, presenter, g_scene_w, g_scene_h, spirv, sizeof(OutputConstants))) return false;
  g.out.presenter = presenter;
  g.out.vk = true;
  return true;
}

// Vulkan: the frame's picture into the next shared frame; returns it (or
// UINT32_MAX when there is no picture).
uint32_t copy_to_presenter_vk() {
  const auto front = g.resolved.find(g.front_buffer);
  if (front == g.resolved.end() || !front->second.texture) return UINT32_MAX;
  const uint32_t k = ge::native_vk::NextFrame();
  RenderTexture* src = front->second.texture.get();
  g.cmd->barriers(RenderBarrierStage::COPY, RenderTextureBarrier(src, RenderTextureLayout::COPY_SOURCE));
  ge::native_vk::RecordCopy(static_cast<VulkanCommandList*>(g.cmd)->vk, static_cast<VulkanTexture*>(src)->vk, k);
  g.cmd->barriers(RenderBarrierStage::GRAPHICS, RenderTextureBarrier(src, RenderTextureLayout::SHADER_READ));
  return k;
}

// Tells the presenter a frame was just handed over. Its pacing thread shows a
// frame at "vblank + present_pacing_offset_us"; reporting the vblank as now
// minus the offset put that moment exactly at the frame's arrival, so the
// thread often woke a hair before the frame, found none, and replaced it with
// the next one a tick later: only ~45 of the game's 60 frames a second reached
// the screen (the logo spin, intro and videos looked uneven). The moment is now
// `margin_us` after `paced` (the frame's paced present moment on the game
// thread): Direct3D 12 hands the frame over right then (2 ms is plenty);
// Vulkan hands it over on a worker after waiting for the GPU, up to ~5 ms
// after the paced moment: anchoring to that jittery hand-over lost ~6 frames
// a second, and the paced moment plus the 5 ms offset still lost ~2 (frames
// arriving just after it), so Vulkan anchors to the steady paced moment with
// 9 ms (the reported vblank may then lie a little in the future).
void report_frame_to_presenter(rex::ui::Presenter* presenter, std::chrono::steady_clock::time_point paced,
                               int64_t margin_us) {
  static const int64_t offset_us = [] {
    const std::string v = rex::cvar::GetFlagByName("present_pacing_offset_us");
    return v.empty() ? int64_t(5000) : std::atoll(v.c_str());
  }();
  const int64_t lead_us = margin_us < offset_us ? offset_us - margin_us : -(margin_us - offset_us);
  presenter->OnGuestVblank(paced - std::chrono::microseconds(lead_us));
}

void present_to_presenter_vk(uint32_t k) {
  OutputConstants c;
  const int kind = fill_output_constants(c, [](int kind, uint32_t& w, uint32_t& h) {
    return ge::native_vk::Backdrop(kind, w, h);
  });
  static uint16_t ramp[768];
  static uint32_t ramp_version = 0;
  if (g.gamma_ramp_version) {
    if (ramp_version != g.gamma_ramp_version) {
      for (int i = 0; i < 768; ++i) ramp[i] = for_display(g.gamma_ramp[i]);
      ramp_version = g.gamma_ramp_version;
    }
    c.mode |= 8;
  }
  // The frame's fence, waited for on the worker without resetting it (the
  // game thread still waits for and resets it before reusing the slot, after
  // ge::native_vk::WaitPresents).
  VkDevice dev = static_cast<VulkanDevice*>(g.device.get())->vk;
  VkFence fence = static_cast<VulkanCommandFence*>(g.fence)->vk;
  rex::ui::Presenter* presenter = g.out.presenter;
  const auto paced = std::chrono::steady_clock::now();
  g.out.vk_handoff[g.slot] = ge::native_vk::PresentAsync(
      k, &c, kind, g.gamma_ramp_version ? ramp : nullptr, ramp_version,
      [dev, fence] { vkWaitForFences(dev, 1, &fence, VK_TRUE, UINT64_MAX); },
      [presenter, paced] { report_frame_to_presenter(presenter, paced, 9000); });
}

// Copies the frame Swap was given into shared frame k (recorded at the end of
// the frame's command list).
bool copy_to_presenter_texture(uint32_t k) {
  const auto front = g.resolved.find(g.front_buffer);
  if (front == g.resolved.end() || !front->second.texture) {
    static uint32_t missing = 0;
    if ((missing++ % 60) == 0)
      REXKRNL_INFO("GENATIVE no resolved image for front buffer {:08X} ({} frames so far)", g.front_buffer, missing);
    return false;
  }
  RenderTexture* src = front->second.texture.get();
  g.cmd->barriers(RenderBarrierStage::COPY, RenderTextureBarrier(src, RenderTextureLayout::COPY_SOURCE));
  ID3D12GraphicsCommandList* l = static_cast<D3D12CommandList*>(g.cmd)->d3d;
  D3D12_RESOURCE_BARRIER b = transition(g.out.tex[k], D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_COPY_DEST);
  l->ResourceBarrier(1, &b);
  l->CopyResource(g.out.tex[k], static_cast<D3D12Texture*>(src)->d3d);
  b = transition(g.out.tex[k], D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_COMMON);
  l->ResourceBarrier(1, &b);
  g.cmd->barriers(RenderBarrierStage::GRAPHICS, RenderTextureBarrier(src, RenderTextureLayout::SHADER_READ));
  prof_mark("present copy");
  return true;
}

// Last-gasp diagnostics. A lost graphics device (a driver reset after a GPU
// hang, or the GPU running out of memory) can end the game without an
// exception the crash log sees: a watchdog thread logs frame stalls, video
// memory use, and the device-removed reason with D3D12's breadcrumbs (the
// command list the GPU was in) and page-fault data, flushing each line.
std::atomic<double> g_last_present_ms{0};
std::atomic<uint64_t> g_present_count{0};

const char* removed_reason_name(HRESULT hr) {
  switch (hr) {
    case DXGI_ERROR_DEVICE_HUNG: return "device hung (GPU stopped responding)";
    case DXGI_ERROR_DEVICE_REMOVED: return "device removed";
    case DXGI_ERROR_DEVICE_RESET: return "device reset (bad command)";
    case DXGI_ERROR_DRIVER_INTERNAL_ERROR: return "driver internal error";
    case DXGI_ERROR_INVALID_CALL: return "invalid call";
    case E_OUTOFMEMORY: return "out of memory";
    default: return "?";
  }
}

void log_dred(ID3D12Device* dev) {
  ID3D12DeviceRemovedExtendedData* dred = nullptr;
  if (FAILED(dev->QueryInterface(IID_PPV_ARGS(&dred)))) {
    REXKRNL_ERROR("GENATIVE lost: no breadcrumb data (gpu_native_dred off or unsupported)");
    return;
  }
  static const char* const kOps[] = {"SetMarker", "BeginEvent", "EndEvent", "DrawInstanced", "DrawIndexedInstanced",
                                     "ExecuteIndirect", "Dispatch", "CopyBufferRegion", "CopyTextureRegion",
                                     "CopyResource", "CopyTiles", "ResolveSubresource", "ClearRenderTargetView",
                                     "ClearUnorderedAccessView", "ClearDepthStencilView", "ResourceBarrier",
                                     "ExecuteBundle", "Present", "ResolveQueryData", "BeginSubmission", "EndSubmission"};
  auto op_name = [&](D3D12_AUTO_BREADCRUMB_OP op) {
    return uint32_t(op) < std::size(kOps) ? std::string(kOps[op]) : fmt::format("op{}", uint32_t(op));
  };
  D3D12_DRED_AUTO_BREADCRUMBS_OUTPUT crumbs = {};
  if (const HRESULT hr = dred->GetAutoBreadcrumbsOutput(&crumbs); FAILED(hr)) {
    REXKRNL_ERROR("GENATIVE lost: no breadcrumbs ({:08X})", uint32_t(hr));
  } else {
    int n = 0;
    for (const D3D12_AUTO_BREADCRUMB_NODE* node = crumbs.pHeadAutoBreadcrumbNode; node && n < 16; node = node->pNext) {
      const uint32_t done = node->pLastBreadcrumbValue ? *node->pLastBreadcrumbValue : 0;
      if (done >= node->BreadcrumbCount) continue;  // finished command lists say nothing
      ++n;
      std::string ops;
      for (uint32_t i = done > 3 ? done - 3 : 0; i < std::min(node->BreadcrumbCount, done + 4); ++i)
        ops += fmt::format("{}{}{}", i == done ? " [" : " ", op_name(node->pCommandHistory[i]), i == done ? "]" : "");
      REXKRNL_ERROR("GENATIVE lost: command list {} finished {} of {} operations; around the stop:{}",
                    node->pCommandListDebugNameA ? node->pCommandListDebugNameA : "(unnamed)", done,
                    node->BreadcrumbCount, ops);
    }
    if (!n) REXKRNL_ERROR("GENATIVE lost: every recorded command list had finished");
  }
  D3D12_DRED_PAGE_FAULT_OUTPUT fault = {};
  if (const HRESULT hr = dred->GetPageFaultAllocationOutput(&fault); FAILED(hr) || !fault.PageFaultVA) {
    REXKRNL_ERROR("GENATIVE lost: no GPU page fault recorded ({:08X})", uint32_t(hr));
  } else {
    std::string live, freed;
    int k = 0;
    for (auto* a = fault.pHeadExistingAllocationNode; a && k < 8; a = a->pNext, ++k)
      live += fmt::format(" {}(type {})", a->ObjectNameA ? a->ObjectNameA : "?", uint32_t(a->AllocationType));
    k = 0;
    for (auto* a = fault.pHeadRecentFreedAllocationNode; a && k < 8; a = a->pNext, ++k)
      freed += fmt::format(" {}(type {})", a->ObjectNameA ? a->ObjectNameA : "?", uint32_t(a->AllocationType));
    REXKRNL_ERROR("GENATIVE lost: GPU page fault at {:X}; live there:{}; recently freed there:{}", fault.PageFaultVA,
                  live.empty() ? " none" : live, freed.empty() ? " none" : freed);
  }
  dred->Release();
}

std::string memory_line(IDXGIAdapter3* adapter) {
  PROCESS_MEMORY_COUNTERS_EX pmc = {sizeof(pmc)};
  K32GetProcessMemoryInfo(GetCurrentProcess(), reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&pmc), sizeof(pmc));
  std::string s = fmt::format("private {} MB", pmc.PrivateUsage >> 20);
  DXGI_QUERY_VIDEO_MEMORY_INFO local = {}, shared = {};
  if (adapter && SUCCEEDED(adapter->QueryVideoMemoryInfo(0, DXGI_MEMORY_SEGMENT_GROUP_LOCAL, &local)) &&
      SUCCEEDED(adapter->QueryVideoMemoryInfo(0, DXGI_MEMORY_SEGMENT_GROUP_NON_LOCAL, &shared)))
    s += fmt::format(", video local {}/{} MB, shared {}/{} MB (used/budget)", local.CurrentUsage >> 20,
                     local.Budget >> 20, shared.CurrentUsage >> 20, shared.Budget >> 20);
  return s;
}

// Depth resolves get the real scene depth (a compute copy). Not on Intel
// integrated graphics: an HD 620 (driver 31.0.101.2145) hung every 5-30 minutes
// in Dam on the draw that reads that copy back; the depth is cleared right
// after it, so the resolve's clear value draws the same.
bool g_depth_copy = true;

void choose_depth_copy() {
  const RenderDeviceDescription& d = g.device->getDescription();
  const bool intel_igpu = d.vendor == RenderDeviceVendor::INTEL &&
                          (d.type == RenderDeviceType::INTEGRATED || d.dedicatedVideoMemory < (1ull << 30));
  g_depth_copy = REXCVAR_GET(gpu_native_depth_copy) && (!intel_igpu || REXCVAR_GET(gpu_native_depth_copy_intel));
  if (intel_igpu) REXKRNL_INFO("GENATIVE Intel integrated graphics: depth copy {}", g_depth_copy ? "on" : "off");
}

ID3D12Device* g_watch_dev = nullptr;
IDXGIAdapter3* g_watch_adapter = nullptr;
std::mutex g_lost_mutex;
bool g_lost_reported = false;

// Logs the device-removed reason and the breadcrumbs once, from whichever
// thread notices first (the watchdog, or the presenter's loss callback,
// which ends the process right after).
void report_device_lost() {
  std::lock_guard<std::mutex> lock(g_lost_mutex);
  if (g_lost_reported || !g_watch_dev) return;
  const HRESULT hr = g_watch_dev->GetDeviceRemovedReason();
  if (hr == S_OK) return;
  g_lost_reported = true;
  REXKRNL_ERROR("GENATIVE graphics device lost: {:08X} {} after {} frames, stage {}; {}", uint32_t(hr),
                removed_reason_name(hr), g_present_count.load(), g32(0x82423E00u), memory_line(g_watch_adapter));
  rex::FlushLogging();
  log_dred(g_watch_dev);
  rex::FlushLogging();
}

void start_watchdog() {
  ID3D12Device* dev = nullptr;
  IDXGIAdapter3* adapter = nullptr;
  if (!g_vulkan) {
    dev = static_cast<D3D12Device*>(g.device.get())->d3d;
    IDXGIFactory4* factory = nullptr;
    if (SUCCEEDED(CreateDXGIFactory1(IID_PPV_ARGS(&factory)))) {
      factory->EnumAdapterByLuid(dev->GetAdapterLuid(), IID_PPV_ARGS(&adapter));
      factory->Release();
    }
  }
  g_watch_dev = dev;
  g_watch_adapter = adapter;
  std::thread([dev, adapter] {
    double stall_logged = 0;
    double last_mem = 0;
    for (;;) {
      Sleep(250);
      const double now = now_ms(), last = g_last_present_ms.load();
      if (const int at = REXCVAR_GET(gpu_native_test_remove); dev && at > 0 && g_present_count.load() >= uint64_t(at)) {
        static bool removed = false;
        ID3D12Device5* d5 = nullptr;
        if (!removed && SUCCEEDED(dev->QueryInterface(IID_PPV_ARGS(&d5)))) {
          REXKRNL_WARN("GENATIVE test: removing the graphics device");
          rex::FlushLogging();
          d5->RemoveDevice();
          d5->Release();
        }
        removed = true;
      }
      if (dev && dev->GetDeviceRemovedReason() != S_OK) {
        report_device_lost();
        return;
      }
      if (last > 0 && now - last > 1500 && stall_logged != last) {
        stall_logged = last;
        REXKRNL_WARN("GENATIVE no frame for {:.0f} ms (after {} frames, stage {}); {}", now - last,
                     g_present_count.load(), g32(0x82423E00u), memory_line(adapter));
        rex::FlushLogging();
      }
      if (now - last_mem > 60000) {
        last_mem = now;
        REXKRNL_INFO("GENATIVE memory: {}", memory_line(adapter));
      }
    }
  }).detach();
}

bool ensure_device() {
  if (g.swapchain || g.out.presenter) return true;
  if (g.failed) return false;
  HWND hwnd = g_game_hwnd_for_resize();
  if (!hwnd) return false;  // window not up yet; try again next frame
  if (REXCVAR_GET(gpu_native_debug_layer)) {
    ID3D12Debug* dbg = nullptr;
    if (SUCCEEDED(D3D12GetDebugInterface(IID_PPV_ARGS(&dbg)))) { dbg->EnableDebugLayer(); dbg->Release(); }
  }
  g_vulkan = REXCVAR_GET(gpu_native_api) == "vulkan";
  if (!g_vulkan) {
    g.api = CreateD3D12Interface();
    if (g.api) g.device = g.api->createDevice(sdk_adapter_name());
    if (!g.device && REXCVAR_GET(gpu_native_api) == "auto") {
      REXKRNL_WARN("GENATIVE could not create a D3D12 device: trying Vulkan");
      g.api.reset();
      g_vulkan = true;
    }
  }
  if (g_vulkan) {
    ge::native_shaders::SetSpirv(true);
    g.api = plume::CreateVulkanInterface();
    if (g.api) {
      // gpu_native_adapter: any part of the card's name (Plume wants it whole).
      std::string want = REXCVAR_GET(gpu_native_adapter);
      if (want.empty()) {  // the presenter's GPU, so frames can be shared with it
        auto* gs = static_cast<rex::system::IGraphicsSystem*>(rex::system::kernel_state()->emulator()->graphics_system());
        auto* pgs = dynamic_cast<rex::ui::PresenterGraphicsSystem*>(gs);
        if (pgs && pgs->api() == rex::ui::PresenterGraphicsSystem::Api::kVulkan)
          want = ge::native_vk::PresenterDeviceName(pgs->presenter());
      }
      if (!want.empty()) {
        auto lower = [](std::string t) { for (char& c : t) c = char(std::tolower(uint8_t(c))); return t; };
        for (const std::string& name : g.api->getDeviceNames())
          if (lower(name).find(lower(want)) != std::string::npos) { want = name; break; }
      }
      g.device = g.api->createDevice(want);
    }
    if (g.device) REXKRNL_INFO("GENATIVE Vulkan device up: {}", g.device->getDescription().name);
  }
  if (!g.device) {
    g.failed = true;
    REXKRNL_ERROR("GENATIVE could not create a {} device: switching to the classic renderer and restarting",
                  g_vulkan ? "Vulkan" : "D3D12");
    rex::cvar::SetFlagByName("gpu_native", "false");
    ge::PersistSettings();
    ge::RestartSelf();
  }
  g.queue = g.device->createCommandQueue(RenderCommandListType::DIRECT);
  // Present wait on (never waited on here): Plume then presents with sync
  // interval 0 and no tearing flag while V-Sync stays on, like the SDK's own
  // presenter. Present(1) left frames queued behind vblanks: the game thread
  // waited 13-36 ms for a frame whose GPU work took 0.5 ms. Three buffers so
  // rendering never waits for the one being scanned out.
  g_scene_scale = std::clamp(ge::ActiveRenderScale(), 1.0f, 3.0f);
  g_scene_w = uint32_t(std::lround(kGuestW * g_scene_scale));
  g_scene_h = uint32_t(std::lround(kGuestH * g_scene_scale));
  REXKRNL_INFO("GENATIVE scene {}x{} (render scale {}x)", g_scene_w, g_scene_h, g_scene_scale);
  if (g_vulkan ? !setup_presenter_output_vk() : !setup_presenter_output()) {
    g.out = {};
    g.swapchain = g.queue->createSwapChain(RenderSwapChainDesc(hwnd, kSceneFormat, 3, true, 2));
    g.swapchain->resize();
  }
  for (uint32_t i = 0; i < kFrames; ++i) {
    g.cmds[i] = g.queue->createCommandList();
    g.fences[i] = g.device->createCommandFence();
  }
  g.cmd = g.cmds[0].get();
  g.fence = g.fences[0].get();
  g.acquire = g.device->createCommandSemaphore();
  choose_depth_copy();
  start_watchdog();
  if (g.swapchain) create_swap_framebuffers();

  // Binding model expected by XenosRecomp's shader_common.h: bindless
  // Texture2D/3D/Cube arrays in spaces 0-2, samplers in space 3, and the
  // vertex / pixel / shared constant buffers as root CBVs b0-b2 in space 4.
  RenderPipelineLayoutBuilder lb;
  lb.begin(false, true);
  RenderDescriptorSetBuilder tb;
  tb.begin();
  tb.addTexture(0, kTextureSlots);
  tb.end(true, kTextureSlots);
  g.textures = tb.create(g.device.get());
  lb.addDescriptorSet(tb);
  lb.addDescriptorSet(tb);
  lb.addDescriptorSet(tb);
  RenderDescriptorSetBuilder sb;
  sb.begin();
  sb.addSampler(0, kSamplerSlots);
  sb.end(true, kSamplerSlots);
  g.samplers = sb.create(g.device.get());
  lb.addDescriptorSet(sb);
  if (g_vulkan) {
    // XenosRecomp's Vulkan shaders read their constants through three buffer
    // addresses in push constants (vertex, pixel, shared).
    lb.addPushConstant(0, 0, 24, RenderShaderStageFlag::VERTEX | RenderShaderStageFlag::PIXEL);
  } else {
    lb.addRootDescriptor(0, 4, RenderRootDescriptorType::CONSTANT_BUFFER);
    lb.addRootDescriptor(1, 4, RenderRootDescriptorType::CONSTANT_BUFFER);
    lb.addRootDescriptor(2, 4, RenderRootDescriptorType::CONSTANT_BUFFER);
  }
  lb.end();
  g.layout = lb.create(g.device.get());

  RenderSamplerDesc sd;
  sd.minFilter = RenderFilter::LINEAR;
  sd.magFilter = RenderFilter::LINEAR;
  sd.mipmapMode = RenderMipmapMode::LINEAR;
  sd.addressU = sd.addressV = sd.addressW = RenderTextureAddressMode::CLAMP;
  g.default_sampler = g.device->createSampler(sd);
  for (uint32_t i = 0; i < 16; ++i) g.samplers->setSampler(i, g.default_sampler.get());

  g.timestamps = g.device->createQueryPool(3 * kFrames);  // per slot: start, end; then constants-copy starts
  g.vertex_pool.flags = RenderBufferFlag::VERTEX;
  g.index_pool.flags = RenderBufferFlag::INDEX;
  g.index_pool.chunk = 4ull << 20;  // Dam's index lists come to ~1 MB
  g.prof_pool = g.device->createQueryPool(4096);
  g.upload = g.device->createBuffer(RenderBufferDesc::UploadBuffer(kUploadSize));
  g.const_upload = g.device->createBuffer(RenderBufferDesc::UploadBuffer(kSlotConsts * kFrames));
  g.const_ptr = static_cast<uint8_t*>(g.const_upload->map());
  for (uint32_t i = 0; i < kFrames; ++i) {
    g.const_gpu[i] = g.device->createBuffer(RenderBufferDesc::DefaultBuffer(
        kSlotConsts, RenderBufferFlag::CONSTANT | (g_vulkan ? RenderBufferFlag::DEVICE_ADDRESSABLE : 0)));
    g.const_lists[i] = g.queue->createCommandList();
  }
  g.upload_ptr = static_cast<uint8_t*>(g.upload->map());

  // The game's scene target is 4x multisampled (RB_SURFACE_INFO, mirrored at
  // device+10940, has msaa_samples = 2): the scene and its depth match, and
  // resolves / present average the samples.
  g.msaa.sampleCount = pick_msaa();
  REXKRNL_INFO("GENATIVE anti-aliasing: {}x MSAA (gpu_native_msaa {})", uint32_t(g.msaa.sampleCount),
               REXCVAR_GET(gpu_native_msaa));
  create_scene_targets();

  // Slot 0 of the texture heap: 1x1 white, sampled by anything not yet supported.
  g.white = g.device->createTexture(RenderTextureDesc::Texture2D(1, 1, 1, RenderFormat::R8G8B8A8_UNORM));
  {
    const uint64_t off = upload_alloc(256, 512);
    std::memset(g.upload_ptr + off, 0xFF, 4);
    g.cmd->begin();
    g.cmd->barriers(RenderBarrierStage::COPY, RenderTextureBarrier(g.white.get(), RenderTextureLayout::COPY_DEST));
    g.cmd->copyTextureRegion(
        RenderTextureCopyLocation::Subresource(g.white.get()),
        RenderTextureCopyLocation::PlacedFootprint(g.upload.get(), RenderFormat::R8G8B8A8_UNORM, 1, 1, 1, 64, off));
    g.cmd->barriers(RenderBarrierStage::GRAPHICS, RenderTextureBarrier(g.white.get(), RenderTextureLayout::SHADER_READ));
    g.cmd->end();
    const RenderCommandList* l = g.cmd;
    g.queue->executeCommandLists(&l, 1, nullptr, 0, nullptr, 0, g.fence);
    g.queue->waitForCommandFence(g.fence);
    g.upload_used = 0;
  }
  for (uint32_t i = 0; i < kTextureSlots; ++i) g.textures->setTexture(i, g.white.get(), RenderTextureLayout::SHADER_READ);

  if (g.swapchain)
    REXKRNL_INFO("GENATIVE D3D12 device up, swapchain {}x{}", g.swapchain->getWidth(), g.swapchain->getHeight());
  else
    REXKRNL_INFO("GENATIVE {} device up, frames go to the SDK presenter", g_vulkan ? "Vulkan" : "D3D12");
  return true;
}


// The scene's sample count from gpu_native_msaa (1, 2 or 4), as far as the GPU
// supports it for both the colour and depth formats.
RenderSampleCounts pick_msaa() {
  const int32_t want = REXCVAR_GET(gpu_native_msaa);
  const RenderSampleCounts supported = g.device->getSampleCountsSupported(kSceneFormat) &
                                       g.device->getSampleCountsSupported(RenderFormat::D32_FLOAT);
  for (RenderSampleCounts c : {RenderSampleCount::COUNT_4, RenderSampleCount::COUNT_2})
    if (want >= int32_t(c) && (supported & c)) return c;
  return RenderSampleCount::COUNT_1;
}

// The scene colour and depth targets at the current size and sample count.
void create_scene_targets() {
  g.scene = g.device->createTexture(RenderTextureDesc::ColorTarget(g_scene_w, g_scene_h, kSceneFormat, g.msaa));
  // Typeless so the depth resolve can read it (D32_FLOAT depth view, R32_FLOAT shader view).
  // (Vulkan samples a D32 depth image directly.)
  g.scene_depth = g.device->createTexture(RenderTextureDesc::DepthTarget(
      g_scene_w, g_scene_h, g_vulkan ? RenderFormat::D32_FLOAT : RenderFormat::R32_TYPELESS, g.msaa));
  const RenderTexture* sc = g.scene.get();
  g.scene_fb = g.device->createFramebuffer(RenderFramebufferDesc(&sc, 1, g.scene_depth.get()));
}

// --- shaders ----------------------------------------------------------------

// Rebuilds the contiguous container (virtual part + physical part) exactly as
// the probe saved it, so the hash matches the translated cache.
bool container_at(uint32_t container, uint32_t physical, std::vector<uint8_t>& out) {
  const uint32_t flags = g32(container), vsize = g32(container + 4), psize = g32(container + 8);
  if ((flags & 0xFFFFFF00u) != 0x102A1100u || vsize < 36 || vsize > 65536 || psize > 65536) return false;
  out.resize(vsize + psize);
  std::memcpy(out.data(), gmem() + container, vsize);
  std::memcpy(out.data() + vsize, gmem() + (physical ? physical : container + vsize), psize);
  return true;
}

// The blob a shader was created from (it may since have been freed: then the
// copy kept in the object, with its microcode at the object's GPU address).
bool container_bytes(uint32_t object, bool vertex, std::vector<uint8_t>& out) {
  const auto created = g.created_blobs.find(object);
  if (created != g.created_blobs.end() && container_at(created->second, 0, out)) return true;
  return container_at(object + (vertex ? 872 : 40), gpu_view(g32(object + (vertex ? 32 : 24))), out);
}


// Vertex shaders registered with XGRegisterVertexShader carry their own vertex
// fetch: each vertex element (usage, index, microcode address) points at a
// vfetch instruction giving the fetch constant slot, offset, stride and format
// (layout as XenosRecomp's VertexFetchInstruction). Mini fetches reuse the
// previous full fetch's slot and stride.
// live_micro: guest address of the microcode the GPU runs (D3D patches the
// vfetch format/stride/offset there when a declaration is bound; boot shaders'
// original blobs keep zeroed templates). 0 = use the container's own copy.
void parse_vertex_inputs(const std::vector<uint8_t>& blob, HostShader& hs, uint32_t live_micro = 0) {
  auto be = [&](size_t o) { return o + 4 <= blob.size() ? __builtin_bswap32(*reinterpret_cast<const uint32_t*>(&blob[o])) : 0u; };
  const uint32_t vsize = be(4), shader = be(24);
  const uint32_t phys_off = be(shader), field18 = be(shader + 24), count = be(shader + 28);
  const size_t micro = size_t(vsize) + phys_off;
  auto mc = [&](size_t o) { return live_micro ? g32(live_micro + uint32_t(o - micro)) : be(o); };
  hs.inputs.clear();
  hs.strides.clear();
  uint16_t last_slot = 0;
  uint32_t last_stride = 0;
  for (uint32_t i = 0; i < count && i < 32; ++i) {
    const uint32_t e = be(shader + 36 + (field18 + i) * 4);
    const uint32_t address = e & 0xFFF, usage = (e >> 12) & 0xF, index = (e >> 16) & 0xF;
    const size_t at = micro + size_t(address) * 12;
    const uint32_t d0 = mc(at), d1 = mc(at + 4), d2 = mc(at + 8);
    if ((d0 & 0x1F) != 0) continue;  // not a vertex fetch
    const bool mini = (d1 >> 30) & 1;
    if (!mini) {
      last_slot = uint16_t(((d0 >> 20) & 0x1F) * 3 + ((d0 >> 25) & 3));
      last_stride = (d2 & 0xFF) * 4;
    }
    const int32_t offset = (int32_t(d2 << 1) >> 9) * 4;
    // d1: bit 12 signed (format_comp_all), bit 13 integer (num_format_all);
    // bit 14 is only the signed rounding mode.
    const uint32_t type = ((d1 >> 16) & 0x3F) | (((d1 >> 12) & 1) << 8) | (((d1 >> 13) & 1) << 9);
    const int8_t exp = int8_t(int32_t((d1 >> 24) & 0x3F) << 26 >> 26);
    const uint16_t templ = uint16_t(be(at + 4) & 0xFFF), live = uint16_t(d1 & 0xFFF);
    hs.inputs.push_back({last_slot, uint16_t(offset), type, uint8_t(usage), uint8_t(index), exp,
                         (templ == kIdentitySwizzle || templ == 0) && live ? live : kIdentitySwizzle});
    hs.strides[last_slot] = last_stride;
  }
}

// <hash>.vs|ps.final.dxil (boot shaders translated locally may lack the kind)
// and its .cbsize from the shader cache.
RenderShader* load_cached_shader(HostShader& hs, bool vertex) {
  const std::wstring& dir = ge::native_shaders::CacheDir();
  wchar_t name[64];
  for (const wchar_t* kind : {vertex ? L"vs." : L"ps.", L""}) {
    swprintf(name, 64, L"\\%016llX.%s%s", static_cast<unsigned long long>(hs.hash), kind,
             ge::native_shaders::FinalExtension());
    FILE* f = _wfopen((dir + name).c_str(), L"rb");
    if (!f) continue;
    std::fseek(f, 0, SEEK_END);
    std::vector<uint8_t> dxil(static_cast<size_t>(std::ftell(f)));
    std::fseek(f, 0, SEEK_SET);
    std::fread(dxil.data(), 1, dxil.size(), f);
    std::fclose(f);
    hs.shader = g.device->createShader(dxil.data(), dxil.size(), "main",
                                       g_vulkan ? RenderShaderFormat::SPIRV : RenderShaderFormat::DXIL);
    if (!hs.shader) continue;
    hs.cb_bytes = vertex ? 4096 : 3584;
    std::wstring cb = dir + name;
    cb.replace(cb.rfind(L"final."), std::wstring(ge::native_shaders::FinalExtension()).size(), L"cbsize");
    if (FILE* c = _wfopen(cb.c_str(), L"rb")) {
      unsigned v = 0;
      if (std::fscanf(c, "%u", &v) == 1) hs.cb_bytes = std::min<uint32_t>(v, hs.cb_bytes);
      std::fclose(c);
    }
    return hs.shader.get();
  }
  return nullptr;
}

// Level shaders are registered in place while the level loads, and their
// microcode streams in a little later. Once an object's bytes read the same
// two frames running, its translation starts (ge::native_shaders::Request does
// nothing for one already cached), so a first run translates a level's shaders
// during its loading instead of at their first draw. A shader drawn before
// that is translated then, as before; the hash is the same either way.
void warm_registered_shaders() {
  for (auto it = g.warm.begin(); it != g.warm.end();) {
    auto& [object, w] = *it;
    std::vector<uint8_t> blob;
    if (g.frames - w.frame > 600 || !container_bytes(object, w.vertex, blob)) {
      if (g.frames - w.frame > 600) it = g.warm.erase(it); else ++it;
      continue;
    }
    // Microcode not streamed in yet: all zeros past the container's header.
    const uint32_t vsize = (uint32_t(blob[4]) << 24) | (uint32_t(blob[5]) << 16) | (uint32_t(blob[6]) << 8) | blob[7];
    bool filled = false;
    for (size_t i = vsize; i < blob.size() && !filled; ++i) filled = blob[i] != 0;
    const uint64_t hash = fnv1a(blob.data(), blob.size());
    if (!filled || hash != w.last_hash) {
      w.last_hash = hash;
      ++it;
      continue;
    }
    ge::native_shaders::Request(hash, std::move(blob));
    ++g.warm_requested;
    it = g.warm.erase(it);
  }
}

// A shader the game just created: start translating it if it isn't cached,
// so it is usually ready by the time a draw needs it.
void request_translation(uint32_t object, bool vertex) {
  std::vector<uint8_t> blob;
  if (!container_bytes(object, vertex, blob)) return;
  const uint64_t hash = fnv1a(blob.data(), blob.size());  // before the move: argument order is unspecified
  ge::native_shaders::Request(hash, std::move(blob));
}

RenderShader* host_shader(uint32_t object, bool vertex) {
  if (!object) return nullptr;
  HostShader& hs = g.shaders[object];
  if (hs.shader) return hs.shader.get();
  if (hs.missing) return nullptr;
  std::vector<uint8_t> blob;
  // Not cached as missing: the post-process PS is bound on frame 1 before its
  // container is filled in, and becomes valid a few frames later.
  if (!container_bytes(object, vertex, blob)) return nullptr;
  hs.hash = fnv1a(blob.data(), blob.size());
  if (vertex) { parse_vertex_inputs(blob, hs); hs.container = blob; }
  // From the cache (native_shaders); a shader not there yet is translated now
  // (usually already started when the game created it, see request_translation)
  // and this draw waits for it, so nothing is drawn with pieces missing.
  for (int attempt = 0; attempt < 2; ++attempt) {
    if (RenderShader* shader = load_cached_shader(hs, vertex)) return shader;
    if (attempt == 0) {
      ge::native_shaders::Request(hs.hash, blob);
      if (!ge::native_shaders::Wait(hs.hash)) break;
    }
  }
  hs.missing = true;
  if (!REXCVAR_GET(gpu_native_translate)) {
    // Leave the container for the local translation step (native/translate_shaders.ps1).
    const std::wstring missing = ge::native_shaders::CacheDir() + L"\\missing";
    CreateDirectoryW(missing.c_str(), nullptr);
    wchar_t name[64];
    swprintf(name, 64, L"\\%016llX.%s.bin", static_cast<unsigned long long>(hs.hash), vertex ? L"vs" : L"ps");
    if (FILE* f = _wfopen((missing + name).c_str(), L"wb")) {
      std::fwrite(blob.data(), 1, blob.size(), f);
      std::fclose(f);
    }
  }
  REXKRNL_WARN("GENATIVE no shader {:016X} ({}) for object {:08X} ({} bytes)", hs.hash, vertex ? "vs" : "ps", object,
               blob.size());
  return nullptr;
}

// --- vertex input -----------------------------------------------------------

const char* kSemantics[] = {"POSITION", "BLENDWEIGHT", "BLENDINDICES", "NORMAL", "PSIZE", "TEXCOORD",
                            "TANGENT", "BINORMAL", "TESSFACTOR", "POSITIONT", "COLOR", "FOG", "DEPTH",
                            "SAMPLE"};


// D3DDECLTYPE on Xbox 360: low 6 bits = Xenos vertex format, bit 8 = signed,
// bit 9 = integer (not normalised); the rest is the component swizzle, which
// the vertex fetch in the microcode applies itself.
RenderFormat element_format(uint32_t type, uint32_t& component_bytes) {
  const uint32_t fmt = type & 0x3F;
  const bool sign = (type >> 8) & 1, integer = (type >> 9) & 1;
  component_bytes = 4;
  switch (fmt) {
    case 6: component_bytes = 1;
      return integer ? (sign ? RenderFormat::R8G8B8A8_SINT : RenderFormat::R8G8B8A8_UINT)
                     : (sign ? RenderFormat::R8G8B8A8_SNORM : RenderFormat::R8G8B8A8_UNORM);
    case 25: component_bytes = 2;
      return integer ? (sign ? RenderFormat::R16G16_SINT : RenderFormat::R16G16_UINT)
                     : (sign ? RenderFormat::R16G16_SNORM : RenderFormat::R16G16_UNORM);
    case 26: component_bytes = 2;
      return integer ? (sign ? RenderFormat::R16G16B16A16_SINT : RenderFormat::R16G16B16A16_UINT)
                     : (sign ? RenderFormat::R16G16B16A16_SNORM : RenderFormat::R16G16B16A16_UNORM);
    case 31: component_bytes = 2; return RenderFormat::R16G16_FLOAT;
    case 32: component_bytes = 2; return RenderFormat::R16G16B16A16_FLOAT;
    case 36: return RenderFormat::R32_FLOAT;
    case 37: return RenderFormat::R32G32_FLOAT;
    case 38: return RenderFormat::R32G32B32A32_FLOAT;
    case 57: return RenderFormat::R32G32B32_FLOAT;
    case 7: return RenderFormat::UNKNOWN;  // 2:10:10:10 - Plume has no such format yet
    default: return RenderFormat::UNKNOWN;
  }
}

std::vector<Element> read_decl(uint32_t decl) {
  std::vector<Element> els;
  if (!decl) return els;
  const uint32_t n = std::min<uint32_t>(g32(decl + 24), 32);
  for (uint32_t i = 0; i < n; ++i) {
    const uint32_t e = decl + 52 + i * 12;
    Element el{g16(e), g16(e + 2), g32(e + 4), gmem()[e + 9], gmem()[e + 10]};
    if (el.stream == 0xFF) break;
    els.push_back(el);
  }
  return els;
}

// Vertex decoding, done on the CPU the way the Xenos vertex fetch does it: read
// the dwords with the fetch constant's endian mode (0 none, 1 8in16, 2 8in32,
// 3 16in32), take components from the low bits up, then normalise (or not,
// for integer formats), apply the sign and scale by 2^exp. Every element is
// written as 16 bytes: float4, or for XenosRecomp's uint4 inputs (BLENDINDICES,
// NORMAL, TANGENT, BINORMAL) the raw bits (float formats as asuint, packed
// 11:11:10 normals as the dword in .x with spec bit 0).
struct Source {
  uint32_t addr = 0, stride = 0, endian = 2;
};

// 16-bit pairs. Vertex buffers follow the fetch constant's swap as is, but the
// game's own memory passed to DrawVerticesUP / Begin+EndVertices has the first
// value as X (the menu font's U and V came out crossed): those sources carry
// endian bit 2, which swaps the halves back.
inline uint32_t fetch_pair(uint32_t addr, uint32_t endian);

inline uint32_t fetch_dword(uint32_t addr, uint32_t endian) {
  uint32_t w;
  std::memcpy(&w, gmem() + addr, 4);
  switch (endian & 3) {
    case 1: return ((w & 0x00FF00FFu) << 8) | ((w >> 8) & 0x00FF00FFu);
    case 2: return __builtin_bswap32(w);
    case 3: return (w << 16) | (w >> 16);
    default: return w;
  }
}

inline bool uint_usage(uint8_t usage) { return usage == 2 || usage == 3 || usage == 6 || usage == 7; }

float half_to_float(uint16_t h) {
  const uint32_t sign = uint32_t(h & 0x8000) << 16, e = (h >> 10) & 0x1F, m = h & 0x3FF;
  uint32_t bits;
  if (e == 0) {
    if (m == 0) bits = sign;
    else { float f = std::ldexp(float(m), -24); std::memcpy(&bits, &f, 4); bits |= sign; }
  } else if (e == 31) bits = sign | 0x7F800000u | (m << 13);
  else bits = sign | ((e + 112) << 23) | (m << 13);
  float f;
  std::memcpy(&f, &bits, 4);
  return f;
}

inline uint32_t fetch_pair(uint32_t addr, uint32_t endian) {
  const uint32_t w = fetch_dword(addr, endian);
  return (endian & 4) ? (w << 16) | (w >> 16) : w;
}

// Host vertex format per element: as small as the source allows (a vertex
// used to be 16 bytes per element: ~100 MB of cached vertices in Dam).
enum class HostKind : uint8_t { kRecord, kF32, kU32, kU8x4, kU16x2, kU16x4, kRaw };
struct HostFmt {
  RenderFormat format;
  uint32_t size;
  HostKind kind;
};

HostFmt host_format(const Element& el) {
  const uint32_t fmt = el.type & 0x3F;
  const bool sign = (el.type >> 8) & 1, integer = (el.type >> 9) & 1, plain = !integer && el.exp == 0;
  if (uint_usage(el.usage)) {
    if ((fmt == 16 || fmt == 17) && el.usage != 2) return {RenderFormat::R32_UINT, 4, HostKind::kU32};  // shader reads .x
    if (el.usage == 2 && fmt == 6) return {RenderFormat::R8G8B8A8_UINT, 4, HostKind::kU8x4};
    return {RenderFormat::R32G32B32A32_UINT, 16, HostKind::kRecord};
  }
  switch (fmt) {
    case 57: return {RenderFormat::R32G32B32_FLOAT, 12, HostKind::kF32};
    case 37: return {RenderFormat::R32G32_FLOAT, 8, HostKind::kF32};
    case 36: return {RenderFormat::R32_FLOAT, 4, HostKind::kF32};
    case 6:
      if (plain) return {sign ? RenderFormat::R8G8B8A8_SNORM : RenderFormat::R8G8B8A8_UNORM, 4, HostKind::kU8x4};
      break;
    case 25:
      if (plain) return {sign ? RenderFormat::R16G16_SNORM : RenderFormat::R16G16_UNORM, 4, HostKind::kU16x2};
      return {RenderFormat::R32G32_FLOAT, 8, HostKind::kF32};
    case 26:
      if (plain) return {sign ? RenderFormat::R16G16B16A16_SNORM : RenderFormat::R16G16B16A16_UNORM, 8, HostKind::kU16x4};
      break;
    case 31: return {RenderFormat::R16G16_FLOAT, 4, HostKind::kRaw};
    case 32: return {RenderFormat::R16G16B16A16_FLOAT, 8, HostKind::kRaw};
    default: break;
  }
  return {RenderFormat::R32G32B32A32_FLOAT, 16, HostKind::kRecord};
}

void decode_element(const Element& el, uint32_t addr, uint32_t endian, uint8_t* out, const HostFmt& hf) {
  const uint32_t fmt = el.type & 0x3F;
  const bool sign = (el.type >> 8) & 1, integer = (el.type >> 9) & 1;
  const float scale = std::ldexp(1.0f, el.exp);
  float f[4] = {0, 0, 0, 1};
  uint32_t u[4] = {0, 0, 0, 1};
  bool is_float = false;
  auto fixed = [&](uint32_t raw, int bits, int c) {
    int64_t v = raw;
    if (sign && ((raw >> (bits - 1)) & 1)) v -= int64_t(1) << bits;
    u[c] = uint32_t(v);
    float x = float(v);
    if (!integer) x =sign ? std::max(x / float((1 << (bits - 1)) - 1), -1.0f) : x / float((int64_t(1) << bits) - 1);
    f[c] = x * scale;
  };
  switch (fmt) {
    case 6: { const uint32_t w = fetch_dword(addr, endian); for (int c = 0; c < 4; ++c) fixed((w >> (8 * c)) & 0xFF, 8, c); break; }
    case 7: { const uint32_t w = fetch_dword(addr, endian);
      fixed(w & 0x3FF, 10, 0); fixed((w >> 10) & 0x3FF, 10, 1); fixed((w >> 20) & 0x3FF, 10, 2); fixed(w >> 30, 2, 3); break; }
    case 16: case 17: { const uint32_t w = fetch_dword(addr, endian);
      if (uint_usage(el.usage)) { u[0] = w; u[1] = u[2] = u[3] = 0; break; }
      fixed(w & 0x7FF, 11, 0); fixed((w >> 11) & 0x7FF, 11, 1); fixed(w >> 22, 10, 2); break; }
    case 25: { const uint32_t w = fetch_pair(addr, endian); fixed(w & 0xFFFF, 16, 0); fixed(w >> 16, 16, 1); break; }
    case 26: { for (int d = 0; d < 2; ++d) { const uint32_t w = fetch_pair(addr + 4 * d, endian);
        fixed(w & 0xFFFF, 16, 2 * d); fixed(w >> 16, 16, 2 * d + 1); } break; }
    case 31: { const uint32_t w = fetch_pair(addr, endian); f[0] = half_to_float(w & 0xFFFF); f[1] = half_to_float(w >> 16);
      u[0] = w; is_float = true; break; }
    case 32: { for (int d = 0; d < 2; ++d) { const uint32_t w = fetch_pair(addr + 4 * d, endian);
        f[2 * d] = half_to_float(w & 0xFFFF); f[2 * d + 1] = half_to_float(w >> 16); u[d] = w; } is_float = true; break; }
    case 36: case 37: case 38: case 57: {
      const int n = fmt == 36 ? 1 : fmt == 37 ? 2 : fmt == 57 ? 3 : 4;
      for (int c = 0; c < n; ++c) { const uint32_t w = fetch_dword(addr + 4 * c, endian); std::memcpy(&f[c], &w, 4); }
      is_float = true; break; }
    default: {
      static uint64_t logged = 0;
      if (fmt < 64 && !(logged & (1ull << fmt))) {
        logged |= 1ull << fmt;
        REXKRNL_WARN("GENATIVE vertex format {} (type {:08X}, usage {}) not decoded yet", fmt, el.type, el.usage);
      }
      break;
    }
  }
  if (el.swizzle != kIdentitySwizzle) {  // the live vfetch's dst swizzle: 0-3 = x..w, 4 = 0, 5 = 1
    const float sf[4] = {f[0], f[1], f[2], f[3]};
    const uint32_t su[4] = {u[0], u[1], u[2], u[3]};
    for (int c = 0; c < 4; ++c) {
      const uint32_t sel = (el.swizzle >> (3 * c)) & 7;
      if (sel < 4) { f[c] = sf[sel]; u[c] = su[sel]; }
      else if (sel == 4) { f[c] = 0.0f; u[c] = 0; }
      else if (sel == 5) { f[c] = 1.0f; u[c] = 1; }
    }
  }
  switch (hf.kind) {
    case HostKind::kF32: std::memcpy(out, f, hf.size); return;
    case HostKind::kU32: std::memcpy(out, &u[0], 4); return;
    case HostKind::kRaw: std::memcpy(out, u, hf.size); return;
    case HostKind::kU8x4: for (int c = 0; c < 4; ++c) out[c] = uint8_t(u[c]); return;
    case HostKind::kU16x2:
    case HostKind::kU16x4:
      for (uint32_t c = 0; c < hf.size / 2; ++c) { const uint16_t v = uint16_t(u[c]); std::memcpy(out + 2 * c, &v, 2); }
      return;
    case HostKind::kRecord: break;
  }
  if (uint_usage(el.usage)) {
    // Normals, tangents and binormals reach the shader through
    // tfetchR11G11B10, which unpacks 11_11_10 bits (spec bit 0) and otherwise
    // reinterprets the input as floats: every other format goes over as float
    // bits (the Rare logo's 2_10_10_10 normals were arriving as raw integers).
    // Blend indices are used as numbers and stay integers.
    const bool packed = fmt == 16 || fmt == 17;
    if (is_float || (el.usage != 2 && !packed)) for (int c = 0; c < 4; ++c) std::memcpy(&u[c], &f[c], 4);
    std::memcpy(out, u, 16);
  } else {
    std::memcpy(out, f, 16);
  }
}

// Physical address -> a guest address the CPU can read. Physical memory shows
// up through several aliases and not every page is mapped in every one: the
// 0xA0000000, 0xE0000000 and 0xC0000000 views, all at view + phys in the
// membase. (Guest pointers into the 0xE0000000 view are 4 KB behind: that is
// gpu_view's +0x1000, not this.) Pick the first whose first and last bytes of
// [phys, phys+size) can be read. A vertex buffer running past the A view's
// mapped pages used to fall back to view + phys - 4 KB: the Rare logo read
// from 4 KB early and drew as a flat sliver.
bool readable_seh(uint32_t a, uint32_t b) {
  __try {
    volatile uint8_t x = gmem()[a];
    volatile uint8_t y = gmem()[b];
    (void)x; (void)y;
  } __except (EXCEPTION_EXECUTE_HANDLER) {
    return false;
  }
  return true;
}

uint32_t cpu_view_of_physical(uint32_t phys, uint32_t size) {
  if (!phys) return 0;
  const uint32_t last = size ? size - 1 : 0;
  for (uint32_t base : {0xA0000000u + phys, 0xE0000000u + phys, 0xC0000000u + phys})
    if (readable_seh(base, base + last)) return base;
  return 0;
}

// --- guest textures -----------------------------------------------------------
//
// A texture's 24-byte fetch constant sits at +28 in its D3D object (the same
// bytes SetTexture copies into the device). Decoded fields (Xenos layout):
//   d0: bits 10-18 clamp x/y/z, 22-30 pitch / 32 texels, 31 tiled
//   d1: bits 0-5 format, 6-7 endian, 12-31 base address / 4 KB (physical)
//   d2: (2D) bits 0-12 width-1, 13-25 height-1
//   d3: bits 1-12 swizzle (3 bits per component: 0-3 = x,y,z,w, 4 = 0, 5 = 1),
//       19-20 mag filter, 21-22 min filter
//   d5: bits 9-10 dimension (1 = 2D)
// Only the base level of 2D textures is uploaded for now (no mips), converted to
// RGBA8 on the CPU with the swizzle applied, since the translated shaders
// leave the fetch-constant swizzle to the hardware.

// XGAddress2DTiledOffset: texel (block) offset of (x, y) in a tiled surface.
uint32_t tiled_offset(uint32_t x, uint32_t y, uint32_t pitch, uint32_t bpb) {
  const uint32_t aligned = (pitch + 31) & ~31u;
  const uint32_t log_bpp = (bpb >> 2) + ((bpb >> 1) >> (bpb >> 2));
  const uint32_t macro = ((x >> 5) + (y >> 5) * (aligned >> 5)) << (log_bpp + 7);
  const uint32_t micro = ((x & 7) + ((y & 6) << 2)) << log_bpp;
  const uint32_t off = macro + ((micro & ~15u) << 1) + (micro & 15) + ((y & 8) << (3 + log_bpp)) + ((y & 1) << 4);
  return (((off & ~511u) << 3) + ((off & 448) << 2) + (off & 63) + ((y & 16) << 7) +
          (((((y & 8) >> 2) + (x >> 3)) & 3) << 6)) >> log_bpp;
}

struct TexFormat {
  uint32_t bpb, block;  // bytes per block, block size in texels (1 or 4)
};
bool tex_format(uint32_t fmt, TexFormat& f) {
  switch (fmt) {
    case 2: f = {1, 1}; return true;                       // k_8
    case 3: case 4: case 10: case 15: f = {2, 1}; return true;  // 1555, 565, 8_8, 4444
    case 6: f = {4, 1}; return true;                       // 8888
    case 18: f = {8, 4}; return true;                      // DXT1
    case 19: case 20: f = {16, 4}; return true;            // DXT2/3, DXT4/5
    default: return false;
  }
}

inline uint32_t swap_by_endian(uint32_t w, uint32_t endian) {
  switch (endian & 3) {
    case 1: return ((w & 0x00FF00FFu) << 8) | ((w >> 8) & 0x00FF00FFu);
    case 2: return __builtin_bswap32(w);
    case 3: return (w << 16) | (w >> 16);
    default: return w;
  }
}

void rgb565(uint16_t c, uint8_t* o) {
  o[0] = uint8_t(((c >> 11) & 31) * 255 / 31);
  o[1] = uint8_t(((c >> 5) & 63) * 255 / 63);
  o[2] = uint8_t((c & 31) * 255 / 31);
}

// Decodes one block (already endian-swapped to little-endian) into RGBA texels
// laid out as x,y,z,w components (Xenos component order: x = lowest bits).
void decode_block(uint32_t fmt, const uint8_t* b, uint8_t out[16][4]) {
  switch (fmt) {
    case 2: out[0][0] = out[0][1] = out[0][2] = out[0][3] = b[0]; break;
    case 10: out[0][0] = b[0]; out[0][1] = b[1]; out[0][2] = 0; out[0][3] = 255; break;
    case 4: { uint16_t c = uint16_t(b[0] | b[1] << 8); out[0][2] = uint8_t((c & 31) * 255 / 31);
      out[0][1] = uint8_t(((c >> 5) & 63) * 255 / 63); out[0][0] = uint8_t(((c >> 11) & 31) * 255 / 31); out[0][3] = 255; break; }
    case 3: { uint16_t c = uint16_t(b[0] | b[1] << 8); out[0][0] = uint8_t((c & 31) * 255 / 31);
      out[0][1] = uint8_t(((c >> 5) & 31) * 255 / 31); out[0][2] = uint8_t(((c >> 10) & 31) * 255 / 31); out[0][3] = (c >> 15) ? 255 : 0; break; }
    case 15: { uint16_t c = uint16_t(b[0] | b[1] << 8);
      for (int k = 0; k < 4; ++k) out[0][k] = uint8_t(((c >> (4 * k)) & 15) * 17); break; }
    case 6: for (int k = 0; k < 4; ++k) out[0][k] = b[k]; break;
    case 18: case 19: case 20: {
      const uint8_t* cb = fmt == 18 ? b : b + 8;
      uint8_t pal[4][4];
      const uint16_t c0 = uint16_t(cb[0] | cb[1] << 8), c1 = uint16_t(cb[2] | cb[3] << 8);
      rgb565(c0, pal[0]); rgb565(c1, pal[1]);
      pal[0][3] = pal[1][3] = pal[2][3] = pal[3][3] = 255;
      if (fmt != 18 || c0 > c1) {
        for (int k = 0; k < 3; ++k) { pal[2][k] = uint8_t((2 * pal[0][k] + pal[1][k]) / 3); pal[3][k] = uint8_t((pal[0][k] + 2 * pal[1][k]) / 3); }
      } else {
        for (int k = 0; k < 3; ++k) { pal[2][k] = uint8_t((pal[0][k] + pal[1][k]) / 2); pal[3][k] = 0; }
        pal[3][3] = 0;
      }
      const uint32_t bits = uint32_t(cb[4]) | uint32_t(cb[5]) << 8 | uint32_t(cb[6]) << 16 | uint32_t(cb[7]) << 24;
      for (int t = 0; t < 16; ++t) std::memcpy(out[t], pal[(bits >> (2 * t)) & 3], 4);
      if (fmt == 19) {
        for (int t = 0; t < 16; ++t) out[t][3] = uint8_t(((b[t / 2] >> (4 * (t & 1))) & 15) * 17);
      } else if (fmt == 20) {
        uint8_t a[8] = {b[0], b[1]};
        if (a[0] > a[1]) for (int k = 2; k < 8; ++k) a[k] = uint8_t(((8 - k) * a[0] + (k - 1) * a[1]) / 7);
        else { for (int k = 2; k < 6; ++k) a[k] = uint8_t(((6 - k) * a[0] + (k - 1) * a[1]) / 5); a[6] = 0; a[7] = 255; }
        uint64_t ab = 0;
        for (int k = 0; k < 6; ++k) ab |= uint64_t(b[2 + k]) << (8 * k);
        for (int t = 0; t < 16; ++t) out[t][3] = a[(ab >> (3 * t)) & 7];
      }
      break;
    }
  }
}

// Decodes a whole 2D level into RGBA8 (with the swizzle); false on a fault.
bool decode_texture_seh(uint32_t addr, uint32_t fmt, uint32_t endian, bool tiled, uint32_t width, uint32_t height,
                        uint32_t pitch, uint32_t swizzle, uint8_t* out, uint32_t row_bytes, uint32_t ox = 0,
                        uint32_t oy = 0) {
  TexFormat tf;
  if (!tex_format(fmt, tf)) return false;
  const uint32_t bw = (width + tf.block - 1) / tf.block, bh = (height + tf.block - 1) / tf.block;
  const uint32_t pitch_blocks = std::max<uint32_t>(pitch / tf.block, bw);
  __try {
    for (uint32_t by = 0; by < bh; ++by) {
      for (uint32_t bx = 0; bx < bw; ++bx) {
        const uint32_t idx = tiled ? tiled_offset(bx + ox, by + oy, pitch_blocks, tf.bpb)
                                   : (by + oy) * pitch_blocks + bx + ox;
        uint8_t raw[16];
        std::memcpy(raw, gmem() + addr + idx * tf.bpb, tf.bpb);
        for (uint32_t k = 0; k + 3 < tf.bpb + 3 && k < tf.bpb; k += 4) {
          uint32_t w = 0;
          std::memcpy(&w, raw + k, std::min<uint32_t>(4, tf.bpb - k));
          w = tf.bpb >= 4 ? swap_by_endian(w, endian) : (endian == 1 || endian == 2 ? ((w & 0xFF) << 8 | (w >> 8 & 0xFF)) : w);
          std::memcpy(raw + k, &w, std::min<uint32_t>(4, tf.bpb - k));
        }
        uint8_t texels[16][4];
        decode_block(fmt, raw, texels);
        const uint32_t n = tf.block;
        for (uint32_t ty = 0; ty < n; ++ty)
          for (uint32_t tx = 0; tx < n; ++tx) {
            const uint32_t x = bx * n + tx, y = by * n + ty;
            if (x >= width || y >= height) continue;
            const uint8_t* src = texels[ty * n + tx];
            uint8_t* dst = out + size_t(y) * row_bytes + size_t(x) * 4;
            for (int c = 0; c < 4; ++c) {
              const uint32_t sel = (swizzle >> (3 * c)) & 7;
              dst[c] = sel < 4 ? src[sel] : sel == 4 ? 0 : 255;
            }
          }
      }
    }
  } __except (EXCEPTION_EXECUTE_HANDLER) {
    return false;
  }
  return true;
}

// Debug: decoded RGBA texture -> <exe>\native_textures\<object>_f<fmt>_<w>x<h>.bmp.
void dump_texture(uint32_t tex, uint32_t fmt, uint32_t width, uint32_t height, const uint8_t* rgba, uint32_t row_bytes) {
  const std::wstring dir = exe_dir() + L"\\native_textures";
  CreateDirectoryW(dir.c_str(), nullptr);
  wchar_t name[96];
  swprintf(name, 96, L"\\%08X_f%u_%ux%u.bmp", tex, fmt, width, height);
  FILE* f = _wfopen((dir + name).c_str(), L"wb");
  if (!f) return;
  const uint32_t image = width * height * 4;
  uint8_t h[54] = {'B', 'M'};
  auto put = [&](int at, uint32_t v) { std::memcpy(h + at, &v, 4); };
  put(2, 54 + image); put(10, 54); put(14, 40); put(18, width); put(22, uint32_t(-int32_t(height)));
  h[26] = 1; h[28] = 32; put(34, image);
  std::fwrite(h, 1, 54, f);
  std::vector<uint8_t> row(width * 4);
  for (uint32_t y = 0; y < height; ++y) {
    const uint8_t* src = rgba + size_t(y) * row_bytes;
    for (uint32_t x = 0; x < width; ++x) {
      row[x * 4 + 0] = src[x * 4 + 2]; row[x * 4 + 1] = src[x * 4 + 1];
      row[x * 4 + 2] = src[x * 4 + 0]; row[x * 4 + 3] = src[x * 4 + 3];
    }
    std::fwrite(row.data(), 1, row.size(), f);
  }
  std::fclose(f);
}

// Packed mips (fetch dword 5 bit 11): a base level whose shorter side (rounded
// up to a power of two) is 16 texels or less lies inside the 32x32 mip-tail
// tile, 16 texels across (taller than wide or square) or down (wider), as in
// the SDK's TextureInfo::GetPackedTileOffset. Offsets are in blocks.
void packed_base_offset(const uint32_t* f, uint32_t width, uint32_t height, uint32_t block, uint32_t& ox, uint32_t& oy) {
  ox = oy = 0;
  if (!((f[5] >> 11) & 1)) return;
  const auto log2_ceil = [](uint32_t v) { uint32_t l = 0; while ((1u << l) < v) ++l; return l; };
  const uint32_t lw = log2_ceil(width), lh = log2_ceil(height);
  if (std::min(lw, lh) > 4) return;
  if (lw > lh) oy = 16 / block; else ox = 16 / block;
}

// Untiles one level of DXT blocks into rows of `row_pitch` bytes, swapping
// each 16-bit word (DXT data is 8in16): the result is a little-endian BCn level.
bool untile_blocks_seh(uint32_t addr, uint32_t endian, bool tiled, uint32_t bw, uint32_t bh, uint32_t pitch_blocks,
                       uint32_t bpb, uint8_t* out, uint32_t row_pitch, uint32_t ox = 0, uint32_t oy = 0) {
  __try {
    for (uint32_t by = 0; by < bh; ++by) {
      uint8_t* row = out + size_t(by) * row_pitch;
      for (uint32_t bx = 0; bx < bw; ++bx) {
        const uint32_t idx = tiled ? tiled_offset(bx + ox, by + oy, pitch_blocks, bpb) : (by + oy) * pitch_blocks + bx + ox;
        const uint8_t* src = gmem() + addr + size_t(idx) * bpb;
        uint8_t* dst = row + size_t(bx) * bpb;
        if (endian == 1 || endian == 2)
          for (uint32_t k = 0; k < bpb; k += 2) { dst[k] = src[k + 1]; dst[k + 1] = src[k]; }
        else
          std::memcpy(dst, src, bpb);
      }
    }
  } __except (EXCEPTION_EXECUTE_HANDLER) {
    return false;
  }
  return true;
}

// A guest texture made ready in ordinary memory, on the game thread or on the
// texture worker: its levels laid out with the row pitch the upload footprint
// needs (256-byte aligned), so committing it is a copy.
struct PreparedTexture {
  bool ok = false;
  RenderFormat format = RenderFormat::R8G8B8A8_UNORM;
  uint32_t width = 0, height = 0;
  uint32_t block_bytes = 4, block = 1;  // footprint row width (texels) = row_pitch / block_bytes * block
  struct Level {
    uint32_t w, h, rows, row_pitch;
    size_t offset;
  };
  std::vector<Level> levels;
  std::vector<uint8_t> data;
};

// DXT1/3/5 textures go to the GPU compressed (BC1/2/3), with the game's own mip
// chain: level 0 at the base address, levels 1.. one after another from the mip
// address, each padded to 32x32-block tiles and 4 KB. From the first level
// with a side of 16 texels or less they're packed together into one tile (the
// mip tail), each at its own offset, as the classic renderer reads them. They
// used to be left out, so small textures had no mips at all: the PP7's 64x32
// texture sampled only its sharp top level on the slide's edge-on bevel and
// drew a bright dotted line (issue #17). Power-of-two sizes with the plain
// swizzle only; returns false to leave the texture to the RGBA path.
bool prepare_bc(const uint32_t* f, uint32_t addr, uint32_t tex, PreparedTexture& p) {
  constexpr uint32_t swizzle_identity = 0x688;
  const uint32_t fmt = f[1] & 0x3F, endian = (f[1] >> 6) & 3;
  const bool tiled = (f[0] >> 31) & 1;
  const uint32_t width = (f[2] & 0x1FFF) + 1, height = ((f[2] >> 13) & 0x1FFF) + 1;
  const uint32_t pitch = ((f[0] >> 22) & 0x1FF) * 32, swizzle = (f[3] >> 1) & 0xFFF;
  if ((fmt != 18 && fmt != 19 && fmt != 20) || swizzle != swizzle_identity || !REXCVAR_GET(gpu_native_bc_textures))
    return false;
  const bool pow2 = (width & (width - 1)) == 0 && (height & (height - 1)) == 0;
  if (!pow2 || width < 4 || height < 4) return false;
  const uint32_t bpb = fmt == 18 ? 8 : 16;
  p.format = fmt == 18 ? RenderFormat::BC1_UNORM : fmt == 19 ? RenderFormat::BC2_UNORM : RenderFormat::BC3_UNORM;
  p.width = width; p.height = height; p.block_bytes = bpb; p.block = 4;
  const uint32_t mip_phys = f[5] & 0xFFFFF000u, guest_last = (f[4] >> 6) & 0xF;
  const auto log2_ceil = [](uint32_t v) { uint32_t l = 0; while ((1u << l) < v) ++l; return l; };
  // The packed mip tail: from the first level whose shorter side is 16 texels or
  // less, the remaining levels share one 32x32-block tile at the address that
  // level would have, each at its own offset (the SDK's GetPackedMipOffset).
  const bool packed_mips = (f[5] >> 11) & 1;
  const uint32_t min_log = std::min(log2_ceil(width), log2_ceil(height));
  const uint32_t packed_level = packed_mips ? (min_log > 4 ? min_log - 4 : 0) : 32;
  uint32_t levels = 1;
  if (tiled && mip_phys)
    while (levels <= guest_last && std::max(width >> levels, height >> levels) >= 1) ++levels;
  const uint32_t mip_addr = levels > 1 ? (mip_phys >= 0xE0000000u ? gpu_view(mip_phys) : cpu_view_of_physical(mip_phys & 0x1FFFFFFFu, 4096))
                                       : 0;
  if (levels > 1 && !mip_addr) levels = 1;
  // Block offset of level l inside the packed tail.
  const auto packed_offset = [&](uint32_t l, uint32_t& bx, uint32_t& by) {
    const uint32_t lw = log2_ceil(width), lh = log2_ceil(height);
    const uint32_t base = min_log > 4 ? min_log - 4 : 0, pm = l - base;
    uint32_t x = 0, y = 0;
    if (pm < 3) {
      if (lw > lh) y = 16 >> pm; else x = 16 >> pm;
    } else {
      if (lw > lh) x = (1u << (lw - base)) >> (pm - 2); else y = (1u << (lh - base)) >> (pm - 2);
    }
    bx = x / 4;
    by = y / 4;
  };
  size_t total = 0;
  for (uint32_t l = 0; l < levels; ++l) {
    const uint32_t w = std::max(1u, width >> l), h = std::max(1u, height >> l), bw = std::max(1u, w / 4),
                   bh = std::max(1u, h / 4);
    const uint32_t row_pitch = (bw * bpb + 255) & ~255u;
    p.levels.push_back({w, h, bh, row_pitch, total});
    total += size_t(row_pitch) * bh;
  }
  p.data.resize(total);
  uint32_t level_addr = addr;
  for (uint32_t l = 0; l < levels; ++l) {
    const PreparedTexture::Level& lv = p.levels[l];
    const uint32_t bw = std::max(1u, lv.w / 4), bh = lv.rows;
    const bool in_tail = l >= 1 && l >= packed_level;
    // A level in the packed tail is addressed within the tail level's tiles.
    const uint32_t tail = std::max(packed_level, 1u);
    uint32_t pitch_blocks = l == 0 ? std::max(pitch / 4, bw) : in_tail ? std::max(1u, (width >> tail) / 4) : bw;
    if (l == 1) level_addr = mip_addr;
    uint32_t ox = 0, oy = 0;
    if (l == 0) packed_base_offset(f, width, height, 4, ox, oy);
    else if (in_tail) packed_offset(l, ox, oy);
    if (!untile_blocks_seh(level_addr, endian, tiled, bw, bh, pitch_blocks, bpb, p.data.data() + lv.offset, lv.row_pitch,
                           ox, oy)) {
      if (l == 0) return true;  // handled, failed
      p.levels.resize(l);       // the chain so far is still usable
      break;
    }
    if (REXCVAR_GET(gpu_native_dump_textures)) {
      std::vector<uint8_t> rgba(size_t(lv.w) * lv.h * 4);
      if (decode_texture_seh(level_addr, fmt, endian, tiled, lv.w, lv.h, pitch_blocks * 4, swizzle_identity, rgba.data(),
                             lv.w * 4, ox, oy))
        dump_texture(tex + l * 0x10000000u, fmt, lv.w, lv.h, rgba.data(), lv.w * 4);
    }
    if (l >= 1 && !in_tail) {  // next level: this one's 32x32-block tiles, 4 KB aligned (the tail stays put)
      const uint64_t size = uint64_t((bw + 31) & ~31u) * ((bh + 31) & ~31u) * bpb;
      level_addr += uint32_t((size + 4095) & ~4095ull);
    }
  }
  p.ok = true;
  if (REXCVAR_GET(gpu_native_log_creation))
    REXKRNL_INFO("GENATIVE texture {:08X}: BC{} {}x{} {} levels (mips at {:08X})", tex, fmt - 17, width, height,
                 p.levels.size(), mip_phys);
  return true;
}

// Everything else is decoded to RGBA8; mip levels the guest texture has (fetch
// dword 4 bits 6-9 = last level) are rebuilt from level 0 with a box filter.
void fill_rgba_levels(PreparedTexture& p, std::vector<uint8_t> prev, uint32_t width, uint32_t height, uint32_t levels);

void prepare_rgba(const uint32_t* f, uint32_t addr, uint32_t tex, PreparedTexture& p) {
  const uint32_t fmt = f[1] & 0x3F, endian = (f[1] >> 6) & 3;
  const bool tiled = (f[0] >> 31) & 1;
  const uint32_t width = (f[2] & 0x1FFF) + 1, height = ((f[2] >> 13) & 0x1FFF) + 1;
  const uint32_t pitch = ((f[0] >> 22) & 0x1FF) * 32, swizzle = (f[3] >> 1) & 0xFFF;
  uint32_t levels = 1;
  const uint32_t guest_last = REXCVAR_GET(gpu_native_all_mips) ? 15 : (f[4] >> 6) & 0xF;
  while (levels <= guest_last && std::max(width, height) >> levels) ++levels;
  std::vector<uint8_t> prev(size_t(width) * height * 4);
  TexFormat tf{};
  tex_format(fmt, tf);
  uint32_t ox = 0, oy = 0;
  packed_base_offset(f, width, height, std::max(1u, tf.block), ox, oy);
  if (!decode_texture_seh(addr, fmt, endian, tiled, width, height, std::max(pitch, width), swizzle, prev.data(), width * 4,
                          ox, oy))
    return;
  if (REXCVAR_GET(gpu_native_log_creation))
    REXKRNL_INFO("GENATIVE texture {:08X}: d0 {:08X} d3 {:08X} fmt {} {}x{} pitch {} tiled {} endian {} swizzle {:03X} d4 {:08X} d5 {:08X} "
                 "levels {}", tex, f[0], f[3], fmt, width, height, pitch, tiled, endian, swizzle, f[4], f[5], levels);
  if (REXCVAR_GET(gpu_native_dump_textures)) dump_texture(tex, fmt, width, height, prev.data(), width * 4);
  fill_rgba_levels(p, std::move(prev), width, height, levels);
}

// `prev`: the base level, RGBA8 rows of width * 4 bytes. Fills `p` with it and
// `levels - 1` box-filtered mips.
void fill_rgba_levels(PreparedTexture& p, std::vector<uint8_t> prev, uint32_t width, uint32_t height, uint32_t levels) {
  std::vector<uint8_t> cur;
  p.format = RenderFormat::R8G8B8A8_UNORM;
  p.width = width; p.height = height; p.block_bytes = 4; p.block = 1;
  size_t total = 0;
  for (uint32_t l = 0, w = width, h = height; l < levels; ++l, w = std::max(1u, w / 2), h = std::max(1u, h / 2)) {
    const uint32_t row_pitch = (w * 4 + 255) & ~255u;
    p.levels.push_back({w, h, h, row_pitch, total});
    total += size_t(row_pitch) * h;
  }
  p.data.resize(total);
  uint32_t w = width, h = height;
  for (uint32_t l = 0; l < levels; ++l) {
    const PreparedTexture::Level& lv = p.levels[l];
    if (l) {  // box-filter the previous level
      cur.resize(size_t(lv.w) * lv.h * 4);
      const size_t sp = size_t(w) * 4;
      for (uint32_t y = 0; y < lv.h; ++y) {
        const uint8_t* r0 = prev.data() + std::min(y * 2, h - 1) * sp;
        const uint8_t* r1 = prev.data() + std::min(y * 2 + 1, h - 1) * sp;
        uint8_t* d = cur.data() + size_t(y) * lv.w * 4;
        for (uint32_t x = 0; x < lv.w; ++x) {
          const uint32_t a = std::min(x * 2, w - 1) * 4, b = std::min(x * 2 + 1, w - 1) * 4;
          for (int c = 0; c < 4; ++c) d[x * 4 + c] = uint8_t((r0[a + c] + r0[b + c] + r1[a + c] + r1[b + c] + 2) >> 2);
        }
      }
      prev.swap(cur);
      w = lv.w; h = lv.h;
    }
    for (uint32_t y = 0; y < lv.h; ++y)
      std::memcpy(p.data.data() + lv.offset + size_t(y) * lv.row_pitch, prev.data() + size_t(y) * lv.w * 4, size_t(lv.w) * 4);
  }
  p.ok = true;
}

// Replacement pictures (textures\<name>.png beside the exe or in resources\,
// exactly the original size), as the Xenos texture cache swaps them: a few
// tiled DXT1 menu textures recognised by size, mip count and the XXH3 of their
// base level. The Xenos path rewrites guest memory with DXT1; here the PNG is
// uploaded directly as RGBA with box-filtered mips.
struct Replacement {
  uint64_t hash;
  uint32_t width, height, max_level;
  const char* name;
};
constexpr Replacement kReplacements[] = {
    {0x36A2FEDF41BB63E8ull, 1124, 714, 9, "folder_cover"},
    {0xECEC23D24447D8BAull, 512, 512, 9, "page_crest"},
    {0xA3EEE5B70378A508ull, 512, 512, 9, "page_plain"},
    {0x26B888F8ABAB91B4ull, 32, 64, 0, "paperclip"},
};

// The PNG for `name` (RGBA8), loaded once; empty if missing or the wrong size.
const std::vector<uint8_t>& replacement_image(const char* name, uint32_t w, uint32_t h) {
  static std::mutex mutex;
  static std::unordered_map<std::string, std::vector<uint8_t>> images;
  std::lock_guard<std::mutex> lock(mutex);
  auto it = images.find(name);
  if (it != images.end()) return it->second;
  std::vector<uint8_t>& img = images[name];
  const std::filesystem::path exe = rex::filesystem::GetExecutableFolder();
  for (const auto& path : {exe / "resources" / "textures" / (std::string(name) + ".png"),
                           exe / "textures" / (std::string(name) + ".png")}) {
    std::ifstream file(path, std::ios::binary);
    if (!file) continue;
    std::vector<uint8_t> bytes((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
    int iw = 0, ih = 0;
    std::vector<uint8_t> rgba = rex::ui::DecodeImageRGBA(bytes.data(), bytes.size(), iw, ih);
    if (rgba.empty() || uint32_t(iw) != w || uint32_t(ih) != h) {
      REXKRNL_WARN("GENATIVE texture {} must be a PNG of exactly {}x{} (it is {}x{}), not used", name, w, h, iw, ih);
      break;
    }
    img = std::move(rgba);
    REXKRNL_INFO("GENATIVE replacement texture {} loaded ({}x{})", name, w, h);
    break;
  }
  return img;
}

bool hash_bytes_seh(uint32_t addr, uint32_t bytes, uint64_t* out);

bool prepare_replacement(const uint32_t* f, uint32_t addr, PreparedTexture& p) {
  const uint32_t fmt = f[1] & 0x3F, max_level = (f[4] >> 6) & 0xF;
  const bool tiled = (f[0] >> 31) & 1;
  const uint32_t width = (f[2] & 0x1FFF) + 1, height = ((f[2] >> 13) & 0x1FFF) + 1;
  if (fmt != 18 || !tiled) return false;  // k_DXT1
  const Replacement* match = nullptr;
  uint64_t hash = 0;
  for (const Replacement& r : kReplacements) {
    if (r.width != width || r.height != height || r.max_level != max_level) continue;
    if (!hash) {
      // The base level as the Xenos cache hashes it: whole 32x32-block tiles.
      const uint32_t pitch = std::max(((f[0] >> 22) & 0x1FF) * 32, width);
      const uint32_t bytes = ((pitch / 4 + 31) & ~31u) * (((height + 3) / 4 + 31) & ~31u) * 8;
      if (!hash_bytes_seh(addr, bytes, &hash)) return false;
      if (REXCVAR_GET(gpu_native_log_creation))
        REXKRNL_INFO("GENATIVE replacement candidate {}x{} hash {:016X}", width, height, hash);
    }
    if (r.hash == hash) match = &r;
  }
  if (!match) return false;
  std::vector<uint8_t> img = replacement_image(match->name, width, height);
  if (img.empty()) return false;
  uint32_t levels = 1;
  while (levels <= max_level && std::max(width, height) >> levels) ++levels;
  fill_rgba_levels(p, std::move(img), width, height, levels);
  return true;
}

// Thread-safe: reads guest memory only (under SEH) and fills `p`.
void prepare_texture(const uint32_t* f, uint32_t addr, uint32_t tex, PreparedTexture& p) {
  if (prepare_replacement(f, addr, p)) return;
  if (!prepare_bc(f, addr, tex, p)) prepare_rgba(f, addr, tex, p);
}

// Game thread, at the start of a draw (may flush the upload ring): creates the
// host texture and records the copies of its levels.
// Descriptor slots: a slot freed by a re-uploaded texture is reused only once
// no frame in flight can still read it.
uint32_t alloc_texture_slot() {
  if (!g.free_slots.empty()) {
    const uint32_t slot = g.free_slots.back();
    g.free_slots.pop_back();
    return slot;
  }
  return g.next_slot < kTextureSlots ? g.next_slot++ : 0;
}

uint32_t commit_texture(ResolvedTexture& t, const PreparedTexture& p) {
  if (!p.ok || p.levels.empty()) { t.slot = UINT32_MAX; return 0; }
  uint64_t total = 0;
  for (const auto& lv : p.levels) total += uint64_t(lv.row_pitch) * lv.rows + 512;
  ensure_room(total);
  t.texture = g.device->createTexture(
      RenderTextureDesc::Texture2D(p.width, p.height, uint32_t(p.levels.size()), p.format));
  t.slot = alloc_texture_slot();
  g.cmd->barriers(RenderBarrierStage::COPY, RenderTextureBarrier(t.texture.get(), RenderTextureLayout::COPY_DEST));
  for (uint32_t l = 0; l < p.levels.size(); ++l) {
    const auto& lv = p.levels[l];
    const uint64_t bytes = uint64_t(lv.row_pitch) * lv.rows;
    const uint64_t off = upload_alloc(bytes, 512);
    if (off == UINT64_MAX) break;
    std::memcpy(g.upload_ptr + off, p.data.data() + lv.offset, bytes);
    g.cmd->copyTextureRegion(RenderTextureCopyLocation::Subresource(t.texture.get(), l),
                             RenderTextureCopyLocation::PlacedFootprint(g.upload.get(), p.format, lv.w, lv.h, 1,
                                                                        lv.row_pitch / p.block_bytes * p.block, off));
  }
  g.cmd->barriers(RenderBarrierStage::GRAPHICS, RenderTextureBarrier(t.texture.get(), RenderTextureLayout::SHADER_READ));
  g.textures->setTexture(t.slot, t.texture.get(), RenderTextureLayout::SHADER_READ);
  return t.slot;
}

// A button icon: blanked (transparent) while ge_hide_button_icons is set.
uint32_t commit_icon(ResolvedTexture& t, PreparedTexture& p) {
  t.icon_hidden = REXCVAR_GET(ge_hide_button_icons);
  if (t.icon_hidden) std::fill(p.data.begin(), p.data.end(), uint8_t(0));
  return commit_texture(t, p);
}

// Texture worker: prepares textures the game thread had no time for this
// frame (gpu_native_async_textures); the game thread commits them later.
struct TextureJob {
  uint32_t f[6] = {}, addr = 0, tex = 0;
  PreparedTexture result;
  std::atomic<bool> done{false};
};

struct TextureWorker {
  std::mutex mutex;
  std::condition_variable cv;
  std::deque<std::shared_ptr<TextureJob>> queue;
  bool started = false;

  void push(std::shared_ptr<TextureJob> job) {
    {
      std::lock_guard<std::mutex> lock(mutex);
      queue.push_back(std::move(job));
      if (!started) {
        started = true;
        std::thread([this] { run(); }).detach();
      }
    }
    cv.notify_one();
  }

  void run() {
    for (;;) {
      std::shared_ptr<TextureJob> job;
      {
        std::unique_lock<std::mutex> lock(mutex);
        cv.wait(lock, [this] { return !queue.empty(); });
        job = std::move(queue.front());
        queue.pop_front();
      }
      prepare_texture(job->f, job->addr, job->tex, job->result);
      job->done.store(true, std::memory_order_release);
    }
  }
};
TextureWorker g_texture_worker;

// Host texture slot for a guest texture object (0 = white when unsupported or
// still being prepared). may_commit: the caller is at the start of a draw,
// where new textures may be created (their uploads can flush the ring).
bool hash_bytes_seh(uint32_t addr, uint32_t bytes, uint64_t* out);

void copy_pending_depth(uint32_t dest, ResolvedTexture& r);
void depth_will_change();

uint32_t texture_slot(uint32_t tex, bool may_commit = false) {
  if (!tex) return 0;
  const auto r = g.resolved.find(tex);
  if (r != g.resolved.end()) {
    if (r->second.depth_pending && may_commit) {
      r->second.depth_used_frame = g.frames + 1;
      copy_pending_depth(tex, r->second);
    }
    return r->second.slot;
  }
  uint32_t f[6];
  for (int i = 0; i < 6; ++i) f[i] = g32(tex + 28 + i * 4);
  const uint64_t key = fnv1a(reinterpret_cast<const uint8_t*>(f), sizeof(f));
  ResolvedTexture& t = g.textures_by_fetch[key];
  if (t.job) {  // on the worker
    if (!may_commit || !t.job->done.load(std::memory_order_acquire)) return 0;
    const std::shared_ptr<TextureJob> job = std::move(t.job);
    return commit_texture(t, job->result);
  }
  if (t.texture && t.dynamic && may_commit && t.checked_frame != g.frames + 1) {
    t.checked_frame = g.frames + 1;
    uint64_t h = 0;
    if (hash_bytes_seh(t.addr, t.span, &h) && h != t.content) {
      // Rewritten by the game: upload again (the old copy and its slot stay
      // until the frames in flight are done with them).
      g.retired_textures.push_back({g.frames, std::move(t.texture), t.slot});
      t.content = h;
      PreparedTexture p;
      prepare_texture(f, t.addr, tex, p);
      return commit_texture(t, p);
    }
  }
  if (t.texture && t.icon && may_commit && REXCVAR_GET(ge_hide_button_icons) != t.icon_hidden) {
    g.retired_textures.push_back({g.frames, std::move(t.texture), t.slot});
    PreparedTexture p;
    prepare_texture(f, t.addr, tex, p);
    return commit_icon(t, p);
  }
  if (t.texture || t.slot == UINT32_MAX || !may_commit) return t.texture ? t.slot : 0;
  struct Timer {
    double t0 = now_ms();
    ~Timer() { g.tex_ms += now_ms() - t0; ++g.tex_n; }
  } timer;
  const uint32_t fmt = f[1] & 0x3F, dim = (f[5] >> 9) & 3;
  const bool tiled = (f[0] >> 31) & 1;
  const uint32_t width = (f[2] & 0x1FFF) + 1, height = ((f[2] >> 13) & 0x1FFF) + 1;
  const uint32_t pitch = ((f[0] >> 22) & 0x1FF) * 32;
  const uint32_t phys = f[1] & 0xFFFFF000u;
  TexFormat tf;
  if (dim != 1 || !tex_format(fmt, tf) || width > 4096 || height > 4096) {
    t.slot = UINT32_MAX;
    static uint64_t logged = 0;
    if (fmt < 64 && !(logged & (1ull << fmt))) {
      logged |= 1ull << fmt;
      REXKRNL_WARN("GENATIVE texture format {} dim {} {}x{} not uploaded yet", fmt, dim, width, height);
    }
    return 0;
  }
  const uint32_t bh = (height + tf.block - 1) / tf.block, pitch_blocks = std::max<uint32_t>(pitch, width) / tf.block;
  const uint32_t span = tiled ? ((pitch_blocks + 31) & ~31u) * ((bh + 31) & ~31u) * tf.bpb : pitch_blocks * bh * tf.bpb;
  // Base addresses stored in the 0xE0000000 range (CAFF-loaded textures) are
  // read 4 KB on, like shader microcode (tested: the title logo only decodes
  // there); true physical addresses go through the mapped alias.
  const uint32_t addr = phys >= 0xE0000000u ? gpu_view(phys) : cpu_view_of_physical(phys & 0x1FFFFFFFu, span);
  if (!addr) { t.slot = UINT32_MAX; return 0; }
  t.dynamic = phys < 0xE0000000u;
  t.addr = addr;
  t.span = span;
  if (fmt == 6 && tiled && width == 64 && height == 64) {  // k_8_8_8_8
    // XXH3 of the original data of the icons (the hashes the SDK's cache keeps).
    static const uint64_t kIconHashes[] = {0x7F9DFBB82F6FC4ADull, 0x43542A8F53233947ull, 0xDD2A7EFD20DE3A6Aull};
    uint64_t h = 0;
    if (hash_bytes_seh(addr, span, &h)) t.icon = std::find(std::begin(kIconHashes), std::end(kIconHashes), h) != std::end(kIconHashes);
    if (t.icon) {
      PreparedTexture p;
      prepare_texture(f, addr, tex, p);
      return commit_icon(t, p);
    }
  }
  if (t.dynamic) {
    hash_bytes_seh(addr, span, &t.content);
    t.checked_frame = g.frames + 1;
  } else if (!REXCVAR_GET(gpu_native_stage_textures)) {
    hash_bytes_seh(addr, span, &t.content);  // (test: kept to count stale ones in begin_frame)
  }
  // In level, a burst of new textures (new objects coming into view) used to
  // cost a 25 ms frame: past ~1 ms of texture work in a frame they go to the
  // worker, and the draws using them wait a frame or two. Frames that ask for
  // many at once are level loads, which are long anyway: those stay here, so
  // a level doesn't open with missing textures.
  // The front end (stage 90) loads in the frame, as the Xenos path does: on
  // the worker, the main menu's first frame or two drew without its pages and
  // photo, showing the bare green folder for a moment.
  ++g.tex_requests;
  if (REXCVAR_GET(gpu_native_async_textures) && g32(0x82423E00u) != 90u &&
      g.tex_ms + (now_ms() - timer.t0) > 1.0 && g.tex_requests <= 24) {
    auto job = std::make_shared<TextureJob>();
    std::memcpy(job->f, f, sizeof(f));
    job->addr = addr;
    job->tex = tex;
    t.job = job;
    g_texture_worker.push(std::move(job));
    return 0;
  }
  const double t_dec = now_ms();
  PreparedTexture p;
  prepare_texture(f, addr, tex, p);
  g.tex_decode_ms += now_ms() - t_dec;
  return commit_texture(t, p);
}

// True while the texture's data is still being prepared on the worker: draws
// using it wait (the object appears a frame or two later instead of white).
bool texture_pending(uint32_t tex) {
  if (!tex || g.resolved.count(tex)) return false;
  uint32_t f[6];
  for (int i = 0; i < 6; ++i) f[i] = g32(tex + 28 + i * 4);
  const auto it = g.textures_by_fetch.find(fnv1a(reinterpret_cast<const uint8_t*>(f), sizeof(f)));
  return it != g.textures_by_fetch.end() && it->second.job != nullptr;
}

// Sampler slot from the texture's fetch constant (filters, clamp modes, mip
// filter, anisotropy, LOD bias and mip range), as the SDK's Xenos texture
// cache sets its samplers up. Clamp and filter come from the device's copy of
// the fetch constant (device+1152 + sampler*24: SetTexture copies the
// texture's in, and SetSamplerState patches it there), not the texture header.
//   d3: 19-20 mag, 21-22 min, 23-24 mip filter (0 point, 1 linear, 2 base map),
//       25-27 aniso (0 off, n = 2^(n-1):1)
//   d4: 2-5 / 6-9 mip min / max level, 12-21 LOD bias (signed, 1/32 units)
// The Texture Filtering setting (anisotropic_override, -1 = the game's own; the
// Xenos plugin's default 3 = 4x when that cvar isn't loaded) applies as there:
// to linear, mipmapped sampling only.
uint32_t sampler_slot(uint32_t dev, uint32_t sampler, uint32_t tex) {
  if (!tex) return 0;
  const uint32_t fetch = dev + 1152 + sampler * 24;
  const uint32_t d0 = g32(fetch), d3 = g32(fetch + 12), d4 = g32(fetch + 16), d5 = g32(fetch + 20);
  const uint32_t clamp_x = (d0 >> 10) & 7, clamp_y = (d0 >> 13) & 7, mag = (d3 >> 19) & 3, min = (d3 >> 21) & 3;
  const uint32_t mip = (d3 >> 23) & 3, mip_min = (d4 >> 2) & 0xF, mip_max = std::max((d4 >> 6) & 0xF, mip_min);
  const int32_t bias = int32_t(d4 << 10) >> 22;  // bits 12-21, sign-extended
  const int32_t override_aniso = ge::g_native_aniso.load(std::memory_order_relaxed);
  uint32_t aniso = (d3 >> 25) & 7;
  // As the classic renderer: the Texture Filtering setting applies only to a
  // texture that really has mips (a mip address; without one the fetch
  // constant's mip range is ignored and the game's own anisotropy stays).
  const bool has_mips = (d5 >> 12) != 0 && mip_max > mip_min;
  if (override_aniso > -1 && override_aniso < 6 && mip != 2 && mag == 1 && min == 1 && has_mips) aniso = uint32_t(override_aniso);
  aniso = std::min<uint32_t>(aniso, 5);
  const uint64_t key = uint64_t(clamp_x) | clamp_y << 3 | mag << 6 | min << 8 | mip << 10 | aniso << 12 | mip_min << 15 |
                       mip_max << 19 | uint64_t(uint32_t(bias) & 0x3FF) << 23;
  const auto it = g.sampler_slots.find(key);
  if (it != g.sampler_slots.end()) return it->second;
  if (g.sampler_objects.size() >= kSamplerSlots - 1) return 0;
  auto addr = [](uint32_t c) {
    switch (c) {
      case 0: return RenderTextureAddressMode::WRAP;
      case 1: return RenderTextureAddressMode::MIRROR;
      case 2: case 3: case 5: return RenderTextureAddressMode::CLAMP;
      default: return RenderTextureAddressMode::BORDER;
    }
  };
  RenderSamplerDesc sd;
  sd.magFilter = mag == 0 ? RenderFilter::NEAREST : RenderFilter::LINEAR;
  sd.minFilter = min == 0 ? RenderFilter::NEAREST : RenderFilter::LINEAR;
  sd.mipmapMode = mip == 0 ? RenderMipmapMode::NEAREST : RenderMipmapMode::LINEAR;
  if (aniso) {
    sd.anisotropyEnabled = true;
    sd.maxAnisotropy = 1u << (aniso - 1);
  }
  sd.mipLODBias = float(bias) / 32.0f + float(REXCVAR_GET(gpu_native_lod_bias));
  sd.minLOD = float(mip_min);
  sd.maxLOD = mip == 2 ? float(mip_min) : float(mip_max);  // base map: no mips
  sd.addressU = addr(clamp_x);
  sd.addressV = addr(clamp_y);
  sd.addressW = RenderTextureAddressMode::CLAMP;
  g.sampler_objects.push_back(g.device->createSampler(sd));
  const uint32_t slot = uint32_t(g.sampler_objects.size());  // slot 0 = default
  g.samplers->setSampler(slot, g.sampler_objects.back().get());
  g.sampler_slots[key] = slot;
  return slot;
}

bool hash_bytes_seh(uint32_t addr, uint32_t bytes, uint64_t* out) {
  __try {
    *out = XXH3_64bits(gmem() + addr, bytes);
  } __except (EXCEPTION_EXECUTE_HANDLER) {
    return false;
  }
  return true;
}

// xxHash of every source's bytes for vertices [first, first + vcount).
bool hash_sources_seh(const Source* srcs, size_t n, uint32_t first, uint32_t vcount, uint64_t* out) {
  __try {
    uint64_t h = 0;
    for (size_t i = 0; i < n; ++i)
      h = XXH3_64bits_withSeed(gmem() + srcs[i].addr + size_t(first) * srcs[i].stride, size_t(vcount) * srcs[i].stride, h);
    *out = h;
  } __except (EXCEPTION_EXECUTE_HANDLER) {
    return false;
  }
  return true;
}

// --- render states from the device shadows --------------------------------
// Field locations come from the D3D render-state getters (d3d_functions.toml):
// RB_DEPTHCONTROL (+10548), PA_SU_SC_MODE_CNTL (+10568), RB_BLENDCONTROL
// (+11840), blend enables (+11844), RB_COLORCONTROL (+10556), ZEnable (+11868),
// ColorWriteEnable (+11852), AlphaRef float (+10500).
struct DrawState {
  bool z_enable, z_write, blend;
  uint8_t z_func, cull, src, dst, op, src_a, dst_a, op_a, write_mask;
  bool alpha_test;
  uint8_t alpha_func;
  float alpha_ref;
  bool alpha_to_mask;  // RB_COLORCONTROL bit 4: alpha also sets the MSAA coverage (soft text edges)
};

DrawState read_state(uint32_t dev) {
  DrawState st{};
  const uint32_t depth = g32(dev + 10548), mode = g32(dev + 10568), blend = g32(dev + 11840);
  const uint32_t enables = g32(dev + 11844), color = g32(dev + 10556);
  st.z_enable = g32(dev + 11868) != 0;
  st.z_write = (depth >> 2) & 1;
  st.z_func = (depth >> 4) & 7;
  st.cull = mode & 7;
  st.blend = (enables >> 31) & 1;
  st.src = blend & 0x1F; st.op = (blend >> 5) & 7; st.dst = (blend >> 8) & 0x1F;
  st.src_a = (blend >> 16) & 0x1F; st.op_a = (blend >> 21) & 7; st.dst_a = (blend >> 24) & 0x1F;
  if (!((enables >> 30) & 1)) { st.src_a = st.src; st.dst_a = st.dst; st.op_a = st.op; }
  st.write_mask = uint8_t(g32(dev + 11852) & 0xF);
  st.alpha_test = (color >> 3) & 1;
  st.alpha_func = color & 7;
  st.alpha_to_mask = (color >> 4) & 1;
  uint32_t ref = g32(dev + 10500);
  std::memcpy(&st.alpha_ref, &ref, 4);
  return st;
}
DrawState g_draw_state{};  // state of the draw being prepared (pipeline key + desc)

RenderBlend to_blend(uint8_t b) {
  switch (b) {
    case 0: return RenderBlend::ZERO;
    case 1: return RenderBlend::ONE;
    case 4: return RenderBlend::SRC_COLOR;
    case 5: return RenderBlend::INV_SRC_COLOR;
    case 6: return RenderBlend::SRC_ALPHA;
    case 7: return RenderBlend::INV_SRC_ALPHA;
    case 8: return RenderBlend::DEST_COLOR;
    case 9: return RenderBlend::INV_DEST_COLOR;
    case 10: return RenderBlend::DEST_ALPHA;
    case 11: return RenderBlend::INV_DEST_ALPHA;
    case 12: return RenderBlend::BLEND_FACTOR;
    case 13: return RenderBlend::INV_BLEND_FACTOR;
    case 16: return RenderBlend::SRC_ALPHA_SAT;
    default: return RenderBlend::ONE;
  }
}
// D3D12/Vulkan reject colour factors in the alpha blend; on the Xbox 360 a
// colour factor used for alpha reads that colour's alpha, so it is the same.
RenderBlend to_blend_alpha(uint8_t b) {
  switch (b) {
    case 4: return RenderBlend::SRC_ALPHA;
    case 5: return RenderBlend::INV_SRC_ALPHA;
    case 8: return RenderBlend::DEST_ALPHA;
    case 9: return RenderBlend::INV_DEST_ALPHA;
    default: return to_blend(b);
  }
}
RenderBlendOperation to_blend_op(uint8_t o) {
  switch (o) {
    case 1: return RenderBlendOperation::SUBTRACT;
    case 2: return RenderBlendOperation::MIN;
    case 3: return RenderBlendOperation::MAX;
    case 4: return RenderBlendOperation::REV_SUBTRACT;
    default: return RenderBlendOperation::ADD;
  }
}
RenderComparisonFunction to_cmp(uint8_t c) {
  switch (c) {
    case 0: return RenderComparisonFunction::NEVER;
    case 1: return RenderComparisonFunction::LESS;
    case 2: return RenderComparisonFunction::EQUAL;
    case 3: return RenderComparisonFunction::LESS_EQUAL;
    case 4: return RenderComparisonFunction::GREATER;
    case 5: return RenderComparisonFunction::NOT_EQUAL;
    case 6: return RenderComparisonFunction::GREATER_EQUAL;
    default: return RenderComparisonFunction::ALWAYS;
  }
}

// Decodes vcount vertices into out; returns false if guest memory faulted.
// Kept free of C++ objects so it can use SEH.
bool decode_vertices_seh(const Element* els, size_t n_els, const Source* srcs, const uint16_t* keys,
                         size_t n_keys, uint32_t first, uint32_t vcount, uint8_t* out, uint32_t host_stride,
                         const HostFmt* fmts, const uint32_t* offsets) {
  __try {
    for (uint32_t v = 0; v < vcount; ++v) {
      uint8_t* dst = out + uint64_t(v) * host_stride;
      for (size_t e = 0; e < n_els; ++e) {
        size_t k = 0;
        while (k < n_keys && keys[k] != els[e].stream) ++k;
        const Source& src = srcs[k];
        decode_element(els[e], src.addr + (first + v) * src.stride + els[e].offset, src.endian, dst + offsets[e], fmts[e]);
      }
    }
  } __except (EXCEPTION_EXECUTE_HANDLER) {
    return false;
  }
  return true;
}

// Helper threads for converting big meshes: the game thread waits for them in
// the draw, so nothing else touches guest memory meanwhile. A level's big
// meshes (17-43k vertices in Dam) cost ~10 ms each on one core; the first
// switch to the other graphics mode converted dozens in one frame.
class DecodeWorkers {
 public:
  static DecodeWorkers& get() {
    static DecodeWorkers* w = new DecodeWorkers;  // never destroyed: threads live as long as the process
    return *w;
  }
  uint32_t count() const { return uint32_t(threads_.size()) + 1; }
  // Runs fn(0..jobs-1) on the helpers and this thread; returns when all are done.
  void run(uint32_t jobs, const std::function<void(uint32_t)>& fn) {
    if (threads_.empty() || jobs < 2) {
      for (uint32_t i = 0; i < jobs; ++i) fn(i);
      return;
    }
    {
      std::lock_guard<std::mutex> lock(mutex_);
      fn_ = &fn;
      jobs_ = jobs;
      next_ = 0;
      left_ = jobs;
      ++generation_;
    }
    wake_.notify_all();
    work();
    std::unique_lock<std::mutex> lock(mutex_);
    done_.wait(lock, [&] { return left_ == 0; });
    fn_ = nullptr;
  }

 private:
  DecodeWorkers() {
    const uint32_t hw = std::max(1u, std::thread::hardware_concurrency());
    const uint32_t helpers = std::min(7u, hw > 2 ? hw - 2 : 0u);
    for (uint32_t i = 0; i < helpers; ++i)
      threads_.emplace_back([this] {
        uint64_t seen = 0;
        for (;;) {
          {
            std::unique_lock<std::mutex> lock(mutex_);
            wake_.wait(lock, [&] { return generation_ != seen; });
            seen = generation_;
          }
          work();
        }
      });
    for (auto& t : threads_) t.detach();
  }
  // Jobs are claimed under the lock with the function they belong to, so a
  // helper late from one run can never call it for another.
  void work() {
    for (;;) {
      const std::function<void(uint32_t)>* fn;
      uint32_t i;
      {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!fn_ || next_ >= jobs_) return;
        fn = fn_;
        i = next_++;
      }
      (*fn)(i);
      std::lock_guard<std::mutex> lock(mutex_);
      if (--left_ == 0) done_.notify_all();
    }
  }
  std::vector<std::thread> threads_;
  std::mutex mutex_;
  std::condition_variable wake_, done_;
  const std::function<void(uint32_t)>* fn_ = nullptr;
  uint32_t jobs_ = 0, left_ = 0, next_ = 0;
  uint64_t generation_ = 0;
};

// decode_vertices_seh, split across the helper threads for big meshes.
bool decode_vertices(const Element* els, size_t n_els, const Source* srcs, const uint16_t* keys, size_t n_keys,
                     uint32_t first, uint32_t vcount, uint8_t* out, uint32_t host_stride, const HostFmt* fmts,
                     const uint32_t* offsets) {
  constexpr uint32_t kChunk = 2048;
  if (vcount < 2 * kChunk)
    return decode_vertices_seh(els, n_els, srcs, keys, n_keys, first, vcount, out, host_stride, fmts, offsets);
  const uint32_t jobs = (vcount + kChunk - 1) / kChunk;
  std::atomic<bool> ok{true};
  DecodeWorkers::get().run(jobs, [&](uint32_t j) {
    const uint32_t a = j * kChunk, n = std::min(kChunk, vcount - a);
    if (!decode_vertices_seh(els, n_els, srcs, keys, n_keys, first + a, n, out + uint64_t(a) * host_stride, host_stride,
                             fmts, offsets))
      ok.store(false);
  });
  return ok.load();
}

// --- frame ------------------------------------------------------------------

void copy_scene_to(RenderTexture* dst);

void bind_target() {
  const bool scene = g.guest_depth != 0 || g.guest_rt == 0;
  if (scene) {
    if (g.guest_rt) g.scene_surface = g.guest_rt;
    if (g.bound_fb == g.scene_fb.get()) return;
    g.cmd->barriers(RenderBarrierStage::GRAPHICS,
                    {RenderTextureBarrier(g.scene.get(), RenderTextureLayout::COLOR_WRITE),
                     RenderTextureBarrier(g.scene_depth.get(), RenderTextureLayout::DEPTH_WRITE)});
    g.cmd->setFramebuffer(g.scene_fb.get());
    g.bound_fb = g.scene_fb.get();
    g.bound_color = g.scene.get();
    g.bound_scene = true;
    return;
  }
  Native::ColorTarget& t = g.color_targets[g.guest_rt];
  if (!t.color) {
    t.color = g.device->createTexture(RenderTextureDesc::ColorTarget(g_scene_w, g_scene_h, kSceneFormat));
    const RenderTexture* c = t.color.get();
    t.fb = g.device->createFramebuffer(RenderFramebufferDesc(&c, 1));
  }
  if (g.bound_fb == t.fb.get()) return;
  // EDRAM aliasing: a surface at the same EDRAM base as the scene starts out
  // holding the scene's pixels on the 360 (the title's post pass blends onto
  // them: without this the never-cleared target smeared old frames). The base
  // is the low 12 bits of the surface's colour info word (+24).
  const uint32_t base_rt = g32(g.guest_rt + 24) & 0xFFF;
  if (g.bound_scene && g.scene_surface && base_rt == (g32(g.scene_surface + 24) & 0xFFF)) {
    copy_scene_to(t.color.get());
    g.cmd->barriers(RenderBarrierStage::GRAPHICS,
                    {RenderTextureBarrier(g.scene.get(), RenderTextureLayout::COLOR_WRITE),
                     RenderTextureBarrier(g.scene_depth.get(), RenderTextureLayout::DEPTH_WRITE)});
  }
  g.cmd->barriers(RenderBarrierStage::GRAPHICS, RenderTextureBarrier(t.color.get(), RenderTextureLayout::COLOR_WRITE));
  g.cmd->setFramebuffer(t.fb.get());
  g.bound_fb = t.fb.get();
  g.bound_color = t.color.get();
  g.bound_scene = false;
}

void begin_frame() {
  if (g.recording) return;
  if (!g.warm.empty() && REXCVAR_GET(gpu_native_warm_shaders)) warm_registered_shaders();
  // The Anti-Aliasing setting changed: wait for the frames in flight, then new
  // scene targets with the new sample count (pipelines are keyed by it, and
  // the depth resolve pass is rebuilt for it).
  if (const RenderSampleCounts want = pick_msaa(); g.scene && want != g.msaa.sampleCount) {
    for (uint32_t i = 0; i < kFrames; ++i) {
      if (!g.pending[i]) continue;
      if (g.out.vk) ge::native_vk::WaitPresent(g.out.vk_handoff[i]);  // (the worker is done with the fence)
      g.queue->waitForCommandFence(g.fences[i].get());
      g.pending[i] = false;
    }
    g.msaa.sampleCount = want;
    g.bound_fb = nullptr;
    g.bound_color = nullptr;
    g.scene_fb.reset();
    create_scene_targets();
    for (auto& [dest, r] : g.resolved) {  // full-size depth resolves: made again with the new pass
      if (!g.depth_sets.count(dest) || !r.texture || r.span != 0) continue;
      g.retired_textures.push_back({g.frames, std::move(r.texture), r.slot});
      r.depth_pending = false;
    }
    g.depth_pending.clear();
    g.depth_sets.clear();
    g.depth_pipe.reset();
    g.depth_shader.reset();
    g.depth_layout.reset();
    g.depth_view.reset();
    g.depth_pass_failed = false;
    REXKRNL_INFO("GENATIVE anti-aliasing: {}x MSAA", uint32_t(want));
  }
  // A new stage (a level, or the front end) loads its textures where the last
  // one's were: the same fetch constants can then name a different picture
  // (Cradle after a run of levels showed red from an earlier one), and the
  // old ones would stay in video memory. Start the texture cache afresh.
  if (const uint32_t stage = g32(0x82423E00u); stage != g.textures_stage && !REXCVAR_GET(gpu_native_stage_textures)) {
    // Test (cache kept across stages): how many cached pictures the new stage
    // has overwritten in memory, i.e. would be drawn with the old picture.
    g.textures_stage = stage;
    size_t cached = 0, stale = 0;
    for (const auto& [key, t] : g.textures_by_fetch) {
      if (!t.texture || t.dynamic || t.icon) continue;
      ++cached;
      uint64_t h = 0;
      if (hash_bytes_seh(t.addr, t.span, &h) && h != t.content) ++stale;
    }
    REXKRNL_INFO("GENATIVE stage {}: {} cached level textures, {} now hold a different picture", stage, cached, stale);
  } else if (const uint32_t stage = g32(0x82423E00u); stage != g.textures_stage) {
    g.textures_stage = stage;
    size_t n = 0;
    for (auto& [key, t] : g.textures_by_fetch) {
      if (!t.texture) continue;
      g.retired_textures.push_back({g.frames, std::move(t.texture), t.slot});
      ++n;
    }
    g.textures_by_fetch.clear();  // (a texture still on the worker is dropped with its job)
    if (n) REXKRNL_INFO("GENATIVE stage {}: {} textures from the last stage released", stage, n);
    // The last stage's converted geometry too (see the geometry budget in end_frame).
    const size_t geometry = g.vertex_cache.size() + g.index_cache.size();
    for (auto& [key, e] : g.vertex_cache) retire(g.vertex_pool, e.buffer);
    for (auto& [key, e] : g.index_cache) retire(g.index_pool, e.buffer);
    g.vertex_cache.clear();
    g.index_cache.clear();
    if (geometry) REXKRNL_INFO("GENATIVE stage {}: {} cached geometry buffers released", stage, geometry);
  }
  g.cmd->begin();
  g.layout_bound = false;
  g.bound_fb = nullptr;
  bind_target();
  if (g.timestamps) g.cmd->writeTimestamp(g.timestamps.get(), g.slot * 2);
  g.prof_active = g.prof_pool && REXCVAR_GET(gpu_native_profile_frame) > 0 &&
                  g.frames + 1 == uint64_t(REXCVAR_GET(gpu_native_profile_frame));
  g.prof_labels.clear();
  if (g.prof_active) { g.cmd->writeTimestamp(g.prof_pool.get(), 0); g.prof_labels.push_back("start"); }
  g.recording = true;
  g.draws = g.skipped = 0;
  std::memset(g.skip_reason, 0, sizeof(g.skip_reason));
}

void prof_mark(std::string label) {
  if (!g.prof_active || g.prof_labels.size() >= 4096) return;
  g.cmd->writeTimestamp(g.prof_pool.get(), uint32_t(g.prof_labels.size()));
  g.prof_labels.push_back(std::move(label));
}

void set_common_state(uint32_t dev) {
  // The root signature and the bindless tables stay bound for the whole
  // command list (re-setting them every draw reset all root bindings).
  if (!g.layout_bound) {
    g.cmd->setGraphicsPipelineLayout(g.layout.get());
    g.cmd->setGraphicsDescriptorSet(g.textures.get(), 0);
    g.cmd->setGraphicsDescriptorSet(g.textures.get(), 1);
    g.cmd->setGraphicsDescriptorSet(g.textures.get(), 2);
    g.cmd->setGraphicsDescriptorSet(g.samplers.get(), 3);
    g.layout_bound = true;
  }
  const uint32_t vp = dev + kDevViewport;
  float f[6];
  for (int i = 0; i < 6; ++i) {
    const uint32_t u = g32(vp + i * 4);
    std::memcpy(&f[i], &u, 4);
  }
  if (f[2] <= 0 || f[3] <= 0 || (g32(dev + 10572) & 0x3F) == 0) {
    f[0] = f[1] = 0; f[2] = float(kGuestW); f[3] = float(kGuestH); f[4] = 0; f[5] = 1;
  }
  for (int i = 0; i < 4; ++i) f[i] *= g_scene_scale;  // guest pixels -> scene pixels
  RenderViewport viewport(f[0], f[1], f[2], f[3], f[4], f[5] > f[4] ? f[5] : 1.0f);
  g.cmd->setViewports(&viewport, 1);
  RenderRect scissor(0, 0, g_scene_w, g_scene_h);
  g.cmd->setScissors(&scissor, 1);

  // Constants: guest big-endian floats -> little-endian.
  // Each shader's constants are only the bytes it reads (<hash>.cbsize), and a
  // block identical to one already uploaded for this submission is reused:
  // copying full 4 KB + 3.5 KB blocks per draw took ~3.7 ms of GPU time a frame.
  uint8_t* cbase = g.const_ptr + g.slot * kSlotConsts;
  auto swap_copy = [](uint8_t* dst, uint32_t src, uint32_t bytes) {
    const uint8_t* s = gmem() + src;
    for (uint32_t i = 0; i < bytes; i += 4) {
      uint32_t w;
      std::memcpy(&w, s + i, 4);
      w = __builtin_bswap32(w);
      std::memcpy(dst + i, &w, 4);
    }
  };
  auto upload_block = [&](uint32_t src, uint32_t bytes) -> uint64_t {
    alignas(16) uint8_t tmp[4096];
    bytes = std::max<uint32_t>(16, std::min<uint32_t>(bytes, 4096));
    swap_copy(tmp, src, bytes);
    const uint64_t key = XXH3_64bits_withSeed(tmp, bytes, bytes);
    const auto it = g.const_dedup.find(key);
    if (it != g.const_dedup.end()) return it->second;
    const uint64_t off = const_alloc(bytes);
    if (off != UINT64_MAX) { std::memcpy(cbase + off, tmp, bytes); g.const_dedup[key] = off; }
    return off;
  };
  const uint64_t vs = upload_block(dev + kDevVsConsts, g_vs_cb_bytes);
  const uint64_t ps = upload_block(dev + kDevPsConsts, g_ps_cb_bytes);
  const uint64_t shared = const_alloc(512);
  if (vs == UINT64_MAX || ps == UINT64_MAX || shared == UINT64_MAX) return;
  alignas(16) uint32_t sh[128] = {};
  for (uint32_t s = 0; s < 16; ++s) {
    sh[s] = texture_slot(g_textures[s]);       // Texture2D descriptor index
    sh[48 + s] = sampler_slot(dev, s, g_textures[s]);  // sampler descriptor index
  }
  // Alpha test: the translated pixel shaders clip(alpha - threshold); only
  // GREATER / GREATER_EQUAL tests map onto that, others draw untested.
  const DrawState st = read_state(dev);
  float threshold = 0.0f;
  if (st.alpha_test && (st.alpha_func == 4 || st.alpha_func == 6)) {
    threshold = st.alpha_ref > 1.0f ? st.alpha_ref / 255.0f : st.alpha_ref;
    if (st.alpha_func == 4) threshold += 1.0f / 512.0f;
  }
  std::memcpy(&sh[68], &threshold, 4);  // g_AlphaThreshold (c17.x)
  sh[64] = (g32(dev + kDevVsBools) & 0xFFFF) | ((g32(dev + kDevPsBools) & 0xFFFF) << 16);  // g_Booleans
  sh[65] = 0;  // g_SwappedTexcoords (16-bit components are fixed on upload)
  sh[69] = g_spec_bits;  // g_NativeSpecConstants (c17.y)
  NTRACE("  bools {:08X} alpha ref {} threshold {} sampler0 slot {} fetch d0 {:08X} d3 {:08X}", sh[64], st.alpha_ref,
         threshold, sh[48], g32(dev + 1152), g32(dev + 1152 + 12));
  if (tracing()) {
    std::string fs;
    for (uint32_t t = 0; t < 4; ++t)
      fs += fmt::format(" s{} d0 {:08X} d3 {:08X} d4 {:08X}", t, g32(dev + 1152 + t * 24), g32(dev + 1152 + t * 24 + 12),
                        g32(dev + 1152 + t * 24 + 16));
    NTRACE("  fetch{}", fs);
    const float* c = reinterpret_cast<const float*>(g.upload_ptr + vs);
    NTRACE("  vs c0-3 [{} {} {} {}] [{} {} {} {}] [{} {} {} {}] [{} {} {} {}]", c[0], c[1], c[2], c[3], c[4], c[5], c[6],
           c[7], c[8], c[9], c[10], c[11], c[12], c[13], c[14], c[15]);
    const float* pc = reinterpret_cast<const float*>(cbase + ps);
    NTRACE("  ps c0-1 [{} {} {} {}] [{} {} {} {}] ({} bytes)", pc[0], pc[1], pc[2], pc[3], pc[4], pc[5], pc[6], pc[7],
           g_ps_cb_bytes);
  }
  // c18: position scale / offset. PA_CL_VTE_CNTL (+10572, written by the
  // VIEWPORTENABLE setter: 0x43F on, 0x400 off). With the viewport transform off
  // the shader outputs window coordinates, mapped here to NDC for the target.
  const bool vte = (g32(dev + 10572) & 0x3F) != 0;
  // The 360 puts pixel centres on whole coordinates (D3D9), D3D12 half a pixel
  // on: everything moves right and down by half a pixel, as XenosRecomp's
  // g_HalfPixelOffset intends (without it text lands on texel centres and
  // comes out thinner than on Xenos).
  const bool half = REXCVAR_GET(gpu_native_half_pixel);
  float pos[4] = {1.0f, 1.0f, half ? 1.0f / kGuestW : 0.0f, half ? -1.0f / kGuestH : 0.0f};
  if (!vte) { pos[0] = 2.0f / kGuestW; pos[1] = -2.0f / kGuestH; pos[2] = -1.0f + pos[2]; pos[3] = 1.0f + pos[3]; }
  std::memcpy(&sh[72], pos, sizeof(pos));
  std::memcpy(cbase + shared, sh, sizeof(sh));
  if (g_vulkan) {
    const uint64_t at = g.const_gpu[g.slot]->getDeviceAddress();
    const uint64_t addrs[3] = {at + vs, at + ps, at + shared};
    g.cmd->setGraphicsPushConstants(0, addrs, 0, sizeof(addrs));
  } else {
    g.cmd->setGraphicsRootDescriptor(RenderBufferReference(g.const_gpu[g.slot].get(), vs), 0);
    g.cmd->setGraphicsRootDescriptor(RenderBufferReference(g.const_gpu[g.slot].get(), ps), 1);
    g.cmd->setGraphicsRootDescriptor(RenderBufferReference(g.const_gpu[g.slot].get(), shared), 2);
  }
}

RenderPipeline* pipeline_for(RenderShader* vs, RenderShader* ps, uint32_t decl,
                             const std::vector<Element>& els, RenderPrimitiveTopology topo,
                             std::vector<RenderInputSlot>& slots) {
  uint64_t key = fnv1a(reinterpret_cast<const uint8_t*>(&vs), sizeof(vs));
  key ^= fnv1a(reinterpret_cast<const uint8_t*>(&ps), sizeof(ps)) * 3;
  key ^= (uint64_t(decl) << 8) ^ uint64_t(topo);
  for (const Element& el : els) key = key * 31 + (uint64_t(el.usage) << 8 | el.index | uint64_t(el.type) << 16);
  key = key * 1000003 + fnv1a(reinterpret_cast<const uint8_t*>(&g_draw_state), sizeof(g_draw_state));
  key ^= g.bound_scene ? uint64_t(g.msaa.sampleCount) << 56 : 0x5A5A5A5A5A5A5A5Aull;  // (else single-sample, no depth)
  const bool known = g.pipelines.count(key) != 0;
  auto& p = g.pipelines[key];
  if (p || known) return p.get();  // known && !p: creation failed before
  std::vector<RenderInputElement> inputs;
  uint32_t offset = 0;
  for (size_t e = 0; e < els.size(); ++e) {
    const Element& el = els[e];
    const HostFmt hf = host_format(el);
    if (el.usage < std::size(kSemantics))
      inputs.emplace_back(kSemantics[el.usage], el.index, vk_location(el.usage, el.index), hf.format, 0, offset);
    offset += hf.size;
  }
  RenderGraphicsPipelineDesc d;
  d.pipelineLayout = g.layout.get();
  d.vertexShader = vs;
  d.pixelShader = ps;
  d.primitiveTopology = topo;
  d.renderTargetFormat[0] = kSceneFormat;
  d.multisampling = g.bound_scene ? g.msaa : RenderMultisampling();
  const DrawState& st = g_draw_state;
  d.alphaToCoverageEnabled = g.bound_scene && st.alpha_to_mask && g.msaa.sampleCount > 1;
  RenderBlendDesc blend = RenderBlendDesc::Copy();
  blend.blendEnabled = st.blend;
  blend.srcBlend = to_blend(st.src); blend.dstBlend = to_blend(st.dst); blend.blendOp = to_blend_op(st.op);
  blend.srcBlendAlpha = to_blend_alpha(st.src_a); blend.dstBlendAlpha = to_blend_alpha(st.dst_a); blend.blendOpAlpha = to_blend_op(st.op_a);
  blend.renderTargetWriteMask = st.write_mask;
  d.renderTargetBlend[0] = blend;
  d.depthEnabled = g.bound_scene && st.z_enable;
  d.depthWriteEnabled = g.bound_scene && st.z_enable && st.z_write;
  d.depthFunction = to_cmp(st.z_func);
  d.frontFace = RenderFrontFace::CLOCKWISE;
  d.cullMode = (st.cull & 3) == 0 ? RenderCullMode::NONE : st.cull == 2 ? RenderCullMode::FRONT : RenderCullMode::BACK;
  d.renderTargetCount = 1;
  d.depthTargetFormat = g.bound_scene ? RenderFormat::D32_FLOAT : RenderFormat::UNKNOWN;
  d.inputSlots = slots.data();
  d.inputSlotsCount = uint32_t(slots.size());
  d.inputElements = inputs.data();
  d.inputElementsCount = uint32_t(inputs.size());
  if (REXCVAR_GET(gpu_native_log_creation)) {
    std::string desc;
    for (const RenderInputElement& in : inputs)
      desc += fmt::format(" {}{}:slot{}+{}:fmt{}", in.semanticName, in.semanticIndex, in.slotIndex,
                          in.alignedByteOffset, int(in.format));
    for (const RenderInputSlot& sl : slots) desc += fmt::format(" [slot {} stride {}]", sl.index, sl.stride);
    REXKRNL_INFO("GENATIVE pipeline {:016X}:{}", key, desc);
  }
  const double t0 = now_ms();
  p = g.device->createGraphicsPipeline(d);
  g.pipe_ms += now_ms() - t0;
  ++g.pipe_n;
  drain_debug_messages("pipeline");
  if (p && !static_cast<D3D12GraphicsPipeline*>(p.get())->d3d) {
    REXKRNL_WARN("GENATIVE pipeline {:016X} failed to create; its draws are skipped", key);
    p.reset();
  }
  return p.get();
}

// One draw. indices == nullptr: non-indexed, `count` vertices from `first`.
// up_data != 0: vertices come from guest memory directly (DrawVerticesUP /
// Begin/EndVertices) with stride up_stride, as stream 0.
// idx_addr != 0: indexed draw, `count` indices from guest memory at idx_addr
// (32-bit when idx_wide, else 16-bit), read only when the cached list is stale.
void native_draw(uint32_t dev, uint32_t prim, uint32_t count, const uint32_t* indices,
                 int32_t base_vertex, uint32_t first, uint32_t up_data, uint32_t up_stride,
                 bool restart_16 = true, uint32_t idx_addr = 0, bool idx_wide = false) {
  if (!ensure_device() || count == 0) return;
  begin_frame();
  const double t_start = now_ms();
  struct Total {
    double t0;
    ~Total() { g.sect[4] += now_ms() - t0; }
  } total{t_start};
  const uint32_t vs_obj = g32(dev + kDevVertexShader);
  RenderShader* vs = host_shader(vs_obj, true);
  RenderShader* ps = host_shader(g32(dev + kDevPixelShader), false);
  if (!vs || !ps) {
    NTRACE("skip prim {} n {}: vs {:08X} {} ps {:08X} {}", prim, count, vs_obj, vs != nullptr,
           g32(dev + kDevPixelShader), ps != nullptr);
    if (tracing() && !ps) {
      const uint32_t o = g32(dev + kDevPixelShader);
      std::string w;
      for (uint32_t i = 0; i < 24; ++i) w += fmt::format(" {:08X}", g32(o + i * 4));
      NTRACE("ps object{}", w);
    }
    ++g.skipped; ++g.skip_reason[0]; return;
  }
  {  // gpu_native_skip_ps: leave out draws with that pixel shader (debugging)
    static const std::string skip = REXCVAR_GET(gpu_native_skip_ps);
    if (!skip.empty() && fmt::format("{:016X}", g.shaders[g32(dev + kDevPixelShader)].hash) == skip) return;
  }
  HostShader& vsh = g.shaders[vs_obj];
  if (!vsh.container.empty()) {
    const uint32_t shader_off = __builtin_bswap32(*reinterpret_cast<const uint32_t*>(&vsh.container[24]));
    const uint32_t phys_off = __builtin_bswap32(*reinterpret_cast<const uint32_t*>(&vsh.container[shader_off]));
    const uint32_t live = gpu_view(g32(vs_obj + 32));
    if (live) parse_vertex_inputs(vsh.container, vsh, live + phys_off);
  }
  const bool from_shader = !vsh.inputs.empty();
  const double t_parsed = now_ms();
  g.sect[0] += t_parsed - t_start;
  const uint32_t decl = from_shader ? 0 : g32(dev + kDevVertexDecl);
  std::vector<Element> els = from_shader ? vsh.inputs : read_decl(decl);
  if (els.empty()) {
    static int logged = 0;
    if (logged++ < 4)
      REXKRNL_WARN("GENATIVE empty decl {:08X}: +0 {:08X} +24 {:08X} +28 {:08X} +52 {:08X} {:08X} {:08X}", decl,
                   decl ? g32(decl) : 0, decl ? g32(decl + 24) : 0, decl ? g32(decl + 28) : 0,
                   decl ? g32(decl + 52) : 0, decl ? g32(decl + 56) : 0, decl ? g32(decl + 60) : 0);
    ++g.skipped; ++g.skip_reason[1]; return;
  }

  // Index list (rebased to the vertex range actually used); fans -> lists.
  // Indexed draws keep the converted list on the GPU between frames, checked
  // against an xxHash of the guest indices.
  Native::IndexCacheEntry* ientry = nullptr;
  uint64_t icontent = 0;
  if (idx_addr) {
    const uint32_t bytes = count * (idx_wide ? 4 : 2);
    if (!hash_bytes_seh(idx_addr, bytes, &icontent)) { ++g.skipped; ++g.skip_reason[3]; return; }
    const uint64_t key = XXH3_64bits_withSeed(&idx_addr, 4, (uint64_t(count) << 32) ^ (uint64_t(prim) << 8) ^ idx_wide) ^
                         uint64_t(uint32_t(base_vertex)) * 0x9E3779B97F4A7C15ull;
    ientry = &g.index_cache[key];
    ientry->last_frame = g.frames;
  }
  // A traced frame decodes every draw afresh (the geometry log below needs the indices and vertices).
  const std::string hide = REXCVAR_GET(gpu_native_hide);  // test: see gpu_native_hide
  const bool ihit = !tracing() && hide.empty() && ientry && ientry->buffer && ientry->content == icontent;
  std::vector<uint32_t> idx;
  std::vector<uint32_t> hidden_tris;  // gpu_native_hide's triangles, traced below
  int64_t lo = 0;
  uint32_t vcount = 0, icount = 0;
  RenderPrimitiveTopology topo = RenderPrimitiveTopology::TRIANGLE_LIST;
  if (ihit) {
    lo = ientry->lo; vcount = ientry->vcount; icount = ientry->count; topo = ientry->topo;
  } else {
  if (idx_addr) {
    idx.resize(count);
    for (uint32_t i = 0; i < count; ++i) idx[i] = idx_wide ? g32(idx_addr + i * 4) : g16(idx_addr + i * 2);
  } else if (indices) idx.assign(indices, indices + count);
  else for (uint32_t i = 0; i < count; ++i) idx.push_back(first + i);
  // Everything becomes a triangle (or point/line) list. Index 0xFFFF / 0xFFFFFFFF
  // is the primitive-restart cut (D3DRS_PRIMITIVERESETINDEX): strips and fans
  // restart there, list triangles touching it are dropped.
  const uint32_t restart = ((indices || idx_addr) && restart_16) ? 0xFFFFu : 0xFFFFFFFFu;
  std::vector<uint32_t> list;
  switch (prim) {
    case 4:
      for (size_t i = 0; i + 2 < idx.size(); i += 3)
        if (idx[i] != restart && idx[i + 1] != restart && idx[i + 2] != restart)
          list.insert(list.end(), {idx[i], idx[i + 1], idx[i + 2]});
      break;
    case 5: case 6: {  // fan / strip, split at restart
      size_t start = 0;
      for (size_t i = 0; i <= idx.size(); ++i) {
        if (i < idx.size() && idx[i] != restart) continue;
        for (size_t k = start; k + 2 < i; ++k) {
          if (prim == 5) list.insert(list.end(), {idx[start], idx[k + 1], idx[k + 2]});
          else if (((k - start) & 1) == 0) list.insert(list.end(), {idx[k], idx[k + 1], idx[k + 2]});
          else list.insert(list.end(), {idx[k + 1], idx[k], idx[k + 2]});
        }
        start = i + 1;
      }
      break;
    }
    case 13:  // quad list
      // Split along the 1-3 diagonal, as the classic renderer does (its quad
      // geometry shader emits the strip 0, 1, 3, 2), so a quad that isn't flat
      // or evenly lit shades the same in both.
      for (size_t i = 0; i + 3 < idx.size(); i += 4)
        list.insert(list.end(), {idx[i], idx[i + 1], idx[i + 3], idx[i + 1], idx[i + 2], idx[i + 3]});
      break;
    case 1: topo = RenderPrimitiveTopology::POINT_LIST; list = idx; break;
    case 2: topo = RenderPrimitiveTopology::LINE_LIST; list = idx; break;
    case 3: topo = RenderPrimitiveTopology::LINE_STRIP; list = idx; break;
    default: ++g.skipped; ++g.skip_reason[2]; return;  // rect lists etc.: not yet
  }
  idx.swap(list);
  if (idx.empty()) return;
  if (!hide.empty()) {  // gpu_native_hide: "a-b" draws, or "d:a-b" triangles of draw d
    unsigned d = 0, a = 0, b = 0;
    if (std::sscanf(hide.c_str(), "%u:%u-%u", &d, &a, &b) == 3) {
      if (g.draws == d && prim != 1 && prim != 2 && prim != 3) {
        std::vector<uint32_t> kept;
        hidden_tris.clear();
        for (size_t t = 0; t + 2 < idx.size(); t += 3)
          if (t / 3 < a || t / 3 > b) kept.insert(kept.end(), {idx[t], idx[t + 1], idx[t + 2]});
          else hidden_tris.insert(hidden_tris.end(), {idx[t], idx[t + 1], idx[t + 2]});
        if (kept.empty()) { ++g.draws; return; }
        idx.swap(kept);
      }
    } else if (std::sscanf(hide.c_str(), "%u-%u", &a, &b) == 2 && g.draws >= a && g.draws <= b) {
      ++g.draws;
      return;
    }
  }
  lo = INT64_MAX;
  int64_t hi = INT64_MIN;
  for (uint32_t i : idx) { lo = std::min<int64_t>(lo, int64_t(i) + base_vertex); hi = std::max<int64_t>(hi, int64_t(i) + base_vertex); }
  if (lo < 0 || hi - lo > 1'000'000) { ++g.skipped; ++g.skip_reason[3]; return; }
  vcount = uint32_t(hi - lo + 1);
  icount = uint32_t(idx.size());
  }
  const double t_indexed = now_ms();
  g.sect[1] += t_indexed - t_parsed;

  // Sources per fetch slot / stream. The shader path reads the device's vertex
  // fetch constants (device+1152 + slot*8: physical address | type, size |
  // endian); fetch constants hold physical addresses, which the CPU sees at
  // 0xA0000000+. The declaration path uses SetStreamSource; UP draws read
  // up_data. Everything is decoded into one interleaved host stream.
  std::vector<uint16_t> keys;
  std::vector<Source> sources;
  for (const Element& el : els) {
    if (std::find(keys.begin(), keys.end(), el.stream) != keys.end()) continue;
    keys.push_back(el.stream);
    Source src;
    if (up_data) { src.addr = up_data; src.stride = up_stride; src.endian = 2 | 4; }
    else if (from_shader) {
      const uint32_t d0 = g32(dev + 1152 + el.stream * 8), d1 = g32(dev + 1152 + el.stream * 8 + 4);
      const uint32_t phys = d0 & 0x1FFFFFFCu;
      src.endian = d1 & 3;
      const auto st = vsh.strides.find(el.stream);
      src.stride = st != vsh.strides.end() ? st->second : 0;
      src.addr = cpu_view_of_physical(phys, (uint32_t(lo) + vcount) * src.stride);
    } else if (el.stream < 16 && g_streams[el.stream].vb) {
      const uint32_t vb = g_streams[el.stream].vb;
      src.addr = gpu_view(g32(vb + 24) & ~3u) + g_streams[el.stream].offset;  // 0xE0000000+: 4 KB on, as index buffers
      src.stride = g_streams[el.stream].stride;
      src.endian = g32(vb + 28) & 3;
    }
    if (!src.addr || !src.stride) { ++g.skipped; ++g.skip_reason[4]; return; }
    sources.push_back(src);
  }
  std::vector<HostFmt> hfmts;
  std::vector<uint32_t> hoffsets;
  uint32_t host_stride = 0;
  for (const Element& el : els) {
    hfmts.push_back(host_format(el));
    hoffsets.push_back(host_stride);
    host_stride += hfmts.back().size;
  }
  // Textures first (their uploads may flush the ring), then room for this
  // draw's vertices, indices and constants in one go.
  for (uint32_t s = 0; s < 16; ++s) texture_slot(g_textures[s], true);
  for (uint32_t s = 0; s < 16; ++s)
    if (texture_pending(g_textures[s])) { ++g.skipped; ++g.skip_reason[6]; return; }
  ensure_room(uint64_t(host_stride) * vcount + uint64_t(icount) * 4 + 16384);
  // Static geometry is decoded once and kept in a GPU buffer, found again by
  // the draw's layout and source range and checked against an xxHash of the
  // guest bytes (decoding every draw every frame cost ~47 ms a frame in Dam).
  // Data that keeps changing, and UP draws, go through the upload ring.
  const uint64_t vbytes = uint64_t(host_stride) * vcount;
  uint64_t layout = XXH3_64bits(sources.data(), sources.size() * sizeof(Source));
  for (const Element& el : els)  // field by field: Element has a padding byte
    layout = layout * 0x100000001B3ull ^ (uint64_t(el.stream) | uint64_t(el.offset) << 16 | uint64_t(el.type) << 32) ^
             (uint64_t(el.usage) << 8 | el.index | uint64_t(uint8_t(el.exp)) << 16 | uint64_t(el.swizzle) << 24) *
                 0xC2B2AE3D27D4EB4Full;
  layout ^= (uint64_t(lo) << 32 | vcount) * 0x9E3779B97F4A7C15ull;
  Native::VertexCacheEntry* entry = up_data ? nullptr : &g.vertex_cache[layout];
  uint64_t content = 0;
  const double t_h = now_ms();
  const bool hashed = !entry || hash_sources_seh(sources.data(), sources.size(), uint32_t(lo), vcount, &content);
  g.hash_ms += now_ms() - t_h;
  if (!hashed) {
    ++g.skipped; ++g.skip_reason[4]; return;
  }
  RenderBufferReference vref;
  const bool hit = !tracing() && entry && entry->buffer && entry->content == content;
  if (entry) entry->last_frame = g.frames;
  if (hit) ++g.vcache_hits; else ++g.vcache_misses;
  const uint64_t voff = hit ? 0 : upload_alloc(vbytes, 16);
  if (voff == UINT64_MAX) return;
  const double t_vd = now_ms();
  const bool decoded = hit || decode_vertices(els.data(), els.size(), sources.data(), keys.data(), keys.size(),
                                              uint32_t(lo), vcount, g.upload_ptr + voff, host_stride, hfmts.data(),
                                              hoffsets.data());
  g.vdecode_ms += now_ms() - t_vd;
  if (!decoded) {
    static int logged = 0;
    if (logged++ < 8)
      REXKRNL_WARN("GENATIVE vertex read faulted: {} sources, first {:08X} stride {} endian {}, vertices {}..{} ({})",
                   sources.size(), sources[0].addr, sources[0].stride, sources[0].endian, lo, lo + vcount - 1,
                   up_data ? "UP" : from_shader ? "fetch" : "decl");
    ++g.skipped; ++g.skip_reason[4];
    return;
  }
  if (hit) {
    vref = RenderBufferReference(entry->buffer.buffer, entry->buffer.offset);
  } else if (entry && (!entry->buffer || entry->misses < 4)) {
    if (entry->buffer) ++entry->misses;
    if (!entry->buffer || entry->capacity < vbytes) {
      retire(g.vertex_pool, entry->buffer);
      entry->buffer = pool_alloc(g.vertex_pool, vbytes);
      entry->capacity = class_bytes(entry->buffer.size_class);
    }
    g.cmd->barriers(RenderBarrierStage::COPY, RenderBufferBarrier(entry->buffer.buffer, RenderBufferAccess::WRITE));
    g.cmd->copyBufferRegion(RenderBufferReference(entry->buffer.buffer, entry->buffer.offset),
                            RenderBufferReference(g.upload.get(), voff), vbytes);
    g.cmd->barriers(RenderBarrierStage::GRAPHICS, RenderBufferBarrier(entry->buffer.buffer, RenderBufferAccess::READ));
    entry->content = content;
    vref = RenderBufferReference(entry->buffer.buffer, entry->buffer.offset);
  } else {
    vref = RenderBufferReference(g.upload.get(), voff);
  }
  const double t_decoded = now_ms();
  g.sect[2] += t_decoded - t_indexed;
  std::vector<RenderInputSlot> slots{RenderInputSlot(0, host_stride)};
  std::vector<RenderVertexBufferView> views{RenderVertexBufferView(vref, uint32_t(vbytes))};
  RenderBufferReference iref;
  if (ihit) {
    iref = RenderBufferReference(ientry->buffer.buffer, ientry->buffer.offset);
  } else {
    const uint64_t ibytes = uint64_t(icount) * (vcount < 0xFFFF ? 2 : 4);
    const uint64_t ioff = upload_alloc(ibytes, 16);
    if (ioff == UINT64_MAX) return;
    if (vcount < 0xFFFF) {  // rebased indices fit 16 bits: half the size and bandwidth
      uint16_t* id = reinterpret_cast<uint16_t*>(g.upload_ptr + ioff);
      for (size_t i = 0; i < idx.size(); ++i) id[i] = uint16_t(int64_t(idx[i]) + base_vertex - lo);
    } else {
      uint32_t* id = reinterpret_cast<uint32_t*>(g.upload_ptr + ioff);
      for (size_t i = 0; i < idx.size(); ++i) id[i] = uint32_t(int64_t(idx[i]) + base_vertex - lo);
    }
    if (ientry && (!ientry->buffer || ientry->misses < 4)) {
      if (ientry->buffer) ++ientry->misses;
      if (!ientry->buffer || ientry->capacity < ibytes) {
        retire(g.index_pool, ientry->buffer);
        ientry->buffer = pool_alloc(g.index_pool, ibytes);
        ientry->capacity = class_bytes(ientry->buffer.size_class);
      }
      g.cmd->barriers(RenderBarrierStage::COPY, RenderBufferBarrier(ientry->buffer.buffer, RenderBufferAccess::WRITE));
      g.cmd->copyBufferRegion(RenderBufferReference(ientry->buffer.buffer, ientry->buffer.offset),
                              RenderBufferReference(g.upload.get(), ioff), ibytes);
      g.cmd->barriers(RenderBarrierStage::GRAPHICS, RenderBufferBarrier(ientry->buffer.buffer, RenderBufferAccess::READ));
      ientry->content = icontent; ientry->lo = lo; ientry->vcount = vcount; ientry->count = icount; ientry->topo = topo;
      iref = RenderBufferReference(ientry->buffer.buffer, ientry->buffer.offset);
    } else {
      iref = RenderBufferReference(g.upload.get(), ioff);
    }
  }

  g_spec_bits = 2;
  for (const Element& el : els) {
    uint32_t cb = 4;
    (void)cb;
    if ((el.usage == 3 || el.usage == 6 || el.usage == 7) && ((el.type & 0x3F) == 16 || (el.type & 0x3F) == 17))
      g_spec_bits |= 1;
  }
  g_draw_state = read_state(dev);
  bind_target();
  g.draws_since_resolve = true;
  if (g.bound_scene && g_draw_state.z_enable && g_draw_state.z_write) {
    depth_will_change();
    g.depth_written = true;
    ++g.depth_gen;
  }
  RenderPipeline* pipe = pipeline_for(vs, ps, from_shader ? vs_obj : decl, els, topo, slots);
  if (!pipe) { ++g.skipped; ++g.skip_reason[5]; return; }
  g_vs_cb_bytes = g.shaders[vs_obj].cb_bytes;
  g_ps_cb_bytes = g.shaders[g32(dev + kDevPixelShader)].cb_bytes;
  set_common_state(dev);
  if (tracing()) {
    const DrawState& st = g_draw_state;
    std::string tex;
    for (uint32_t t = 0; t < 8; ++t)
      if (g_textures[t]) tex += fmt::format(" s{}={:08X}->{} sign {:02X} fmt {}", t, g_textures[t], texture_slot(g_textures[t]), (g32(dev + 1152 + t * 24) >> 2) & 0xFF, g32(dev + 1152 + t * 24 + 4) & 0x3F);
    NTRACE("draw#{} prim {} n {} verts {} vs {:016X} ps {:016X} z {}{} f{} cull {} blend {} {}/{}/{} mask {:X} at {} {}{} "
           "| rt {:08X} depth {:08X} {}",
           g.draws, prim, count, vcount, g.shaders[vs_obj].hash, g.shaders[g32(dev + kDevPixelShader)].hash,
           st.z_enable, st.z_write ? "w" : "", st.z_func, st.cull, st.blend, st.src, st.dst, st.op, st.write_mask,
           st.alpha_test, st.alpha_func, tex, g.guest_rt, g.guest_depth, g.bound_scene ? "scene" : "single-sample");
    float vp[6];
    for (int i = 0; i < 6; ++i) { const uint32_t u = g32(dev + kDevViewport + i * 4); std::memcpy(&vp[i], &u, 4); }
    const float* p0 = reinterpret_cast<const float*>(g.upload_ptr + voff);
    NTRACE("  vte {:X} viewport {} {} {} {} {} {} pos0 {} {} {} {} (el0 usage {} fmt {})", g32(dev + 10572), vp[0], vp[1],
           vp[2], vp[3], vp[4], vp[5], p0[0], p0[1], p0[2], p0[3], els[0].usage, els[0].type & 0x3F);
    std::string e;
    for (size_t i = 0; i < els.size() * std::min<size_t>(vcount, 4); ++i) {
      const float* v = p0 + i * 4;
      if (i % els.size() == 0 && i) e += " |";
      const Element& el = els[i % els.size()];
      e += fmt::format(" [u{}.{} t{:X} exp{} off{} = {:.3f} {:.3f} {:.3f} {:.3f}]", el.usage, el.index,
                       el.type, int(el.exp), el.offset, v[0], v[1], v[2], v[3]);
    }
    NTRACE("  elements{} {}", e, from_shader ? "fetch" : "decl");
    std::string raw;
    for (uint32_t i = 0; !sources.empty() && i < 8; ++i)
      raw += fmt::format(" {:08X}", g32(sources[0].addr + uint32_t(lo) * sources[0].stride + i * 4));
    NTRACE("  source0 {:08X} stride {} endian {} {} first vertex:{}", sources[0].addr, sources[0].stride,
           sources[0].endian, up_data ? "UP" : "", raw);
    // Stretched triangles (one edge far longer than the draw's usual edge):
    // a vertex out of place in the data, e.g. a spike across the sky.
    size_t pe = els.size();
    for (size_t e = 0; e < els.size(); ++e)
      if (els[e].usage == 0 && hfmts[e].size >= 12 && pe == els.size()) pe = e;
    // Triangles hidden by gpu_native_hide, with their vertices' guest addresses.
    if (pe < els.size() && !sources.empty())
      for (size_t t = 0; t + 2 < hidden_tris.size(); t += 3) {
        uint32_t src_addr = 0, src_stride = 0;
        for (size_t k = 0; k < keys.size(); ++k)
          if (keys[k] == els[pe].stream) { src_addr = sources[k].addr; src_stride = sources[k].stride; }
        std::string vs;
        for (int k = 0; k < 3; ++k) {
          const uint32_t gv = uint32_t(int64_t(hidden_tris[t + k]) + base_vertex);
          const uint32_t ga = src_addr ? src_addr + gv * src_stride + els[pe].offset : 0;
          vs += fmt::format(" v{} at {:08X}: {:08X} {:08X} {:08X}", gv, ga, ga ? g32(ga) : 0, ga ? g32(ga + 4) : 0,
                            ga ? g32(ga + 8) : 0);
        }
        NTRACE("  HIDDEN tri of draw#{}:{}", g.draws, vs);
      }
    if (pe < els.size() && topo == RenderPrimitiveTopology::TRIANGLE_LIST && idx.size() >= 3) {
      auto pos = [&](uint32_t i) {
        const float* f = reinterpret_cast<const float*>(g.upload_ptr + voff +
                                                        uint64_t(int64_t(i) + base_vertex - lo) * host_stride +
                                                        hoffsets[pe]);
        return std::array<float, 3>{f[0], f[1], f[2]};
      };
      auto dist = [](const std::array<float, 3>& a, const std::array<float, 3>& b) {
        return std::sqrt((a[0] - b[0]) * (a[0] - b[0]) + (a[1] - b[1]) * (a[1] - b[1]) + (a[2] - b[2]) * (a[2] - b[2]));
      };
      std::vector<float> edges;
      for (size_t t = 0; t + 2 < idx.size(); t += 3)
        for (int k = 0; k < 3; ++k) edges.push_back(dist(pos(idx[t + k]), pos(idx[t + (k + 1) % 3])));
      std::vector<float> sorted = edges;
      std::nth_element(sorted.begin(), sorted.begin() + sorted.size() / 2, sorted.end());
      const float median = std::max(sorted[sorted.size() / 2], 1e-3f);
      int reported = 0;
      uint32_t src_addr = 0, src_stride = 0;
      for (size_t k = 0; k < keys.size(); ++k)
        if (keys[k] == els[pe].stream) { src_addr = sources[k].addr; src_stride = sources[k].stride; }
      for (size_t t = 0; t + 2 < idx.size() && reported < 8; t += 3) {
        const float m = std::max({edges[t], edges[t + 1], edges[t + 2]});
        if (m < median * 8.0f) continue;
        ++reported;
        std::string vs;
        for (int k = 0; k < 3; ++k) {
          const auto q = pos(idx[t + k]);
          const uint32_t gv = uint32_t(int64_t(idx[t + k]) + base_vertex);
          const uint32_t ga = src_addr ? src_addr + gv * src_stride + els[pe].offset : 0;
          vs += fmt::format(" v{} ({:.1f} {:.1f} {:.1f}) at {:08X}: {:08X} {:08X} {:08X}", gv, q[0], q[1], q[2], ga,
                            ga ? g32(ga) : 0, ga ? g32(ga + 4) : 0, ga ? g32(ga + 8) : 0);
        }
        NTRACE("  STRETCHED tri {} of draw#{}: longest edge {:.1f} vs median {:.1f}, fmt {:X}:{}", t / 3, g.draws, m,
               median, els[pe].type, vs);
      }
    }
  }
  g.cmd->setPipeline(pipe);
  g.cmd->setVertexBuffers(0, views.data(), uint32_t(views.size()), slots.data());
  const bool narrow = vcount < 0xFFFF;
  RenderIndexBufferView ib(iref, icount * (narrow ? 2 : 4), narrow ? RenderFormat::R16_UINT : RenderFormat::R32_UINT);
  g.cmd->setIndexBuffer(&ib);
  g.cmd->drawIndexedInstanced(icount, 1, 0, 0, 0);
  if (g.prof_active)
    prof_mark(fmt::format("draw#{} prim {} idx {} verts {} vs {:016X} ps {:016X} blend {}", g.draws, prim, icount, vcount,
                          g.shaders[vs_obj].hash, g.shaders[g32(dev + kDevPixelShader)].hash, g_draw_state.blend));
  ++g.draws;
}

// Scene -> a single-sampled texture of the same size and format: a copy, or
// a resolve (sample average) when the scene is multisampled.
void copy_scene_to(RenderTexture* dst) {
  if (g.msaa.sampleCount == RenderSampleCount::COUNT_1) {
    g.cmd->barriers(RenderBarrierStage::COPY, {RenderTextureBarrier(g.scene.get(), RenderTextureLayout::COPY_SOURCE),
                                               RenderTextureBarrier(dst, RenderTextureLayout::COPY_DEST)});
    g.cmd->copyTexture(dst, g.scene.get());
  } else {
    g.cmd->barriers(RenderBarrierStage::COPY, {RenderTextureBarrier(g.scene.get(), RenderTextureLayout::RESOLVE_SOURCE),
                                               RenderTextureBarrier(dst, RenderTextureLayout::RESOLVE_DEST)});
    g.cmd->resolveTexture(dst, g.scene.get());
  }
}

// Paced presents (gpu_native_pace): like the SDK presenter's pacing, each frame
// is presented at a fixed moment instead of whenever the game thread gets to
// Swap. Without a GPU plugin there is no guest vblank tick to anchor to, so the
// moments run on the game's own frame period (smoothed from Swap arrivals) and
// sit gpu_native_pace_margin_us after the typical arrival; a slow phase lock
// keeps them there. Frames arriving after their moment present at once (and
// the schedule re-anchors after a real hitch).
void precise_wait_until(double target_ms) {
  static HANDLE timer = CreateWaitableTimerExW(nullptr, nullptr, 0x00000002 /* HIGH_RESOLUTION */, TIMER_ALL_ACCESS);
  const double ahead = 0.7;
  const double now = now_ms();
  if (timer && target_ms - now > ahead) {
    LARGE_INTEGER due;
    due.QuadPart = -int64_t((target_ms - now - ahead) * 10000.0);
    if (SetWaitableTimer(timer, &due, 0, nullptr, nullptr, FALSE)) WaitForSingleObject(timer, INFINITE);
  }
  while (now_ms() < target_ms) std::this_thread::yield();
}

void pace_present() {
  const double now = now_ms();
  if (g.pace_last_arrival > 0) {
    const double iv = now - g.pace_last_arrival;
    if (iv > 12.0 && iv < 22.0) g.pace_period += (iv - g.pace_period) * 0.01;
  }
  g.pace_last_arrival = now;
  if (!REXCVAR_GET(gpu_native_pace)) { g.pace_waited_ms = 0; return; }
  const double margin = REXCVAR_GET(gpu_native_pace_margin_us) / 1000.0;
  double target = g.pace_next;
  if (target <= 0 || now > target + g.pace_period * 0.5 || target - now > g.pace_period * 1.5) {
    target = now + margin;  // (re)anchor
  } else {
    target += (now + margin - target) * 0.02;  // keep the moment `margin` after the usual arrival
  }
  g.pace_waited_ms = std::max(0.0, target - now);
  if (target > now) precise_wait_until(target);
  g.pace_next = std::max(target, now) + g.pace_period;
}

// Present-to-present intervals and the game's sim steps per frame, for the
// frame stats line (double steps as in the 0.9.6c "ge frames" line).
void note_present() {
  const double now = now_ms();
  g_last_present_ms.store(now);
  g_present_count.fetch_add(1);
  if (g.pace_last_present > 0) g.present_intervals.push_back(now - g.pace_last_present);
  g.pace_last_present = now;
  g.pace_wait_sum += g.pace_waited_ms;
  const uint32_t counter = g32(0x8308851Cu);
  if (g.last_counter && counter - g.last_counter >= 2 && counter - g.last_counter < 4 && !g.present_intervals.empty() &&
      g.present_intervals.back() < 20.0)
    ++g.double_steps;
  g.last_counter = counter;
}

// Hidden warm-up of the other graphics mode. The first switch (F / RB) in a
// level drew the other mode's models and textures for the first time, all in
// one frame (a 30-65 ms hitch). Once per stage, when it first draws a real
// scene, two frames are drawn in the other mode and not shown, so that work is
// done while the level starts. The game copies its graphics setting into the
// render flag every frame (sub_82099708, from the main loop); those two frames
// the copy is inverted afterwards.
struct ModeWarm {
  uint32_t stage = 0;
  bool done = true;
  int left = 0;  // frames still to draw in the other mode (and hide)
} g_mode_warm;
constexpr uint32_t kRenderModeFlag = 0x82F0DF11u;  // 1 = classic graphics
constexpr uint32_t kFrontEndStage = 90u;
constexpr int kWarmFrames = 2;
constexpr uint32_t kWarmMinDraws = 100;  // a level scene, not a loading picture

void present_frame() {
  if (!ensure_device()) return;
  const bool hide = g_mode_warm.left > 0;
  if (REXCVAR_GET(gpu_native_trace_frame) == -1)  // the test pipe's TRACE: the next frame
    rex::cvar::SetFlagByName("gpu_native_trace_frame", std::to_string(g.frames + 2));
  if (const int32_t pf = REXCVAR_GET(gpu_native_profile_frame); pf < 0)  // test: -n = the frame n from now
    rex::cvar::SetFlagByName("gpu_native_profile_frame", std::to_string(g.frames + 1 - pf));
  begin_frame();
  const int32_t debug = REXCVAR_GET(gpu_native_debug_clear);
  if (g.draws == 0 && debug) {
    const uint32_t c = uint32_t(debug);
    g.cmd->clearColor(0, RenderColor(((c >> 16) & 0xFF) / 255.0f, ((c >> 8) & 0xFF) / 255.0f, (c & 0xFF) / 255.0f, 1.0f));
  }
  uint32_t index = 0;
  bool have_image = false;
  uint32_t vk_frame = UINT32_MAX;  // Vulkan presenter: the shared frame written
  RenderCommandSemaphore* signal = nullptr;
  const double p0 = now_ms();
  double p1 = p0, p2 = p0, p3 = p0;
  if (g.out.vk) {
    // The presenter's device reads the shared frame once its GPU work is done:
    // a worker waits for that (present_to_presenter_vk), not the game thread.
    vk_frame = hide ? UINT32_MAX : copy_to_presenter_vk();
    have_image = vk_frame != UINT32_MAX;
    if (g.timestamps) g.cmd->writeTimestamp(g.timestamps.get(), g.slot * 2 + 1);
    g.cmd->end();
    g.recording = false;
    p1 = p2 = now_ms();
    submit_frame_lists(nullptr, 0, nullptr, 0);
    p3 = now_ms();
  } else if (g.out.presenter) {
    const uint32_t k = g.out.next;
    have_image = !hide && copy_to_presenter_texture(k);
    if (g.timestamps) g.cmd->writeTimestamp(g.timestamps.get(), g.slot * 2 + 1);
    g.cmd->end();
    g.recording = false;
    p1 = p2 = now_ms();
    ID3D12CommandQueue* q = static_cast<D3D12CommandQueue*>(g.queue.get())->d3d;
    if (have_image && g.out.tex_done[k]) q->Wait(g.out.done, g.out.tex_done[k]);  // the presenter has read it
    submit_frame_lists(nullptr, 0, nullptr, 0);
    if (have_image) {
      q->Signal(g.out.ready, ++g.out.ready_value);
      g.out.tex_ready[k] = g.out.ready_value;
    }
    p3 = now_ms();
  } else {
  if (g.swapchain->needsResize()) {
    for (uint32_t i = 0; i < kFrames; ++i)
      if (g.pending[i]) { g.queue->waitForCommandFence(g.fences[i].get()); g.pending[i] = false; }
    g.swap_framebuffers.clear();
    if (g.swapchain->resize()) create_swap_framebuffers();
  }
  have_image = g.swapchain->acquireTexture(g.acquire.get(), &index);
  p1 = now_ms();
  RenderTexture* target = have_image ? g.swapchain->getTexture(index) : nullptr;
  const auto front = g.resolved.find(g.front_buffer);
  if (target && g.swapchain->getWidth() == g_scene_w && g.swapchain->getHeight() == g_scene_h &&
      front != g.resolved.end() && front->second.texture) {
    // The texture Swap was given: what the game resolved its final picture to.
    RenderTexture* src = front->second.texture.get();
    g.cmd->barriers(RenderBarrierStage::COPY, {RenderTextureBarrier(src, RenderTextureLayout::COPY_SOURCE),
                                               RenderTextureBarrier(target, RenderTextureLayout::COPY_DEST)});
    g.cmd->copyTexture(target, src);
    g.cmd->barriers(RenderBarrierStage::GRAPHICS, RenderTextureBarrier(src, RenderTextureLayout::SHADER_READ));
    prof_mark("present copy");
  } else if (target && g.swapchain->getWidth() == g_scene_w && g.swapchain->getHeight() == g_scene_h) {
    copy_scene_to(target);
    prof_mark("present copy");
  } else if (target) {
    g.cmd->barriers(RenderBarrierStage::GRAPHICS, RenderTextureBarrier(target, RenderTextureLayout::COLOR_WRITE));
    g.cmd->setFramebuffer(g.swap_framebuffers[index].get());
    g.cmd->clearColor(0, RenderColor(0.3f, 0.0f, 0.3f, 1.0f));  // window not 1280x720: scaling comes later
  }
  if (target) g.cmd->barriers(RenderBarrierStage::NONE, RenderTextureBarrier(target, RenderTextureLayout::PRESENT));
  if (g.timestamps) g.cmd->writeTimestamp(g.timestamps.get(), g.slot * 2 + 1);
  g.cmd->end();
  g.recording = false;
  while (g.release.size() < g.swapchain->getTextureCount()) g.release.emplace_back(g.device->createCommandSemaphore());
  RenderCommandSemaphore* wait = g.acquire.get();
  signal = have_image ? g.release[index].get() : nullptr;
  p2 = now_ms();
  submit_frame_lists(have_image ? &wait : nullptr, have_image ? 1 : 0, signal ? &signal : nullptr, signal ? 1 : 0);
  p3 = now_ms();
  }
  g.pending[g.slot] = true;
  if (g.prof_active) {  // wait for this frame now and log where its GPU time went
    g.queue->waitForCommandFence(g.fence);
    g.pending[g.slot] = false;
    g.prof_pool->queryResults();
    const uint64_t* ts = g.prof_pool->getResults();
    std::vector<std::pair<double, size_t>> ops;
    std::unordered_map<std::string, std::pair<double, int>> kinds;
    for (size_t i = 1; i < g.prof_labels.size(); ++i) {
      const double ms = ts[i] > ts[i - 1] ? double(ts[i] - ts[i - 1]) / 1e6 : 0.0;
      ops.emplace_back(ms, i);
      const std::string& l = g.prof_labels[i];
      std::string kind = l.rfind("draw#", 0) == 0 ? l.substr(l.find(" vs ")) : l.substr(0, l.find(' '));
      kinds[kind].first += ms;
      ++kinds[kind].second;
    }
    const double total = g.prof_labels.size() > 1 ? double(ts[g.prof_labels.size() - 1] - ts[0]) / 1e6 : 0.0;
    REXKRNL_INFO("GENATIVE profile frame {}: {} operations, {:.3f} ms GPU", g.frames + 1, ops.size(), total);
    std::sort(ops.begin(), ops.end(), [](auto& a, auto& b) { return a.first > b.first; });
    for (size_t i = 0; i < ops.size() && i < 25; ++i)
      REXKRNL_INFO("GENATIVE profile op {:.3f} ms {}", ops[i].first, g.prof_labels[ops[i].second]);
    std::vector<std::pair<double, std::string>> ks;
    for (auto& [k, v] : kinds) ks.emplace_back(v.first, fmt::format("{} x{}", k, v.second));
    std::sort(ks.begin(), ks.end(), [](auto& a, auto& b) { return a.first > b.first; });
    for (size_t i = 0; i < ks.size() && i < 20; ++i)
      REXKRNL_INFO("GENATIVE profile kind {:.3f} ms {}", ks[i].first, ks[i].second);
    g.prof_active = false;
  }
  if (have_image) {
    pace_present();
    if (g.out.vk) {
      present_to_presenter_vk(vk_frame);  // (reports the vblank itself, once handed over)
    } else if (g.out.presenter) {
      present_to_presenter(g.out.next);
      g.out.next = (g.out.next + 1) % 3;
      // The presenter shows a refreshed frame on its pacing thread at "guest
      // vblank + present_pacing_offset_us"; without the Xenos vsync worker
      // nothing reported vblanks, so frames only reached the screen when
      // something else repainted the window (the black main menu). This frame
      // is already paced: report a vblank that puts its moment just after now.
      report_frame_to_presenter(g.out.presenter, std::chrono::steady_clock::now(), 2000);
    } else {
      g.swapchain->present(index, &signal, 1);
    }
    note_present();
  }
  // Next frame records into the next slot once the GPU is done with the frame
  // that used it last (kFrames frames ago): the game thread doesn't wait for
  // this frame's GPU work, nor for one GPU clock stall.
  g.slot = (g.slot + 1) % kFrames;
  g.cmd = g.cmds[g.slot].get();
  g.fence = g.fences[g.slot].get();
  const bool finished = g.pending[g.slot];
  // (Vulkan: the worker is done with that frame's fence before it is reset.)
  if (g.out.vk && finished) ge::native_vk::WaitPresent(g.out.vk_handoff[g.slot]);
  const double p4 = now_ms();
  if (finished) { g.queue->waitForCommandFence(g.fence); g.pending[g.slot] = false; }
  g.present_ms[0] = p1 - p0; g.present_ms[1] = p3 - p2; g.present_ms[2] = p4 - p3; g.present_ms[3] = now_ms() - p4;
  g.const_used = 0;
  g.const_dedup.clear();
  drain_debug_messages("frame");
  // GPU time of that finished frame's work (submitted at once, so no CPU gaps
  // inside), logged like the Xenos backend's "ge gpu time" line every 600 frames.
  if (g.timestamps && finished) {
    g.timestamps->queryResults();
    const uint64_t* ts = g.timestamps->getResults() + g.slot * 2;  // nanoseconds
    const double ms = ts[1] > ts[0] ? double(ts[1] - ts[0]) / 1e6 : 0.0;
    const uint64_t copy_start = g.timestamps->getResults()[2 * kFrames + g.slot];  // constants copy, just before the frame
    g.copy_ms_sum += ts[0] > copy_start && ts[0] - copy_start < 100000000ull ? double(ts[0] - copy_start) / 1e6 : 0.0;
    g.gpu_ms_sum += ms;
    g.gpu_ms_max = std::max(g.gpu_ms_max, ms);
    if (++g.gpu_frames >= 600) {
      REXKRNL_INFO("GENATIVE gpu time per frame: {:.2f} ms (max {:.2f}) + constants copy {:.2f} ms",
                   g.gpu_ms_sum / g.gpu_frames, g.gpu_ms_max, g.copy_ms_sum / g.gpu_frames);
      g.gpu_ms_sum = g.gpu_ms_max = g.copy_ms_sum = 0;
      g.gpu_frames = 0;
    }
  }
  g.upload_used = 0;
  for (int i = 0; i < 5; ++i) { g.last_sect[i] = g.sect[i]; g.sect[i] = 0; }
  g.last_vdecode_ms = g.vdecode_ms; g.vdecode_ms = 0;
  g.last_flush_ms = g.flush_ms; g.last_alloc_ms = g.alloc_ms; g.last_hash_ms = g.hash_ms; g.flush_ms = g.alloc_ms = g.hash_ms = 0; g.last_flushes = g.flushes; g.flushes = 0;
  g.last_tex_decode_ms = g.tex_decode_ms; g.last_tex_mip_ms = g.tex_mip_ms; g.tex_decode_ms = g.tex_mip_ms = 0;
  g.last_vcache_hits = g.vcache_hits; g.last_vcache_misses = g.vcache_misses; g.vcache_hits = g.vcache_misses = 0;
  // Buffers dropped two or more frames ago are no longer used by the GPU.
  std::erase_if(g.retired_textures, [](const auto& r) {
    if (r.frame + kFrames > g.frames) return false;
    if (r.slot) g.free_slots.push_back(r.slot);
    return true;
  });
  std::erase_if(g.retired_sets, [](const auto& r) { return r.frame + kFrames <= g.frames; });
  std::erase_if(g.retired, [](const auto& r) {
    if (r.frame + kFrames > g.frames) return false;
    r.pool->free_lists[r.alloc.size_class].push_back(r.alloc);
    return true;
  });
  // Converted geometry stays while the stage lasts (dropped at a stage change,
  // begin_frame): each graphics mode has its own models, and dropping the other
  // mode's after 10 s unused made a switch after a while convert them all again
  // in one frame (a stutter). Past a budget, geometry unused for 10 s goes.
  if ((g.frames % 600) == 0) {
    uint64_t bytes = 0;
    for (const auto& [key, e] : g.vertex_cache) bytes += e.capacity;
    for (const auto& [key, e] : g.index_cache) bytes += e.capacity;
    const bool over = bytes > kGeometryBudget;
    if (over) REXKRNL_INFO("GENATIVE geometry cache {} MB over budget: unused entries released", bytes >> 20);
    else if (REXCVAR_GET(gpu_native_frame_stats) > 0)
      REXKRNL_INFO("GENATIVE geometry cache {} MB ({} vertex, {} index buffers)", bytes >> 20, g.vertex_cache.size(),
                   g.index_cache.size());
    for (auto it = g.vertex_cache.begin(); it != g.vertex_cache.end();) {
      if (!over || g.frames - it->second.last_frame <= 600) { ++it; continue; }
      retire(g.vertex_pool, it->second.buffer);
      it = g.vertex_cache.erase(it);
    }
    for (auto it = g.index_cache.begin(); it != g.index_cache.end();) {
      if (!over || g.frames - it->second.last_frame <= 600) { ++it; continue; }
      retire(g.index_pool, it->second.buffer);
      it = g.index_cache.erase(it);
    }
  }
  g.last_pipe_ms = g.pipe_ms; g.last_tex_ms = g.tex_ms; g.last_pipe_n = g.pipe_n; g.last_tex_n = g.tex_n;
  g.pipe_ms = g.tex_ms = 0; g.pipe_n = g.tex_n = 0; g.tex_requests = 0;
  if ((g.frames % 600) == 599) {
    auto used = [](const BufferPool& p) {
      uint64_t free_bytes = 0;
      for (uint32_t c = 0; c < kSizeClasses; ++c) free_bytes += p.free_lists[c].size() * class_bytes(c);
      return fmt::format("{} chunks, {:.1f} MB free-listed, {:.1f} MB bump left", p.chunks.size(), free_bytes / 1048576.0,
                         p.bump == UINT64_MAX ? 0.0 : (p.chunk - std::min(p.bump, p.chunk)) / 1048576.0);
    };
    REXKRNL_INFO("GENATIVE pools: vertex {} ({} entries); index {} ({} entries); constants {:.1f} MB last frame",
                 used(g.vertex_pool), g.vertex_cache.size(), used(g.index_pool), g.index_cache.size(), g.const_used / 1048576.0);
  }
  if (++g.frames == 1 || (g.frames % 600) == 0)
    REXKRNL_INFO("GENATIVE frame {}: {} draws, {} skipped (shader {} decl {} prim {} range {} stream {} pipeline {}), "
                 "{} depth copies in all",
                 g.frames, g.draws, g.skipped, g.skip_reason[0], g.skip_reason[1], g.skip_reason[2],
                 g.skip_reason[3], g.skip_reason[4], g.skip_reason[5], g.depth_copies);
}

// Submits what has been recorded this frame, waits, and carries on with an
// empty upload ring (level loads upload far more texture data than fits).
void flush_mid_frame() {
  if (!g.recording) { g.upload_used = 0; return; }
  const double t0 = now_ms();
  struct T { double t0; ~T() { g.flush_ms += now_ms() - t0; } } timer{t0};
  g.cmd->end();
  submit_frame_lists(nullptr, 0, nullptr, 0);
  g.queue->waitForCommandFence(g.fence);
  g.upload_used = 0;
  g.const_used = 0;
  g.const_dedup.clear();
  g.cmd->begin();
  g.layout_bound = false;
  g.bound_fb = nullptr;
  bind_target();
  ++g.flushes;
}

// Depth resolve (D3DRESOLVE_DEPTHSTENCIL, flag 0x4): the scene's depth into an
// R32F texture the game samples (the ZSprite pass reads it back as depth).
// Sample 0 of each pixel, as the game asks for one fragment. While nothing has
// written depth since its last clear, a 1x1 texture of the clear value is
// enough (and cheaper); otherwise a compute pass copies the whole target.
constexpr char kDepthCopyShader[] = R"(
#ifdef MSAA
Texture2DMS<float> src : register(t0);
#else
Texture2D<float> src : register(t0);
#endif
RWTexture2D<float> dst : register(u1);
[numthreads(8, 8, 1)] void main(uint3 id : SV_DispatchThreadID) {
  uint w, h;
  dst.GetDimensions(w, h);
  if (id.x >= w || id.y >= h) return;
#ifdef MSAA
  dst[id.xy] = src.Load(int2(id.xy), 0);
#else
  dst[id.xy] = src.Load(int3(id.xy, 0));
#endif
}
)";

bool ensure_depth_pass() {
  if (g.depth_pipe) return true;
  if (g.depth_pass_failed) return false;
  g.depth_pass_failed = true;  // until it is up
  std::string source = kDepthCopyShader;
  if (g.msaa.sampleCount > 1) source = "#define MSAA 1\n" + source;
  std::vector<uint8_t> dxil;
  std::string errors;
  if (!ge::native_shaders::CompileDxil(source, L"cs_6_0", dxil, errors)) {
    REXKRNL_WARN("GENATIVE depth copy shader: {}", errors.substr(0, 300));
    return false;
  }
  RenderDescriptorSetBuilder sb;
  sb.begin();
  sb.addTexture(0);
  sb.addReadWriteTexture(1);
  sb.end();
  RenderPipelineLayoutBuilder lb;
  lb.begin();
  lb.addDescriptorSet(sb);
  lb.end();
  g.depth_layout = lb.create(g.device.get());
  g.depth_shader = g.device->createShader(dxil.data(), dxil.size(), "main",
                                         g_vulkan ? RenderShaderFormat::SPIRV : RenderShaderFormat::DXIL);
  g.depth_view = g.scene_depth->createTextureView(
      RenderTextureViewDesc::Texture2D(g_vulkan ? RenderFormat::D32_FLOAT : RenderFormat::R32_FLOAT));
  g.depth_pipe = g.device->createComputePipeline(RenderComputePipelineDesc(g.depth_layout.get(), g.depth_shader.get(), 8, 8, 1));
  if (!g.depth_pipe) return false;
  g.depth_pass_failed = false;
  return true;
}

void native_depth_resolve(uint32_t dest) {
  ResolvedTexture& r = g.resolved[dest];
  if (!g.depth_written || !g_depth_copy) {  // every texel is the clear value
    const float value = g.depth_clear;
    r.depth_pending = false;
    if (r.texture && r.span == 1 && r.content == uint64_t(std::bit_cast<uint32_t>(value))) return;
    if (r.texture) g.retired_textures.push_back({g.frames, std::move(r.texture), r.slot});
    r.texture = g.device->createTexture(RenderTextureDesc::Texture2D(1, 1, 1, RenderFormat::R32_FLOAT));
    r.slot = alloc_texture_slot();
    r.span = 1;
    r.content = std::bit_cast<uint32_t>(value);
    ensure_room(512);
    const uint64_t off = upload_alloc(256, 512);
    if (off == UINT64_MAX) return;
    std::memcpy(g.upload_ptr + off, &value, 4);
    g.cmd->barriers(RenderBarrierStage::COPY, RenderTextureBarrier(r.texture.get(), RenderTextureLayout::COPY_DEST));
    g.cmd->copyTextureRegion(RenderTextureCopyLocation::Subresource(r.texture.get(), 0),
                             RenderTextureCopyLocation::PlacedFootprint(g.upload.get(), RenderFormat::R32_FLOAT, 1, 1, 1, 64, off));
    g.cmd->barriers(RenderBarrierStage::GRAPHICS, RenderTextureBarrier(r.texture.get(), RenderTextureLayout::SHADER_READ));
    g.textures->setTexture(r.slot, r.texture.get(), RenderTextureLayout::SHADER_READ);
    g.bound_fb = nullptr;
    bind_target();
    return;
  }
  if (!ensure_depth_pass()) return;
  if (r.texture && r.span == 0 && r.content == g.depth_gen) return;  // already holds this depth
  if (!r.texture || r.span != 0) {  // full size
    if (r.texture) g.retired_textures.push_back({g.frames, std::move(r.texture), r.slot});
    r.texture = g.device->createTexture(RenderTextureDesc::Texture2D(g_scene_w, g_scene_h, 1, RenderFormat::R32_FLOAT,
                                                                    RenderTextureFlag::UNORDERED_ACCESS));
    r.slot = alloc_texture_slot();
    r.span = 0;
    r.content = 0;
    RenderDescriptorSetBuilder sb;
    sb.begin();
    sb.addTexture(0);
    sb.addReadWriteTexture(1);
    sb.end();
    std::unique_ptr<RenderDescriptorSet>& set = g.depth_sets[dest];
    if (set) g.retired_sets.push_back({g.frames, std::move(set)});
    set = sb.create(g.device.get());
    set->setTexture(0, g.scene_depth.get(), RenderTextureLayout::SHADER_READ, g.depth_view.get());
    set->setTexture(1, r.texture.get(), RenderTextureLayout::GENERAL);
  }
  if (!r.depth_pending) {
    r.depth_pending = true;
    g.depth_pending.push_back(dest);
  }
}

// The copy of a recorded depth resolve: the scene depth as it is now.
void copy_pending_depth(uint32_t dest, ResolvedTexture& r) {
  r.depth_pending = false;
  std::erase(g.depth_pending, dest);
  if (r.content == g.depth_gen) return;
  g.cmd->barriers(RenderBarrierStage::COMPUTE, {RenderTextureBarrier(g.scene_depth.get(), RenderTextureLayout::SHADER_READ),
                                                RenderTextureBarrier(r.texture.get(), RenderTextureLayout::GENERAL)});
  g.cmd->setComputePipelineLayout(g.depth_layout.get());
  g.cmd->setPipeline(g.depth_pipe.get());
  g.cmd->setComputeDescriptorSet(g.depth_sets[dest].get(), 0);
  g.cmd->dispatch((g_scene_w + 7) / 8, (g_scene_h + 7) / 8, 1);
  r.content = g.depth_gen;
  ++g.depth_copies;
  g.cmd->barriers(RenderBarrierStage::GRAPHICS, {RenderTextureBarrier(g.scene_depth.get(), RenderTextureLayout::DEPTH_WRITE),
                                                 RenderTextureBarrier(r.texture.get(), RenderTextureLayout::SHADER_READ)});
  g.textures->setTexture(r.slot, r.texture.get(), RenderTextureLayout::SHADER_READ);
  g.bound_fb = nullptr;
  bind_target();
}

// Before a draw or clear changes the scene depth: recorded depth resolves are
// copied now if their texture has been sampled lately (so it gets the depth as
// it was at the resolve); the rest stay recorded and cost nothing unless a
// draw samples them later (then they get the depth of that moment).
void depth_will_change() {
  for (size_t i = 0; i < g.depth_pending.size();) {
    const uint32_t dest = g.depth_pending[i];
    ResolvedTexture& r = g.resolved[dest];
    if (r.depth_used_frame && r.depth_used_frame + 120 > g.frames + 1)
      copy_pending_depth(dest, r);  // erases it from the list
    else
      ++i;
  }
}

// Resolve: the bound target's colour into a texture keyed by the guest
// destination (depth: native_depth_resolve); the game resolves each EDRAM tile
// separately, so repeats with no draws in between are skipped. Flags (XDK):
// 0x4 depth/stencil, 0x8 all fragments, 0x10.. one fragment, 0x100 / 0x200
// clear colour / depth afterwards (the game clears explicitly as well).
void native_resolve(uint32_t dest, uint32_t flags) {
  if (!dest || !ensure_device() || REXCVAR_GET(gpu_native_skip_resolves)) return;
  if (flags & 0x4) {
    begin_frame();
    native_depth_resolve(dest);
    return;
  }
  begin_frame();
  bind_target();
  if (dest == g.last_resolve_dest && !g.draws_since_resolve) return;
  g.last_resolve_dest = dest;
  g.draws_since_resolve = false;
  ResolvedTexture& r = g.resolved[dest];
  if (!r.texture) {
    r.texture = g.device->createTexture(RenderTextureDesc::Texture2D(g_scene_w, g_scene_h, 1, kSceneFormat));
    r.slot = g.next_slot < kTextureSlots ? g.next_slot++ : 0;
    g.textures->setTexture(r.slot, r.texture.get(), RenderTextureLayout::SHADER_READ);
  }
  if (g.bound_scene) {
    copy_scene_to(r.texture.get());
  } else {
    g.cmd->barriers(RenderBarrierStage::COPY, {RenderTextureBarrier(g.bound_color, RenderTextureLayout::COPY_SOURCE),
                                               RenderTextureBarrier(r.texture.get(), RenderTextureLayout::COPY_DEST)});
    g.cmd->copyTexture(r.texture.get(), g.bound_color);
  }
  g.cmd->barriers(RenderBarrierStage::GRAPHICS, RenderTextureBarrier(r.texture.get(), RenderTextureLayout::SHADER_READ));
  g.bound_fb = nullptr;  // the source left COLOR_WRITE: bind it again
  bind_target();
  prof_mark(fmt::format("resolve {:08X}", dest));
}

}  // namespace

namespace ge { bool NativeRendererActive(); }  // ge_native_menu.cpp: fixed for the run
namespace ge {  // ge_hooks.cpp: GPU loss hook and crash log
void NativeReportDeviceLost() { report_device_lost(); }
// D3D12 breadcrumbs and page-fault data for the devices made from here on. Called
// before the presenter makes its device: the native renderer's D3D12CreateDevice
// on the same GPU gets that same device back.
void NativeEnableDred() {
  if (!REXCVAR_GET(gpu_native_dred)) return;
  ID3D12DeviceRemovedExtendedDataSettings* dred = nullptr;
  if (SUCCEEDED(D3D12GetDebugInterface(IID_PPV_ARGS(&dred)))) {
    dred->SetAutoBreadcrumbsEnablement(D3D12_DRED_ENABLEMENT_FORCED_ON);
    dred->SetPageFaultEnablement(D3D12_DRED_ENABLEMENT_FORCED_ON);
    dred->Release();
  }
}
bool NativeDeviceLost() { return g_watch_dev && g_watch_dev->GetDeviceRemovedReason() != S_OK; }
}  // namespace ge
#define NATIVE_ON() ge::NativeRendererActive()

// The game's frame limiter (native/native_hooks.toml): it spins until
// (now - last) * 60 reaches `limit` (1000000: 1/60 s in microseconds). Sleep
// through all but the last ~1.5 ms of that on a high-resolution timer; the
// game's own loop then waits out the rest, so its timing is unchanged.
void ge_native_frame_wait(PPCRegister& r3, PPCRegister& r31, PPCRegister& r23) {
  if (!NATIVE_ON()) return;
  const uint64_t elapsed = r3.u64 - r31.u64;
  const uint64_t frame_us = r23.u64 / 60;
  constexpr uint64_t kMarginUs = 1500;
  if (r3.u64 < r31.u64 || elapsed >= frame_us || frame_us - elapsed <= kMarginUs || frame_us > 100000) return;
  static thread_local HANDLE timer =
      CreateWaitableTimerExW(nullptr, nullptr, 0x00000002 /* HIGH_RESOLUTION */, TIMER_ALL_ACCESS);
  LARGE_INTEGER due;
  due.QuadPart = -int64_t(frame_us - elapsed - kMarginUs) * 10;  // 100 ns units
  if (timer && SetWaitableTimer(timer, &due, 0, nullptr, nullptr, FALSE)) WaitForSingleObject(timer, INFINITE);
}

// D3DDevice_SetGammaRamp(pDevice, Flags, pRamp): D3DGAMMARAMP, 256 big-endian
// WORDs per channel (R, G, B). GoldenEye sets a non-linear one after startup
// (Xenos: entry 64 -> 193 of 1023); the output pass applies it.
extern "C" REX_FUNC(sub_821955C8) {
  if (NATIVE_ON() && ctx.r5.u32) {
    for (uint32_t i = 0; i < 256 * 3; ++i) g.gamma_ramp[i] = uint16_t(g32(ctx.r5.u32 + (i & ~1u) * 2) >> ((i & 1) ? 0 : 16));
    ++g.gamma_ramp_version;
    REXKRNL_INFO("GENATIVE gamma ramp set: R 8:{:04X} 64:{:04X} 128:{:04X} 192:{:04X} 255:{:04X}", g.gamma_ramp[8],
                 g.gamma_ramp[64], g.gamma_ramp[128], g.gamma_ramp[192], g.gamma_ramp[255]);
  }
  __imp__sub_821955C8(ctx, base);
}

// D3DDevice_CreateVertexShader / CreatePixelShader(pFunction) -> shader object.
extern "C" REX_FUNC(sub_8218F930) {
  const uint32_t blob = ctx.r3.u32;
  __imp__sub_8218F930(ctx, base);
  if (NATIVE_ON() && ctx.r3.u32) {
    g.created_blobs[ctx.r3.u32] = blob;
    g.shaders.erase(ctx.r3.u32);
    request_translation(ctx.r3.u32, true);
  }
}
extern "C" REX_FUNC(sub_8218F820) {
  const uint32_t blob = ctx.r3.u32;
  __imp__sub_8218F820(ctx, base);
  if (NATIVE_ON() && ctx.r3.u32) {
    g.created_blobs[ctx.r3.u32] = blob;
    g.shaders.erase(ctx.r3.u32);
    request_translation(ctx.r3.u32, false);
  }
}
// In-place shader registration: forget any host shader cached for that object,
// and watch it for translating ahead (warm_registered_shaders).
extern "C" REX_FUNC(sub_8218DFF8) {
  const uint32_t object = ctx.r3.u32;
  if (NATIVE_ON()) { g.shaders.erase(object); g.created_blobs.erase(object); }
  __imp__sub_8218DFF8(ctx, base);
  if (NATIVE_ON() && object) g.warm[object] = {true, 0, g.frames};
}
extern "C" REX_FUNC(sub_8218DE00) {
  const uint32_t object = ctx.r3.u32;
  if (NATIVE_ON()) { g.shaders.erase(object); g.created_blobs.erase(object); }
  __imp__sub_8218DE00(ctx, base);
  if (NATIVE_ON() && object) g.warm[object] = {false, 0, g.frames};
}

// D3DDevice_SetStreamSource(pDevice, Stream, pVB, Offset, Stride).
extern "C" REX_FUNC(sub_821953D8) {
  if (NATIVE_ON() && ctx.r4.u32 < 16) g_streams[ctx.r4.u32] = {ctx.r5.u32, ctx.r6.u32, ctx.r7.u32};
  __imp__sub_821953D8(ctx, base);
}

// D3DDevice_SetTexture(pDevice, Sampler, pTexture). Jungle's untextured plants
// get their leaf texture first (ge_hooks.cpp). On this branch the probe's
// midasm hook holds the function's address, so this call is the one that runs.
void ge_jungle_set_texture(PPCRegister& r5);
extern "C" REX_FUNC(sub_8218BE00) {
  ge_jungle_set_texture(ctx.r5);
  if (NATIVE_ON() && ctx.r4.u32 < 32) g_textures[ctx.r4.u32] = ctx.r5.u32;
  __imp__sub_8218BE00(ctx, base);
}

// D3DDevice_DrawIndexedVertices(pDevice, PrimType, BaseVertexIndex, StartIndex, IndexCount).
extern "C" REX_FUNC(sub_8219A6A0) {
  if (NATIVE_ON()) {
    const uint32_t dev = ctx.r3.u32, prim = ctx.r4.u32, count = ctx.r7.u32, start = ctx.r6.u32;
    const int32_t base_vertex = ctx.r5.s32;
    const uint32_t ibo = g32(dev + 12428);
    if (ibo && count) {
      const bool wide = (g32(ibo) & 0x80000000u) != 0;
      const uint32_t addr = gpu_view(g32(ibo + 24) & ~3u);  // 0xE0000000+ buffers: data is 4 KB on
      native_draw(dev, prim, count, nullptr, base_vertex, 0, 0, 0, !wide, addr + start * (wide ? 4 : 2), wide);
    }
  }
  __imp__sub_8219A6A0(ctx, base);
}

// D3DDevice_DrawVertices(pDevice, PrimType, StartVertex, VertexCount).
extern "C" REX_FUNC(sub_8219A2B8) {
  if (NATIVE_ON()) native_draw(ctx.r3.u32, ctx.r4.u32, ctx.r6.u32, nullptr, 0, ctx.r5.u32, 0, 0);
  __imp__sub_8219A2B8(ctx, base);
}

// D3DDevice_DrawVerticesUP(pDevice, PrimType, VertexCount, pData, Stride).
extern "C" REX_FUNC(sub_8219A270) {
  if (NATIVE_ON()) native_draw(ctx.r3.u32, ctx.r4.u32, ctx.r5.u32, nullptr, 0, 0, ctx.r6.u32, ctx.r7.u32);
  __imp__sub_8219A270(ctx, base);
  g_up_data = 0;  // it draws through BeginVertices but ends inline, never calling EndVertices
}

// D3DDevice_BeginVertices(pDevice, PrimType, VertexCount, Stride) -> data pointer;
// the game fills it and calls EndVertices, which draws it here.
extern "C" REX_FUNC(sub_82199DC0) {
  const uint32_t prim = ctx.r4.u32, count = ctx.r5.u32, stride = ctx.r6.u32;
  __imp__sub_82199DC0(ctx, base);
  if (NATIVE_ON()) { g_up_prim = prim; g_up_count = count; g_up_stride = stride; g_up_data = ctx.r3.u32; }
}
extern "C" REX_FUNC(sub_8219A260) {
  if (NATIVE_ON() && g_up_data) {
    native_draw(ctx.r3.u32, g_up_prim, g_up_count, nullptr, 0, 0, g_up_data, g_up_stride);
    g_up_data = 0;
  }
  __imp__sub_8219A260(ctx, base);
}

// D3DDevice_Clear(pDevice, Count, pRects, Flags, Color, Z, Stencil).
extern "C" REX_FUNC(sub_821974B8) {
  if (NATIVE_ON() && ensure_device()) {
    begin_frame();
    bind_target();
    g.draws_since_resolve = true;
    const uint32_t flags = ctx.r6.u32, c = ctx.r7.u32;
    if (flags & 1) {
      g_clear_color.store(c);
      g.cmd->clearColor(0, RenderColor(((c >> 16) & 0xFF) / 255.0f, ((c >> 8) & 0xFF) / 255.0f,
                                       (c & 0xFF) / 255.0f, ((c >> 24) & 0xFF) / 255.0f));
    }
    if ((flags & 0x30) && g.bound_scene) {
      depth_will_change();
      g.cmd->clearDepthStencil(true, true, float(ctx.f1.f64), ctx.r9.u32 & 0xFF);
      g.depth_clear = float(ctx.f1.f64);
      g.depth_written = false;
      ++g.depth_gen;
    }
    prof_mark(fmt::format("clear {:X}", flags));
    NTRACE("clear flags {:X} color {:08X} z {}", flags, c, ctx.f1.f64);
  }
  __imp__sub_821974B8(ctx, base);
}

// D3DDevice_Resolve(pDevice, Flags, pSourceRect, pDestTexture, ...): copy the scene.
extern "C" REX_FUNC(sub_82197C48) {
  if (NATIVE_ON()) { NTRACE("resolve flags {:X} dest {:08X}", ctx.r4.u32, ctx.r6.u32); native_resolve(ctx.r6.u32, ctx.r4.u32); }
  __imp__sub_82197C48(ctx, base);
}

// D3DDevice_SetRenderTarget / SetDepthStencilSurface: traced only (one scene target for now).
extern "C" REX_FUNC(sub_82195C00) {
  if (NATIVE_ON() && tracing()) {
    std::string w;
    for (uint32_t i = 0; ctx.r5.u32 && i < 16; ++i) w += fmt::format(" {:08X}", g32(ctx.r5.u32 + i * 4));
    NTRACE("set rt{} {:08X}:{}", ctx.r4.u32, ctx.r5.u32, w);
  }
  const uint32_t dev = ctx.r3.u32;
  if (NATIVE_ON() && ctx.r4.u32 == 0) g.guest_rt = ctx.r5.u32;
  __imp__sub_82195C00(ctx, base);
  if (NATIVE_ON()) NTRACE("  after set rt: dev+10936 {:08X} {:08X} {:08X}", g32(dev + 10936), g32(dev + 10940), g32(dev + 10944));
}
extern "C" REX_FUNC(sub_82195F68) {
  if (NATIVE_ON()) { NTRACE("set depth {:08X}", ctx.r4.u32); g.guest_depth = ctx.r4.u32; }
  __imp__sub_82195F68(ctx, base);
}

// D3DDevice_Swap(pDevice, pFrontBuffer, pParameters).
// Frame-time log for comparing the renderers: guest Swap-to-Swap intervals and
// the time spent inside Swap (guest D3D + native present) on the game thread.
void frame_stats(double interval_ms, double swap_ms) {
  static std::vector<double> intervals;
  static double swap_total = 0, swap_max = 0;
  const int32_t every = REXCVAR_GET(gpu_native_frame_stats);
  if (every <= 0) return;
  if (interval_ms > 0) intervals.push_back(interval_ms);
  if (interval_ms > double(REXCVAR_GET(gpu_native_long_frame_ms)))
    REXKRNL_INFO("GENATIVE long frame {:.1f} ms (swap {:.1f}; present: acquire {:.1f} submit {:.1f} present {:.1f} "
                 "fence {:.1f}): {} pipelines {:.1f} ms, {} textures {:.1f} ms; draws: "
                 "parse {:.1f} indices {:.1f} decode {:.1f} total {:.1f} ms; vertex cache {} hits {} misses; texture decode {:.1f} "
                 "mips {:.1f} ms; vertex decode {:.1f} ms; {} mid-frame flushes {:.1f} ms; pool alloc {:.1f} ms; vertex hash {:.1f} ms",
                 interval_ms,
                 swap_ms, g.present_ms[0], g.present_ms[1], g.present_ms[2], g.present_ms[3], g.last_pipe_n, g.last_pipe_ms, g.last_tex_n, g.last_tex_ms, g.last_sect[0], g.last_sect[1],
                 g.last_sect[2], g.last_sect[4], g.last_vcache_hits, g.last_vcache_misses, g.last_tex_decode_ms,
                 g.last_tex_mip_ms, g.last_vdecode_ms, g.last_flushes, g.last_flush_ms, g.last_alloc_ms, g.last_hash_ms);
  swap_total += swap_ms;
  swap_max = std::max(swap_max, swap_ms);
  if (intervals.size() < size_t(every)) return;
  std::vector<double> v = intervals;
  std::sort(v.begin(), v.end());
  double sum = 0;
  for (double x : v) sum += x;
  const auto pct = [&](double q) { return v[std::min(v.size() - 1, size_t(q * double(v.size())))]; };
  REXKRNL_INFO("GENATIVE frame_stats {} {} frames: avg {:.3f} ms p50 {:.3f} p99 {:.3f} max {:.3f} | swap avg {:.3f} "
               "max {:.3f} ms | thread {}", NATIVE_ON() ? "native" : "xenos", v.size(), sum / double(v.size()),
               pct(0.5), pct(0.99), v.back(), swap_total / double(v.size()), swap_max, GetCurrentThreadId());
  // Spread of the intervals: <15.7, 15.7-16.2, 16.2-16.7, 16.7-17.2, 17.2-17.7, 17.7-19, 19-25, >25 ms.
  int hist[8] = {};
  for (double x : v) {
    const double edges[] = {15.7, 16.2, 16.7, 17.2, 17.7, 19.0, 25.0};
    int b = 0;
    while (b < 7 && x >= edges[b]) ++b;
    ++hist[b];
  }
  REXKRNL_INFO("GENATIVE frame_hist {} {} {} {} {} {} {} {}", hist[0], hist[1], hist[2], hist[3], hist[4], hist[5], hist[6],
               hist[7]);
  if (!g.present_intervals.empty()) {
    int phist[8] = {};
    double pmax = 0;
    for (double x : g.present_intervals) {
      const double edges[] = {15.7, 16.2, 16.7, 17.2, 17.7, 19.0, 25.0};
      int b = 0;
      while (b < 7 && x >= edges[b]) ++b;
      ++phist[b];
      pmax = std::max(pmax, x);
    }
    double psum = 0, psq = 0;
    for (double x : g.present_intervals) { psum += x; psq += x * x; }
    const double pmean = psum / g.present_intervals.size();
    REXKRNL_INFO("GENATIVE present_hist {} {} {} {} {} {} {} {} | interval mean {:.3f} sd {:.3f} max {:.2f} ms | pacing "
                 "wait avg {:.2f} ms, period {:.3f} ms | {} double steps", phist[0], phist[1], phist[2], phist[3], phist[4],
                 phist[5], phist[6], phist[7], pmean, std::sqrt(std::max(0.0, psq / g.present_intervals.size() - pmean * pmean)),
                 pmax, g.pace_wait_sum / g.present_intervals.size(), g.pace_period, g.double_steps);
    g.present_intervals.clear();
    g.pace_wait_sum = 0;
    g.double_steps = 0;
  }
  intervals.clear();
  swap_total = swap_max = 0;
}

// The game's per-frame copy of its graphics setting into the render flag (see
// ModeWarm): inverted while a hidden warm-up frame is drawn.
extern "C" REX_FUNC(sub_82099708) {
  __imp__sub_82099708(ctx, base);
  if (NATIVE_ON() && g_mode_warm.left > 0) base[kRenderModeFlag] ^= 1;
}

extern "C" REX_FUNC(sub_821996F8) {
  static LARGE_INTEGER freq{}, last{};
  if (!freq.QuadPart) QueryPerformanceFrequency(&freq);
  LARGE_INTEGER t0, t1;
  QueryPerformanceCounter(&t0);
  // No GPU plugin: nothing ever retires the game's GPU fences. Put the game's
  // own "GPU hung" mode on (device+10941 bit 2: it fakes its fences on the CPU
  // and stops kicking the ring; sub_821A2D50 / sub_8219CEA0) - harmless here,
  // since the native renderer takes the frame at the D3D calls. 0.9.6c stopped
  // setting it from the GPU-wait watchdog because under Xenos it froze the
  // picture; native mode has to set it itself or the game waits forever.
  if (NATIVE_ON() && ctx.r3.u32) gmem()[ctx.r3.u32 + 10941] |= 0x02;
  if (NATIVE_ON()) g.front_buffer = ctx.r4.u32;
  __imp__sub_821996F8(ctx, base);
  if (NATIVE_ON()) {
    present_frame();
    ModeWarm& w = g_mode_warm;
    const uint32_t stage = g32(0x82423E00u);
    if (stage != w.stage) {
      w.stage = stage;
      w.done = stage == kFrontEndStage || !REXCVAR_GET(gpu_native_warm_modes);
      w.left = 0;
    }
    if (w.left > 0) {
      --w.left;  // that frame was drawn in the other mode and not shown
    } else if (!w.done && g.draws >= kWarmMinDraws) {
      w.done = true;
      w.left = kWarmFrames;
      REXKRNL_INFO("GENATIVE stage {}: drawing {} hidden frames in the other graphics mode", stage, kWarmFrames);
    }
  }
  QueryPerformanceCounter(&t1);
  const auto ms = [&](int64_t ticks) { return double(ticks) * 1000.0 / double(freq.QuadPart); };
  frame_stats(last.QuadPart ? ms(t0.QuadPart - last.QuadPart) : 0.0, ms(t1.QuadPart - t0.QuadPart));
  last = t0;
}
