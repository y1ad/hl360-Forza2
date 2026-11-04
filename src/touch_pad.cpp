// See touch_pad.h. Adapted from src/ui/touch_pad.cpp of arcanite24/pinyon-shift
// (BSD 3-Clause, Copyright (c) 2026 Pinyon Shift contributors).
#include "touch_pad.h"

#include <algorithm>
#include <cmath>

#include <imgui.h>

#include <rex/logging.h>
#include <rex/ui/ui_event.h>
#include <rex/ui/window.h>

REXCVAR_DEFINE_BOOL(forza2_touch_controls, true, "Forza2",
                    "On-screen controls for touch screens: a steering stick, throttle, brake "
                    "and buttons, shown on a touch and hidden after a while without one")
    .lifecycle(rex::cvar::Lifecycle::kRequiresRestart);

namespace forza2 {
namespace {

using rex::input::DeviceId;
using rex::X_RESULT;
using rex::X_STATUS;

constexpr DeviceId kTouchDevice = static_cast<DeviceId>(0x54434850);  // TCHP
// Hidden this long after the last touch: a controller player never sees it.
constexpr auto kVisibleAfterTouch = std::chrono::seconds(20);
// The stick's travel, as a fraction of the window height.
constexpr float kStickTravel = 0.11f;

class TouchPadDriver final : public rex::input::InputDriver {
 public:
  explicit TouchPadDriver(TouchPad& pad) : InputDriver(nullptr, 0), pad_(pad) {}
  X_STATUS Setup() override { return X_STATUS_SUCCESS; }
  void EnumerateDevices(std::vector<rex::input::DeviceInfo>& out) override {
    rex::input::DeviceInfo info;
    info.id = kTouchDevice;
    info.name = "Touch screen controls";
    info.synthetic = true;
    out.push_back(std::move(info));
  }
  X_RESULT GetDeviceState(DeviceId id, rex::input::X_INPUT_STATE* out) override {
    if (id != kTouchDevice) {
      return X_ERROR_DEVICE_NOT_CONNECTED;
    }
    if (out) {
      *out = {};
      out->gamepad = pad_.State();
      out->packet_number = ++packet_;
    }
    return X_ERROR_SUCCESS;
  }
  X_RESULT GetDeviceCapabilities(DeviceId id, uint32_t,
                                 rex::input::X_INPUT_CAPABILITIES* out) override {
    if (id != kTouchDevice) {
      return X_ERROR_DEVICE_NOT_CONNECTED;
    }
    if (out) {
      *out = {};
      out->type = 1;
      out->sub_type = 1;
      out->gamepad.buttons = UINT16_MAX;
      out->gamepad.left_trigger = UINT8_MAX;
      out->gamepad.right_trigger = UINT8_MAX;
      out->gamepad.thumb_lx = INT16_MAX;
      out->gamepad.thumb_ly = INT16_MAX;
    }
    return X_ERROR_SUCCESS;
  }
  X_RESULT SetDeviceVibration(DeviceId id, rex::input::X_INPUT_VIBRATION*) override {
    return id == kTouchDevice ? X_ERROR_SUCCESS : X_ERROR_DEVICE_NOT_CONNECTED;
  }
  X_RESULT GetDeviceKeystroke(DeviceId id, uint32_t, rex::input::X_INPUT_KEYSTROKE*) override {
    return id == kTouchDevice ? X_ERROR_EMPTY : X_ERROR_DEVICE_NOT_CONNECTED;
  }

 private:
  TouchPad& pad_;
  uint32_t packet_ = 0;
};

class TouchOverlay final : public rex::ui::ImGuiDialog {
 public:
  explicit TouchOverlay(rex::ui::ImGuiDrawer* drawer) : rex::ui::ImGuiDialog(drawer) {}

 protected:
  void OnDraw(ImGuiIO& io) override {
    TouchPad& pad = TouchPad::Get();
    if (!pad.Visible()) {
      return;
    }
    const auto [width, height] = pad.PhysicalSize();
    if (width <= 0.0f || height <= 0.0f) {
      return;
    }
    // The shapes are in window pixels; ImGui may work in logical units.
    const float scale = io.DisplaySize.x / width;
    ImDrawList* draw = ImGui::GetForegroundDrawList();
    for (const TouchPad::Shape& shape : pad.Shapes(width, height)) {
      const ImVec2 centre(shape.x * scale, shape.y * scale);
      const float radius = shape.radius * scale;
      if (shape.stick) {
        draw->AddCircleFilled(centre, radius, IM_COL32(255, 255, 255, 28), 48);
        draw->AddCircle(centre, radius, IM_COL32(255, 255, 255, 120), 48, 2.0f);
        const ImVec2 knob(shape.stick_x * scale, shape.stick_y * scale);
        draw->AddCircleFilled(knob, radius * 0.45f, IM_COL32(255, 255, 255, shape.pressed ? 130 : 70),
                              32);
        continue;
      }
      draw->AddCircleFilled(centre, radius, IM_COL32(255, 255, 255, shape.pressed ? 110 : 45), 40);
      draw->AddCircle(centre, radius, IM_COL32(255, 255, 255, 160), 40, 2.0f);
      if (!shape.label.empty()) {
        const ImVec2 text = ImGui::CalcTextSize(shape.label.c_str());
        draw->AddText(ImVec2(centre.x - text.x * 0.5f, centre.y - text.y * 0.5f),
                      IM_COL32(255, 255, 255, 230), shape.label.c_str());
      }
    }
  }
};

}  // namespace

TouchPad& TouchPad::Get() {
  static TouchPad pad;
  return pad;
}

const std::vector<TouchPad::Layout>& TouchPad::Controls() {
  // Thumb reach on a landscape phone: throttle under the right thumb with
  // brake beside it, the face buttons above, the stick anywhere in the lower
  // left (its centre is where the finger lands).
  static const std::vector<Layout> controls = {
      {Control::kThrottle, "RT", 0.905f, 0.78f, 0.13f},
      {Control::kBrake, "LT", 0.745f, 0.84f, 0.10f},
      {Control::kA, "A", 0.83f, 0.52f, 0.075f},
      {Control::kB, "B", 0.93f, 0.43f, 0.075f},
      {Control::kX, "X", 0.73f, 0.43f, 0.075f},
      {Control::kY, "Y", 0.83f, 0.34f, 0.075f},
      {Control::kLeftBumper, "LB", 0.07f, 0.20f, 0.075f},
      {Control::kRightBumper, "RB", 0.93f, 0.20f, 0.075f},
      {Control::kBack, "BACK", 0.40f, 0.08f, 0.06f},
      {Control::kStart, "START", 0.60f, 0.08f, 0.06f},
  };
  return controls;
}

std::unique_ptr<rex::input::InputDriver> TouchPad::CreateDriver() {
  return std::make_unique<TouchPadDriver>(*this);
}

void TouchPad::Place(const Layout& control, float width, float height, float& x, float& y) const {
  // The layout's fractions span the safe area, so no control sits under a
  // camera cutout or a rounded corner.
  const float safe_width = std::max(1.0f, width - safe_left_ - safe_right_);
  const float safe_height = std::max(1.0f, height - safe_top_ - safe_bottom_);
  x = safe_left_ + control.x * safe_width;
  y = safe_top_ + control.y * safe_height;
}

TouchPad::Control TouchPad::Hit(float x, float y, float width, float height) const {
  Control best = Control::kNone;
  float best_distance = 0.0f;
  for (const Layout& control : Controls()) {
    float control_x, control_y;
    Place(control, width, height, control_x, control_y);
    const float dx = x - control_x;
    const float dy = y - control_y;
    // A generous target: a thumb lands off-centre.
    const float reach = control.radius * height * 1.25f;
    const float distance = std::sqrt(dx * dx + dy * dy);
    if (distance <= reach && (best == Control::kNone || distance < best_distance)) {
      best = control.control;
      best_distance = distance;
    }
  }
  if (best == Control::kNone && x < width * 0.45f && y > height * 0.35f) {
    return Control::kStick;
  }
  return best;
}

void TouchPad::OnTouchEvent(rex::ui::TouchEvent& e) {
  const auto* window = e.target();
  if (!window) {
    return;
  }
  const float width = float(window->GetActualPhysicalWidth());
  const float height = float(window->GetActualPhysicalHeight());
  if (width <= 0.0f || height <= 0.0f) {
    return;
  }
  std::lock_guard lock(mutex_);
  width_ = width;
  height_ = height;
  const auto insets = window->GetSafeAreaInsets();
  safe_left_ = float(insets.left);
  safe_top_ = float(insets.top);
  safe_right_ = float(insets.right);
  safe_bottom_ = float(insets.bottom);
  last_touch_ = std::chrono::steady_clock::now();
  switch (e.action()) {
    case rex::ui::TouchEvent::Action::kDown: {
      Finger finger;
      finger.control = Hit(e.x(), e.y(), width, height);
      finger.origin_x = finger.x = e.x();
      finger.origin_y = finger.y = e.y();
      fingers_[e.pointer_id()] = finger;
      break;
    }
    case rex::ui::TouchEvent::Action::kMove: {
      auto it = fingers_.find(e.pointer_id());
      if (it != fingers_.end()) {
        it->second.x = e.x();
        it->second.y = e.y();
      }
      break;
    }
    case rex::ui::TouchEvent::Action::kUp:
    case rex::ui::TouchEvent::Action::kCancel:
      fingers_.erase(e.pointer_id());
      break;
  }
}

bool TouchPad::Visible() const {
  std::lock_guard lock(mutex_);
  return last_touch_ != std::chrono::steady_clock::time_point{} &&
         std::chrono::steady_clock::now() - last_touch_ < kVisibleAfterTouch;
}

std::pair<float, float> TouchPad::PhysicalSize() const {
  std::lock_guard lock(mutex_);
  return {width_, height_};
}

rex::input::X_INPUT_GAMEPAD TouchPad::State() const {
  rex::input::X_INPUT_GAMEPAD pad = {};
  std::lock_guard lock(mutex_);
  uint16_t buttons = 0;
  for (const auto& [id, finger] : fingers_) {
    switch (finger.control) {
      case Control::kStick: {
        const float travel = std::max(1.0f, kStickTravel * height_);
        const float x = std::clamp((finger.x - finger.origin_x) / travel, -1.0f, 1.0f);
        const float y = std::clamp((finger.y - finger.origin_y) / travel, -1.0f, 1.0f);
        pad.thumb_lx = int16_t(x * 32767.0f);
        pad.thumb_ly = int16_t(-y * 32767.0f);
        break;
      }
      case Control::kThrottle:
        pad.right_trigger = 255;
        break;
      case Control::kBrake:
        pad.left_trigger = 255;
        break;
      case Control::kA:
        buttons |= rex::input::X_INPUT_GAMEPAD_A;
        break;
      case Control::kB:
        buttons |= rex::input::X_INPUT_GAMEPAD_B;
        break;
      case Control::kX:
        buttons |= rex::input::X_INPUT_GAMEPAD_X;
        break;
      case Control::kY:
        buttons |= rex::input::X_INPUT_GAMEPAD_Y;
        break;
      case Control::kLeftBumper:
        buttons |= rex::input::X_INPUT_GAMEPAD_LEFT_SHOULDER;
        break;
      case Control::kRightBumper:
        buttons |= rex::input::X_INPUT_GAMEPAD_RIGHT_SHOULDER;
        break;
      case Control::kBack:
        buttons |= rex::input::X_INPUT_GAMEPAD_BACK;
        break;
      case Control::kStart:
        buttons |= rex::input::X_INPUT_GAMEPAD_START;
        break;
      case Control::kNone:
        break;
    }
  }
  pad.buttons = buttons;
  return pad;
}

std::vector<TouchPad::Shape> TouchPad::Shapes(float width, float height) const {
  std::vector<Shape> shapes;
  std::lock_guard lock(mutex_);
  for (const Layout& control : Controls()) {
    Shape shape;
    Place(control, width, height, shape.x, shape.y);
    shape.radius = control.radius * height;
    shape.label = control.label;
    for (const auto& [id, finger] : fingers_) {
      shape.pressed |= finger.control == control.control;
    }
    shapes.push_back(std::move(shape));
  }
  // The stick where the finger holds it, or its resting place.
  Shape stick;
  stick.stick = true;
  Place({Control::kStick, "", 0.16f, 0.70f, kStickTravel}, width, height, stick.x, stick.y);
  stick.radius = kStickTravel * height;
  stick.stick_x = stick.x;
  stick.stick_y = stick.y;
  for (const auto& [id, finger] : fingers_) {
    if (finger.control == Control::kStick) {
      stick.pressed = true;
      stick.x = finger.origin_x;
      stick.y = finger.origin_y;
      const float dx = finger.x - finger.origin_x, dy = finger.y - finger.origin_y;
      const float length = std::sqrt(dx * dx + dy * dy);
      const float scale = length > stick.radius ? stick.radius / length : 1.0f;
      stick.stick_x = stick.x + dx * scale;
      stick.stick_y = stick.y + dy * scale;
    }
  }
  shapes.push_back(std::move(stick));
  return shapes;
}

std::unique_ptr<rex::ui::ImGuiDialog> CreateTouchOverlay(rex::ui::ImGuiDrawer* drawer) {
  return std::make_unique<TouchOverlay>(drawer);
}

}  // namespace forza2
