#include "settings.h"

#include <fstream>
#include <algorithm>

#include <toml++/toml.hpp>

namespace launcher {

namespace {

// Defaults are the runtime's own (src/input/mnk/mnk_input_driver.cpp), except
// throttle/brake which also get E/Q as their first, easiest key.
const KeyBind kDefaultBinds[] = {
    {"right_trigger", "Accelerate  (RT)", "Acelerar  (RT)", "Driving", "Conducción", "E,O", {}},
    {"left_trigger", "Brake  (LT)", "Frenar  (LT)", "Driving", "Conducción", "Q,I", {}},
    {"lstick_left", "Steer left", "Girar a la izquierda", "Driving", "Conducción", "A", {}},
    {"lstick_right", "Steer right", "Girar a la derecha", "Driving", "Conducción", "D", {}},
    {"lstick_up", "Left stick up", "Stick izquierdo arriba", "Driving", "Conducción", "W", {}},
    {"lstick_down", "Left stick down", "Stick izquierdo abajo", "Driving", "Conducción", "S", {}},
    {"a", "A", "A", "Buttons", "Botones", "Semicolon,Space", {}},
    {"b", "B", "B", "Buttons", "Botones", "Quote,Backspace", {}},
    {"x", "X", "X", "Buttons", "Botones", "L", {}},
    {"y", "Y", "Y", "Buttons", "Botones", "P", {}},
    {"left_shoulder", "LB", "LB", "Buttons", "Botones", "1", {}},
    {"right_shoulder", "RB", "RB", "Buttons", "Botones", "3", {}},
    {"start", "Start", "Start", "Buttons", "Botones", "X,Return", {}},
    {"back", "Back", "Back", "Buttons", "Botones", "Z,Tab", {}},
    {"lstick_press", "Left stick click", "Pulsar stick izquierdo", "Buttons", "Botones", "F", {}},
    {"dpad_up", "Up", "Arriba", "D-pad", "Cruceta", "Shift+Up", {}},
    {"dpad_down", "Down", "Abajo", "D-pad", "Cruceta", "Shift+Down", {}},
    {"dpad_left", "Left", "Izquierda", "D-pad", "Cruceta", "Shift+Left", {}},
    {"dpad_right", "Right", "Derecha", "D-pad", "Cruceta", "Shift+Right", {}},
    {"rstick_up", "Look up", "Mirar arriba", "Camera", "Cámara", "Up", {}},
    {"rstick_down", "Look down", "Mirar abajo", "Camera", "Cámara", "Down", {}},
    {"rstick_left", "Look left", "Mirar a la izquierda", "Camera", "Cámara", "Left", {}},
    {"rstick_right", "Look right", "Mirar a la derecha", "Camera", "Cámara", "Right", {}},
    {"rstick_press", "Right stick click", "Pulsar stick derecho", "Camera", "Cámara", "K", {}},
};

template <typename T>
void Read(const toml::table& t, const char* key, T& out) {
  if (auto v = t[key].value<T>()) out = *v;
}

}  // namespace

Settings::Settings() { ResetBinds(); }

void Settings::ApplyGraphicsPreset(int preset) {
  graphics_preset = std::clamp(preset, 0, 3);
  if (graphics_preset == 3) return;
  resolution_scale = graphics_preset == 0 ? 1 : 2;
  anisotropic_override = 3 + graphics_preset;
  present_effect = graphics_preset == 2 ? "bilinear" : "fsr";
  swap_post_effect = "none";
}

void Settings::ResetBinds() {
  binds.assign(std::begin(kDefaultBinds), std::end(kDefaultBinds));
  for (auto& b : binds) b.keys = b.default_keys;
}

void Settings::ResetDisplay() {
  Settings defaults;
  fullscreen = defaults.fullscreen;
  vsync = defaults.vsync;
  experimental_120fps = defaults.experimental_120fps;
  monitor = defaults.monitor;
  window_width = defaults.window_width;
  window_height = defaults.window_height;
  refresh_rate = defaults.refresh_rate;
  present_letterbox = defaults.present_letterbox;
  resolution_scale = defaults.resolution_scale;
  anisotropic_override = defaults.anisotropic_override;
  native_2x_msaa = defaults.native_2x_msaa;
  swap_post_effect = defaults.swap_post_effect;
  present_effect = defaults.present_effect;
  ApplyGraphicsPreset(2);
}

bool Settings::Load(const std::filesystem::path& file) {
  toml::table t;
  try {
    t = toml::parse_file(file.u8string());
  } catch (const toml::parse_error&) {
    return false;
  }
  Read(t, "game_data_root", game_data_root);
  Read(t, "gamertag", gamertag);
  int64_t language = user_language;
  Read(t, "user_language", language);
  user_language = int(language);

  Read(t, "fullscreen", fullscreen);
  int64_t v = monitor;
  Read(t, "monitor", v), monitor = int(v);
  v = window_width, Read(t, "window_width", v), window_width = v > 0 ? int(v) : window_width;
  v = window_height, Read(t, "window_height", v), window_height = v > 0 ? int(v) : window_height;
  Read(t, "vsync", vsync);
  double hz = 60.0;
  Read(t, "video_mode_refresh_rate", hz);
  experimental_120fps = hz == 120.0;
  refresh_rate = experimental_120fps ? 120 : 60;
  Read(t, "present_letterbox", present_letterbox);

  v = resolution_scale, Read(t, "resolution_scale", v), resolution_scale = int(v);
  v = anisotropic_override, Read(t, "anisotropic_override", v), anisotropic_override = int(v);
  // FM2 keeps its native MSAA path enabled; no separate technical toggle.
  native_2x_msaa = true;
  if (resolution_scale < 1 || resolution_scale > 4) resolution_scale = 1;
  if (anisotropic_override < 0 || anisotropic_override > 5) anisotropic_override = 3;
  Read(t, "swap_post_effect", swap_post_effect);
  Read(t, "present_effect", present_effect);
  if (present_effect != "bilinear" && present_effect != "cas" && present_effect != "fsr")
    present_effect = "fsr";
  swap_post_effect = "none";
  int64_t preset = 3; // Preserve legacy individual settings as Custom.
  Read(t, "graphics_preset", preset);
  ApplyGraphicsPreset(int(std::clamp<int64_t>(preset, 0, 3)));

  Read(t, "input_backend", input_backend);
  Read(t, "mnk_mode", mnk_mode);
  Read(t, "mnk_mouse", mnk_mouse);
  Read(t, "mnk_sensitivity", mnk_sensitivity);
  for (auto& b : binds) {
    Read(t, (std::string("keybind_") + b.id).c_str(), b.keys);
  }
  Read(t, "audio_mute", audio_mute);
  // Profiles saved before online play have no fm2_relay: keep the default.
  if (auto relay = t["fm2_relay"].value<std::string>()) {
    online = !relay->empty();
    // The build's server wins, so a server move only needs a new launcher.
    if (online && online_server.empty()) online_server = *relay;
  }
  return true;
}

bool Settings::Save(const std::filesystem::path& file) const {
  toml::table t;
  t.insert("game_data_root", game_data_root);
  t.insert("gamertag", gamertag);
  t.insert("user_language", user_language);

  t.insert("gpu_plugin", "fh1");
  t.insert("gpu_backend", "vulkan");
  // FM2 Xenia Vulkan profile: generic framebuffers, no FH1 native executor.
  t.insert("clear_memory_page_state", false);
  t.insert("depth_float24_convert_in_pixel_shader", false);
  t.insert("depth_float24_round", false);
  t.insert("draw_resolution_scaled_texture_offsets", true);
  t.insert("gpu_3d_to_2d_texture", true);
  t.insert("gpu_allow_invalid_fetch_constants", true);
  t.insert("readback_memexport", false);
  t.insert("readback_resolve", "none");
  t.insert("readback_resolve_half_pixel_offset", false);
  t.insert("resolve_resolution_scale_fill_half_pixel_offset", true);
  t.insert("use_fuzzy_alpha_epsilon", false);
  t.insert("depth_transfer_not_equal_test", true);
  t.insert("execute_unclipped_draw_vs_on_cpu", true);
  t.insert("execute_unclipped_draw_vs_on_cpu_for_psi_render_backend", true);
  t.insert("execute_unclipped_draw_vs_on_cpu_with_scissor", false);
  t.insert("gamma_render_target_as_unorm16", true);
  t.insert("half_pixel_offset", true);
  t.insert("mrt_edram_used_range_clamp_to_min", true);
  t.insert("non_seamless_cube_map", true);
  t.insert("snorm16_render_target_full_range", true);
  t.insert("store_shaders", true);
  t.insert("texture_cache_memory_limit_hard", 768);
  t.insert("texture_cache_memory_limit_render_to_texture", 24);
  t.insert("texture_cache_memory_limit_soft", 384);
  t.insert("texture_cache_memory_limit_soft_lifetime", 30);
  t.insert("vulkan_allow_present_mode_fifo_relaxed", true);
  t.insert("vulkan_allow_present_mode_immediate", true);
  t.insert("vulkan_allow_present_mode_mailbox", true);
  t.insert("vulkan_depth_bias_force_unorm", true);
  t.insert("vulkan_pipeline_creation_threads", -1);
  t.insert("vulkan_sparse_shared_memory", true);
  t.insert("vulkan_fh1_native_executor", false);
  t.insert("render_target_path_vulkan", "fbo");
  t.insert("fh1_scaled_msaa_single_sample", false);
  t.insert("host_present_fps_limit", experimental_120fps ? 120 : 60);
  t.insert("fh1_color_overwrite_claims", false);
  t.insert("fh1_scrambled_stencil_claims", false);
  t.insert("fh1_surfaceless_clears", false);
  t.insert("fullscreen", fullscreen);
  t.insert("monitor", monitor);
  t.insert("window_width", window_width);
  t.insert("window_height", window_height);
  t.insert("vsync", vsync);
  t.insert("video_mode_refresh_rate", experimental_120fps ? 120.0 : 60.0);
  t.insert("present_letterbox", present_letterbox);

  t.insert("resolution_scale", resolution_scale);
  t.insert("anisotropic_override", anisotropic_override);
  t.insert("native_2x_msaa", true);
  t.insert("swap_post_effect", "none");
  t.insert("present_effect", present_effect);
  t.insert("graphics_preset", graphics_preset);
  t.insert("video_mode_width", 1280);
  t.insert("video_mode_height", 720);
  t.insert("present_fsr_sharpness_reduction", 0.2);

  t.insert("input_backend", input_backend);
  t.insert("mnk_mode", mnk_mode);
  t.insert("mnk_mouse", mnk_mouse);
  t.insert("mnk_sensitivity", mnk_sensitivity);
  for (const auto& b : binds) {
    t.insert(std::string("keybind_") + b.id, b.keys);
  }
  // Same asynchronous Vulkan pipeline policy as the new FM4 runtime.
  t.insert("async_shader_compilation", true);
  // Keep first-use driver compilation off the render thread.
  t.insert("vulkan_async_pipeline_submission_wait", false);
  t.insert("vulkan_async_pipeline_skip_pending", true);
  t.insert("frame_stats", false);
  // FM2 has no render-readback requirement like FM3/FM4's road/thumbnail path.
  t.insert("audio_maxqframes", 24);
  t.insert("audio_mute", audio_mute);
  t.insert("fm2_relay", online && !online_server.empty() ? online_server : std::string());

  std::ofstream out(file, std::ios::trunc);
  if (!out) return false;
  out << "# Written by Forza2Launcher. The game loads this after forza2.toml,\n"
         "# so these values win. Edit from the launcher.\n\n"
      << t << "\n";
  return bool(out);
}

bool IsValidGamertag(const std::string& name) {
  if (name.empty() || name.size() > 15 || name.front() == ' ' || name.back() == ' ') {
    return false;
  }
  char prev = 0;
  for (char c : name) {
    const bool alnum = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9');
    if (!alnum && !(c == ' ' && prev != ' ')) return false;
    prev = c;
  }
  return true;
}

}  // namespace launcher
