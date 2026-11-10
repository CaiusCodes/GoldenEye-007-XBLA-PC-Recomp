// GoldenEye 007 XBLA PC Recomp - launcher entry point.
//
//   GoldenEye 007.exe                 start the game; shows setup first when the
//                                     game data has not been imported (or a
//                                     needed file is missing)
//   GoldenEye 007.exe --setup         open the setup window (as does holding
//                                     Shift while opening it, or the game's
//                                     Video Settings > Run Setup Again)
//   GoldenEye 007.exe --update        the update window (the game's Online
//                                     Settings > Check for Updates > Update Now):
//                                     installs the latest release's program files
//
// GE_UPDATE_API (environment, for testing) points --update and the game's
// check at another releases URL (https, or http on 127.0.0.1).
//
// For automated tests (no window):
//   --setup --unattended [--package <file or folder>] [--extras fullscreen,shortcut,complete]
//           [--shortcut-folder <dir>] [--saves-folder <dir>]
//   (--saves-folder: saves and settings in that folder instead of resources\userdata)
//   Exit code 0 imported, 1 failed, 2 several packages found (pass --package), 3 none found.
//
// Any other arguments are passed on to the game.
//
// A GUI-subsystem program, so starting it never flashes a console.
#include "launcher.h"

#include <objbase.h>
#include <shellapi.h>

#define GE_WIDEN2(x) L##x
#define GE_WIDEN(x) GE_WIDEN2(x)
#define GE_STRINGIZE2(x) #x
#define GE_STRINGIZE(x) GE_STRINGIZE2(x)

namespace ge {
const wchar_t* const kVersion = GE_WIDEN(GE_STRINGIZE(GE_VERSION));
}

namespace {

using namespace ge;

int RunUnattended(Paths& p, const SetupOptions& options, const std::wstring& extras, const fs::path& saves_folder) {
  Log(L"Unattended setup");
  std::vector<Source> found;
  std::wstring why;
  if (!options.package.empty()) {
    found = SourcesIn(options.package, &why);
  } else {
    found = DetectSources(p);
    why = L"no package found";
  }
  if (found.size() != 1) {
    Log(L"Unattended: " + (found.empty() ? why : std::to_wstring(found.size()) + L" candidates, pass --package"));
    return found.empty() ? 3 : 2;
  }
  std::atomic<bool> cancel{false};
  const fs::path saves_from = found[0].kind == SourceKind::PreviousInstall ? found[0].path : FindPreviousInstall(p);
  ImportResult result = Import(p, found[0], saves_from, cancel, nullptr);
  if (!result.ok) return 1;
  // The saves folder first, so the extras (fullscreen, 100%) are written there.
  if (!saves_folder.empty()) {
    std::wstring message;
    const bool ok = SetUserDataLocation(p, saves_folder, &message);
    Log(L"Unattended: " + message);
    if (!ok) return 1;
  }
  Extras want;
  want.fullscreen = extras.find(L"fullscreen") != std::wstring::npos;
  want.shortcut = extras.find(L"shortcut") != std::wstring::npos;
  want.complete = extras.find(L"complete") != std::wstring::npos;
  // As on a first setup: unticked extras change nothing; never replace a shortcut that is not ours.
  const std::wstring note = ApplyExtras(p, want, Extras{}, options.shortcut_folder, nullptr);
  if (!note.empty()) Log(L"Unattended: " + note);
  return 0;
}

}  // namespace

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR, int) {
  int argc = 0;
  LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc);
  bool setup = false, unattended = false, update = false;
  std::wstring extras;
  fs::path saves_folder;
  SetupOptions options;
  for (int i = 1; i < argc; ++i) {
    const std::wstring arg = argv[i];
    if (arg == L"--setup") {
      setup = true;
    } else if (arg == L"--update") {
      update = true;
    } else if (arg == L"--unattended") {
      unattended = true;
    } else if (arg == L"--package" && i + 1 < argc) {
      options.package = argv[++i];
    } else if (arg == L"--extras" && i + 1 < argc) {
      extras = argv[++i];
    } else if (arg == L"--saves-folder" && i + 1 < argc) {
      saves_folder = argv[++i];
    } else if (arg == L"--shortcut-folder" && i + 1 < argc) {
      options.shortcut_folder = argv[++i];
    } else {
      options.game_args += (options.game_args.empty() ? L"" : L" ") + Quote(arg);
    }
  }
  if (argv) LocalFree(argv);
  // Holding Shift while opening the launcher (or its shortcut) opens setup,
  // so players can re-run it without the command line, even when the game
  // itself won't start.
  if (GetAsyncKeyState(VK_SHIFT) & 0x8000) setup = true;

  Paths p = MakePaths();
  CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);

  // What an update cut off half-way (or the old launcher exe it could not
  // delete while running) left behind is put back or cleared first: a cheap
  // look at the program folders only.
  RecoverInterruptedUpdate(p);
  // --update: the latest release replaces the program files. One window at a time.
  if (update) {
    HANDLE update_mutex = CreateMutexW(nullptr, TRUE, L"Local\\GoldenEye007Update");
    if (update_mutex && GetLastError() == ERROR_ALREADY_EXISTS) {
      if (HWND other = FindWindowW(L"GoldenEye007Update", nullptr)) {
        ShowWindow(other, SW_RESTORE);
        SetForegroundWindow(other);
      }
      CloseHandle(update_mutex);
      CoUninitialize();
      return 0;
    }
    LogTo(p.logs);
    const int code = RunUpdateWindow(instance, p);
    if (update_mutex) {
      ReleaseMutex(update_mutex);
      CloseHandle(update_mutex);
    }
    CoUninitialize();
    return code;
  }

  // The normal case: everything is in place, so start the game at once.
  std::wstring missing;
  bool installed = IsInstalled(p, &missing);
  if (!installed) {
    RecoverInterruptedSwap(p);
    installed = IsInstalled(p, &missing);
  }
  if (installed && !setup && options.package.empty() && p.custom_userdata &&
      GetFileAttributesW(p.userdata.c_str()) == INVALID_FILE_ATTRIBUTES) {
    // A saves folder chosen in setup that is gone (a drive not connected):
    // never start the game with empty saves in its place.
    LogTo(p.logs);
    Log(L"The saves folder is missing: " + p.userdata.wstring());
    const std::wstring text = L"GoldenEye 007 can't find your saves and settings folder:\n\n" + p.userdata.wstring() +
                              L"\n\nConnect the drive it is on and try again, or press OK to open setup "
                              L"and choose another folder.";
    if (MessageBoxW(nullptr, text.c_str(), L"GoldenEye 007", MB_OKCANCEL | MB_ICONWARNING) != IDOK) {
      CoUninitialize();
      return 1;
    }
    setup = true;
  }
  if (installed && !setup && options.package.empty()) {
    if (GetFileAttributesW(p.config.c_str()) == INVALID_FILE_ATTRIBUTES) {
      LogTo(p.logs);
      EnsureSettings(p);
    }
    RepairShortcut(p, options.shortcut_folder);
    std::wstring error;
    int code = 0;
    if (!LaunchGame(p, options.game_args, &error)) {
      LogTo(p.logs);
      Log(L"FAILED to start the game: " + error);
      MessageBoxW(nullptr, error.c_str(), L"GoldenEye 007", MB_OK | MB_ICONERROR);
      code = 1;
    }
    CoUninitialize();
    return code;
  }

  // Setup. One window per folder: a second launch brings the first to the front.
  std::wstring mutex_name = L"Local\\GoldenEye007Setup-";
  for (wchar_t c : p.root.wstring()) mutex_name += (c == L'\\' || c == L':') ? L'_' : static_cast<wchar_t>(towlower(c));
  if (mutex_name.size() > 250) mutex_name.resize(250);
  HANDLE mutex = CreateMutexW(nullptr, TRUE, mutex_name.c_str());
  if (mutex && GetLastError() == ERROR_ALREADY_EXISTS) {
    if (HWND other = FindWindowW(L"GoldenEye007Setup", nullptr)) {
      ShowWindow(other, SW_RESTORE);
      SetForegroundWindow(other);
    }
    CloseHandle(mutex);
    CoUninitialize();
    return 0;
  }

  LogTo(p.logs);
  options.forced = installed;
  options.reason = installed ? L"--setup" : L"missing " + missing;
  const int code = unattended ? RunUnattended(p, options, extras, saves_folder) : RunSetupWindow(instance, p, options);
  if (mutex) {
    ReleaseMutex(mutex);
    CloseHandle(mutex);
  }
  CoUninitialize();
  return code;
}
