// The on-screen controller for Android's touch screens: a steering stick on
// the left, throttle, brake and the face buttons on the right, Back and Start
// at the top. It is drawn over the game by an ImGui dialog and read by the
// title as a synthetic pad on user 0 beside any real controller. It shows
// itself on the first touch and hides after a while without one, so a player
// with a gamepad never sees it.
//
// Adapted from src/ui/touch_pad.{h,cpp} of arcanite24/pinyon-shift (BSD
// 3-Clause, Copyright (c) 2026 Pinyon Shift contributors).
#pragma once

#include <chrono>
#include <cstdint>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

#include <rex/cvar.h>
#include <rex/input/input.h>
#include <rex/input/input_driver.h>
#include <rex/ui/imgui_dialog.h>
#include <rex/ui/window_listener.h>

REXCVAR_DECLARE(bool, forza2_touch_controls);

namespace forza2 {

class TouchPad final : public rex::ui::WindowInputListener {
 public:
  // One control drawn as a disc, in window pixels.
  struct Shape {
    float x = 0.0f, y = 0.0f, radius = 0.0f;
    std::string label;
    bool pressed = false;
    bool stick = false;
    float stick_x = 0.0f, stick_y = 0.0f;  // the knob, for the stick
  };

  static TouchPad& Get();

  // The synthetic pad the title reads; owned by the input system.
  std::unique_ptr<rex::input::InputDriver> CreateDriver();

  void OnTouchEvent(rex::ui::TouchEvent& e) override;

  bool Visible() const;
  // The window's size in pixels as of the last touch (0 before the first).
  std::pair<float, float> PhysicalSize() const;
  std::vector<Shape> Shapes(float width, float height) const;
  rex::input::X_INPUT_GAMEPAD State() const;

 private:
  enum class Control : uint8_t {
    kNone,
    kStick,
    kThrottle,
    kBrake,
    kA,
    kB,
    kX,
    kY,
    kLeftBumper,
    kRightBumper,
    kBack,
    kStart,
  };
  struct Finger {
    Control control = Control::kNone;
    float origin_x = 0.0f, origin_y = 0.0f;  // the stick's centre
    float x = 0.0f, y = 0.0f;
  };
  struct Layout {
    Control control;
    const char* label;
    float x, y;    // fraction of the window width and height
    float radius;  // fraction of the window height
  };

  static const std::vector<Layout>& Controls();
  // A control's centre in window pixels, inside the safe area.
  void Place(const Layout& control, float width, float height, float& x, float& y) const;
  Control Hit(float x, float y, float width, float height) const;

  mutable std::mutex mutex_;
  std::map<uint32_t, Finger> fingers_;
  float width_ = 0.0f, height_ = 0.0f;
  // The display cutout's margins (Window::GetSafeAreaInsets), in pixels.
  float safe_left_ = 0.0f, safe_top_ = 0.0f, safe_right_ = 0.0f, safe_bottom_ = 0.0f;
  std::chrono::steady_clock::time_point last_touch_{};
};

// The dialog that draws the controls over the game. Owned by the app; reset it
// before the ImGui drawer goes away.
std::unique_ptr<rex::ui::ImGuiDialog> CreateTouchOverlay(rex::ui::ImGuiDrawer* drawer);

}  // namespace forza2
