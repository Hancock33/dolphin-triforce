// Copyright 2016 Dolphin Emulator Project
// Licensed under GPLv2+
// Refer to the license.txt file included.

#include "DolphinWX/Input/MicButtonConfigDiag.h"

#include "Core/HW/GCPad.h"
#include "Core/HW/GCPadEmu.h"

MicButtonConfigDialog::MicButtonConfigDialog(wxWindow* const parent, InputConfig& config,
                                             const wxString& name, const int port_num)
    : InputConfigDialog(parent, config, name, port_num)
{
  const int space5 = FromDIP(5);

  auto* const device_chooser = CreateDeviceChooserGroupBox();

  auto* const group_box_button =
      new ControlGroupBox(Pad::GetGroup(port_num, PadGroup::Mic), this, this);

  auto* const controls_sizer = new wxBoxSizer(wxHORIZONTAL);
  controls_sizer->Add(group_box_button, 0, wxEXPAND);

  auto* const szr_main = new wxBoxSizer(wxVERTICAL);
  szr_main->AddSpacer(space5);
  szr_main->Add(device_chooser, 0, wxEXPAND);
  szr_main->AddSpacer(space5);
  szr_main->Add(controls_sizer, 1, wxEXPAND | wxLEFT | wxRIGHT, space5);
  szr_main->AddSpacer(space5);
  szr_main->Add(CreateButtonSizer(wxCLOSE | wxNO_DEFAULT), 0, wxEXPAND | wxLEFT | wxRIGHT, space5);
  szr_main->AddSpacer(space5);

  SetSizer(szr_main);
  Center();
  UpdateGUI();
}
