// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "InputCommon/ControllerInterface/Switch/Switch.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

#include <switch.h>

#include "Common/Config/Config.h"
#include "Common/Logging/Log.h"
#include "Common/MathUtil.h"
#include "InputCommon/ControllerInterface/ControllerInterface.h"
#include "InputCommon/ControllerInterface/CoreDevice.h"
#include "InputCommon/ControllerInterface/InputBackend.h"

namespace ciface::Switch
{
namespace
{
constexpr std::size_t PLAYER_COUNT = 4;
constexpr std::chrono::seconds DEVICE_RETRY_INTERVAL{1};
constexpr u64 RUNTIME_OVERLAY_CHORD = HidNpadButton_L | HidNpadButton_R | HidNpadButton_Plus;
constexpr u32 NSO_CONTROLLER_STYLES =
    HidNpadStyleTag_NpadLagon | HidNpadStyleTag_NpadLucia | HidNpadStyleTag_NpadLager |
    HidNpadStyleTag_NpadLark;
constexpr u32 SUPPORTED_CONTROLLER_STYLES =
    HidNpadStyleSet_NpadStandard | NSO_CONTROLLER_STYLES |
    HidNpadStyleTag_NpadHandheldLark | HidNpadStyleTag_NpadGc;
constexpr std::array<HidNpadIdType, PLAYER_COUNT> PLAYER_IDS = {
    HidNpadIdType_No1, HidNpadIdType_No2, HidNpadIdType_No3, HidNpadIdType_No4};
constexpr u64 TOUCHSCREEN_PRESSED = 1ULL << 63;
std::atomic<u64> s_touchscreen_state{};

void UpdateTouchscreenState()
{
  HidTouchScreenState state{};
  if (hidGetTouchScreenStates(&state, 1) == 0)
  {
    s_touchscreen_state.store(0, std::memory_order_release);
    return;
  }

  for (s32 index = 0; index < state.count; ++index)
  {
    const HidTouchState& touch = state.touches[index];
    if ((touch.attributes & HidTouchAttribute_End) != 0)
      continue;

    const u64 packed = TOUCHSCREEN_PRESSED | static_cast<u64>(touch.x) |
                       (static_cast<u64>(touch.y) << 32);
    s_touchscreen_state.store(packed, std::memory_order_release);
    return;
  }

  s_touchscreen_state.store(0, std::memory_order_release);
}

const Config::Info<int>& GetJoyConLayoutInfo(std::size_t player)
{
  static const std::array<Config::Info<int>, PLAYER_COUNT> infos = {
      Config::Info<int>{{Config::System::Main, "Input", "SwitchJoyConLayout0"}, 0},
      Config::Info<int>{{Config::System::Main, "Input", "SwitchJoyConLayout1"}, 0},
      Config::Info<int>{{Config::System::Main, "Input", "SwitchJoyConLayout2"}, 0},
      Config::Info<int>{{Config::System::Main, "Input", "SwitchJoyConLayout3"}, 0},
  };
  return infos[std::min(player, PLAYER_COUNT - 1)];
}

void MergeNsoCommonState(PadState* pad, HidNpadIdType id, u32 style,
                         const HidNpadCommonState& state)
{
  if (!(state.attributes & HidNpadAttribute_IsConnected))
    return;

  pad->active_id_mask |= static_cast<u8>(1U << static_cast<u32>(id));
  pad->style_set |= style & NSO_CONTROLLER_STYLES;
  pad->attributes |= state.attributes;
  pad->buttons_cur |= state.buttons;

  if ((style & HidNpadStyleTag_NpadLagon) != 0)
  {
    if (pad->sticks[0].x == 0 && pad->sticks[0].y == 0)
      pad->sticks[0] = state.analog_stick_l;
    if (pad->sticks[1].x == 0 && pad->sticks[1].y == 0)
    {
      const bool left = (state.buttons & HidNpadButton_LagonCLeft) != 0;
      const bool right = (state.buttons & HidNpadButton_LagonCRight) != 0;
      const bool up = (state.buttons & HidNpadButton_LagonCUp) != 0;
      const bool down = (state.buttons & HidNpadButton_LagonCDown) != 0;
      pad->sticks[1].x = (right ? 32767 : 0) - (left ? 32767 : 0);
      pad->sticks[1].y = (up ? 32767 : 0) - (down ? 32767 : 0);
    }
  }
}

void MergeNsoPadState(PadState* pad)
{
  for (std::size_t player = 0; player < PLAYER_COUNT; ++player)
  {
    if ((pad->id_mask & (1U << player)) == 0)
      continue;

    const HidNpadIdType id = PLAYER_IDS[player];
    const u32 style = hidGetNpadStyleSet(id);
    if ((style & NSO_CONTROLLER_STYLES) == 0)
      continue;

    HidNpadFullKeyState state{};
    if (hidGetNpadStatesFullKey(id, &state, 1) != 0)
      MergeNsoCommonState(pad, id, style, state);
  }

  if (!pad->read_handheld)
    return;

  const u32 handheld_style = hidGetNpadStyleSet(HidNpadIdType_Handheld);
  if ((handheld_style & HidNpadStyleTag_NpadHandheldLark) == 0)
    return;

  HidNpadHandheldLarkState state{};
  if (hidGetNpadStatesHandheldLark(HidNpadIdType_Handheld, &state, 1) == 0 ||
      !(state.attributes & HidNpadAttribute_IsConnected))
  {
    return;
  }

  pad->active_handheld = true;
  pad->style_set |= HidNpadStyleTag_NpadHandheldLark;
  pad->attributes |= state.attributes;
  pad->buttons_cur |= state.buttons;
  if (pad->sticks[0].x == 0 && pad->sticks[0].y == 0)
    pad->sticks[0] = state.analog_stick_l;
  if (pad->sticks[1].x == 0 && pad->sticks[1].y == 0)
    pad->sticks[1] = state.analog_stick_r;
}

void UpdatePadStateImpl(PadState* pad)
{
  padUpdate(pad);
  MergeNsoPadState(pad);
}

class JoyConAssignmentManager final
{
public:
  void Initialize()
  {
    m_initialized = true;
    if (R_FAILED(hidGetNpadJoyHoldType(&m_initial_hold_type)))
      m_initial_hold_type = HidNpadJoyHoldType_Vertical;

    for (std::size_t player = 0; player < PLAYER_COUNT; ++player)
    {
      Snapshot& snapshot = m_snapshots[player];
      snapshot.style = hidGetNpadStyleSet(PLAYER_IDS[player]);
      m_layouts[player] = GetJoyConLayout(player);
    }

    ApplyDualLayouts();
    ApplySingleLayouts();
    Update(true);
  }

  void Update(bool force = false)
  {
    if (!m_initialized)
      return;

    bool single = false;
    bool style_changed = false;
    for (std::size_t player = 0; player < PLAYER_COUNT; ++player)
    {
      const u32 style = hidGetNpadStyleSet(PLAYER_IDS[player]);
      style_changed |= style != m_last_styles[player];
      m_last_styles[player] = style;
      single |= (style & (HidNpadStyleTag_NpadJoyLeft | HidNpadStyleTag_NpadJoyRight)) != 0;
      single |= m_layouts[player] == JoyConLayout::Left ||
                m_layouts[player] == JoyConLayout::Right;
    }

    const auto now = Clock::now();
    if (!force && !style_changed && now < m_next_hold_probe)
      return;
    m_next_hold_probe = now + DEVICE_RETRY_INTERVAL;

    const HidNpadJoyHoldType desired =
        single ? HidNpadJoyHoldType_Horizontal : m_initial_hold_type;
    HidNpadJoyHoldType current{};
    if (R_FAILED(hidGetNpadJoyHoldType(&current)) || current != desired)
      (void)hidSetNpadJoyHoldType(desired);
  }

  void Restore()
  {
    if (!m_initialized)
      return;

    for (std::size_t player = 0; player < PLAYER_COUNT; ++player)
    {
      if (m_modified[player])
        (void)hidSetNpadJoyAssignmentModeDual(PLAYER_IDS[player]);
    }
    for (const SplitPair& pair : m_split_pairs)
    {
      (void)hidSetNpadJoyAssignmentModeDual(pair.source);
      (void)hidSetNpadJoyAssignmentModeDual(pair.destination);
      if (pair.source_was_dual)
        (void)hidMergeSingleJoyAsDualJoy(pair.source, pair.destination);
    }
    for (std::size_t player = 0; player < PLAYER_COUNT; ++player)
    {
      if (!m_modified[player])
        continue;
      const u32 style = m_snapshots[player].style;
      if ((style & HidNpadStyleTag_NpadJoyLeft) != 0)
      {
        (void)hidSetNpadJoyAssignmentModeSingle(PLAYER_IDS[player],
                                                HidNpadJoyDeviceType_Left);
      }
      else if ((style & HidNpadStyleTag_NpadJoyRight) != 0)
      {
        (void)hidSetNpadJoyAssignmentModeSingle(PLAYER_IDS[player],
                                                HidNpadJoyDeviceType_Right);
      }
    }

    (void)hidSetNpadJoyHoldType(m_initial_hold_type);
    m_initialized = false;
  }

private:
  using Clock = std::chrono::steady_clock;

  struct Snapshot
  {
    u32 style = 0;
  };

  struct SplitPair
  {
    HidNpadIdType source{};
    HidNpadIdType destination{};
    bool source_was_dual = false;
  };

  std::optional<std::size_t> FindComplementarySingle(std::size_t player, u32 style) const
  {
    const bool needs_right = (style & HidNpadStyleTag_NpadJoyLeft) != 0;
    const bool needs_left = (style & HidNpadStyleTag_NpadJoyRight) != 0;
    if (!needs_left && !needs_right)
      return std::nullopt;

    for (std::size_t candidate = 0; candidate < PLAYER_COUNT; ++candidate)
    {
      if (candidate == player || m_layouts[candidate] == JoyConLayout::Left ||
          m_layouts[candidate] == JoyConLayout::Right)
      {
        continue;
      }
      const u32 candidate_style = hidGetNpadStyleSet(PLAYER_IDS[candidate]);
      if ((needs_right && (candidate_style & HidNpadStyleTag_NpadJoyRight)) ||
          (needs_left && (candidate_style & HidNpadStyleTag_NpadJoyLeft)))
      {
        return candidate;
      }
    }
    return std::nullopt;
  }

  void ApplyDualLayouts()
  {
    for (std::size_t player = 0; player < PLAYER_COUNT; ++player)
    {
      if (m_layouts[player] != JoyConLayout::Dual)
        continue;

      const HidNpadIdType id = PLAYER_IDS[player];
      const u32 style = hidGetNpadStyleSet(id);
      if (R_SUCCEEDED(hidSetNpadJoyAssignmentModeDual(id)))
        m_modified[player] = true;

      const std::optional<std::size_t> partner = FindComplementarySingle(player, style);
      if (!partner)
        continue;
      if (R_FAILED(hidSetNpadJoyAssignmentModeDual(PLAYER_IDS[*partner])))
        continue;
      if (R_SUCCEEDED(hidMergeSingleJoyAsDualJoy(id, PLAYER_IDS[*partner])))
      {
        m_modified[player] = true;
        m_modified[*partner] = true;
      }
    }
  }

  void ApplySingleLayouts()
  {
    for (std::size_t player = 0; player < PLAYER_COUNT; ++player)
    {
      if (m_layouts[player] != JoyConLayout::Left &&
          m_layouts[player] != JoyConLayout::Right)
      {
        continue;
      }

      const HidNpadIdType id = PLAYER_IDS[player];
      const HidNpadJoyDeviceType side = m_layouts[player] == JoyConLayout::Left ?
                                            HidNpadJoyDeviceType_Left :
                                            HidNpadJoyDeviceType_Right;
      bool has_destination = false;
      HidNpadIdType destination{};
      Result result = hidSetNpadJoyAssignmentModeSingleWithDestination(
          id, side, &has_destination, &destination);
      if (R_FAILED(result))
        result = hidSetNpadJoyAssignmentModeSingle(id, side);
      if (R_FAILED(result))
        continue;

      m_modified[player] = true;
      if (has_destination)
      {
        const auto destination_index = static_cast<std::size_t>(destination);
        if (destination_index < PLAYER_COUNT)
          m_modified[destination_index] = true;
        m_split_pairs.push_back(
            {id, destination, (m_snapshots[player].style & HidNpadStyleTag_NpadJoyDual) != 0});
      }
    }
  }

  bool m_initialized = false;
  HidNpadJoyHoldType m_initial_hold_type = HidNpadJoyHoldType_Vertical;
  std::array<Snapshot, PLAYER_COUNT> m_snapshots{};
  std::array<JoyConLayout, PLAYER_COUNT> m_layouts{};
  std::array<bool, PLAYER_COUNT> m_modified{};
  std::array<u32, PLAYER_COUNT> m_last_styles{};
  std::vector<SplitPair> m_split_pairs;
  Clock::time_point m_next_hold_probe{};
};

class ControllerState final
{
public:
  void Initialize(HidNpadIdType npad_id, bool use_handheld)
  {
    std::lock_guard lock{m_mutex};
    m_npad_id = npad_id;
    m_use_handheld = use_handheld;
    // Reserve the overlay chord at the native input boundary.
    m_filter_runtime_overlay_chord = npad_id == HidNpadIdType_No1;
    m_runtime_overlay_chord_latched = false;
    if (use_handheld)
      padInitialize(&m_pad, npad_id, HidNpadIdType_Handheld);
    else
      padInitialize(&m_pad, npad_id);
    UpdatePadStateImpl(&m_pad);
  }

  void Shutdown()
  {
    std::lock_guard lock{m_mutex};

    for (const VibrationDevice& device : m_vibration_devices)
    {
      if (device.ready)
        SendVibrationDeviceLocked(device, 0.0f);
    }

    for (std::size_t i = 0; i < m_motion_handles.size(); ++i)
    {
      if (m_motion_ready[i])
        (void)hidStopSixAxisSensor(m_motion_handles[i]);
      m_motion_ready[i] = false;
    }
    m_runtime_overlay_chord_latched = false;
    ClearMotionLocked();
  }

  void Update()
  {
    std::lock_guard lock{m_mutex};
    UpdatePadStateImpl(&m_pad);
    RefreshVibrationDeviceLocked();
    UpdateMotionLocked();
  }

  bool IsConnected() const
  {
    std::lock_guard lock{m_mutex};
    return padIsConnected(&m_pad);
  }

  u64 GetButtons() const
  {
    std::lock_guard lock{m_mutex};
    u64 buttons = padGetButtons(&m_pad);
    if (!m_filter_runtime_overlay_chord)
      return buttons;

    if ((buttons & RUNTIME_OVERLAY_CHORD) == RUNTIME_OVERLAY_CHORD)
      m_runtime_overlay_chord_latched = true;

    if (!m_runtime_overlay_chord_latched)
      return buttons;

    // Release the chord after all three buttons are up.
    if ((buttons & RUNTIME_OVERLAY_CHORD) == 0)
    {
      m_runtime_overlay_chord_latched = false;
      return buttons;
    }
    return buttons & ~RUNTIME_OVERLAY_CHORD;
  }

  HidAnalogStickState GetStick(unsigned stick) const
  {
    std::lock_guard lock{m_mutex};
    return padGetStickPos(&m_pad, stick);
  }

  ControlState GetTrigger(unsigned trigger) const
  {
    std::lock_guard lock{m_mutex};
    return static_cast<ControlState>(padGetGcTriggerPos(&m_pad, trigger)) / 0x7fff;
  }

  ControlState GetMotion(bool gyro, bool nunchuk, std::size_t axis) const
  {
    std::lock_guard lock{m_mutex};
    if (nunchuk)
      return m_nunchuk_accel[axis];
    return gyro ? m_gyro[axis] : m_accel[axis];
  }

  void SetRumble(ControlState state)
  {
    const float amplitude = std::clamp(static_cast<float>(state), 0.0f, 1.0f);
    std::lock_guard lock{m_mutex};
    if (std::abs(m_requested_rumble - amplitude) < 0.0001f)
      return;

    m_requested_rumble = amplitude;
    UpdatePadStateImpl(&m_pad);
    RefreshVibrationDeviceLocked();
    SendActiveVibrationLocked();
  }

private:
  using Clock = std::chrono::steady_clock;

  struct VibrationDevice
  {
    HidNpadIdType id{};
    HidNpadStyleTag style{};
    std::array<HidVibrationDeviceHandle, 2> handles{};
    s32 count = 0;
    bool gc_erm = false;
    bool ready = false;
  };

  void InitializeVibrationDeviceLocked(std::size_t index, HidNpadIdType id,
                                       HidNpadStyleTag style, s32 count)
  {
    VibrationDevice& device = m_vibration_devices[index];
    if (device.ready || Clock::now() < m_next_vibration_probe)
      return;

    device.id = id;
    device.style = style;
    device.count = count;
    const Result result =
        hidInitializeVibrationDevices(device.handles.data(), count, id, style);
    if (R_SUCCEEDED(result))
    {
      HidVibrationDeviceInfo info{};
      device.gc_erm = count == 1 &&
                      R_SUCCEEDED(hidGetVibrationDeviceInfo(device.handles[0], &info)) &&
                      info.type == HidVibrationDeviceType_GcErm;
      device.ready = true;
    }
    else
    {
      m_next_vibration_probe = Clock::now() + DEVICE_RETRY_INTERVAL;
      DEBUG_LOG_FMT(CONTROLLERINTERFACE,
                    "Switch vibration initialization failed for player {} style 0x{:X}: 0x{:X}",
                    static_cast<u32>(m_npad_id) + 1, static_cast<u32>(style), result);
    }
  }

  void InitializeVibrationLocked()
  {
    const u32 style = padGetStyleSet(&m_pad);
    if (m_use_handheld && padIsHandheld(&m_pad))
    {
      InitializeVibrationDeviceLocked(0, HidNpadIdType_Handheld,
                                      HidNpadStyleTag_NpadHandheld, 2);
    }
    else if (style & HidNpadStyleTag_NpadFullKey)
    {
      InitializeVibrationDeviceLocked(1, m_npad_id, HidNpadStyleTag_NpadFullKey, 2);
    }
    else if (style & HidNpadStyleTag_NpadJoyDual)
    {
      InitializeVibrationDeviceLocked(2, m_npad_id, HidNpadStyleTag_NpadJoyDual, 2);
    }
    else if (style & HidNpadStyleTag_NpadJoyLeft)
    {
      InitializeVibrationDeviceLocked(3, m_npad_id, HidNpadStyleTag_NpadJoyLeft, 1);
    }
    else if (style & HidNpadStyleTag_NpadJoyRight)
    {
      InitializeVibrationDeviceLocked(4, m_npad_id, HidNpadStyleTag_NpadJoyRight, 1);
    }
    else if (style & HidNpadStyleTag_NpadGc)
    {
      InitializeVibrationDeviceLocked(5, m_npad_id, HidNpadStyleTag_NpadGc, 1);
    }
  }

  int GetActiveVibrationDeviceLocked() const
  {
    const u32 style = padGetStyleSet(&m_pad);
    if (m_use_handheld && padIsHandheld(&m_pad) && m_vibration_devices[0].ready)
      return 0;

    for (std::size_t i = 1; i < m_vibration_devices.size(); ++i)
    {
      const VibrationDevice& device = m_vibration_devices[i];
      if (device.ready && (style & device.style))
        return static_cast<int>(i);
    }
    return -1;
  }

  static void SendVibrationDeviceLocked(const VibrationDevice& device, float amplitude)
  {
    if (device.gc_erm)
    {
      const HidVibrationGcErmCommand command = amplitude > 0.0f ?
                                                   HidVibrationGcErmCommand_Start :
                                                   HidVibrationGcErmCommand_Stop;
      (void)hidSendVibrationGcErmCommand(device.handles[0], command);
      return;
    }

    std::array<HidVibrationValue, 2> values{};
    for (s32 i = 0; i < device.count; ++i)
      values[i] = HidVibrationValue{amplitude, 160.0f, amplitude, 320.0f};
    (void)hidSendVibrationValues(device.handles.data(), values.data(), device.count);
  }

  void SendActiveVibrationLocked()
  {
    if (m_active_vibration_device < 0)
      return;
    if (m_sent_vibration_device == m_active_vibration_device &&
        std::abs(m_sent_rumble - m_requested_rumble) < 0.0001f)
    {
      return;
    }

    SendVibrationDeviceLocked(m_vibration_devices[m_active_vibration_device], m_requested_rumble);
    m_sent_vibration_device = m_active_vibration_device;
    m_sent_rumble = m_requested_rumble;
  }

  void RefreshVibrationDeviceLocked()
  {
    InitializeVibrationLocked();
    const int active = GetActiveVibrationDeviceLocked();
    if (active == m_active_vibration_device)
      return;

    if (m_active_vibration_device >= 0)
      SendVibrationDeviceLocked(m_vibration_devices[m_active_vibration_device], 0.0f);

    m_active_vibration_device = active;
    m_sent_vibration_device = -1;
    m_sent_rumble = -1.0f;
    SendActiveVibrationLocked();
  }

  void StartSingleMotionHandleLocked(std::size_t index, HidNpadIdType id,
                                     HidNpadStyleTag style)
  {
    if (m_motion_ready[index])
      return;

    HidSixAxisSensorHandle handle{};
    const Result get_result = hidGetSixAxisSensorHandles(&handle, 1, id, style);
    if (R_SUCCEEDED(get_result) && R_SUCCEEDED(hidStartSixAxisSensor(handle)))
    {
      m_motion_handles[index] = handle;
      m_motion_ready[index] = true;
    }
  }

  void StartMotionHandleLocked(std::size_t index, HidSixAxisSensorHandle handle)
  {
    if (m_motion_ready[index])
      return;
    if (R_SUCCEEDED(hidStartSixAxisSensor(handle)))
    {
      m_motion_handles[index] = handle;
      m_motion_ready[index] = true;
    }
  }

  void InitializeMotionLocked()
  {
    const u32 style = padGetStyleSet(&m_pad);
    if (m_use_handheld && padIsHandheld(&m_pad))
    {
      StartSingleMotionHandleLocked(0, HidNpadIdType_Handheld,
                                    HidNpadStyleTag_NpadHandheld);
    }
    else if (style & HidNpadStyleTag_NpadFullKey)
    {
      StartSingleMotionHandleLocked(1, m_npad_id, HidNpadStyleTag_NpadFullKey);
    }
    else if (style & HidNpadStyleTag_NpadJoyDual)
    {
      std::array<HidSixAxisSensorHandle, 2> handles{};
      if (R_SUCCEEDED(hidGetSixAxisSensorHandles(handles.data(), 2, m_npad_id,
                                                 HidNpadStyleTag_NpadJoyDual)))
      {
        const u32 attributes = padGetAttributes(&m_pad);
        if ((attributes & HidNpadAttribute_IsLeftConnected) != 0)
          StartMotionHandleLocked(2, handles[0]);
        if ((attributes & HidNpadAttribute_IsRightConnected) != 0)
          StartMotionHandleLocked(3, handles[1]);
      }
    }
    else if (style & HidNpadStyleTag_NpadJoyLeft)
    {
      StartSingleMotionHandleLocked(4, m_npad_id, HidNpadStyleTag_NpadJoyLeft);
    }
    else if (style & HidNpadStyleTag_NpadJoyRight)
    {
      StartSingleMotionHandleLocked(5, m_npad_id, HidNpadStyleTag_NpadJoyRight);
    }
  }

  int GetActiveMotionHandleLocked() const
  {
    const u32 style = padGetStyleSet(&m_pad);
    if (m_use_handheld && padIsHandheld(&m_pad) && m_motion_ready[0])
      return 0;
    if ((style & HidNpadStyleTag_NpadFullKey) && m_motion_ready[1])
      return 1;
    if (style & HidNpadStyleTag_NpadJoyDual)
    {
      const u32 attributes = padGetAttributes(&m_pad);
      if ((attributes & HidNpadAttribute_IsRightConnected) && m_motion_ready[3])
        return 3;
      if ((attributes & HidNpadAttribute_IsLeftConnected) && m_motion_ready[2])
        return 2;
    }
    if ((style & HidNpadStyleTag_NpadJoyLeft) && m_motion_ready[4])
      return 4;
    if ((style & HidNpadStyleTag_NpadJoyRight) && m_motion_ready[5])
      return 5;
    return -1;
  }

  int GetNunchukMotionHandleLocked() const
  {
    const u32 style = padGetStyleSet(&m_pad);
    if (!(style & HidNpadStyleTag_NpadJoyDual))
      return -1;
    const u32 attributes = padGetAttributes(&m_pad);
    if ((attributes & HidNpadAttribute_IsLeftConnected) == 0 ||
        (attributes & HidNpadAttribute_IsRightConnected) == 0 || !m_motion_ready[2] ||
        !m_motion_ready[3])
    {
      return -1;
    }
    return 2;
  }

  bool NeedsMotionProbeLocked() const
  {
    const u32 style = padGetStyleSet(&m_pad);
    if (m_use_handheld && padIsHandheld(&m_pad))
      return !m_motion_ready[0];
    if (style & HidNpadStyleTag_NpadFullKey)
      return !m_motion_ready[1];
    if (style & HidNpadStyleTag_NpadJoyDual)
    {
      const u32 attributes = padGetAttributes(&m_pad);
      return ((attributes & HidNpadAttribute_IsLeftConnected) && !m_motion_ready[2]) ||
             ((attributes & HidNpadAttribute_IsRightConnected) && !m_motion_ready[3]);
    }
    if (style & HidNpadStyleTag_NpadJoyLeft)
      return !m_motion_ready[4];
    if (style & HidNpadStyleTag_NpadJoyRight)
      return !m_motion_ready[5];
    return false;
  }

  void StopUnusedMotionHandlesLocked(int primary, int nunchuk)
  {
    for (std::size_t index = 0; index < m_motion_handles.size(); ++index)
    {
      if (!m_motion_ready[index] || static_cast<int>(index) == primary ||
          static_cast<int>(index) == nunchuk)
      {
        continue;
      }
      (void)hidStopSixAxisSensor(m_motion_handles[index]);
      m_motion_ready[index] = false;
    }
  }

  bool ReadMotionHandleLocked(int index, std::array<float, 3>* accel,
                              std::array<float, 3>* gyro)
  {
    HidSixAxisSensorState sensor{};
    if (hidGetSixAxisSensorStates(m_motion_handles[index], &sensor, 1) != 1 ||
        !(sensor.attributes & HidSixAxisSensorAttribute_IsConnected))
    {
      (void)hidStopSixAxisSensor(m_motion_handles[index]);
      m_motion_ready[index] = false;
      return false;
    }

    const HidVector& sensor_accel = sensor.acceleration;
    const HidVector& sensor_gyro = sensor.angular_velocity;
    if (!std::isfinite(sensor_accel.x) || !std::isfinite(sensor_accel.y) ||
        !std::isfinite(sensor_accel.z) || !std::isfinite(sensor_gyro.x) ||
        !std::isfinite(sensor_gyro.y) || !std::isfinite(sensor_gyro.z))
    {
      return false;
    }

    constexpr float gravity = static_cast<float>(MathUtil::GRAVITY_ACCELERATION);
    constexpr float tau = static_cast<float>(MathUtil::TAU);
    *accel = {sensor_accel.x * gravity, sensor_accel.y * gravity,
              sensor_accel.z * gravity};
    if (gyro)
      *gyro = {sensor_gyro.x * tau, sensor_gyro.y * tau, sensor_gyro.z * tau};
    return true;
  }

  void ClearNunchukMotionLocked()
  {
    m_nunchuk_accel = {0.0f, 0.0f,
                       -static_cast<float>(MathUtil::GRAVITY_ACCELERATION)};
  }

  void ClearMotionLocked()
  {
    m_accel = {};
    m_gyro = {};
    ClearNunchukMotionLocked();
  }

  void UpdateMotionLocked()
  {
    const auto now = Clock::now();
    int active = GetActiveMotionHandleLocked();
    int nunchuk = GetNunchukMotionHandleLocked();
    if ((active < 0 || NeedsMotionProbeLocked()) && now >= m_next_motion_probe)
    {
      InitializeMotionLocked();
      active = GetActiveMotionHandleLocked();
      nunchuk = GetNunchukMotionHandleLocked();
      m_next_motion_probe = now + DEVICE_RETRY_INTERVAL;
    }

    StopUnusedMotionHandlesLocked(active, nunchuk);
    if (active < 0)
    {
      ClearMotionLocked();
      return;
    }

    if (!ReadMotionHandleLocked(active, &m_accel, &m_gyro))
    {
      m_next_motion_probe = now + DEVICE_RETRY_INTERVAL;
      ClearMotionLocked();
      return;
    }

    if (nunchuk < 0 || !ReadMotionHandleLocked(nunchuk, &m_nunchuk_accel, nullptr))
      ClearNunchukMotionLocked();
  }

  mutable std::mutex m_mutex;
  PadState m_pad{};
  HidNpadIdType m_npad_id{};
  bool m_use_handheld = false;
  bool m_filter_runtime_overlay_chord = false;
  mutable bool m_runtime_overlay_chord_latched = false;

  std::array<VibrationDevice, 6> m_vibration_devices{};
  Clock::time_point m_next_vibration_probe{};
  int m_active_vibration_device = -1;
  int m_sent_vibration_device = -1;
  float m_requested_rumble = 0.0f;
  float m_sent_rumble = -1.0f;

  std::array<HidSixAxisSensorHandle, 6> m_motion_handles{};
  std::array<bool, 6> m_motion_ready{};
  Clock::time_point m_next_motion_probe{};
  std::array<float, 3> m_accel{};
  std::array<float, 3> m_gyro{};
  std::array<float, 3> m_nunchuk_accel{
      0.0f, 0.0f, -static_cast<float>(MathUtil::GRAVITY_ACCELERATION)};
};

class Button final : public Core::Device::Input
{
public:
  Button(const ControllerState* state, u64 mask, std::string name)
      : m_state(state), m_mask(mask), m_name(std::move(name))
  {
  }

  std::string GetName() const override { return m_name; }
  ControlState GetState() const override
  {
    return (m_state->GetButtons() & m_mask) != 0 ? 1.0 : 0.0;
  }

private:
  const ControllerState* m_state;
  u64 m_mask;
  std::string m_name;
};

class Axis final : public Core::Device::Input
{
public:
  Axis(const ControllerState* state, unsigned stick, bool y_axis, s32 range, std::string name)
      : m_state(state), m_stick(stick), m_y_axis(y_axis), m_range(range),
        m_name(std::move(name))
  {
  }

  std::string GetName() const override { return m_name; }
  ControlState GetState() const override
  {
    const HidAnalogStickState state = m_state->GetStick(m_stick);
    const s32 value = m_y_axis ? state.y : state.x;
    return std::clamp(static_cast<ControlState>(value) / m_range, -1.0, 1.0);
  }

private:
  const ControllerState* m_state;
  unsigned m_stick;
  bool m_y_axis;
  s32 m_range;
  std::string m_name;
};

class Trigger final : public Core::Device::Input
{
public:
  Trigger(const ControllerState* state, unsigned trigger, std::string name)
      : m_state(state), m_trigger(trigger), m_name(std::move(name))
  {
  }

  std::string GetName() const override { return m_name; }
  ControlState GetState() const override { return m_state->GetTrigger(m_trigger); }

private:
  const ControllerState* m_state;
  unsigned m_trigger;
  std::string m_name;
};

class MotionInput final : public Core::Device::Input
{
public:
  MotionInput(const ControllerState* state, bool gyro, bool nunchuk, std::size_t axis,
              ControlState scale, std::string name)
      : m_state(state), m_gyro(gyro), m_nunchuk(nunchuk), m_axis(axis), m_scale(scale),
        m_name(std::move(name))
  {
  }

  std::string GetName() const override { return m_name; }
  bool IsDetectable() const override { return false; }
  ControlState GetState() const override
  {
    return m_state->GetMotion(m_gyro, m_nunchuk, m_axis) * m_scale;
  }

private:
  const ControllerState* m_state;
  bool m_gyro;
  bool m_nunchuk;
  std::size_t m_axis;
  ControlState m_scale;
  std::string m_name;
};

class Motor final : public Core::Device::Output
{
public:
  explicit Motor(ControllerState* state) : m_state(state) {}

  std::string GetName() const override { return "Motor"; }
  void SetState(ControlState state) override { m_state->SetRumble(state); }

private:
  ControllerState* m_state;
};

class Device final : public Core::Device
{
public:
  Device(ControllerState* state, unsigned player) : m_state(state), m_player(player)
  {
    AddInput(new Button(m_state, HidNpadButton_A, "A"));
    AddInput(new Button(m_state, HidNpadButton_B, "B"));
    AddInput(new Button(m_state, HidNpadButton_X, "X"));
    AddInput(new Button(m_state, HidNpadButton_Y, "Y"));
    AddInput(new Button(m_state, HidNpadButton_L | HidNpadButton_AnySL, "L"));
    AddInput(new Button(m_state, HidNpadButton_R | HidNpadButton_AnySR, "R"));
    AddInput(new Button(m_state, HidNpadButton_ZL, "Z"));
    AddInput(new Button(m_state, HidNpadButton_ZR, "R2"));
    AddInput(new Button(m_state, HidNpadButton_Plus, "Start"));
    AddInput(new Button(m_state, HidNpadButton_Minus, "Select"));
    AddInput(new Button(m_state, HidNpadButton_Up, "Up"));
    AddInput(new Button(m_state, HidNpadButton_Down, "Down"));
    AddInput(new Button(m_state, HidNpadButton_Left, "Left"));
    AddInput(new Button(m_state, HidNpadButton_Right, "Right"));
    AddInput(new Button(m_state, HidNpadButton_StickL, "L3"));
    AddInput(new Button(m_state, HidNpadButton_StickR, "R3"));
    AddInput(new Button(m_state, HidNpadButton_AnySL, "SL"));
    AddInput(new Button(m_state, HidNpadButton_AnySR, "SR"));
    AddInput(new Button(m_state, HidNpadButton_LagonCUp, "CUp"));
    AddInput(new Button(m_state, HidNpadButton_LagonCDown, "CDown"));
    AddInput(new Button(m_state, HidNpadButton_LagonCLeft, "CLeft"));
    AddInput(new Button(m_state, HidNpadButton_LagonCRight, "CRight"));

    AddInput(new Axis(m_state, 0, false, -32768, "X0-"));
    AddInput(new Axis(m_state, 0, false, 32767, "X0+"));
    AddInput(new Axis(m_state, 0, true, -32768, "Y0-"));
    AddInput(new Axis(m_state, 0, true, 32767, "Y0+"));
    AddInput(new Axis(m_state, 1, false, -32768, "X1-"));
    AddInput(new Axis(m_state, 1, false, 32767, "X1+"));
    AddInput(new Axis(m_state, 1, true, -32768, "Y1-"));
    AddInput(new Axis(m_state, 1, true, 32767, "Y1+"));
    AddInput(new Trigger(m_state, 0, "Trigger L"));
    AddInput(new Trigger(m_state, 1, "Trigger R"));

    // A vertical right Joy-Con uses Z for up/down, X for left/right, and Y for forward/back.
    AddInput(new MotionInput(m_state, false, false, 2, -1, "Accel Up"));
    AddInput(new MotionInput(m_state, false, false, 2, 1, "Accel Down"));
    AddInput(new MotionInput(m_state, false, false, 0, 1, "Accel Left"));
    AddInput(new MotionInput(m_state, false, false, 0, -1, "Accel Right"));
    AddInput(new MotionInput(m_state, false, false, 1, -1, "Accel Forward"));
    AddInput(new MotionInput(m_state, false, false, 1, 1, "Accel Backward"));

    AddInput(new MotionInput(m_state, true, false, 0, 1, "Gyro Pitch Up"));
    AddInput(new MotionInput(m_state, true, false, 0, -1, "Gyro Pitch Down"));
    AddInput(new MotionInput(m_state, true, false, 1, -1, "Gyro Roll Left"));
    AddInput(new MotionInput(m_state, true, false, 1, 1, "Gyro Roll Right"));
    AddInput(new MotionInput(m_state, true, false, 2, 1, "Gyro Yaw Left"));
    AddInput(new MotionInput(m_state, true, false, 2, -1, "Gyro Yaw Right"));

    AddInput(new MotionInput(m_state, false, true, 2, -1, "Nunchuk Accel Up"));
    AddInput(new MotionInput(m_state, false, true, 2, 1, "Nunchuk Accel Down"));
    AddInput(new MotionInput(m_state, false, true, 0, 1, "Nunchuk Accel Left"));
    AddInput(new MotionInput(m_state, false, true, 0, -1, "Nunchuk Accel Right"));
    AddInput(new MotionInput(m_state, false, true, 1, -1, "Nunchuk Accel Forward"));
    AddInput(new MotionInput(m_state, false, true, 1, 1, "Nunchuk Accel Backward"));

    AddOutput(new Motor(m_state));
  }

  std::string GetName() const override { return "Joypad"; }
  std::string GetSource() const override { return "Switch"; }
  std::optional<int> GetPreferredId() const override { return static_cast<int>(m_player); }
  int GetSortPriority() const override { return DEFAULT_DEVICE_SORT_PRIORITY - m_player; }
  bool IsValid() const override { return m_state->IsConnected(); }

private:
  ControllerState* m_state;
  unsigned m_player;
};

class Backend final : public ciface::InputBackend
{
public:
  explicit Backend(ControllerInterface* controller_interface)
      : ciface::InputBackend(controller_interface)
  {
    const Result result = hidInitialize();
    m_hid_initialized = R_SUCCEEDED(result);
    if (!m_hid_initialized)
    {
      ERROR_LOG_FMT(CONTROLLERINTERFACE, "Switch HID initialization failed: 0x{:X}", result);
      return;
    }

    hidInitializeTouchScreen();
    padConfigureInput(PLAYER_COUNT, SUPPORTED_CONTROLLER_STYLES);
    m_joycon_assignments.Initialize();
    m_controllers[0].Initialize(HidNpadIdType_No1, true);
    m_controllers[1].Initialize(HidNpadIdType_No2, false);
    m_controllers[2].Initialize(HidNpadIdType_No3, false);
    m_controllers[3].Initialize(HidNpadIdType_No4, false);
  }

  ~Backend() override
  {
    if (!m_hid_initialized)
      return;
    for (ControllerState& controller : m_controllers)
      controller.Shutdown();
    s_touchscreen_state.store(0, std::memory_order_release);
    m_joycon_assignments.Restore();
    hidExit();
  }

  void PopulateDevices() override
  {
    if (!m_hid_initialized)
      return;

    for (std::size_t player = 0; player < m_controllers.size(); ++player)
      GetControllerInterface().AddDevice(
          std::make_shared<Device>(&m_controllers[player], player));
  }

  void UpdateInput(std::vector<std::weak_ptr<Core::Device>>& devices_to_remove) override
  {
    if (!m_hid_initialized)
      return;
    m_joycon_assignments.Update();
    UpdateTouchscreenState();
    for (ControllerState& controller : m_controllers)
      controller.Update();
  }

private:
  JoyConAssignmentManager m_joycon_assignments;
  std::array<ControllerState, PLAYER_COUNT> m_controllers{};
  bool m_hid_initialized = false;
};
}  // namespace

std::unique_ptr<ciface::InputBackend> CreateInputBackend(
    ControllerInterface* controller_interface)
{
  return std::make_unique<Backend>(controller_interface);
}

JoyConLayout GetJoyConLayout(std::size_t player)
{
  const int value = Config::GetBase(GetJoyConLayoutInfo(player));
  if (value < static_cast<int>(JoyConLayout::Auto) ||
      value > static_cast<int>(JoyConLayout::Right))
  {
    return JoyConLayout::Auto;
  }
  return static_cast<JoyConLayout>(value);
}

void SetJoyConLayout(std::size_t player, JoyConLayout layout)
{
  Config::SetBase(GetJoyConLayoutInfo(player), static_cast<int>(layout));
}

std::string_view GetJoyConLayoutName(JoyConLayout layout)
{
  switch (layout)
  {
  case JoyConLayout::Auto:
    return "Auto";
  case JoyConLayout::Dual:
    return "Dual Joy-Con";
  case JoyConLayout::Left:
    return "Left Joy-Con";
  case JoyConLayout::Right:
    return "Right Joy-Con";
  }
  return "Auto";
}

void UpdatePadState(PadState* pad)
{
  if (pad)
    UpdatePadStateImpl(pad);
}

TouchscreenState GetTouchscreenState()
{
  const u64 packed = s_touchscreen_state.load(std::memory_order_acquire);
  return {
      .pressed = (packed & TOUCHSCREEN_PRESSED) != 0,
      .x = static_cast<u32>(packed),
      .y = static_cast<u32>((packed >> 32) & 0x7FFFFFFFULL),
  };
}
}  // namespace ciface::Switch
