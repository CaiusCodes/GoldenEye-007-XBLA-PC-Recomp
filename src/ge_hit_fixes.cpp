// ge_hit_fixes.cpp
//
// Door windows and camera lenses take bullets the N64 way (GitHub issue #24).
//
// On the N64 the hit test for a shot against an object (sub_GAME_7F04E720 in
// n64decomp/007) knows which model node was hit, and two rules use it:
//  - a door model built on skeleton_door: a hit on its window node
//    (Switches[3]) does not stop the bullet, unless the object is flagged
//    invincible (flags 0x20000); the window gets a glass bullet hole, and on
//    the third such hit objHit shatters it (sub_GAME_7F04DD68: glass shards
//    over the window, the window's bullet holes cleared, the glass hidden);
//  - a security camera (skeleton_cctv): a hit on its lens node (Switches[1])
//    does 100x damage in objHit, so one shot destroys it.
// Rare's XBLA hit test (sub_820DAEF0) traces new collision meshes and no longer
// reports the node: it passes 0 to chrpropAddBulletHit (sub_820CD6C0), so both
// rules compare against 0 and never fire. Windowed doors stop every shot, and a
// camera only falls to body shots (5 on 00 Agent / 007, 3 on Agent). XBLA's
// objHit still counts window hits (door +0xBD) but the shatter call is gone.
//
// ge_obj_hit_part runs just before that call (0x820DB2E0). r1+176 holds the hit
// point in the space of the matrix that was hit (r9; the matrices, in view
// space, are at r29), r10 the node (0) and r1+92 the "counts as penetration"
// flag. For the windowed door models and the camera it checks whether the point
// lies on the window pane / lens, measured from the game's own collision meshes
// (new\prop\<model>_hits, which both graphics modes trace), and if so passes the
// real node, so the rest of the N64 code runs as on the N64: the window lets the
// bullet through with a glass bullet hole, the lens takes 100x damage. Hits on
// the door's frame and metal, the camera's body and anything else are left
// exactly as they were.
//
// Shattering (ge_door_window_shatter, in objHit after the window hit count):
// on the third window hit the window's glass shards are thrown with the game's
// own shard code (sub_82146040, as the glass panels use it) over the pane, in
// the pane's world position taken from the hit, the window's bullet holes are
// cleared (sub_821414D8(prop, 1), as on the N64) and the door is marked broken.
// A broken window is not hit any more (ge_obj_hit_part skips it) and its glass
// is not drawn: each door model's draw list draws the glass as its one blended
// quad, so around that door's own draw (ge_door_draw_begin/_end, the model is
// known from sub_8209AB98) that block is jumped over, in both graphics modes.

#include <array>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <unordered_map>
#include <vector>

#include "ge_init.h"  // PPCRegister/PPCContext + generated function decls
#include <rex/cvar.h>
#include <rex/hook.h>
#include <rex/logging/api.h>
#include <rex/runtime.h>
#include <rex/system/kernel_state.h>
#include <rex/system/xmemory.h>

REXCVAR_DEFINE_BOOL(ge_n64_glass_hits, true, "Gameplay",
                    "Bullets pass through door windows (which break after 3 hits) and a shot on a camera's lens "
                    "destroys it, as on the N64");
REXCVAR_DEFINE_BOOL(ge_hit_log, false, "Diagnostics",
                    "Test-only: log each shot that hits a door or camera (hit point in model space)");

namespace {

inline uint32_t LD32(uint8_t* b, uint32_t ga) {
  uint32_t v;
  std::memcpy(&v, b + ga, 4);
  return __builtin_bswap32(v);
}
inline uint16_t LD16(uint8_t* b, uint32_t ga) {
  uint16_t v;
  std::memcpy(&v, b + ga, 2);
  return __builtin_bswap16(v);
}
inline float LDF(uint8_t* b, uint32_t ga) {
  const uint32_t v = LD32(b, ga);
  float f;
  std::memcpy(&f, &v, 4);
  return f;
}
inline void ST32(uint8_t* b, uint32_t ga, uint32_t val) {
  const uint32_t v = __builtin_bswap32(val);
  std::memcpy(b + ga, &v, 4);
}
inline void STF(uint8_t* b, uint32_t ga, float f) {
  uint32_t v;
  std::memcpy(&v, &f, 4);
  ST32(b, ga, v);
}

constexpr uint32_t kSkeletonCctv = 0x825C0DD4u;  // both compared in objHit (sub_820E7F68)
constexpr uint32_t kSkeletonDoor = 0x825C0DF0u;
constexpr uint32_t kFlagInvincible = 0x00020000u;  // obj +8: its window stops bullets too
constexpr uint32_t kCurrentPlayer = 0x82F1FAACu;
constexpr uint32_t kPlayerViewToWorld = 4492u;     // player +, the matrix sub_8210D870 returns
constexpr uint32_t kDoorWindowHits = 0xBDu;        // door +, objHit counts window hits here
constexpr int kShatterHits = 3;

// The glass pane of each windowed door model, in the door's own space: the
// pane is the one face of the model's collision mesh (new\prop\<name>_hits)
// that lies inside the door's thickness, between its front and back faces, so
// a hit on it can only be a hit on the glass. Rect on x/y, plane at z. These
// are all the skeleton_door models placed in the levels (Bunker 1/2, Surface
// 1/2, Facility, Train, MP Bunker); none is flagged invincible there.
struct Pane {
  uint16_t model;
  float x0, x1, y0, y1, z;
};
constexpr Pane kPanes[] = {
    {138, -33.34f, 50.00f, 346.30f, 431.89f, -8.33f},              // sevdoor4wind (Bunker)
    {139, -83.33f, -16.67f, 33.33f, 499.51f, -1266.67f},           // sevtrislide (Bunker, Surface 2)
    {160, 17775.00f, 17925.00f, -25.00f, 475.00f, -487.50f},       // gasplantcleardoor (Facility)
    {162, -221.15f, -171.15f, 208.33f, 337.50f, -208.33f},         // traindoor2 (Train)
    {166, -21946.49f, -21896.49f, 500.00f, 550.00f, -37849.68f},  // sevdoorwood (Surface)
    {167, 4866.67f, 4916.67f, 533.33f, 583.33f, -21783.33f},       // sevdoorwind (Surface)
};
constexpr float kEdge = 0.5f;  // the collision meshes' own rounding

const Pane* FindPane(uint16_t model) {
  for (const Pane& p : kPanes)
    if (p.model == model) return &p;
  return nullptr;
}

bool OnPane(const Pane& p, float x, float y, float z) {
  return std::fabs(z - p.z) <= kEdge && x >= p.x0 - kEdge && x <= p.x1 + kEdge && y >= p.y0 - kEdge &&
         y <= p.y1 + kEdge;
}

// The security camera's lens, in the space of its head (matrix 1, the CAM bone):
// in cctv_hits the head is a box under a hood, and the glass is the slanted face
// recessed in the hood's opening, from (y -71.58, z 16.58) down to (y -99.58,
// z 4.33), 35 wide. A face behind it at z 2.58 can only be reached through it.
bool OnLens(uint32_t mtx, float x, float y, float z) {
  if (mtx != 1 || std::fabs(x) > 17.5f + kEdge || y < -99.58f - kEdge || y > -71.58f + kEdge) return false;
  const float glass_z = 4.33f + (y + 99.58f) * ((16.58f - 4.33f) / (99.58f - 71.58f));
  return std::fabs(z - glass_z) <= kEdge || std::fabs(z - 2.58f) <= kEdge;
}

// A 4x3 transform as the game keeps them: point' = x*r[0] + y*r[1] + z*r[2] + r[3].
using Mtx = std::array<std::array<float, 3>, 4>;

Mtx LoadMtx(uint8_t* base, uint32_t at) {  // a 4x4 float matrix, rows of 16 bytes
  Mtx m;
  for (int r = 0; r < 4; ++r)
    for (int c = 0; c < 3; ++c) m[r][c] = LDF(base, at + 16u * r + 4u * c);
  return m;
}

Mtx Multiply(const Mtx& inner, const Mtx& outer) {  // inner first, then outer
  Mtx w;
  for (int r = 0; r < 4; ++r)
    for (int c = 0; c < 3; ++c) {
      float v = inner[r][0] * outer[0][c] + inner[r][1] * outer[1][c] + inner[r][2] * outer[2][c];
      if (r == 3) v += outer[3][c];
      w[r][c] = v;
    }
  return w;
}

// Doors whose window was hit: where the window was (world space, from the last
// window hit) and whether it is broken. Keyed by the door object; an entry is
// trusted only while the object still has the same model and its own window
// hit count, so a level reload (fresh objects) starts clean.
struct DoorWindow {
  uint32_t model = 0;
  Mtx world{};
  bool broken = false;
};
std::unordered_map<uint32_t, DoorWindow> g_windows;
std::unordered_map<uint32_t, uint32_t> g_broken_models;  // model -> door object

bool IsBroken(uint8_t* base, uint32_t obj) {
  const auto it = g_windows.find(obj);
  return it != g_windows.end() && it->second.broken && LD32(base, obj + 20u) == it->second.model &&
         base[obj + kDoorWindowHits] >= kShatterHits;
}

}  // namespace

bool ge_obj_hit_part(PPCRegister& r1, PPCRegister& r9, PPCRegister& r10, PPCRegister& r25, PPCRegister& r29) {
  if (!REXCVAR_GET(ge_n64_glass_hits) && !REXCVAR_GET(ge_hit_log)) return false;
  uint8_t* base = rex::system::kernel_state()->memory()->virtual_membase();
  const uint32_t obj = r25.u32;
  const uint32_t model = obj ? LD32(base, obj + 20u) : 0;
  const uint32_t file = model ? LD32(base, model + 8u) : 0;
  if (!file) return false;
  const uint32_t skeleton = LD32(base, file + 4u);
  const uint32_t switches = LD32(base, file + 8u);
  if (!switches || (skeleton != kSkeletonDoor && skeleton != kSkeletonCctv)) return false;
  const uint16_t modelnum = LD16(base, obj + 4u);
  const float x = LDF(base, r1.u32 + 176u), y = LDF(base, r1.u32 + 180u), z = LDF(base, r1.u32 + 184u);
  bool glass = false, skip = false;
  if (skeleton == kSkeletonDoor) {
    // N64: a hit on the window node lets the bullet through unless the door is invincible.
    const uint32_t window = LD32(base, switches + 12u);
    const Pane* pane = FindPane(modelnum);
    glass = window && pane && OnPane(*pane, x, y, z);
    if (REXCVAR_GET(ge_n64_glass_hits)) {
      if (glass && IsBroken(base, obj)) {
        skip = true;  // the glass is gone: nothing there to hit
      } else {
        if (glass) {
          r10.u64 = window;
          // Where the window is now, for the shards if this hit breaks it:
          // the hit matrix (view space, r29) then view to world.
          const uint32_t player = LD32(base, kCurrentPlayer);
          const uint32_t view_to_world = player ? LD32(base, player + kPlayerViewToWorld) : 0;
          if (view_to_world && r9.u32 < 64u) {
            DoorWindow& w = g_windows[obj];
            if (w.model != model) w = DoorWindow{};
            w.model = model;
            w.world = Multiply(LoadMtx(base, r29.u32 + 64u * r9.u32), LoadMtx(base, view_to_world));
          }
        }
        ST32(base, r1.u32 + 92u, glass && !(LD32(base, obj + 8u) & kFlagInvincible) ? 0u : 1u);
      }
    }
  } else {
    // N64: a hit on the lens node does 100x damage in objHit.
    const uint32_t lens = LD32(base, switches + 4u);
    glass = lens && OnLens(r9.u32, x, y, z);
    if (glass && REXCVAR_GET(ge_n64_glass_hits)) r10.u64 = lens;
  }
  if (REXCVAR_GET(ge_hit_log))
    REXKRNL_INFO("GEHITS {} {:08X} model {} matrix {} at {:.2f} {:.2f} {:.2f}: {} (bullet {})",
                 skeleton == kSkeletonDoor ? "door" : "camera", obj, modelnum, r9.u32, x, y, z,
                 skip ? "broken window" : glass ? "glass" : "body",
                 skip || !LD32(base, r1.u32 + 92u) ? "goes on" : "stops");
  return skip;
}

// objHit, after it counted a window hit (door +0xBD, only for hits that let the
// bullet through): the third breaks the window, as sub_GAME_7F04DD68 did.
void ge_door_window_shatter(PPCRegister& r27, PPCRegister& r31) {
  if (!REXCVAR_GET(ge_n64_glass_hits)) return;
  uint8_t* base = rex::system::kernel_state()->memory()->virtual_membase();
  const uint32_t obj = r27.u32, hit = r31.u32;
  const uint32_t model = obj ? LD32(base, obj + 20u) : 0;
  const uint32_t file = model ? LD32(base, model + 8u) : 0;
  if (!file || LD32(base, file + 4u) != kSkeletonDoor || LD32(base, hit + 60u) != 0) return;
  if (base[obj + kDoorWindowHits] < kShatterHits) return;
  const auto it = g_windows.find(obj);
  const Pane* pane = FindPane(LD16(base, obj + 4u));
  if (it == g_windows.end() || it->second.model != model || it->second.broken || !pane) return;
  DoorWindow& w = it->second;
  w.broken = true;
  g_broken_models[model] = obj;

  PPCContext ctx = *rex::runtime::ThreadState::Get()->context();
  const uint32_t args = (ctx.r1.u32 - 0x200u) & ~15u;  // below the guest stack pointer
  ctx.r1.u64 = args - 0x200u;                         // the callees' frames go below the args
  for (int r = 0; r < 4; ++r)
    for (int c = 0; c < 3; ++c) STF(base, args + 16u * r + 4u * c, w.world[r][c]);
  // Shards over the pane: its rect, and a sliver of the door's thickness.
  ctx.r3.u64 = args + 48u;  // position (the window's origin)
  ctx.r4.u64 = args;        // its x, y and z axes
  ctx.r5.u64 = args + 16u;
  ctx.r6.u64 = args + 32u;
  ctx.f1.f64 = pane->x0;
  ctx.f2.f64 = pane->x1;
  ctx.f3.f64 = pane->y0;
  ctx.f4.f64 = pane->y1;
  ctx.f5.f64 = pane->z - 1.0f;
  ctx.f6.f64 = pane->z + 1.0f;
  sub_82146040(ctx, base);
  // The window's bullet holes go with it (the glass impacts are the room-clear-1 ones).
  ctx.r3.u64 = LD32(base, obj + 16u);
  ctx.r4.u64 = 1;
  if (ctx.r3.u32) sub_821414D8(ctx, base);
  const Mtx& m = w.world;
  const float cx = (pane->x0 + pane->x1) * 0.5f, cy = (pane->y0 + pane->y1) * 0.5f;
  REXKRNL_INFO("GEHITS door {:08X} model {} window broken, glass at {:.0f} {:.0f} {:.0f}", obj, LD16(base, obj + 4u),
               cx * m[0][0] + cy * m[1][0] + pane->z * m[2][0] + m[3][0],
               cx * m[0][1] + cy * m[1][1] + pane->z * m[2][1] + m[3][1],
               cx * m[0][2] + cy * m[1][2] + pane->z * m[2][2] + m[3][2]);
}

// Drawing: sub_8209AB98(model, ...) draws an object's model; its rendergraph
// call (sub_8209E9B0 at 0x8209D36C in sub_8209D250(res)) reads the model file's
// draw list. For a door whose window is broken, the list's glass block is
// jumped over for that one draw and put back right after it.
namespace {
uint32_t g_drawing_model = 0;
uint32_t g_patched_at = 0;
uint8_t g_patched_saved[8];
struct GlassBlock {
  uint32_t at = 0, next = 0;  // the block, and where the list goes on after it
};
std::unordered_map<uint32_t, GlassBlock> g_glass_blocks;  // draw list start -> glass block

// The glass is the one draw block whose last primitive is a quad (op 0x01,
// type 0xD, 4 indices) drawn with the blended material 0x4702 (set by op 0x1A,
// or as the second word of op 0x15). All 12 windowed door lists (6 models, both
// graphics modes) have exactly one such block; anything else is left alone.
GlassBlock FindGlass(uint8_t* base, uint32_t start, uint32_t end) {
  std::vector<uint32_t> blocks;
  std::vector<bool> candidate;
  uint32_t material = 0;
  bool quad = false;
  uint32_t p = start;
  auto close = [&]() {
    if (!blocks.empty()) candidate.push_back(quad);
    quad = false;
  };
  while (p + 4 <= end) {
    const uint32_t size = LD32(base, p) >> 16;
    const uint8_t op = base[p + 2];
    if (size < 4 || base[p + 3] != 0 || p + size > end) return {};
    if (op == 0x16) {
      close();
      blocks.push_back(p);
    } else if (op == 0x1A && size >= 8) {
      material = LD32(base, p + 4);
    } else if (op == 0x15 && size >= 12) {
      material = LD32(base, p + 8);
    } else if (op == 0x01 && size >= 12) {
      quad = material == 0x4702u && LD32(base, p + 4) == 0xDu && LD32(base, p + 8) == 4u;
    } else if (op == 0x1D) {
      break;
    }
    p += size;
  }
  close();
  if (p + 4 > end || base[p + 2] != 0x1D) return {};
  GlassBlock found;
  int count = 0;
  for (size_t i = 0; i < blocks.size(); ++i) {
    if (!candidate[i]) continue;
    ++count;
    found.at = blocks[i];
    found.next = i + 1 < blocks.size() ? blocks[i + 1] : p;
  }
  return count == 1 ? found : GlassBlock{};
}
}  // namespace

void ge_model_draw_object(PPCRegister& r25) { g_drawing_model = r25.u32; }

void ge_door_draw_begin(PPCRegister& r31) {
  if (g_broken_models.empty() || !g_drawing_model) return;
  uint8_t* base = rex::system::kernel_state()->memory()->virtual_membase();
  const auto it = g_broken_models.find(g_drawing_model);
  if (it == g_broken_models.end()) return;
  const uint32_t model = it->first, obj = it->second;
  if (LD32(base, obj + 20u) != model || !IsBroken(base, obj)) {
    g_broken_models.erase(it);  // a reloaded level: not this door any more
    return;
  }
  // Only the model's own files (sub_8209AB98 draws model file +32 or +40).
  const uint32_t res = r31.u32, file = LD32(base, model + 8u);
  if (!file || (res != LD32(base, file + 32u) && res != LD32(base, file + 40u))) return;
  const uint32_t rg = LD32(base, res + 8u);
  const uint32_t hdr = rg ? LD32(base, rg + 0xECu) : 0;
  const uint32_t start = hdr ? LD32(base, hdr) : 0;
  const uint32_t end = hdr ? LD32(base, hdr + 4u) : 0;
  if (!start || end <= start || end - start > 0x10000u) return;
  auto found = g_glass_blocks.find(start);
  if (found == g_glass_blocks.end()) {
    found = g_glass_blocks.emplace(start, FindGlass(base, start, end)).first;
    if (!found->second.at)
      REXKRNL_WARN("GEHITS {}: no single glass block in its draw list; the glass stays",
                   reinterpret_cast<const char*>(base + res + 16u));
    else
      REXKRNL_INFO("GEHITS {}: broken glass = draw list block at +{:X}",
                   reinterpret_cast<const char*>(base + res + 16u), found->second.at - start);
  }
  const GlassBlock& g = found->second;
  if (!g.at || LD32(base, g.at) >> 16 == 0 || base[g.at + 2] != 0x16) return;
  g_patched_at = g.at;
  std::memcpy(g_patched_saved, base + g.at, 8);
  ST32(base, g.at, 0x00081900u);  // op 0x19, size 8: goto
  ST32(base, g.at + 4u, g.next);
}

void ge_door_draw_end() {
  if (!g_patched_at) return;
  uint8_t* base = rex::system::kernel_state()->memory()->virtual_membase();
  std::memcpy(base + g_patched_at, g_patched_saved, 8);
  g_patched_at = 0;
}
