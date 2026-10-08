// Character select pictures for the characters the Xbox game had none for.
//
// The game loads 27 pictures into 16-byte slots at 0x83066B58 (sub_82146538;
// slot 26 is the "?" picture, texture/characters/who) and every multiplayer
// character entry (12 bytes from 0x8272BA80) names one by number in byte +3.
// The slots after 26 hold other menu textures, so the pictures are moved to a
// longer table of our own: the four places that use the table (character
// select, the lobby's player card and its small icon, the character list) have
// their table address swapped for ours. For each picture in portraits\ beside
// the exe a fresh copy of "who" is loaded into a new slot and its pixels are
// replaced with the PNG (129 x 133, like "who"); the character entry then
// points at that slot. Entries 33-38 (the Arkangelsk / Severnaya / ...
// costumes) borrowed Bond's, Natalya's and Trevelyan's photos; they show "?"
// until they have their own.

#include <cmath>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <string>
#include <vector>

#include "ge_init.h"

#include <rex/filesystem.h>
#include <rex/logging.h>
#include <rex/runtime.h>
#include <rex/system/kernel_state.h>
#include <rex/system/xmemory.h>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <wincodec.h>

namespace ge {
bool SetGameText(uint8_t* base, uint32_t id, const std::string& text);  // ge_native_menu.cpp
}

namespace {

inline void getcb(PPCContext*& ctx, uint8_t*& base) {
  ctx = rex::runtime::ThreadState::Get()->context();
  base = rex::system::kernel_state()->memory()->virtual_membase();
}
inline uint32_t LD32(uint8_t* b, uint32_t ga) {
  uint32_t v; std::memcpy(&v, b + ga, 4); return __builtin_bswap32(v);
}
inline void ST32(uint8_t* b, uint32_t ga, uint32_t val) {
  uint32_t v = __builtin_bswap32(val); std::memcpy(b + ga, &v, 4);
}

constexpr uint32_t kGameSlots = 0x83066B58u;   // 27 picture slots, 16 bytes each
constexpr uint32_t kGameSlotCount = 27;
constexpr uint32_t kWhoSlot = 26;
constexpr uint32_t kWhoName = 0x82018160u;     // "who"
constexpr uint32_t kCharTable = 0x8272BA80u;   // MP characters, 12 bytes each
constexpr uint32_t kOurSlotCount = 64;
constexpr int kPicW = 129, kPicH = 133;        // the size of "who"

// Characters with no picture of their own, by their place in the list, and the
// file in portraits\ that gives them one.
struct Pic {
  int index;
  const char* file;
};
constexpr Pic kPics[] = {
    {14, "Scientist 1"},   {15, "Scientist 2"},   {16, "Russian Commandant"},
    {21, "Civilian 1"},    {23, "Civilian 2"},    {30, "Janus Special Forces"},
    {32, "Moonraker Elite 2"}, {33, "Arkangelsk 1"}, {34, "Severnaya"},
    {35, "St Petersburg"}, {36, "Cuba 1"},        {37, "Cuba 2"},
    {38, "Arkangelsk 2"},  {39, "Karl"},          {40, "Martin"},
    {41, "Mark"},          {42, "Dave"},          {43, "Grant"},
    {44, "Graeme"},        {45, "Steve E"},       {46, "Steve H"},
    {47, "Rosika"},        {48, "Terrorist"},     {49, "Biker"},
};
// Costumes that borrowed another character's photo: "?" until they have one.
constexpr int kBorrowed[] = {33, 34, 35, 36, 37, 38};

uint32_t g_slots = 0;  // our table (guest address), kOurSlotCount slots
int g_assigned[64];    // character index -> our slot, 0 = none

void AssignPictures(uint8_t* base) {
  for (int i : kBorrowed) base[kCharTable + i * 12 + 3] = static_cast<uint8_t>(kWhoSlot);
  for (int i = 0; i < 64; ++i)
    if (g_assigned[i]) base[kCharTable + i * 12 + 3] = static_cast<uint8_t>(g_assigned[i]);
}

// A PNG scaled to kPicW x kPicH, as RGBA rows; empty if it cannot be read.
std::vector<uint8_t> LoadPicture(const std::filesystem::path& path) {
  std::vector<uint8_t> out;
  const HRESULT co = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
  IWICImagingFactory* factory = nullptr;
  IWICBitmapDecoder* decoder = nullptr;
  IWICBitmapFrameDecode* frame = nullptr;
  IWICBitmapScaler* scaler = nullptr;
  IWICFormatConverter* conv = nullptr;
  if (SUCCEEDED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER,
                                 IID_PPV_ARGS(&factory))) &&
      SUCCEEDED(factory->CreateDecoderFromFilename(path.c_str(), nullptr, GENERIC_READ,
                                                   WICDecodeMetadataCacheOnDemand, &decoder)) &&
      SUCCEEDED(decoder->GetFrame(0, &frame)) && SUCCEEDED(factory->CreateBitmapScaler(&scaler)) &&
      SUCCEEDED(scaler->Initialize(frame, kPicW, kPicH, WICBitmapInterpolationModeFant)) &&
      SUCCEEDED(factory->CreateFormatConverter(&conv)) &&
      SUCCEEDED(conv->Initialize(scaler, GUID_WICPixelFormat32bppRGBA, WICBitmapDitherTypeNone,
                                 nullptr, 0.0, WICBitmapPaletteTypeCustom))) {
    out.resize(kPicW * kPicH * 4);
    if (FAILED(conv->CopyPixels(nullptr, kPicW * 4, static_cast<UINT>(out.size()), out.data())))
      out.clear();
  }
  if (conv) conv->Release();
  if (scaler) scaler->Release();
  if (frame) frame->Release();
  if (decoder) decoder->Release();
  if (factory) factory->Release();
  if (SUCCEEDED(co)) CoUninitialize();
  return out;
}

// Byte offset of texel (x, y) in a tiled 32-bit Xenos texture (width aligned to 32).
uint32_t TiledOffset(uint32_t x, uint32_t y, uint32_t width) {
  const uint32_t log_bpp = 2;
  const uint32_t aligned = (width + 31) & ~31u;
  const uint32_t macro = ((x >> 5) + (y >> 5) * (aligned >> 5)) << (log_bpp + 7);
  const uint32_t micro = ((x & 7) + ((y & 0xE) << 2)) << log_bpp;
  const uint32_t off = macro + ((micro & ~0xFu) << 1) + (micro & 0xF) + ((y & 1) << 4);
  return ((off & ~0x1FFu) << 3) + ((y & 16) << 7) + ((off & 0x1C0) << 2) +
         (((((y & 8) >> 2) + (x >> 3)) & 3) << 6) + (off & 0x3F);
}

// Write RGBA pixels over a loaded "who" texture (8888, tiled, 8-in-32 swapped).
bool WritePixels(uint8_t* base, uint32_t slot, const std::vector<uint8_t>& rgba) {
  const uint32_t res = LD32(base, slot);
  if (!res) return false;
  const uint32_t w = LD32(base, slot + 8), h = LD32(base, slot + 12);
  // The fetch constant's base (an 0xE... address) is one page below the data
  // the GPU reads (the physical heap's 4 KB offset); write through the
  // 0xA0000000 physical view, as the renderer sees it.
  const uint32_t fetch = LD32(base, res + 0x5C);
  if (w != kPicW || h != kPicH || (fetch & 0xE0000000u) != 0xE0000000u) return false;
  const uint32_t data = 0xA0000000u + (fetch & 0x1FFFF000u) + 0x1000u;
  for (uint32_t y = 0; y < h; ++y) {
    for (uint32_t x = 0; x < w; ++x) {
      const uint8_t* p = &rgba[(y * w + x) * 4];
      uint8_t* t = base + data + TiledOffset(x, y, w);
      t[0] = p[3];  // A B G R in memory
      t[1] = p[2];
      t[2] = p[1];
      t[3] = p[0];
    }
  }
  // Only the full-size level is replaced, so the GPU must not use the smaller
  // levels (they still hold "who": the lobby's small icon showed the "?" through
  // the new picture). Fetch constant at +0x58: dword_4 bits 6-9 = mip_max_level.
  const uint32_t d4 = LD32(base, res + 0x68);
  ST32(base, res + 0x68, d4 & ~(0xFu << 6));
  return true;
}

void RefreshOurTable(uint8_t* base) {
  std::memcpy(base + g_slots, base + kGameSlots, kGameSlotCount * 16);
}

}  // namespace

// After the game has loaded its pictures (sub_82146538, just after "who" at
// 0x8214A508; r30 is the folder argument it passes): our table, new slots.
void ge_portraits_load(PPCRegister& r30) {
  PPCContext* ctx; uint8_t* base; getcb(ctx, base);
  if (!g_slots) {
    g_slots = rex::system::kernel_state()->memory()->SystemHeapAlloc(kOurSlotCount * 16);
    if (!g_slots) return;
    std::memset(base + g_slots, 0, kOurSlotCount * 16);
  }
  RefreshOurTable(base);
  // Game\resources\portraits (beside the exe in older layouts).
  std::filesystem::path dir = rex::filesystem::GetExecutableFolder() / "resources" / "portraits";
  if (!std::filesystem::is_directory(dir)) dir = rex::filesystem::GetExecutableFolder() / "portraits";
  uint32_t next = kGameSlotCount;
  int loaded = 0;
  for (const Pic& pic : kPics) {
    const std::vector<uint8_t> rgba = LoadPicture(dir / (std::string(pic.file) + ".png"));
    if (rgba.empty()) continue;
    if (next >= kOurSlotCount) break;
    const uint32_t slot = g_slots + next * 16;
    if (!LD32(base, slot)) {
      const PPCContext saved = *ctx;
      ctx->r1.u64 = (ctx->r1.u32 - 0x400) & ~0xFu;
      ctx->r3.u64 = slot;
      ctx->r4.u64 = kWhoName;
      ctx->r5.u64 = r30.u32;
      ctx->r6.s64 = -1;
      ctx->r7.s64 = -1;
      sub_82146418(*ctx, base);
      *ctx = saved;
    }
    if (!WritePixels(base, slot, rgba)) {
      REXKRNL_WARN("GEPORTRAIT {}: could not place the picture", pic.file);
      continue;
    }
    g_assigned[pic.index] = static_cast<int>(next);
    ++next;
    ++loaded;
  }
  AssignPictures(base);
  REXKRNL_INFO("GEPORTRAIT {} new character pictures", loaded);
}

namespace ge {
// After the Community Edition data patches (they write the character list on
// the first controller poll, which can come after the pictures were loaded).
void ApplyPortraitTable(uint8_t* base) { AssignPictures(base); }
}

// The places that index the picture table use ours instead (after their
// `addi rX,rX,27480`), with the game's 27 refreshed in case they were reloaded.
static void UseOurTable(PPCRegister& r) {
  if (!g_slots) return;
  uint8_t* base = rex::system::kernel_state()->memory()->virtual_membase();
  RefreshOurTable(base);
  r.u64 = g_slots;
}
void ge_portrait_table_r9(PPCRegister& r9) { UseOurTable(r9); }
void ge_portrait_table_r10(PPCRegister& r10) { UseOurTable(r10); }
void ge_portrait_table_r11(PPCRegister& r11) { UseOurTable(r11); }
void ge_portrait_table_r25(PPCRegister& r25) { UseOurTable(r25); }

// Two characters share the name "Scientist" (text 0x9D21). Where the character
// select and the lobby's player card look the name up (just before the lookup
// call: r3 = text id, r11 = character * 12) they get "Scientist 1" / "2". Only
// these two places see our text ids: elsewhere the game uses the id itself.
void ge_charname(PPCRegister& r3, PPCRegister& r11) {
  struct Name { uint32_t index; uint32_t id; const char* text; };
  static const Name kNames[] = {{14, 0xF0E0u, "Scientist 1"}, {15, 0xF0E1u, "Scientist 2"}};
  const uint32_t index = r11.u32 / 12;
  for (const Name& n : kNames) {
    if (index != n.index) continue;
    uint8_t* base = rex::system::kernel_state()->memory()->virtual_membase();
    if (ge::SetGameText(base, n.id, n.text)) r3.u64 = n.id;
    return;
  }
}
