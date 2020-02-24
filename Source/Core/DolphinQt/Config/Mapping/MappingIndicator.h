// Copyright 2018 Dolphin Emulator Project
// Licensed under GPLv2+
// Refer to the license.txt file included.

#pragma once

#include <QToolButton>
#include <QWidget>

#include <deque>

#include "Core/HW/WiimoteEmu/Dynamics.h"
#include "InputCommon/ControllerEmu/StickGate.h"

namespace ControllerEmu
{
class Control;
class ControlGroup;
class Cursor;
class Force;
class MixedTriggers;
}  // namespace ControllerEmu

class QPainter;
class QPaintEvent;
class QTimer;

class CalibrationWidget;

class MappingIndicator : public QWidget
{
public:
  MappingIndicator();

  QPen GetBBoxPen() const;
  QBrush GetBBoxBrush() const;
  QColor GetRawInputColor() const;
  QPen GetInputShapePen() const;
  QColor GetCenterColor() const;
  QColor GetAdjustedInputColor() const;
  QColor GetDeadZoneColor() const;
  QPen GetDeadZonePen() const;
  QBrush GetDeadZoneBrush() const;
  QColor GetTextColor() const;
  QColor GetAltTextColor() const;
  void AdjustGateColor(QColor*);

protected:
  double GetScale() const;

  virtual void Draw() {}

private:
  void paintEvent(QPaintEvent*) override;
};

class ReshapableInputIndicator : public MappingIndicator
{
public:
  void SetCalibrationWidget(CalibrationWidget* widget);

protected:
  void DrawReshapableInput(ControllerEmu::ReshapableInput& group, QColor gate_color,
                           std::optional<ControllerEmu::ReshapableInput::ReshapeData> adj_coord);

  bool IsCalibrating() const;

  void DrawCalibration(QPainter& p, Common::DVec2 point);
  void UpdateCalibrationWidget(Common::DVec2 point);

private:
  CalibrationWidget* m_calibration_widget{};
};

class AnalogStickIndicator : public ReshapableInputIndicator
{
public:
  explicit AnalogStickIndicator(ControllerEmu::ReshapableInput& stick) : m_group(stick) {}

private:
  void Draw() override;

  ControllerEmu::ReshapableInput& m_group;
};

class TiltIndicator : public ReshapableInputIndicator
{
public:
  explicit TiltIndicator(ControllerEmu::Tilt& tilt) : m_group(tilt) {}

private:
  void Draw() override;

  ControllerEmu::Tilt& m_group;
  WiimoteEmu::MotionState m_motion_state{};
};

class CursorIndicator : public ReshapableInputIndicator
{
public:
  explicit CursorIndicator(ControllerEmu::Cursor& cursor) : m_cursor_group(cursor) {}

private:
  void Draw() override;

  ControllerEmu::Cursor& m_cursor_group;
};

class MixedTriggersIndicator : public MappingIndicator
{
public:
  explicit MixedTriggersIndicator(ControllerEmu::MixedTriggers& triggers);

private:
  void Draw() override;

  ControllerEmu::MixedTriggers& m_group;
};

class SwingIndicator : public ReshapableInputIndicator
{
public:
  explicit SwingIndicator(ControllerEmu::Force& swing) : m_swing_group(swing) {}

private:
  void Draw() override;

  ControllerEmu::Force& m_swing_group;
  WiimoteEmu::MotionState m_motion_state{};
};

class ShakeMappingIndicator : public MappingIndicator
{
public:
  explicit ShakeMappingIndicator(ControllerEmu::Shake& shake) : m_shake_group(shake) {}

private:
  void Draw() override;

  ControllerEmu::Shake& m_shake_group;
  WiimoteEmu::MotionState m_motion_state{};
  std::deque<ControllerEmu::Shake::StateData> m_position_samples;
  int m_grid_line_position = 0;
};

class AccelerometerMappingIndicator : public MappingIndicator
{
public:
  explicit AccelerometerMappingIndicator(ControllerEmu::IMUAccelerometer& accel)
      : m_accel_group(accel)
  {
  }

private:
  void Draw() override;

  ControllerEmu::IMUAccelerometer& m_accel_group;
};

class GyroMappingIndicator : public MappingIndicator
{
public:
  explicit GyroMappingIndicator(ControllerEmu::IMUGyroscope& gyro) : m_gyro_group(gyro) {}

private:
  void Draw() override;

  ControllerEmu::IMUGyroscope& m_gyro_group;
  Common::Matrix33 m_state = Common::Matrix33::Identity();
  Common::Vec3 m_previous_velocity = {};
  u32 m_stable_steps = 0;
};

class CalibrationWidget : public QToolButton
{
public:
  CalibrationWidget(ControllerEmu::ReshapableInput& input, ReshapableInputIndicator& indicator);

  void Update(Common::DVec2 point);

  double GetCalibrationRadiusAtAngle(double angle) const;

  Common::DVec2 GetCenter() const;

  bool IsCalibrating() const;

private:
  void StartCalibration();
  void SetupActions();

  ControllerEmu::ReshapableInput& m_input;
  ReshapableInputIndicator& m_indicator;
  QAction* m_completion_action;
  ControllerEmu::ReshapableInput::CalibrationData m_calibration_data;
  QTimer* m_informative_timer;

  bool m_is_centering = false;
  Common::DVec2 m_new_center;
};
