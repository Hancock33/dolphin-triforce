// Copyright 2016 Dolphin Emulator Project
// Licensed under GPLv2+
// Refer to the license.txt file included.

#include "Core/IPC_HLE/WII_IPC_HLE_Device_stm.h"

IPCCommandResult CWII_IPC_HLE_Device_stm_immediate::Open(u32 command_address, u32 mode)
{
  INFO_LOG(WII_IPC_STM, "STM immediate: Open");
  Memory::Write_U32(GetDeviceID(), command_address + 4);
  m_Active = true;
  return GetDefaultReply();
}

IPCCommandResult CWII_IPC_HLE_Device_stm_immediate::Close(u32 command_address, bool force)
{
  INFO_LOG(WII_IPC_STM, "STM immediate: Close");
  if (!force)
    Memory::Write_U32(0, command_address + 4);
  m_Active = false;
  return GetDefaultReply();
}

IPCCommandResult CWII_IPC_HLE_Device_stm_immediate::IOCtl(u32 command_address)
{
  u32 parameter = Memory::Read_U32(command_address + 0x0C);
  u32 buffer_in = Memory::Read_U32(command_address + 0x10);
  u32 buffer_in_size = Memory::Read_U32(command_address + 0x14);
  u32 buffer_out = Memory::Read_U32(command_address + 0x18);
  u32 buffer_out_size = Memory::Read_U32(command_address + 0x1C);

  // Prepare the out buffer(s) with zeroes as a safety precaution
  // to avoid returning bad values
  Memory::Memset(buffer_out, 0, buffer_out_size);
  u32 return_value = 0;

  switch (parameter)
  {
  case IOCTL_STM_RELEASE_EH:
    INFO_LOG(WII_IPC_STM, "%s - IOCtl:", GetDeviceName().c_str());
    INFO_LOG(WII_IPC_STM, "    IOCTL_STM_RELEASE_EH");
    break;

  case IOCTL_STM_HOTRESET:
    INFO_LOG(WII_IPC_STM, "%s - IOCtl:", GetDeviceName().c_str());
    INFO_LOG(WII_IPC_STM, "    IOCTL_STM_HOTRESET");
    break;

  case IOCTL_STM_VIDIMMING:  // (Input: 20 bytes, Output: 20 bytes)
    INFO_LOG(WII_IPC_STM, "%s - IOCtl:", GetDeviceName().c_str());
    INFO_LOG(WII_IPC_STM, "    IOCTL_STM_VIDIMMING");
    // DumpCommands(buffer_in, buffer_in_size / 4, LogTypes::WII_IPC_STM);
    // Memory::Write_U32(1, buffer_out);
    // return_value = 1;
    break;

  case IOCTL_STM_LEDMODE:  // (Input: 20 bytes, Output: 20 bytes)
    INFO_LOG(WII_IPC_STM, "%s - IOCtl:", GetDeviceName().c_str());
    INFO_LOG(WII_IPC_STM, "    IOCTL_STM_LEDMODE");
    break;

  default:
  {
    _dbg_assert_msg_(WII_IPC_STM, 0, "CWII_IPC_HLE_Device_stm_immediate: 0x%x", parameter);

    INFO_LOG(WII_IPC_STM, "%s - IOCtl:", GetDeviceName().c_str());
    DEBUG_LOG(WII_IPC_STM, "    parameter: 0x%x", parameter);
    DEBUG_LOG(WII_IPC_STM, "    InBuffer: 0x%08x", buffer_in);
    DEBUG_LOG(WII_IPC_STM, "    InBufferSize: 0x%08x", buffer_in_size);
    DEBUG_LOG(WII_IPC_STM, "    OutBuffer: 0x%08x", buffer_out);
    DEBUG_LOG(WII_IPC_STM, "    OutBufferSize: 0x%08x", buffer_out_size);
  }
  break;
  }

  // Write return value to the IPC call
  Memory::Write_U32(return_value, command_address + 0x4);
  return GetDefaultReply();
}

IPCCommandResult CWII_IPC_HLE_Device_stm_eventhook::Open(u32 command_address, u32 mode)
{
  Memory::Write_U32(GetDeviceID(), command_address + 4);
  m_Active = true;
  return GetDefaultReply();
}

IPCCommandResult CWII_IPC_HLE_Device_stm_eventhook::Close(u32 command_address, bool force)
{
  m_event_hook_address = 0;

  INFO_LOG(WII_IPC_STM, "STM eventhook: Close");
  if (!force)
    Memory::Write_U32(0, command_address + 4);
  m_Active = false;
  return GetDefaultReply();
}

IPCCommandResult CWII_IPC_HLE_Device_stm_eventhook::IOCtl(u32 command_address)
{
  u32 parameter = Memory::Read_U32(command_address + 0x0C);
  if (parameter != IOCTL_STM_EVENTHOOK)
  {
    ERROR_LOG(WII_IPC_STM, "Bad IOCtl in CWII_IPC_HLE_Device_stm_eventhook");
    Memory::Write_U32(FS_EINVAL, command_address + 4);
    return GetDefaultReply();
  }

  // IOCTL_STM_EVENTHOOK waits until the reset button or power button
  // is pressed.
  m_event_hook_address = command_address;
  return GetNoReply();
}

void CWII_IPC_HLE_Device_stm_eventhook::ResetButton()
{
  if (!m_Active || m_event_hook_address == 0)
  {
    // If the device isn't open, ignore the button press.
    return;
  }

  // The reset button returns STM_EVENT_RESET.
  u32 buffer_out = Memory::Read_U32(m_event_hook_address + 0x18);
  Memory::Write_U32(STM_EVENT_RESET, buffer_out);

  // Fill in command buffer.
  Memory::Write_U32(FS_SUCCESS, m_event_hook_address + 4);
  Memory::Write_U32(IPC_REP_ASYNC, m_event_hook_address);
  Memory::Write_U32(IPC_CMD_IOCTL, m_event_hook_address + 8);

  // Generate a reply to the IPC command.
  WII_IPC_HLE_Interface::EnqueueReply(m_event_hook_address);
}
