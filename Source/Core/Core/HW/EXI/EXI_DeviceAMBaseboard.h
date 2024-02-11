
#pragma once

#include "Common/IOFile.h"
#include "Core/HW/EXI/EXI_Device.h"

class PointerWrap;

namespace ExpansionInterface
{
class CEXIAMBaseboard : public IEXIDevice
{
public:
  CEXIAMBaseboard(Core::System& system);
  virtual ~CEXIAMBaseboard();

  void SetCS(int cs) override;
  bool IsPresent() const override;
  void DoState(PointerWrap& p) override;
  bool IsInterruptSet() override;
  void DMAWrite(u32 addr, u32 size) override;
  void DMARead(u32 addr, u32 size) override;

private:
  enum
  {
    AMBB_ISR_READ = 0x82,
    AMBB_UNKNOWN = 0x83,
    AMBB_IMR_READ = 0x86,
    AMBB_IMR_WRITE = 0x87,
    AMBB_LANCNT_WRITE = 0xFF,
  };

  union AD16Reg
  {
    u32 U32 = 0;
    u32 U8[4];
  };

  int m_position;
  u32 m_backup_dma_off;
  u32 m_backup_dma_len;
  bool m_have_irq;
  u32 m_irq_timer;
  u32 m_irq_status;
  unsigned char m_command[4];
  unsigned short m_backoffset;
  File::IOFile* m_backup;

  void TransferByte(u8& _uByte) override;
};
}  // namespace ExpansionInterface
