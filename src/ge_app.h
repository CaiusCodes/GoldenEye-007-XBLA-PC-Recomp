
// ge - ReXGlue Recompiled Project
//
// This file is yours to edit. 'rexglue migrate' will NOT overwrite it.
// Customize your app by overriding virtual hooks from rex::ReXApp.

#pragma once

#include <rex/cvar.h>
#include <rex/filesystem.h>
#include <rex/system.h>
#include <rex/rex_app.h>
#include <rex/system/kernel_state.h>
#include <rex/system/xam/user_profile.h>
#include <rex/ui/keybinds.h>
#include <rex/ui/window.h>
#include <rex/ui/windowed_app_context.h>

#include <functional>
#include <string>

#include "ge_menu.h"
#include "ge_postfx.h"

// Relaunch the current executable as a fresh process (implemented in
// ge_hooks.cpp, which owns the Win32 includes). Used by the ONLINE menu's
// "Save & Restart" so username/server/enable changes take effect on a clean
// boot -- they are read at startup (UserProfile ctor, online client start).
namespace ge {
void LaunchSelfDetached();
// Start a fresh copy of the game and end this one at once (settings saved first).
[[noreturn]] void RestartSelf();
// Windowed mode: resize the game window's client area (call on the UI thread).
void ResizeGameWindow(int w, int h);
// Native Video Settings page: resize the window for a chosen resolution.
void SetWindowSizeHandler(std::function<void(int, int)> handler);
// Start the raw-mouse + cursor-capture thread at startup. Implemented in
// ge_hooks.cpp.
void InitMouseLook();
bool MigrateKeymap();  // ge_hooks.cpp: true when saved action keys were reset
// Suppress mouse-look while the pause menu is open (cursor is needed for the
// menu, and motion shouldn't turn into look). Implemented in ge_hooks.cpp.
void SetMouselookSuppressed(bool suppressed);
// Lets the main menu's PC SETTINGS item (ge_open_pc_settings) open the menu.
void SetPcSettingsOpener(std::function<void()> opener);
// Show or hide the Windows pointer over the game (the watch pause menu shows it).
void SetPointerVisibleHandler(std::function<void(bool)> handler);
// Native PC settings pages (ge_native_menu.cpp): switch fullscreen, save the
// config, and save-and-restart for settings read at startup.
void SetNativeMenuCallbacks(std::function<void(bool)> fullscreen, std::function<void()> persist,
                            std::function<void()> restart);
}

class GeApp : public rex::ReXApp {
 public:
  using rex::ReXApp::ReXApp;

  static std::unique_ptr<rex::ui::WindowedApp> Create(
      rex::ui::WindowedAppContext& ctx) {
    return std::unique_ptr<GeApp>(new GeApp(ctx, "ge",
        PPCImageConfig));
  }

  // GoldenEye boot defaults. Runs before the config file is loaded, so these
  // are just defaults -- ge.toml (written by the in-game menu) overrides them.
  void OnConfigurePaths(rex::PathConfig& paths) override {
    // A normal Windows release keeps the assets directory beside GoldenEye.exe.
    // Retain an explicit --game_data_root override, but make Explorer
    // double-click launches work without requiring a command-line argument.
    if (paths.game_data_root.empty()) {
      const auto local_assets =
          rex::filesystem::GetExecutableFolder() / "assets";
      if (std::filesystem::is_directory(local_assets)) {
        paths.game_data_root = local_assets;
      } else {
        // Say what to do instead of the SDK's "--game_data_root" message,
        // which names a command-line flag the player never typed.
        rex::ShowSimpleMessageBox(
            rex::SimpleMessageBoxType::Error,
            "GoldenEye 007's game data is not installed yet.\n\n"
            "Run \"Setup GoldenEye 007.exe\" from the release folder, select your "
            "Xbox 360 GoldenEye 007 package, then launch the game again.");
      }
    }
    // Portable install: saves and caches live beside the game (Game\userdata),
    // like the PD and Hexic ports, instead of in the Windows user profile.
    const auto user_data = rex::filesystem::GetExecutableFolder() / "userdata";
    paths.user_data_root = user_data;
    paths.cache_root = user_data / "cache";
    // NOTE: vsync is NOT forced here. Its SDK default is false (off), so the
    // in-menu toggle persists: turning it ON differs from default -> written to
    // ge.toml; OFF == default -> not written but still boots off. Forcing it here
    // would re-assert off every boot and the "on" choice would never survive a
    // restart (SaveConfig only writes cvars that differ from their default).
    // SDK 0.9+: the Xenos GPU lives in a plugin DLL and is off unless named.
    rex::cvar::SetFlagByName("gpu_plugin", "xenos");
    // max_fps defaults to 60 in the SDK (forcing it here did not survive the
    // config load, so new installs ran uncapped).
    // Windowed mode opens at 720p; fullscreen uses the display's own size.
    rex::cvar::SetFlagByName("window_width", "1280");
    rex::cvar::SetFlagByName("window_height", "720");
    // NOTE: fullscreen is NOT forced here. Its default is set to true at the
    // framework level (window.cpp) instead. That makes "windowed" the
    // non-default value, so toggling to windowed actually saves to ge.toml --
    // SaveConfig only writes cvars that differ from their default. Forcing
    // fullscreen=true here would re-assert it every boot and the windowed
    // choice would never persist. The throttle is the same story: its default
    // lives in its REXCVAR_DEFINE and it is tuned live from the pause menu, so
    // it is never written here (writing default==default is a no-op anyway).
  }

  // Register the ESC pause-menu keybind and create the always-on Post-FX
  // filter overlay once the ImGui drawer exists.
  void OnCreateDialogs(rex::ui::ImGuiDrawer* drawer) override {
    // Window/taskbar title shown while running. Overrides the SDK default
    // ("ge <build stamp>"); the internal app name stays "ge" so ge.toml and the
    // user data dir are unchanged.
    if (window()) window()->SetTitle("GoldenEye 007");
    // The PC menu opens from the main menu's PC SETTINGS item. Esc is the
    // game's Start button now, so here it only closes the PC menu.
    rex::ui::RegisterBind("bind_pause_menu", "Escape", "Close PC menu", [this] {
      if (menu_) menu_->RequestClose();
    });
    ge::SetPcSettingsOpener([this] {
      app_context().CallInUIThreadDeferred([this] {
        if (!menu_) TogglePauseMenu();
      });
    });
    ge::SetWindowSizeHandler([this](int w, int h) {
      app_context().CallInUIThreadDeferred([w, h] { ge::ResizeGameWindow(w, h); });
    });
    ge::SetPointerVisibleHandler([this](bool visible) {
      app_context().CallInUIThreadDeferred([this, visible] {
        if (window() && !menu_)
          window()->SetCursorVisibility(visible ? rex::ui::Window::CursorVisibility::kVisible
                                                : rex::ui::Window::CursorVisibility::kHidden);
      });
    });
    ge::SetNativeMenuCallbacks(
        [this](bool fs) {
          app_context().CallInUIThreadDeferred([this, fs] {
            if (window()) window()->SetFullscreen(fs);
            if (!fs) {
              // Back to windowed: use the Resolution chosen on the Video page.
              const int w = std::atoi(rex::cvar::GetFlagByName("ge_window_width").c_str());
              const int h = std::atoi(rex::cvar::GetFlagByName("ge_window_height").c_str());
              app_context().CallInUIThreadDeferred([w, h] { ge::ResizeGameWindow(w, h); });
            }
          });
        },
        [this] { PersistConfig(); },
        [this] {
          app_context().CallInUIThreadDeferred([this] {
            PersistConfig();
            ge::RestartSelf();
          });
        });
    // The game draws its own crosshair cursor in menus, so the Windows pointer
    // stays hidden except while the PC menu is open.
    if (window()) window()->SetCursorVisibility(rex::ui::Window::CursorVisibility::kHidden);
    if (ge::MigrateKeymap()) PersistConfig();
    // Windowed size chosen on the Video Settings page. The SDK's own
    // window_width/height stay 1280x720: it also uses them as the guest video
    // mode, which must not change.
    {
      const int w = std::atoi(rex::cvar::GetFlagByName("ge_window_width").c_str());
      const int h = std::atoi(rex::cvar::GetFlagByName("ge_window_height").c_str());
      if ((w != 1280 || h != 720) && window() && !window()->IsFullscreen())
        app_context().CallInUIThreadDeferred([w, h] { ge::ResizeGameWindow(w, h); });
    }  // older saved action keys -> new layout
    ge::InitMouseLook();  // start raw-mouse capture/look thread
    backdrop_bars_ = std::make_unique<ge::BackdropBars>(drawer);  // under the filter
    postfx_ = std::make_unique<ge::PostFxOverlay>(drawer);
    // Username/server are set in the ONLINE pause-menu tab now -- no first-boot
    // prompt. They apply on the Save & Restart the ONLINE tab triggers.
  }

  // Tear down the menu, overlay and keybind before the drawer is destroyed.
  void OnShutdown() override {
    rex::ui::UnregisterBind("bind_pause_menu");
    if (menu_) {
      // Direct delete (not Close()) so we don't re-enter pause bookkeeping
      // during shutdown; removes itself from the drawer in its destructor.
      delete menu_;
      menu_ = nullptr;
    }
    postfx_.reset();
    backdrop_bars_.reset();
  }

 private:
  void PersistConfig() {
    rex::cvar::SaveConfig(rex::filesystem::GetExecutableFolder() / "ge.toml");
  }

  // ESC handler: open or close the menu. The game keeps running underneath.
  void TogglePauseMenu() {
    if (menu_) {
      menu_->RequestClose();  // on_closed clears menu_
      return;
    }
    GeMenuDialog::Callbacks cb;
    cb.on_closed = [this] {
      menu_ = nullptr;
      ge::SetMouselookSuppressed(false);  // re-enable mouse-look on menu close
      app_context().CallInUIThreadDeferred([this] {
        if (window()) window()->SetCursorVisibility(rex::ui::Window::CursorVisibility::kHidden);
      });
    };
    cb.on_quit = [this] {
      if (runtime() && runtime()->kernel_state()) {
        runtime()->kernel_state()->TerminateTitle();
      }
      app_context().QuitFromUIThread();
    };
    cb.get_fullscreen = [this] { return window() && window()->IsFullscreen(); };
    cb.request_fullscreen = [this](bool v) {
      // Persist the choice: update the cvar (so SaveConfig writes it) and flush
      // ge.toml now. Without this the window changes but reverts next boot.
      rex::cvar::SetFlagByName("fullscreen", v ? "true" : "false");
      PersistConfig();
      // Defer off the paint thread: applying a window/surface change from inside
      // the ImGui draw (which runs during the presenter's paint) tears down the
      // surface being painted and crashes. Running it from the UI loop between
      // frames is the same safe path as a normal window resize.
      app_context().CallInUIThreadDeferred([this, v] {
        if (window()) window()->SetFullscreen(v);
      });
    };
    cb.persist_config = [this] { PersistConfig(); };
    cb.request_restart = [this] {
      // ONLINE tab "Save & Restart": the menu has already persisted the cvars;
      // launch a fresh process (which reads the new ge.toml at boot) then tear
      // this one down. Deferred to the UI thread -- never quit/relaunch from
      // inside the paint (same reason as request_fullscreen).
      app_context().CallInUIThreadDeferred([this] {
        PersistConfig();
        ge::RestartSelf();
      });
    };
    ge::SetMouselookSuppressed(true);  // freeze mouse-look while the menu is up
    if (window()) window()->SetCursorVisibility(rex::ui::Window::CursorVisibility::kVisible);
    menu_ = new GeMenuDialog(imgui_drawer(), std::move(cb));
  }

  GeMenuDialog* menu_ = nullptr;  // non-owning; self-deletes via the drawer
  std::unique_ptr<ge::BackdropBars> backdrop_bars_;  // menu desk picture in side bars
  std::unique_ptr<ge::PostFxOverlay> postfx_;       // always-on filter layer
};
