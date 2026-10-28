// GoldenEye 007 XBLA PC Recomp - launcher entry point for Linux (command line).
//
//   GoldenEye 007                      start the game; sets it up first when the
//                                      game data has not been imported (or a
//                                      needed file is missing)
//   GoldenEye 007 --setup              run setup again
//   GoldenEye 007 --package <file|dir> import this package (or the one found in this folder)
//   GoldenEye 007 --unattended         never ask anything, never start the game
//   GoldenEye 007 --extras a,b,c       fullscreen, shortcut, complete (skips the questions)
//   GoldenEye 007 --shortcut-folder <dir>   where the desktop entry goes (default
//                                      $XDG_DATA_HOME/applications), a test aid
//   GoldenEye 007 --list <package>     list what an Xbox 360 STFS package holds
//   GoldenEye 007 --extract <package> <outdir>   unpack any STFS package (no checks)
//   GoldenEye 007 --help | --version
//
// Exit codes of a setup without questions: 0 imported, 1 failed, 2 several
// packages found (pass --package), 3 none found.
//
// Any other arguments are passed on to the game.
#include <fcntl.h>
#include <sys/file.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <iostream>

#include <SDL3/SDL.h>

#include "launcher.h"
#include "stfs.h"

namespace ge {
const char* const kVersion = GE_VERSION;
}

namespace {

using namespace ge;

std::atomic<bool> g_cancel{false};

void OnInterrupt(int) { g_cancel.store(true); }

struct Cli {
  bool setup = false;
  bool unattended = false;
  bool terminal = false;  // --cli: no window
  bool have_extras = false;
  std::string extras;
  bool help = false, version = false;
  bool list = false, extract = false;
  fs::path list_package, extract_package, extract_out;
  SetupOptions options;
};

const char* const kHelp =
    "GoldenEye 007 (XBLA PC Recomp) launcher\n"
    "\n"
    "Usage: GoldenEye\\ 007 [options] [game arguments]\n"
    "\n"
    "  (no options)                 start the game; the first time (or when a file is\n"
    "                               missing) setup imports your game package first\n"
    "  --setup                      run setup again\n"
    "  --package <file|dir>         import this Xbox 360 package (or the one in this folder)\n"
    "  --unattended                 no questions, never starts the game. Exit code: 0 imported,\n"
    "                               1 failed, 2 several packages found, 3 none found\n"
    "  --extras a,b,c               optional extras: fullscreen, shortcut, complete\n"
    "                               (complete = 100% game completion); skips the questions\n"
    "  --shortcut-folder <dir>      put the desktop entry here instead of\n"
    "                               ~/.local/share/applications\n"
    "  --list <package>             list the files in an Xbox 360 STFS package\n"
    "  --extract <package> <outdir> unpack any STFS package into a folder (no game checks)\n"
    "  --cli                        run setup in the terminal instead of the window\n"
    "  --help, --version\n"
    "\n"
    "Other arguments are passed on to the game.\n";

bool Parse(int argc, char** argv, Cli* cli) {
  for (int i = 1; i < argc; ++i) {
    const std::string arg = argv[i];
    if (arg == "--setup") {
      cli->setup = true;
    } else if (arg == "--cli") {
      cli->terminal = true;
    } else if (arg == "--unattended") {
      cli->unattended = true;
    } else if (arg == "--package" && i + 1 < argc) {
      cli->options.package = PathFromUtf8(argv[++i]);
    } else if (arg == "--extras" && i + 1 < argc) {
      cli->extras = argv[++i];
      cli->have_extras = true;
    } else if (arg == "--shortcut-folder" && i + 1 < argc) {
      cli->options.shortcut_folder = PathFromUtf8(argv[++i]);
    } else if (arg == "--help" || arg == "-h") {
      cli->help = true;
    } else if (arg == "--version") {
      cli->version = true;
    } else if (arg == "--list" && i + 1 < argc) {
      cli->list = true;
      cli->list_package = PathFromUtf8(argv[++i]);
    } else if (arg == "--extract" && i + 2 < argc) {
      cli->extract = true;
      cli->extract_package = PathFromUtf8(argv[i + 1]);
      cli->extract_out = PathFromUtf8(argv[i + 2]);
      i += 2;
    } else {
      cli->options.game_args.push_back(arg);
    }
  }
  return true;
}

// ---------------------------------------------------------------- input --
bool Interactive() { return isatty(STDIN_FILENO) && isatty(STDOUT_FILENO); }

// One line from the player; false at end of input.
bool ReadLine(std::string* line) {
  std::fflush(stdout);
  if (!std::getline(std::cin, *line)) return false;
  const size_t a = line->find_first_not_of(" \t\r");
  const size_t b = line->find_last_not_of(" \t\r");
  *line = a == std::string::npos ? std::string() : line->substr(a, b - a + 1);
  return true;
}

bool Ask(const std::string& question, bool default_yes) {
  std::printf("%s %s ", question.c_str(), default_yes ? "[Y/n]" : "[y/N]");
  std::string line;
  if (!ReadLine(&line) || line.empty()) return default_yes;
  return line[0] == 'y' || line[0] == 'Y' ? true : (line[0] == 'n' || line[0] == 'N' ? false : default_yes);
}

// ------------------------------------------------------------- progress --
class ProgressPrinter {
 public:
  explicit ProgressPrinter(bool tty) : tty_(tty) {}

  void Update(int percent, const std::string& phase, const std::string& detail) {
    std::string text = phase;  // "IMPORTING YOUR GAME DATA" -> "Importing your game data…"
    for (size_t i = 0; i < text.size(); ++i) {
      if (i > 0 && text[i] >= 'A' && text[i] <= 'Z') text[i] = static_cast<char>(text[i] - 'A' + 'a');
    }
    percent = std::clamp(percent, 0, 100);
    std::string line = text + "… " + std::to_string(percent) + "%";
    if (!detail.empty()) line += " (" + detail + ")";
    if (tty_) {
      std::printf("\r%s\x1b[K", line.c_str());
      open_ = true;
    } else if (percent / 10 != last_decile_ || text != last_phase_) {
      std::printf("%s\n", line.c_str());
      last_decile_ = percent / 10;
      last_phase_ = text;
    }
    std::fflush(stdout);
  }

  // Ends the updating line so later output starts on a fresh one.
  void Finish() {
    if (open_) std::printf("\n");
    open_ = false;
    std::fflush(stdout);
  }

 private:
  bool tty_;
  bool open_ = false;
  int last_decile_ = -1;
  std::string last_phase_;
};

// -------------------------------------------------------- list / extract --
const char* HexTitle(uint32_t id, char* out) {
  std::snprintf(out, 16, "%08X", id);
  return out;
}

int RunList(const fs::path& file, bool extract, const fs::path& out_dir) {
  stfs::Package package;
  std::string error;
  if (!package.Open(file, &error)) {
    std::fprintf(stderr, "%s: %s\n", Utf8(file).c_str(), error.c_str());
    return 1;
  }
  uint64_t files = 0, folders = 0;
  for (const stfs::Entry& e : package.entries()) (e.directory ? folders : files)++;
  char id[16];
  std::printf("Title ID: %s  Content type: %08X\n", HexTitle(package.title_id(), id), package.content_type());
  std::printf("Entries: %zu (%llu files, %llu folders, %s)\n", package.entries().size(),
              static_cast<unsigned long long>(files), static_cast<unsigned long long>(folders),
              FormatMB(package.total_file_bytes()).c_str());
  if (!extract) {
    for (const stfs::Entry& e : package.entries()) {
      if (e.directory)
        std::printf("          <dir>  %s/\n", Utf8(e.path).c_str());
      else
        std::printf("%12u  %s\n", e.size, Utf8(e.path).c_str());
    }
    return 0;
  }
  std::error_code ec;
  fs::create_directories(out_dir, ec);
  if (ec) {
    std::fprintf(stderr, "Could not create %s: %s\n", Utf8(out_dir).c_str(), ec.message().c_str());
    return 1;
  }
  for (const stfs::Entry& e : package.entries()) {
    const fs::path target = out_dir / e.path;
    if (e.directory) {
      fs::create_directories(target, ec);
      continue;
    }
    fs::create_directories(target.parent_path(), ec);
    std::ofstream out(target, std::ios::binary | std::ios::trunc);
    if (!out) {
      std::fprintf(stderr, "Could not write %s\n", Utf8(target).c_str());
      return 1;
    }
    std::string why;
    const bool read_ok = package.Read(
        e,
        [&](const uint8_t* data, size_t size) {
          out.write(reinterpret_cast<const char*>(data), static_cast<std::streamsize>(size));
          return static_cast<bool>(out);
        },
        &why);
    out.close();
    if (!read_ok || !out) {
      std::fprintf(stderr, "Could not extract %s: %s\n", Utf8(e.path).c_str(),
                   why.empty() ? "write failed (disk full?)" : why.c_str());
      return 1;
    }
    std::printf("  %s (%u bytes)\n", Utf8(e.path).c_str(), e.size);
  }
  std::printf("Extracted %llu files to %s\n", static_cast<unsigned long long>(files), Utf8(out_dir).c_str());
  return 0;
}

// ---------------------------------------------------------------- setup --
Extras ParseExtras(const std::string& text) {
  Extras e;
  e.fullscreen = text.find("fullscreen") != std::string::npos;
  e.shortcut = text.find("shortcut") != std::string::npos;
  e.complete = text.find("complete") != std::string::npos;
  return e;
}

void PrintCandidates(const std::vector<Source>& found) {
  std::printf("Found %zu candidate%s:\n", found.size(), found.size() == 1 ? "" : "s");
  for (size_t i = 0; i < found.size(); ++i) {
    std::printf("  [%zu] %s  (%s)\n", i + 1, Describe(found[i]).c_str(), FormatMB(found[i].bytes).c_str());
  }
}

// The interactive way to a source: detected candidates, a choice among several,
// or a path typed by the player. False when the player gives up.
bool ChooseSource(const Paths& p, const Cli& cli, Source* chosen) {
  std::vector<Source> found;
  std::string why;
  if (!cli.options.package.empty()) {
    found = SourcesIn(cli.options.package, &why);
  } else {
    std::printf("Looking beside %s ...\n", Utf8(p.root).c_str());
    found = DetectSources(p);
    if (found.empty()) why = "No GoldenEye 007 package was found beside the launcher or in a \"Game Package\" folder.";
  }
  for (;;) {
    if (found.size() == 1) {
      *chosen = found[0];
      std::printf("Found %s  (%s)\n", Describe(found[0]).c_str(), FormatMB(found[0].bytes).c_str());
      return true;
    }
    if (found.size() > 1) {
      PrintCandidates(found);
      for (;;) {
        std::printf("Import which one? [1-%zu, empty to quit]: ", found.size());
        std::string line;
        if (!ReadLine(&line) || line.empty()) return false;
        const int n = std::atoi(line.c_str());
        if (n >= 1 && n <= static_cast<int>(found.size())) {
          *chosen = found[static_cast<size_t>(n - 1)];
          return true;
        }
      }
    }
    if (!why.empty()) std::printf("%s\n", why.c_str());
    std::printf("Path to your GoldenEye 007 package (file or folder, empty to quit): ");
    std::string line;
    if (!ReadLine(&line) || line.empty()) return false;
    if (line.size() >= 2 && line.front() == '\'' && line.back() == '\'') line = line.substr(1, line.size() - 2);
    why.clear();
    found = SourcesIn(PathFromUtf8(line), &why);
  }
}

int RunSetup(const Paths& p, const Cli& cli, bool installed) {
  const bool interactive = !cli.unattended && Interactive();
  // Questions start from what is set now; without them (like --unattended) only ticked extras are applied.
  const bool ask_extras = interactive && !cli.have_extras;
  const Extras was = ask_extras && installed ? CurrentExtras(p, cli.options.shortcut_folder) : Extras{};
  Source source;
  if (!interactive) {
    // As the Windows --unattended: exactly one candidate is imported, nothing is asked.
    Log("Unattended setup");
    std::vector<Source> found;
    std::string why;
    if (!cli.options.package.empty()) {
      found = SourcesIn(cli.options.package, &why);
    } else {
      found = DetectSources(p);
      why = "no package found";
    }
    if (found.size() != 1) {
      const std::string message =
          found.empty() ? why : std::to_string(found.size()) + " candidates, pass --package <file>";
      Log("Unattended: " + message);
      std::fprintf(stderr, "Setup: %s\n", message.c_str());
      if (found.size() > 1) PrintCandidates(found);
      return found.empty() ? 3 : 2;
    }
    source = found[0];
  } else {
    std::printf("GoldenEye 007 setup. Your package is only read, never changed.\n");
    if (!ChooseSource(p, cli, &source)) {
      Log("Setup: no package chosen");
      return 3;
    }
  }

  // The extras are asked once, before the import (unticked ones change nothing on a first setup).
  Extras want;
  if (cli.have_extras) {
    want = ParseExtras(cli.extras);
  } else if (ask_extras) {
    std::printf("\nOptional extras:\n");
    want.fullscreen = Ask("  Start the game in fullscreen?", was.fullscreen);
    want.shortcut = Ask("  Add GoldenEye 007 to your applications menu (desktop entry)?", was.shortcut);
    want.complete = Ask("  Unlock everything (100% game completion)?", was.complete);
    std::printf("\n");
  }

  const bool tty = isatty(STDOUT_FILENO);
  ProgressPrinter printer(tty);
  g_cancel = false;
  std::signal(SIGINT, OnInterrupt);
  const fs::path saves_from = source.kind == SourceKind::PreviousInstall ? source.path : FindPreviousInstall(p);
  const ImportResult result = Import(p, source, saves_from, g_cancel, [&](int percent, const std::string& phase, const std::string& detail) {
    printer.Update(percent, phase, detail);
  });
  std::signal(SIGINT, SIG_DFL);
  printer.Finish();
  if (!result.ok) {
    std::fprintf(stderr, "%s%s\n", result.cancelled ? "" : "Import failed: ", result.message.c_str());
    if (!result.cancelled) std::fprintf(stderr, "Details: %s\n", Utf8(p.logs / "setup.log").c_str());
    return 1;
  }

  const std::string note = ApplyExtras(
      p, want, was, cli.options.shortcut_folder,
      interactive ? std::function<bool(const fs::path&)>([](const fs::path& link) {
        std::printf("A different GoldenEye 007 entry already exists:\n  %s\n", Utf8(link).c_str());
        return Ask("Replace it with one for this folder?", false);
      })
                  : std::function<bool(const fs::path&)>());
  if (!note.empty()) std::printf("%s\n", note.c_str());

  std::printf("\nGoldenEye 007 is ready.\n");
  if (!result.migrated.empty()) std::printf("%s\n", result.migrated.c_str());
  std::printf("  Game data:           %s\n", Utf8(p.assets).c_str());
  std::printf("  Saves and settings:  %s  (settings: ge.toml)\n", Utf8(p.userdata).c_str());
  std::printf("  Logs:                %s\n", Utf8(p.logs).c_str());
  std::printf("Next time just run %s to play.\n", Utf8(p.launcher).c_str());

  if (interactive && Ask("\nPlay now?", true)) {
    std::string error;
    if (!LaunchGame(p, cli.options.game_args, &error)) {
      Log("FAILED to start the game: " + error);
      std::fprintf(stderr, "%s\n", error.c_str());
      return 1;
    }
    Log("Play: game started.");
  }
  return 0;
}

// Setup runs once per folder at a time: the lock lives as long as this process
// (and is not handed to the game: close-on-exec). Returns false when another
// setup already holds it.
bool TakeSetupLock(const Paths& p) {
  std::error_code ec;
  fs::create_directories(p.resources, ec);
  const int fd = open((p.resources / ".setup.lock").c_str(), O_CREAT | O_RDWR | O_CLOEXEC, 0644);
  if (fd < 0) return true;  // cannot lock (read-only folder): the import will say so
  return flock(fd, LOCK_EX | LOCK_NB) == 0;
}

bool HasDisplay() {
  const char* wayland = std::getenv("WAYLAND_DISPLAY");
  const char* x11 = std::getenv("DISPLAY");
  return (wayland && *wayland) || (x11 && *x11);
}

}  // namespace

int main(int argc, char** argv) {
  Cli cli;
  Parse(argc, argv, &cli);
  if (cli.help) {
    std::fputs(kHelp, stdout);
    return 0;
  }
  if (cli.version) {
    std::printf("GoldenEye 007 launcher %s\n", kVersion);
    return 0;
  }
  if (cli.list) return RunList(cli.list_package, false, {});
  if (cli.extract) return RunList(cli.extract_package, true, cli.extract_out);

  const Paths p = MakePaths();

  // The normal case: everything is in place, so start the game at once.
  std::string missing;
  bool installed = IsInstalled(p, &missing);
  if (!installed) {
    RecoverInterruptedSwap(p);
    installed = IsInstalled(p, &missing);
  }
  if (installed && !cli.setup && cli.options.package.empty()) {
    std::error_code ec;
    if (!fs::exists(p.config, ec)) {
      LogTo(p.logs);
      EnsureSettings(p);
    }
    RepairShortcut(p, cli.options.shortcut_folder);
    std::string error;
    if (!LaunchGame(p, cli.options.game_args, &error)) {
      LogTo(p.logs);
      Log("FAILED to start the game: " + error);
      std::fprintf(stderr, "%s\n", error.c_str());
      return 1;
    }
    return 0;
  }

  // Setup. One per folder: a second launch tells the player and leaves.
  if (!TakeSetupLock(p)) {
    std::fprintf(stderr, "Setup is already running for this folder.\n");
    return 0;
  }
  LogTo(p.logs);
  Log(std::string("Setup opened (") + (installed ? "--setup" : "missing " + missing) + ")");
  if (!installed) std::printf("Needed file missing: %s\n", missing.c_str());
  if (!cli.unattended && !cli.terminal && HasDisplay() && SDL_Init(SDL_INIT_VIDEO)) {
    const int code = RunSetupWindow(p, cli.options);
    SDL_Quit();
    if (code >= 0) return code;
  }
  return RunSetup(p, cli, installed);
}
