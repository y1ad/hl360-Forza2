#pragma once

#include <filesystem>
#include <string>
#include <vector>

namespace launcher {

// One keyboard binding of the virtual pad (cvar keybind_<id>). `keys` uses the
// runtime's syntax: comma-separated alternatives, optional "Shift+" prefix.
struct KeyBind {
  const char* id;      // cvar suffix
  const char* label;     // shown in the launcher (English)
  const char* label_es;  // ... (Spanish)
  const char* group;     // section in the Controls page (English)
  const char* group_es;
  const char* default_keys;
  std::string keys;
};

// Everything the launcher lets the player change. Written to
// forza2.user.toml, which the game loads after its own forza2.toml.
struct Settings {
  // Game / profile
  std::string game_data_root;
  std::string gamertag = "Player";
  int user_language = 1;  // XC_LANGUAGE: 1 en, 3 de, 4 fr, 5 es, 6 it

  // Display
  bool fullscreen = true;
  bool vsync = false;
  bool experimental_120fps = false;
  int monitor = 0;
  int window_width = 1600;
  int window_height = 900;
  // Guest timing follows the launcher frame-rate selection.
  int refresh_rate = 60;
  bool present_letterbox = true;

  // Quality
  int resolution_scale = 2;      // internal render scale over 1280x720
  int anisotropic_override = 5;  // 0..5 = off,1x,2x,4x,8x,16x
  bool native_2x_msaa = true;  // Internal FM2 compatibility setting.
  std::string swap_post_effect = "none";   // none | fxaa | fxaa_extreme
  std::string present_effect = "bilinear";  // bilinear | cas | fsr

  int graphics_preset = 2;
  void ApplyGraphicsPreset(int preset);

  // Input
  std::string input_backend = "sdl";  // sdl | xinput
  bool mnk_mode = true;
  bool mnk_mouse = false;
  double mnk_sensitivity = 1.0;
  std::vector<KeyBind> binds;

  // Audio
  bool audio_mute = false;

  // Online: System Link tunnelled through the FM2 relay (tools/fm2-relay).
  // Saved as the game's fm2_relay cvar; empty means offline. The server is a
  // build setting (-DFM2_ONLINE_SERVER=host:port) so it stays out of the
  // public sources; builds without it have no online option.
  bool online = true;
  std::string online_server = kDefaultOnlineServer;
#ifndef FM2_ONLINE_SERVER
#define FM2_ONLINE_SERVER ""
#endif
  static constexpr const char* kDefaultOnlineServer = FM2_ONLINE_SERVER;

  Settings();
  void ResetBinds();
  void ResetDisplay();
  bool Load(const std::filesystem::path& file);
  bool Save(const std::filesystem::path& file) const;
};

// Gamertag rules shared with the game (src/profile.cpp): 1-15 letters,
// digits and single spaces.
bool IsValidGamertag(const std::string& name);

}  // namespace launcher
