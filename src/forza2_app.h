// forza2 - ReXGlue Recompiled Project
//
// Customize your app by overriding virtual hooks from rex::ReXApp.

#pragma once

#include <cstdlib>
#include <filesystem>
#include <system_error>

#include <rex/cvar.h>
#include <rex/filesystem.h>
#include <rex/filesystem/devices/host_path_device.h>
#include <rex/filesystem/vfs.h>
#include <rex/rex_app.h>
#include <rex/ui/overlay/achievement_notification.h>
#include <rex/system/kernel_state.h>

#include "profile.h"
#include "lan.h"

REXCVAR_DECLARE(std::string, game_data_root);
REXCVAR_DECLARE(std::string, user_data_root);

class Forza2App : public rex::ReXApp {
 public:
  using rex::ReXApp::ReXApp;

  static std::unique_ptr<rex::ui::WindowedApp> Create(
      rex::ui::WindowedAppContext& ctx) {
    return std::unique_ptr<Forza2App>(new Forza2App(ctx, "forza2",
        PPCImageConfig));
  }

  // Override virtual hooks for customization:
  // void OnPostInitLogging() override {}
  // void OnPreSetup(rex::RuntimeConfig& config) override {}
  // void OnLoadXexImage(std::string& xex_image) override {}
  // void OnPostLoadXexImage() override {}
  // void OnPostSetup() override {}
  // void OnCreateDialogs(rex::ui::ImGuiDrawer* drawer) override {}
  // std::unique_ptr<rex::ui::ImGuiDialog> CreateAchievementsOverlay() override;
  // std::unique_ptr<rex::ui::AchievementNotificationDialog>
  // CreateAchievementNotificationDialog() override;
  void OnShutdown() override { forza2::lan::Shutdown(); }

  void OnPreSetup(rex::RuntimeConfig& config) override {
    forza2::lan::ValidateConfiguration();
    // Migrate old user configs so they cannot select the legacy D3D12 plugin.
    rex::cvar::SetFlagByName("gpu_backend", "vulkan");
    rex::cvar::SetFlagByName("gpu_plugin", "fh1");
    config.gpu_plugin = "fh1";
    // FM2 must not inherit the Forza Horizon native surface executor.
    rex::cvar::SetFlagByName("vulkan_fh1_native_executor", "false");
  }

  void OnPostInitLogging() override {
    forza2::InstallCrashReporter();
  }

  // Two config files next to the exe:
  //   forza2.toml       port settings (what the title needs to run), shipped
  //   forza2.user.toml  the player's choices, written by the launcher
  // The user file loads second so it wins. It also becomes the SDK's config
  // path, because the SDK reloads that file after this hook.
  //
  // The SDK reads game_data_root/user_data_root before it loads any config,
  // so both files are loaded here first: then they can carry the game path and
  // the gamertag.
  //
  // Android has no exe folder to write to: Forza2Activity exports the folder
  // (FORZA2_ANDROID_ROOT) that holds the settings it copies out of the APK at
  // every start (forza2.toml, then forza2.android.toml over it), the player's
  // forza2.user.toml, the game (game/), the saves and caches (state/) and the
  // log (logs/).
  void OnConfigurePaths(rex::PathConfig& paths) override {
    std::filesystem::path android_root;
#if defined(__ANDROID__)
    if (const char* root = std::getenv("FORZA2_ANDROID_ROOT")) {
      android_root = root;
      std::error_code ec;
      std::filesystem::create_directories(android_root / "logs", ec);
      paths.config_path = android_root / "forza2.toml";
      paths.cache_root = android_root / "state" / "cache";
      rex::cvar::SetFlagByName("log_file", (android_root / "logs" / "forza2.log").string());
    }
#endif
    const auto user_config = paths.config_path.parent_path() / "forza2.user.toml";
    if (std::filesystem::exists(paths.config_path)) {
      rex::cvar::LoadConfig(paths.config_path);
    }
#if defined(__ANDROID__)
    if (!android_root.empty()) {
      const auto overlay = android_root / "forza2.android.toml";
      if (std::filesystem::exists(overlay)) {
        rex::cvar::LoadConfig(overlay);
      }
    }
#endif
    if (std::filesystem::exists(user_config)) {
      rex::cvar::LoadConfig(user_config);
      paths.config_path = user_config;
    }
    if (paths.game_data_root.empty()) {
      std::string from_config = REXCVAR_GET(game_data_root);
      const std::filesystem::path default_root =
          (android_root.empty() ? rex::filesystem::GetExecutableFolder() : android_root) / "game";
      paths.game_data_root =
          from_config.empty() ? default_root : std::filesystem::path(from_config);
    }
    // One user-data folder per profile, unless the user pinned one explicitly.
    if (std::string(REXCVAR_GET(user_data_root)).empty()) {
      const std::filesystem::path base =
          android_root.empty() ? paths.user_data_root : android_root / "state";
      paths.user_data_root = forza2::ProfileUserDataRoot(base);
    }
    user_data_root_ = paths.user_data_root;
    game_data_root_ = paths.game_data_root;
  }

  // The title keeps race replication data on the console's cache partition
  // (cache:\replication_stream_0000N) and uses those streams without checking
  // that they opened. The SDK leaves cache: unmounted on purpose (most titles
  // cope), so give it a real, writable folder per profile.
  void OnPostSetup() override {
    forza2::lan::ConfigureIdentity();
    const auto cache_dir = user_data_root_ / "cache-partition";
    std::error_code ec;
    std::filesystem::create_directories(cache_dir, ec);
    auto device = std::make_unique<rex::filesystem::HostPathDevice>("\\Device\\Forza2Cache",
                                                                    cache_dir, false);
    if (device->Initialize() && REX_KERNEL_FS()->RegisterDevice(std::move(device))) {
      REX_KERNEL_FS()->RegisterSymbolicLink("cache:", "\\Device\\Forza2Cache");
    }
    // The "gpu" logger only has its sinks once the runtime is set up; at
    // OnPostInitLogging there is nothing to wrap yet.
    forza2::InstallGpuLogFilter();
    if (forza2::ShaderWarmupMode()) {
      forza2::HideOwnWindows();
    } else {
      forza2::InstallBundledDlc(game_data_root_, user_data_root_);
    }
  }

  // Warm-up mode: the shader storage was just rebuilt (blocking, right before
  // this hook); quit before the title does anything. The launcher waits for
  // the process and reads the counts from the log.
  // No achievement toasts. The SDK's notification dialog stays registered with
  // the ImGui drawer even with nothing on screen, and while the drawer has a
  // dialog the presenter repaints the window on its own, at the monitor's
  // refresh rate or faster (224 presents/s for 60 game frames on a 164 Hz
  // screen). Without it the window is presented once per game frame, so the
  // frame rate a player sees is the chosen 60 or 120.
  std::unique_ptr<rex::ui::AchievementNotificationDialog> CreateAchievementNotificationDialog() override {
    return nullptr;
  }

  void OnPostLaunchModule(rex::system::XThread* thread) override {
    (void)thread;
    if (forza2::lan::SelfTestRequested()) {
      forza2::lan::RunTransportSelfTest();
      rex::FlushLogging();
      if (window()) window()->RequestClose();
      return;
    }
    if (!forza2::ShaderWarmupMode()) return;
    REXLOG_INFO("forza2: shader warm-up complete");
    rex::FlushLogging();
    if (window()) window()->RequestClose();
  }

 private:
  std::filesystem::path user_data_root_;
  std::filesystem::path game_data_root_;
};
