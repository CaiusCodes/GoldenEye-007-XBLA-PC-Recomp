// ge_hit_fixes.cpp
//
// Door windows and camera lenses take bullets the N64 way (GitHub issue #24).
//
// On the N64 the hit test for a shot against an object (sub_GAME_7F04E720 in
// n64decomp/007) knows which model node was hit, and two rules use it:
//  - a door model built on skeleton_door: a hit on its window node
//    (Switches[3]) does not stop the bullet, unless the object is flagged
//    invincible (flags 0x20000); the window still gets a glass bullet hole;
//  - a security camera (skeleton_cctv): a hit on its lens node (Switches[1])
//    does 100x damage in objHit, so one shot destroys it.
// Rare's XBLA hit test (sub_820DAEF0) traces new collision meshes and no longer
// reports the node: it passes 0 to chrpropAddBulletHit (sub_820CD6C0), so both
// rules compare against 0 and never fire. Windowed doors stop every shot, and a
// camera only falls to body shots (5 on 00 Agent / 007, 3 on Agent).
//
// ge_obj_hit_part runs just before that call (0x820DB2E0). r1+176 holds the hit
// point in the space of the matrix that was hit (r9), r10 the node (0) and
// r1+92 the "counts as penetration" flag. For the windowed door models and the
// camera it checks whether the point lies on the window pane / lens, measured
// from the game's own collision meshes (new\prop\<model>_hits, which both
// graphics modes trace), and if so passes the real node, so the rest of the
// N64 code runs as on the N64: the window lets the bullet through with a glass
// bullet hole, the lens takes 100x damage. Hits on the door's frame and metal,
// the camera's body and anything else are left exactly as they were.
// (The N64 also breaks a door window after 3 hits; XBLA removed that call and
// the remastered door models have no broken-glass state, so windows only get
// bullet holes.)

#include <cmath>
#include <cstdint>
#include <cstring>

#include "ge_init.h"  // PPCRegister
#include <rex/cvar.h>
#include <rex/hook.h>
#include <rex/logging/api.h>
#include <rex/system/kernel_state.h>
#include <rex/system/xmemory.h>

REXCVAR_DEFINE_BOOL(ge_n64_glass_hits, true, "Gameplay",
                    "Bullets pass through door windows and a shot on a camera's lens destroys it, as on the N64");
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

constexpr uint32_t kSkeletonCctv = 0x825C0DD4u;  // both compared in objHit (sub_820E7F68)
constexpr uint32_t kSkeletonDoor = 0x825C0DF0u;
constexpr uint32_t kFlagInvincible = 0x00020000u;  // obj +8: its window stops bullets too

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

bool OnPane(uint16_t model, float x, float y, float z) {
  for (const Pane& p : kPanes) {
    if (p.model != model) continue;
    return std::fabs(z - p.z) <= kEdge && x >= p.x0 - kEdge && x <= p.x1 + kEdge && y >= p.y0 - kEdge &&
           y <= p.y1 + kEdge;
  }
  return false;
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

}  // namespace

void ge_obj_hit_part(PPCRegister& r1, PPCRegister& r9, PPCRegister& r10, PPCRegister& r25) {
  if (!REXCVAR_GET(ge_n64_glass_hits) && !REXCVAR_GET(ge_hit_log)) return;
  uint8_t* base = rex::system::kernel_state()->memory()->virtual_membase();
  const uint32_t obj = r25.u32;
  const uint32_t model = obj ? LD32(base, obj + 20u) : 0;
  const uint32_t file = model ? LD32(base, model + 8u) : 0;
  if (!file) return;
  const uint32_t skeleton = LD32(base, file + 4u);
  const uint32_t switches = LD32(base, file + 8u);
  if (!switches || (skeleton != kSkeletonDoor && skeleton != kSkeletonCctv)) return;
  const uint16_t modelnum = LD16(base, obj + 4u);
  const float x = LDF(base, r1.u32 + 176u), y = LDF(base, r1.u32 + 180u), z = LDF(base, r1.u32 + 184u);
  bool glass = false;
  if (skeleton == kSkeletonDoor) {
    // N64: a hit on the window node lets the bullet through unless the door is invincible.
    const uint32_t window = LD32(base, switches + 12u);
    glass = window && OnPane(modelnum, x, y, z);
    if (REXCVAR_GET(ge_n64_glass_hits)) {
      if (glass) r10.u64 = window;
      ST32(base, r1.u32 + 92u, glass && !(LD32(base, obj + 8u) & kFlagInvincible) ? 0u : 1u);
    }
  } else {
    // N64: a hit on the lens node does 100x damage in objHit.
    const uint32_t lens = LD32(base, switches + 4u);
    glass = lens && OnLens(r9.u32, x, y, z);
    if (glass && REXCVAR_GET(ge_n64_glass_hits)) r10.u64 = lens;
  }
  if (REXCVAR_GET(ge_hit_log))
    REXKRNL_INFO("GEHITS {} model {} matrix {} at {:.2f} {:.2f} {:.2f}: {} (bullet {})",
                 skeleton == kSkeletonDoor ? "door" : "camera", modelnum, r9.u32, x, y, z,
                 glass ? "glass" : "body", LD32(base, r1.u32 + 92u) ? "stops" : "goes on");
}
