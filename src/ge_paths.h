// GoldenEye 007 XBLA PC Recomp - where the game's files are.
//
// Release layout: the game program lives in <release>\resources\runtime and its
// data beside that folder (resources\assets, resources\userdata with ge.toml);
// the launcher in the release root starts it. Dev builds keep everything
// beside the exe (assets, userdata, ge.toml).
//
// Saves, settings and caches are in resources\userdata unless the player chose
// another folder in setup ("Keep saves and settings in another folder"). Then
// resources\userdata-location.txt holds that folder (one line, UTF-8); the
// launcher writes it, and an update never touches it.
#pragma once

#include <rex/filesystem.h>

#include <cctype>
#include <filesystem>
#include <fstream>
#include <string>

namespace ge {

inline bool InRuntimeFolder() {
  std::string name = rex::filesystem::GetExecutableFolder().filename().string();
  for (char& c : name) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  return name == "runtime";
}

inline std::filesystem::path DataRoot() {
  const auto exe = rex::filesystem::GetExecutableFolder();
  return InRuntimeFolder() ? exe.parent_path() : exe;
}

// The folder the player chose for saves and settings; empty when none (or a dev build).
inline std::filesystem::path CustomUserDataRoot() {
  if (!InRuntimeFolder()) return {};
  std::ifstream in(DataRoot() / "userdata-location.txt", std::ios::binary);
  std::string line;
  if (!in || !std::getline(in, line)) return {};
  if (line.size() >= 3 && static_cast<unsigned char>(line[0]) == 0xEF) line.erase(0, 3);  // BOM
  while (!line.empty() && (line.back() == '\r' || line.back() == ' ')) line.pop_back();
  if (line.empty()) return {};
  return std::filesystem::path(std::u8string(line.begin(), line.end()));
}

// Saves, settings and caches: the chosen folder, else <data root>\userdata.
inline std::filesystem::path UserDataRoot() {
  const auto custom = CustomUserDataRoot();
  return custom.empty() ? DataRoot() / "userdata" : custom;
}

inline std::filesystem::path ConfigPath() {
  return InRuntimeFolder() ? UserDataRoot() / "ge.toml" : DataRoot() / "ge.toml";
}

}  // namespace ge
