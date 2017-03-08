// Copyright 2009 Dolphin Emulator Project
// Licensed under GPLv2+
// Refer to the license.txt file included.

#include "Core/IOS/ES/ES.h"

#include <algorithm>
#include <cctype>
#include <cinttypes>
#include <cstdio>
#include <cstring>
#include <memory>
#include <utility>
#include <vector>

#include <mbedtls/aes.h>

#include "Common/Align.h"
#include "Common/Assert.h"
#include "Common/ChunkFile.h"
#include "Common/FileUtil.h"
#include "Common/Logging/Log.h"
#include "Common/MsgHandler.h"
#include "Common/NandPaths.h"
#include "Common/Swap.h"
#include "Core/Boot/Boot.h"
#include "Core/ConfigManager.h"
#include "Core/HLE/HLE.h"
#include "Core/HW/Memmap.h"
#include "Core/IOS/ES/Formats.h"
#include "Core/PatchEngine.h"
#include "Core/PowerPC/PPCSymbolDB.h"
#include "Core/ec_wii.h"
#include "DiscIO/NANDContentLoader.h"
#include "DiscIO/Volume.h"
#include "VideoCommon/HiresTextures.h"

namespace IOS
{
namespace HLE
{
namespace Device
{
struct TitleContext
{
  void Clear();
  void DoState(PointerWrap& p);
  void Update(const DiscIO::CNANDContentLoader& content_loader);
  void Update(const IOS::ES::TMDReader& tmd_, const IOS::ES::TicketReader& ticket_);
  void UpdateRunningGame() const;

  IOS::ES::TicketReader ticket;
  IOS::ES::TMDReader tmd;
  bool active = false;
  bool first_change = true;
};

// Shared across all ES instances.
static std::string s_content_file;
static TitleContext s_title_context;

// Title to launch after IOS has been reset and reloaded (similar to /sys/launch.sys).
static u64 s_title_to_launch;

constexpr u8 s_key_sd[0x10] = {0xab, 0x01, 0xb9, 0xd8, 0xe1, 0x62, 0x2b, 0x08,
                               0xaf, 0xba, 0xd8, 0x4d, 0xbf, 0xc2, 0xa5, 0x5d};
constexpr u8 s_key_ecc[0x1e] = {0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
                                0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
                                0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x01};
constexpr u8 s_key_empty[0x10] = {0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
                                  0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00};

// default key table
constexpr const u8* s_key_table[11] = {
    s_key_ecc,    // ECC Private Key
    s_key_empty,  // Console ID
    s_key_empty,  // NAND AES Key
    s_key_empty,  // NAND HMAC
    s_key_empty,  // Common Key
    s_key_empty,  // PRNG seed
    s_key_sd,     // SD Key
    s_key_empty,  // Unknown
    s_key_empty,  // Unknown
    s_key_empty,  // Unknown
    s_key_empty,  // Unknown
};

ES::ES(u32 device_id, const std::string& device_name) : Device(device_id, device_name)
{
}

void ES::Init()
{
  s_content_file = "";
  s_title_context = TitleContext{};

  if (s_title_to_launch != 0)
  {
    NOTICE_LOG(IOS, "Re-launching title after IOS reload.");
    LaunchTitle(s_title_to_launch, true);
    s_title_to_launch = 0;
  }
}

void TitleContext::Clear()
{
  ticket.SetBytes({});
  tmd.SetBytes({});
  active = false;
}

void TitleContext::DoState(PointerWrap& p)
{
  ticket.DoState(p);
  tmd.DoState(p);
  p.Do(active);
}

void TitleContext::Update(const DiscIO::CNANDContentLoader& content_loader)
{
  if (!content_loader.IsValid())
    return;
  Update(content_loader.GetTMD(), content_loader.GetTicket());
}

void TitleContext::Update(const IOS::ES::TMDReader& tmd_, const IOS::ES::TicketReader& ticket_)
{
  if (!tmd_.IsValid() || !ticket_.IsValid())
  {
    ERROR_LOG(IOS_ES, "TMD or ticket is not valid -- refusing to update title context");
    return;
  }

  ticket = ticket_;
  tmd = tmd_;
  active = true;

  // Interesting title changes (channel or disc game launch) always happen after an IOS reload.
  if (first_change)
  {
    UpdateRunningGame();
    first_change = false;
  }
}

void TitleContext::UpdateRunningGame() const
{
  // This one does not always make sense for Wii titles, so let's reset it back to a sane value.
  SConfig::GetInstance().m_strName = "";
  if (IOS::ES::IsTitleType(tmd.GetTitleId(), IOS::ES::TitleType::Game) ||
      IOS::ES::IsTitleType(tmd.GetTitleId(), IOS::ES::TitleType::GameWithChannel))
  {
    const u32 title_identifier = Common::swap32(static_cast<u32>(tmd.GetTitleId()));
    const u16 group_id = Common::swap16(tmd.GetGroupId());

    char ascii_game_id[6];
    std::memcpy(ascii_game_id, &title_identifier, sizeof(title_identifier));
    std::memcpy(ascii_game_id + sizeof(title_identifier), &group_id, sizeof(group_id));

    SConfig::GetInstance().m_strGameID = ascii_game_id;
  }
  else
  {
    SConfig::GetInstance().m_strGameID = StringFromFormat("%016" PRIX64, tmd.GetTitleId());
  }

  SConfig::GetInstance().m_title_id = tmd.GetTitleId();

  // TODO: have a callback mechanism for title changes?
  g_symbolDB.Clear();
  CBoot::LoadMapFromFilename();
  ::HLE::Clear();
  ::HLE::PatchFunctions();
  PatchEngine::Shutdown();
  PatchEngine::LoadPatches();
  HiresTexture::Update();

  NOTICE_LOG(IOS_ES, "Active title: %016" PRIx64, tmd.GetTitleId());
}

void ES::LoadWAD(const std::string& _rContentFile)
{
  s_content_file = _rContentFile;
  // XXX: Ideally, this should be done during a launch, but because we support launching WADs
  // without installing them (which is a bit of a hack), we have to do this manually here.
  const auto& content_loader = DiscIO::CNANDContentManager::Access().GetNANDLoader(s_content_file);
  s_title_context.Update(content_loader);
  INFO_LOG(IOS_ES, "LoadWAD: Title context changed: %016" PRIx64, s_title_context.tmd.GetTitleId());
}

void ES::DecryptContent(u32 key_index, u8* iv, u8* input, u32 size, u8* new_iv, u8* output)
{
  mbedtls_aes_context AES_ctx;
  mbedtls_aes_setkey_dec(&AES_ctx, s_key_table[key_index], 128);
  memcpy(new_iv, iv, 16);
  mbedtls_aes_crypt_cbc(&AES_ctx, MBEDTLS_AES_DECRYPT, size, new_iv, input, output);
}

bool ES::LaunchTitle(u64 title_id, bool skip_reload)
{
  s_title_context.Clear();
  INFO_LOG(IOS_ES, "ES_Launch: Title context changed: (none)");

  NOTICE_LOG(IOS_ES, "Launching title %016" PRIx64 "...", title_id);

  // ES_Launch should probably reset the whole state, which at least means closing all open files.
  // leaving them open through ES_Launch may cause hangs and other funky behavior
  // (supposedly when trying to re-open those files).
  DiscIO::CNANDContentManager::Access().ClearCache();

  if (IsTitleType(title_id, IOS::ES::TitleType::System) && title_id != TITLEID_SYSMENU)
    return LaunchIOS(title_id);
  return LaunchPPCTitle(title_id, skip_reload);
}

bool ES::LaunchIOS(u64 ios_title_id)
{
  return Reload(ios_title_id);
}

bool ES::LaunchPPCTitle(u64 title_id, bool skip_reload)
{
  const DiscIO::CNANDContentLoader& content_loader = AccessContentDevice(title_id);
  if (!content_loader.IsValid())
  {
    PanicAlertT("Could not launch title %016" PRIx64 " because it is missing from the NAND.\n"
                "The emulated software will likely hang now.",
                title_id);
    return false;
  }

  if (!content_loader.GetTMD().IsValid() || !content_loader.GetTicket().IsValid())
    return false;

  // Before launching a title, IOS first reads the TMD and reloads into the specified IOS version,
  // even when that version is already running. After it has reloaded, ES_Launch will be called
  // again with the reload skipped, and the PPC will be bootstrapped then.
  if (!skip_reload)
  {
    s_title_to_launch = title_id;
    const u64 required_ios = content_loader.GetTMD().GetIOSId();
    return LaunchTitle(required_ios);
  }

  s_title_context.Update(content_loader);
  INFO_LOG(IOS_ES, "LaunchPPCTitle: Title context changed: %016" PRIx64,
           s_title_context.tmd.GetTitleId());
  return BootstrapPPC(content_loader);
}

void ES::DoState(PointerWrap& p)
{
  Device::DoState(p);
  p.Do(s_content_file);
  p.Do(m_AccessIdentID);
  s_title_context.DoState(p);

  m_addtitle_tmd.DoState(p);
  p.Do(m_addtitle_content_id);
  p.Do(m_addtitle_content_buffer);

  p.Do(m_export_title_context.valid);
  m_export_title_context.tmd.DoState(p);
  p.Do(m_export_title_context.title_key);
  p.Do(m_export_title_context.contents);

  u32 Count = (u32)(m_ContentAccessMap.size());
  p.Do(Count);

  if (p.GetMode() == PointerWrap::MODE_READ)
  {
    for (u32 i = 0; i < Count; i++)
    {
      u32 cfd = 0;
      OpenedContent content;
      p.Do(cfd);
      p.Do(content);
      cfd = OpenTitleContent(cfd, content.m_title_id, content.m_content.index);
    }
  }
  else
  {
    for (const auto& pair : m_ContentAccessMap)
    {
      p.Do(pair.first);
      p.Do(pair.second);
    }
  }
}

ReturnCode ES::Open(const OpenRequest& request)
{
  if (m_is_active)
    INFO_LOG(IOS_ES, "Device was re-opened.");
  return Device::Open(request);
}

void ES::Close()
{
  // XXX: does IOS really clear the content access map here?
  m_ContentAccessMap.clear();
  m_AccessIdentID = 0;

  INFO_LOG(IOS_ES, "ES: Close");
  m_is_active = false;
  // clear the NAND content cache to make sure nothing remains open.
  DiscIO::CNANDContentManager::Access().ClearCache();
}

u32 ES::OpenTitleContent(u32 CFD, u64 TitleID, u16 Index)
{
  const DiscIO::CNANDContentLoader& Loader = AccessContentDevice(TitleID);

  if (!Loader.IsValid() || !Loader.GetTMD().IsValid() || !Loader.GetTicket().IsValid())
  {
    WARN_LOG(IOS_ES, "ES: loader not valid for %" PRIx64, TitleID);
    return 0xffffffff;
  }

  const DiscIO::SNANDContent* pContent = Loader.GetContentByIndex(Index);

  if (pContent == nullptr)
  {
    return 0xffffffff;  // TODO: what is the correct error value here?
  }

  OpenedContent content;
  content.m_position = 0;
  content.m_content = pContent->m_metadata;
  content.m_title_id = TitleID;

  pContent->m_Data->Open();

  m_ContentAccessMap[CFD] = content;
  return CFD;
}

IPCCommandResult ES::IOCtlV(const IOCtlVRequest& request)
{
  DEBUG_LOG(IOS_ES, "%s (0x%x)", GetDeviceName().c_str(), request.request);

  // Clear the IO buffers. Note that this is unsafe for other ioctlvs.
  for (const auto& io_vector : request.io_vectors)
  {
    if (!request.HasInputVectorWithAddress(io_vector.address))
      Memory::Memset(io_vector.address, 0, io_vector.size);
  }

  switch (request.request)
  {
  case IOCTL_ES_ADDTICKET:
    return AddTicket(request);
  case IOCTL_ES_ADDTMD:
    return AddTMD(request);
  case IOCTL_ES_ADDTITLESTART:
    return AddTitleStart(request);
  case IOCTL_ES_ADDCONTENTSTART:
    return AddContentStart(request);
  case IOCTL_ES_ADDCONTENTDATA:
    return AddContentData(request);
  case IOCTL_ES_ADDCONTENTFINISH:
    return AddContentFinish(request);
  case IOCTL_ES_ADDTITLEFINISH:
    return AddTitleFinish(request);
  case IOCTL_ES_GETDEVICEID:
    return ESGetDeviceID(request);
  case IOCTL_ES_GETTITLECONTENTSCNT:
    return GetTitleContentsCount(request);
  case IOCTL_ES_GETTITLECONTENTS:
    return GetTitleContents(request);
  case IOCTL_ES_OPENTITLECONTENT:
    return OpenTitleContent(request);
  case IOCTL_ES_OPENCONTENT:
    return OpenContent(request);
  case IOCTL_ES_READCONTENT:
    return ReadContent(request);
  case IOCTL_ES_CLOSECONTENT:
    return CloseContent(request);
  case IOCTL_ES_SEEKCONTENT:
    return SeekContent(request);
  case IOCTL_ES_GETTITLEDIR:
    return GetTitleDirectory(request);
  case IOCTL_ES_GETTITLEID:
    return GetTitleID(request);
  case IOCTL_ES_SETUID:
    return SetUID(request);

  case IOCTL_ES_GETOWNEDTITLECNT:
    return GetOwnedTitleCount(request);
  case IOCTL_ES_GETOWNEDTITLES:
    return GetOwnedTitles(request);
  case IOCTL_ES_GETTITLECNT:
    return GetTitleCount(request);
  case IOCTL_ES_GETTITLES:
    return GetTitles(request);

  case IOCTL_ES_GETVIEWCNT:
    return GetViewCount(request);
  case IOCTL_ES_GETVIEWS:
    return GetViews(request);
  case IOCTL_ES_DIGETTICKETVIEW:
    return DIGetTicketView(request);

  case IOCTL_ES_GETTMDVIEWCNT:
    return GetTMDViewSize(request);
  case IOCTL_ES_GETTMDVIEWS:
    return GetTMDViews(request);

  case IOCTL_ES_DIGETTMDVIEWSIZE:
    return DIGetTMDViewSize(request);
  case IOCTL_ES_DIGETTMDVIEW:
    return DIGetTMDView(request);

  case IOCTL_ES_GETCONSUMPTION:
    return GetConsumption(request);
  case IOCTL_ES_DELETETITLE:
    return DeleteTitle(request);
  case IOCTL_ES_DELETETICKET:
    return DeleteTicket(request);
  case IOCTL_ES_DELETETITLECONTENT:
    return DeleteTitleContent(request);
  case IOCTL_ES_GETSTOREDTMDSIZE:
    return GetStoredTMDSize(request);
  case IOCTL_ES_GETSTOREDTMD:
    return GetStoredTMD(request);
  case IOCTL_ES_ENCRYPT:
    return Encrypt(request);
  case IOCTL_ES_DECRYPT:
    return Decrypt(request);
  case IOCTL_ES_LAUNCH:
    return Launch(request);
  case IOCTL_ES_LAUNCHBC:
    return LaunchBC(request);
  case IOCTL_ES_EXPORTTITLEINIT:
    return ExportTitleInit(request);
  case IOCTL_ES_EXPORTCONTENTBEGIN:
    return ExportContentBegin(request);
  case IOCTL_ES_EXPORTCONTENTDATA:
    return ExportContentData(request);
  case IOCTL_ES_EXPORTCONTENTEND:
    return ExportContentEnd(request);
  case IOCTL_ES_EXPORTTITLEDONE:
    return ExportTitleDone(request);
  case IOCTL_ES_CHECKKOREAREGION:
    return CheckKoreaRegion(request);
  case IOCTL_ES_GETDEVICECERT:
    return GetDeviceCertificate(request);
  case IOCTL_ES_SIGN:
    return Sign(request);
  case IOCTL_ES_GETBOOT2VERSION:
    return GetBoot2Version(request);
  default:
    request.DumpUnknown(GetDeviceName(), LogTypes::IOS_ES);
    break;
  }

  return GetDefaultReply(IPC_SUCCESS);
}

IPCCommandResult ES::AddTicket(const IOCtlVRequest& request)
{
  if (!request.HasNumberOfValidVectors(3, 0))
    return GetDefaultReply(ES_PARAMETER_SIZE_OR_ALIGNMENT);

  INFO_LOG(IOS_ES, "IOCTL_ES_ADDTICKET");
  std::vector<u8> ticket(request.in_vectors[0].size);
  Memory::CopyFromEmu(ticket.data(), request.in_vectors[0].address, request.in_vectors[0].size);

  DiscIO::AddTicket(IOS::ES::TicketReader{std::move(ticket)});

  return GetDefaultReply(IPC_SUCCESS);
}

// TODO: write this to /tmp (or /import?) first, as title imports can be cancelled.
static bool WriteTMD(const IOS::ES::TMDReader& tmd)
{
  const std::string tmd_path = Common::GetTMDFileName(tmd.GetTitleId(), Common::FROM_SESSION_ROOT);
  File::CreateFullPath(tmd_path);

  File::IOFile fp(tmd_path, "wb");
  return fp.WriteBytes(tmd.GetRawTMD().data(), tmd.GetRawTMD().size());
}

IPCCommandResult ES::AddTMD(const IOCtlVRequest& request)
{
  // This may appear to be very similar to AddTitleStart, but AddTitleStart takes
  // three additional vectors and may do some additional processing -- so let's keep these separate.

  if (!request.HasNumberOfValidVectors(1, 0))
    return GetDefaultReply(ES_PARAMETER_SIZE_OR_ALIGNMENT);

  std::vector<u8> tmd(request.in_vectors[0].size);
  Memory::CopyFromEmu(tmd.data(), request.in_vectors[0].address, request.in_vectors[0].size);

  m_addtitle_tmd.SetBytes(tmd);
  if (!m_addtitle_tmd.IsValid())
    return GetDefaultReply(ES_INVALID_TMD);

  if (!WriteTMD(m_addtitle_tmd))
    return GetDefaultReply(ES_WRITE_FAILURE);

  return GetDefaultReply(IPC_SUCCESS);
}

IPCCommandResult ES::AddTitleStart(const IOCtlVRequest& request)
{
  if (!request.HasNumberOfValidVectors(4, 0))
    return GetDefaultReply(ES_PARAMETER_SIZE_OR_ALIGNMENT);

  INFO_LOG(IOS_ES, "IOCTL_ES_ADDTITLESTART");
  std::vector<u8> tmd(request.in_vectors[0].size);
  Memory::CopyFromEmu(tmd.data(), request.in_vectors[0].address, request.in_vectors[0].size);

  m_addtitle_tmd.SetBytes(tmd);
  if (!m_addtitle_tmd.IsValid())
  {
    ERROR_LOG(IOS_ES, "Invalid TMD while adding title (size = %zd)", tmd.size());
    return GetDefaultReply(ES_INVALID_TMD);
  }

  if (!WriteTMD(m_addtitle_tmd))
    return GetDefaultReply(ES_WRITE_FAILURE);

  DiscIO::cUIDsys uid_sys{Common::FROM_CONFIGURED_ROOT};
  uid_sys.AddTitle(m_addtitle_tmd.GetTitleId());

  return GetDefaultReply(IPC_SUCCESS);
}

IPCCommandResult ES::AddContentStart(const IOCtlVRequest& request)
{
  if (!request.HasNumberOfValidVectors(2, 0))
    return GetDefaultReply(ES_PARAMETER_SIZE_OR_ALIGNMENT);

  u64 title_id = Memory::Read_U64(request.in_vectors[0].address);
  u32 content_id = Memory::Read_U32(request.in_vectors[1].address);

  if (m_addtitle_content_id != 0xFFFFFFFF)
  {
    ERROR_LOG(IOS_ES, "Trying to add content when we haven't finished adding "
                      "another content. Unsupported.");
    return GetDefaultReply(ES_WRITE_FAILURE);
  }
  m_addtitle_content_id = content_id;

  m_addtitle_content_buffer.clear();

  INFO_LOG(IOS_ES, "IOCTL_ES_ADDCONTENTSTART: title id %016" PRIx64 ", "
                   "content id %08x",
           title_id, m_addtitle_content_id);

  if (!m_addtitle_tmd.IsValid())
    return GetDefaultReply(ES_PARAMETER_SIZE_OR_ALIGNMENT);

  if (title_id != m_addtitle_tmd.GetTitleId())
  {
    ERROR_LOG(IOS_ES, "IOCTL_ES_ADDCONTENTSTART: title id %016" PRIx64 " != "
                      "TMD title id %016" PRIx64 ", ignoring",
              title_id, m_addtitle_tmd.GetTitleId());
  }

  // We're supposed to return a "content file descriptor" here, which is
  // passed to further AddContentData / AddContentFinish. But so far there is
  // no known content installer which performs content addition concurrently.
  // Instead we just log an error (see above) if this condition is detected.
  s32 content_fd = 0;
  return GetDefaultReply(content_fd);
}

IPCCommandResult ES::AddContentData(const IOCtlVRequest& request)
{
  if (!request.HasNumberOfValidVectors(2, 0))
    return GetDefaultReply(ES_PARAMETER_SIZE_OR_ALIGNMENT);

  u32 content_fd = Memory::Read_U32(request.in_vectors[0].address);
  INFO_LOG(IOS_ES, "IOCTL_ES_ADDCONTENTDATA: content fd %08x, "
                   "size %d",
           content_fd, request.in_vectors[1].size);

  u8* data_start = Memory::GetPointer(request.in_vectors[1].address);
  u8* data_end = data_start + request.in_vectors[1].size;
  m_addtitle_content_buffer.insert(m_addtitle_content_buffer.end(), data_start, data_end);
  return GetDefaultReply(IPC_SUCCESS);
}

IPCCommandResult ES::AddContentFinish(const IOCtlVRequest& request)
{
  if (!request.HasNumberOfValidVectors(1, 0))
    return GetDefaultReply(ES_PARAMETER_SIZE_OR_ALIGNMENT);

  u32 content_fd = Memory::Read_U32(request.in_vectors[0].address);
  INFO_LOG(IOS_ES, "IOCTL_ES_ADDCONTENTFINISH: content fd %08x", content_fd);

  if (!m_addtitle_tmd.IsValid())
    return GetDefaultReply(ES_PARAMETER_SIZE_OR_ALIGNMENT);

  // Try to find the title key from a pre-installed ticket.
  IOS::ES::TicketReader ticket = DiscIO::FindSignedTicket(m_addtitle_tmd.GetTitleId());
  if (!ticket.IsValid())
  {
    return GetDefaultReply(ES_NO_TICKET_INSTALLED);
  }

  mbedtls_aes_context aes_ctx;
  mbedtls_aes_setkey_dec(&aes_ctx, ticket.GetTitleKey().data(), 128);

  // The IV for title content decryption is the lower two bytes of the
  // content index, zero extended.
  IOS::ES::Content content_info;
  if (!m_addtitle_tmd.FindContentById(m_addtitle_content_id, &content_info))
  {
    return GetDefaultReply(ES_INVALID_TMD);
  }
  u8 iv[16] = {0};
  iv[0] = (content_info.index >> 8) & 0xFF;
  iv[1] = content_info.index & 0xFF;
  std::vector<u8> decrypted_data(m_addtitle_content_buffer.size());
  mbedtls_aes_crypt_cbc(&aes_ctx, MBEDTLS_AES_DECRYPT, m_addtitle_content_buffer.size(), iv,
                        m_addtitle_content_buffer.data(), decrypted_data.data());

  std::string content_path;
  if (content_info.IsShared())
  {
    DiscIO::CSharedContent shared_content{Common::FROM_SESSION_ROOT};
    content_path = shared_content.AddSharedContent(content_info.sha1.data());
  }
  else
  {
    content_path = StringFromFormat(
        "%s%08x.app",
        Common::GetTitleContentPath(m_addtitle_tmd.GetTitleId(), Common::FROM_SESSION_ROOT).c_str(),
        m_addtitle_content_id);
  }

  File::IOFile fp(content_path, "wb");
  fp.WriteBytes(decrypted_data.data(), content_info.size);

  m_addtitle_content_id = 0xFFFFFFFF;
  return GetDefaultReply(IPC_SUCCESS);
}

IPCCommandResult ES::AddTitleFinish(const IOCtlVRequest& request)
{
  if (!request.HasNumberOfValidVectors(0, 0) || !m_addtitle_tmd.IsValid())
    return GetDefaultReply(ES_PARAMETER_SIZE_OR_ALIGNMENT);

  INFO_LOG(IOS_ES, "IOCTL_ES_ADDTITLEFINISH");
  m_addtitle_tmd.SetBytes({});
  return GetDefaultReply(IPC_SUCCESS);
}

IPCCommandResult ES::ESGetDeviceID(const IOCtlVRequest& request)
{
  if (!request.HasNumberOfValidVectors(0, 1))
    return GetDefaultReply(ES_PARAMETER_SIZE_OR_ALIGNMENT);

  const EcWii& ec = EcWii::GetInstance();
  INFO_LOG(IOS_ES, "IOCTL_ES_GETDEVICEID %08X", ec.GetNGID());
  Memory::Write_U32(ec.GetNGID(), request.io_vectors[0].address);
  return GetDefaultReply(IPC_SUCCESS);
}

IPCCommandResult ES::GetTitleContentsCount(const IOCtlVRequest& request)
{
  if (!request.HasNumberOfValidVectors(1, 1))
    return GetDefaultReply(ES_PARAMETER_SIZE_OR_ALIGNMENT);

  u64 TitleID = Memory::Read_U64(request.in_vectors[0].address);

  const DiscIO::CNANDContentLoader& nand_content = AccessContentDevice(TitleID);
  if (!nand_content.IsValid() || !nand_content.GetTMD().IsValid())
    return GetDefaultReply(ES_PARAMETER_SIZE_OR_ALIGNMENT);

  const u16 num_contents = nand_content.GetTMD().GetNumContents();

  if ((u32)(TitleID >> 32) == 0x00010000)
    Memory::Write_U32(0, request.io_vectors[0].address);
  else
    Memory::Write_U32(num_contents, request.io_vectors[0].address);

  INFO_LOG(IOS_ES, "IOCTL_ES_GETTITLECONTENTSCNT: TitleID: %08x/%08x  content count %i",
           (u32)(TitleID >> 32), (u32)TitleID, num_contents);

  return GetDefaultReply(IPC_SUCCESS);
}

IPCCommandResult ES::GetTitleContents(const IOCtlVRequest& request)
{
  if (!request.HasNumberOfValidVectors(2, 1))
    return GetDefaultReply(ES_PARAMETER_SIZE_OR_ALIGNMENT);

  u64 TitleID = Memory::Read_U64(request.in_vectors[0].address);

  const DiscIO::CNANDContentLoader& rNANDContent = AccessContentDevice(TitleID);
  if (!rNANDContent.IsValid() || !rNANDContent.GetTMD().IsValid())
    return GetDefaultReply(ES_PARAMETER_SIZE_OR_ALIGNMENT);

  for (const auto& content : rNANDContent.GetTMD().GetContents())
  {
    const u16 index = content.index;
    Memory::Write_U32(content.id, request.io_vectors[0].address + index * 4);
    INFO_LOG(IOS_ES, "IOCTL_ES_GETTITLECONTENTS: Index %d: %08x", index, content.id);
  }

  return GetDefaultReply(IPC_SUCCESS);
}

IPCCommandResult ES::OpenTitleContent(const IOCtlVRequest& request)
{
  if (!request.HasNumberOfValidVectors(3, 0))
    return GetDefaultReply(ES_PARAMETER_SIZE_OR_ALIGNMENT);

  u64 TitleID = Memory::Read_U64(request.in_vectors[0].address);
  u32 Index = Memory::Read_U32(request.in_vectors[2].address);

  s32 CFD = OpenTitleContent(m_AccessIdentID++, TitleID, Index);

  INFO_LOG(IOS_ES, "IOCTL_ES_OPENTITLECONTENT: TitleID: %08x/%08x  Index %i -> got CFD %x",
           (u32)(TitleID >> 32), (u32)TitleID, Index, CFD);

  return GetDefaultReply(CFD);
}

IPCCommandResult ES::OpenContent(const IOCtlVRequest& request)
{
  if (!request.HasNumberOfValidVectors(1, 0))
    return GetDefaultReply(ES_PARAMETER_SIZE_OR_ALIGNMENT);
  u32 Index = Memory::Read_U32(request.in_vectors[0].address);

  if (!s_title_context.active)
    return GetDefaultReply(ES_PARAMETER_SIZE_OR_ALIGNMENT);

  s32 CFD = OpenTitleContent(m_AccessIdentID++, s_title_context.tmd.GetTitleId(), Index);
  INFO_LOG(IOS_ES, "IOCTL_ES_OPENCONTENT: Index %i -> got CFD %x", Index, CFD);

  return GetDefaultReply(CFD);
}

IPCCommandResult ES::ReadContent(const IOCtlVRequest& request)
{
  if (!request.HasNumberOfValidVectors(1, 1))
    return GetDefaultReply(ES_PARAMETER_SIZE_OR_ALIGNMENT);

  u32 CFD = Memory::Read_U32(request.in_vectors[0].address);
  u32 Size = request.io_vectors[0].size;
  u32 Addr = request.io_vectors[0].address;

  auto itr = m_ContentAccessMap.find(CFD);
  if (itr == m_ContentAccessMap.end())
  {
    return GetDefaultReply(-1);
  }
  OpenedContent& rContent = itr->second;

  u8* pDest = Memory::GetPointer(Addr);

  if (rContent.m_position + Size > rContent.m_content.size)
  {
    Size = static_cast<u32>(rContent.m_content.size) - rContent.m_position;
  }

  if (Size > 0)
  {
    if (pDest)
    {
      const DiscIO::CNANDContentLoader& ContentLoader = AccessContentDevice(rContent.m_title_id);
      // ContentLoader should never be invalid; rContent has been created by it.
      if (ContentLoader.IsValid() && ContentLoader.GetTicket().IsValid())
      {
        const DiscIO::SNANDContent* pContent =
            ContentLoader.GetContentByIndex(rContent.m_content.index);
        if (!pContent->m_Data->GetRange(rContent.m_position, Size, pDest))
          ERROR_LOG(IOS_ES, "ES: failed to read %u bytes from %u!", Size, rContent.m_position);
      }

      rContent.m_position += Size;
    }
    else
    {
      PanicAlert("IOCTL_ES_READCONTENT - bad destination");
    }
  }

  DEBUG_LOG(IOS_ES,
            "IOCTL_ES_READCONTENT: CFD %x, Address 0x%x, Size %i -> stream pos %i (Index %i)", CFD,
            Addr, Size, rContent.m_position, rContent.m_content.index);

  return GetDefaultReply(Size);
}

IPCCommandResult ES::CloseContent(const IOCtlVRequest& request)
{
  if (!request.HasNumberOfValidVectors(1, 0))
    return GetDefaultReply(ES_PARAMETER_SIZE_OR_ALIGNMENT);

  u32 CFD = Memory::Read_U32(request.in_vectors[0].address);

  INFO_LOG(IOS_ES, "IOCTL_ES_CLOSECONTENT: CFD %x", CFD);

  auto itr = m_ContentAccessMap.find(CFD);
  if (itr == m_ContentAccessMap.end())
  {
    return GetDefaultReply(-1);
  }

  const DiscIO::CNANDContentLoader& ContentLoader = AccessContentDevice(itr->second.m_title_id);
  // ContentLoader should never be invalid; we shouldn't be here if ES_OPENCONTENT failed before.
  if (ContentLoader.IsValid())
  {
    const DiscIO::SNANDContent* pContent =
        ContentLoader.GetContentByIndex(itr->second.m_content.index);
    pContent->m_Data->Close();
  }

  m_ContentAccessMap.erase(itr);

  return GetDefaultReply(IPC_SUCCESS);
}

IPCCommandResult ES::SeekContent(const IOCtlVRequest& request)
{
  if (!request.HasNumberOfValidVectors(3, 0))
    return GetDefaultReply(ES_PARAMETER_SIZE_OR_ALIGNMENT);

  u32 CFD = Memory::Read_U32(request.in_vectors[0].address);
  u32 Addr = Memory::Read_U32(request.in_vectors[1].address);
  u32 Mode = Memory::Read_U32(request.in_vectors[2].address);

  auto itr = m_ContentAccessMap.find(CFD);
  if (itr == m_ContentAccessMap.end())
  {
    return GetDefaultReply(-1);
  }
  OpenedContent& rContent = itr->second;

  switch (Mode)
  {
  case 0:  // SET
    rContent.m_position = Addr;
    break;

  case 1:  // CUR
    rContent.m_position += Addr;
    break;

  case 2:  // END
    rContent.m_position = static_cast<u32>(rContent.m_content.size) + Addr;
    break;
  }

  DEBUG_LOG(IOS_ES, "IOCTL_ES_SEEKCONTENT: CFD %x, Address 0x%x, Mode %i -> Pos %i", CFD, Addr,
            Mode, rContent.m_position);

  return GetDefaultReply(rContent.m_position);
}

IPCCommandResult ES::GetTitleDirectory(const IOCtlVRequest& request)
{
  if (!request.HasNumberOfValidVectors(1, 1))
    return GetDefaultReply(ES_PARAMETER_SIZE_OR_ALIGNMENT);

  u64 TitleID = Memory::Read_U64(request.in_vectors[0].address);

  char* Path = (char*)Memory::GetPointer(request.io_vectors[0].address);
  sprintf(Path, "/title/%08x/%08x/data", (u32)(TitleID >> 32), (u32)TitleID);

  INFO_LOG(IOS_ES, "IOCTL_ES_GETTITLEDIR: %s", Path);
  return GetDefaultReply(IPC_SUCCESS);
}

IPCCommandResult ES::GetTitleID(const IOCtlVRequest& request)
{
  if (!request.HasNumberOfValidVectors(0, 1))
    return GetDefaultReply(ES_PARAMETER_SIZE_OR_ALIGNMENT);

  if (!s_title_context.active)
    return GetDefaultReply(ES_PARAMETER_SIZE_OR_ALIGNMENT);

  const u64 title_id = s_title_context.tmd.GetTitleId();
  Memory::Write_U64(title_id, request.io_vectors[0].address);
  INFO_LOG(IOS_ES, "IOCTL_ES_GETTITLEID: %08x/%08x", static_cast<u32>(title_id >> 32),
           static_cast<u32>(title_id));
  return GetDefaultReply(IPC_SUCCESS);
}

IPCCommandResult ES::SetUID(const IOCtlVRequest& request)
{
  if (!request.HasNumberOfValidVectors(1, 0))
    return GetDefaultReply(ES_PARAMETER_SIZE_OR_ALIGNMENT);

  // TODO: fs permissions based on this
  u64 TitleID = Memory::Read_U64(request.in_vectors[0].address);
  INFO_LOG(IOS_ES, "IOCTL_ES_SETUID titleID: %08x/%08x", (u32)(TitleID >> 32), (u32)TitleID);
  return GetDefaultReply(IPC_SUCCESS);
}

static bool IsValidPartOfTitleID(const std::string& string)
{
  if (string.length() != 8)
    return false;
  return std::all_of(string.begin(), string.end(),
                     [](const auto character) { return std::isxdigit(character) != 0; });
}

// Returns a vector of title IDs. IOS does not check the TMD at all here.
static std::vector<u64> GetInstalledTitles()
{
  const std::string titles_dir = Common::RootUserPath(Common::FROM_SESSION_ROOT) + "/title";
  if (!File::IsDirectory(titles_dir))
  {
    ERROR_LOG(IOS_ES, "/title is not a directory");
    return {};
  }

  std::vector<u64> title_ids;

  // The /title directory contains one directory per title type, and each of them contains
  // a directory per title (where the name is the low 32 bits of the title ID in %08x format).
  const auto entries = File::ScanDirectoryTree(titles_dir, true);
  for (const File::FSTEntry& title_type : entries.children)
  {
    if (!title_type.isDirectory || !IsValidPartOfTitleID(title_type.virtualName))
      continue;

    if (title_type.children.empty())
      continue;

    for (const File::FSTEntry& title_identifier : title_type.children)
    {
      if (!title_identifier.isDirectory || !IsValidPartOfTitleID(title_identifier.virtualName))
        continue;

      const u32 type = std::stoul(title_type.virtualName, nullptr, 16);
      const u32 identifier = std::stoul(title_identifier.virtualName, nullptr, 16);
      title_ids.push_back(static_cast<u64>(type) << 32 | identifier);
    }
  }

  return title_ids;
}

// Returns a vector of title IDs for which there is a ticket.
static std::vector<u64> GetTitlesWithTickets()
{
  const std::string titles_dir = Common::RootUserPath(Common::FROM_SESSION_ROOT) + "/ticket";
  if (!File::IsDirectory(titles_dir))
  {
    ERROR_LOG(IOS_ES, "/ticket is not a directory");
    return {};
  }

  std::vector<u64> title_ids;

  // The /ticket directory contains one directory per title type, and each of them contains
  // one ticket per title (where the name is the low 32 bits of the title ID in %08x format).
  const auto entries = File::ScanDirectoryTree(titles_dir, true);
  for (const File::FSTEntry& title_type : entries.children)
  {
    if (!title_type.isDirectory || !IsValidPartOfTitleID(title_type.virtualName))
      continue;

    if (title_type.children.empty())
      continue;

    for (const File::FSTEntry& ticket : title_type.children)
    {
      const std::string name_without_ext = ticket.virtualName.substr(0, 8);
      if (ticket.isDirectory || !IsValidPartOfTitleID(name_without_ext) ||
          name_without_ext + ".tik" != ticket.virtualName)
      {
        continue;
      }

      const u32 type = std::stoul(title_type.virtualName, nullptr, 16);
      const u32 identifier = std::stoul(name_without_ext, nullptr, 16);
      title_ids.push_back(static_cast<u64>(type) << 32 | identifier);
    }
  }

  return title_ids;
}

IPCCommandResult ES::GetTitleCount(const std::vector<u64>& titles, const IOCtlVRequest& request)
{
  if (!request.HasNumberOfValidVectors(0, 1) || request.io_vectors[0].size != 4)
    return GetDefaultReply(ES_PARAMETER_SIZE_OR_ALIGNMENT);

  Memory::Write_U32(static_cast<u32>(titles.size()), request.io_vectors[0].address);

  return GetDefaultReply(IPC_SUCCESS);
}

IPCCommandResult ES::GetTitles(const std::vector<u64>& titles, const IOCtlVRequest& request)
{
  if (!request.HasNumberOfValidVectors(1, 1))
    return GetDefaultReply(ES_PARAMETER_SIZE_OR_ALIGNMENT);

  const size_t max_count = Memory::Read_U32(request.in_vectors[0].address);
  for (size_t i = 0; i < std::min(max_count, titles.size()); i++)
  {
    Memory::Write_U64(titles[i], request.io_vectors[0].address + static_cast<u32>(i) * sizeof(u64));
    INFO_LOG(IOS_ES, "     title %016" PRIx64, titles[i]);
  }
  return GetDefaultReply(IPC_SUCCESS);
}

IPCCommandResult ES::GetTitleCount(const IOCtlVRequest& request)
{
  const std::vector<u64> titles = GetInstalledTitles();
  INFO_LOG(IOS_ES, "GetTitleCount: %zu titles", titles.size());
  return GetTitleCount(titles, request);
}

IPCCommandResult ES::GetTitles(const IOCtlVRequest& request)
{
  return GetTitles(GetInstalledTitles(), request);
}

IPCCommandResult ES::GetViewCount(const IOCtlVRequest& request)
{
  if (!request.HasNumberOfValidVectors(1, 1))
    return GetDefaultReply(ES_PARAMETER_SIZE_OR_ALIGNMENT);

  u64 TitleID = Memory::Read_U64(request.in_vectors[0].address);

  const DiscIO::CNANDContentLoader& Loader = AccessContentDevice(TitleID);

  size_t view_count = 0;
  if (Loader.IsValid() && Loader.GetTicket().IsValid())
  {
    view_count = Loader.GetTicket().GetNumberOfTickets();
  }

  INFO_LOG(IOS_ES, "IOCTL_ES_GETVIEWCNT for titleID: %08x/%08x (View Count = %zu)",
           static_cast<u32>(TitleID >> 32), static_cast<u32>(TitleID), view_count);

  Memory::Write_U32(static_cast<u32>(view_count), request.io_vectors[0].address);
  return GetDefaultReply(IPC_SUCCESS);
}

IPCCommandResult ES::GetViews(const IOCtlVRequest& request)
{
  if (!request.HasNumberOfValidVectors(2, 1))
    return GetDefaultReply(ES_PARAMETER_SIZE_OR_ALIGNMENT);

  u64 TitleID = Memory::Read_U64(request.in_vectors[0].address);
  u32 maxViews = Memory::Read_U32(request.in_vectors[1].address);

  const DiscIO::CNANDContentLoader& Loader = AccessContentDevice(TitleID);

  if (Loader.IsValid() && Loader.GetTicket().IsValid())
  {
    u32 number_of_views = std::min(maxViews, Loader.GetTicket().GetNumberOfTickets());
    for (u32 view = 0; view < number_of_views; ++view)
    {
      const std::vector<u8> ticket_view = Loader.GetTicket().GetRawTicketView(view);
      Memory::CopyToEmu(request.io_vectors[0].address + view * sizeof(IOS::ES::TicketView),
                        ticket_view.data(), ticket_view.size());
    }
  }

  INFO_LOG(IOS_ES, "IOCTL_ES_GETVIEWS for titleID: %08x/%08x (MaxViews = %i)", (u32)(TitleID >> 32),
           (u32)TitleID, maxViews);

  return GetDefaultReply(IPC_SUCCESS);
}

IPCCommandResult ES::GetTMDViewSize(const IOCtlVRequest& request)
{
  if (!request.HasNumberOfValidVectors(1, 1))
    return GetDefaultReply(ES_PARAMETER_SIZE_OR_ALIGNMENT);

  u64 TitleID = Memory::Read_U64(request.in_vectors[0].address);

  const DiscIO::CNANDContentLoader& Loader = AccessContentDevice(TitleID);

  if (!Loader.IsValid())
    return GetDefaultReply(FS_ENOENT);

  const u32 view_size = static_cast<u32>(Loader.GetTMD().GetRawView().size());
  Memory::Write_U32(view_size, request.io_vectors[0].address);

  INFO_LOG(IOS_ES, "IOCTL_ES_GETTMDVIEWCNT: title: %08x/%08x (view size %i)", (u32)(TitleID >> 32),
           (u32)TitleID, view_size);
  return GetDefaultReply(IPC_SUCCESS);
}

IPCCommandResult ES::GetTMDViews(const IOCtlVRequest& request)
{
  if (!request.HasNumberOfValidVectors(2, 1))
    return GetDefaultReply(ES_PARAMETER_SIZE_OR_ALIGNMENT);

  u64 TitleID = Memory::Read_U64(request.in_vectors[0].address);
  u32 MaxCount = Memory::Read_U32(request.in_vectors[1].address);

  const DiscIO::CNANDContentLoader& Loader = AccessContentDevice(TitleID);

  INFO_LOG(IOS_ES, "IOCTL_ES_GETTMDVIEWCNT: title: %08x/%08x   buffer size: %i",
           (u32)(TitleID >> 32), (u32)TitleID, MaxCount);

  if (!Loader.IsValid())
    return GetDefaultReply(FS_ENOENT);

  const std::vector<u8> raw_view = Loader.GetTMD().GetRawView();
  if (raw_view.size() != request.io_vectors[0].size)
    return GetDefaultReply(ES_PARAMETER_SIZE_OR_ALIGNMENT);

  Memory::CopyToEmu(request.io_vectors[0].address, raw_view.data(), raw_view.size());

  INFO_LOG(IOS_ES, "IOCTL_ES_GETTMDVIEWS: title: %08x/%08x (buffer size: %i)", (u32)(TitleID >> 32),
           (u32)TitleID, MaxCount);
  return GetDefaultReply(IPC_SUCCESS);
}

IPCCommandResult ES::DIGetTMDViewSize(const IOCtlVRequest& request)
{
  if (!request.HasNumberOfValidVectors(1, 1))
    return GetDefaultReply(ES_PARAMETER_SIZE_OR_ALIGNMENT);

  // Sanity check the TMD size.
  if (request.in_vectors[0].size >= 4 * 1024 * 1024)
    return GetDefaultReply(ES_PARAMETER_SIZE_OR_ALIGNMENT);

  if (request.io_vectors[0].size != sizeof(u32))
    return GetDefaultReply(ES_PARAMETER_SIZE_OR_ALIGNMENT);

  const bool has_tmd = request.in_vectors[0].size != 0;
  size_t tmd_view_size = 0;

  if (has_tmd)
  {
    std::vector<u8> tmd_bytes(request.in_vectors[0].size);
    Memory::CopyFromEmu(tmd_bytes.data(), request.in_vectors[0].address, tmd_bytes.size());
    const IOS::ES::TMDReader tmd{std::move(tmd_bytes)};

    // Yes, this returns -1017, not ES_INVALID_TMD.
    // IOS simply checks whether the TMD has all required content entries.
    if (!tmd.IsValid())
      return GetDefaultReply(ES_PARAMETER_SIZE_OR_ALIGNMENT);

    tmd_view_size = tmd.GetRawView().size();
  }
  else
  {
    // If no TMD was passed in and no title is active, IOS returns -1017.
    if (!s_title_context.active)
      return GetDefaultReply(ES_PARAMETER_SIZE_OR_ALIGNMENT);

    tmd_view_size = s_title_context.tmd.GetRawView().size();
  }

  Memory::Write_U32(static_cast<u32>(tmd_view_size), request.io_vectors[0].address);
  return GetDefaultReply(IPC_SUCCESS);
}

IPCCommandResult ES::DIGetTMDView(const IOCtlVRequest& request)
{
  if (!request.HasNumberOfValidVectors(2, 1))
    return GetDefaultReply(ES_PARAMETER_SIZE_OR_ALIGNMENT);

  // Sanity check the TMD size.
  if (request.in_vectors[0].size >= 4 * 1024 * 1024)
    return GetDefaultReply(ES_PARAMETER_SIZE_OR_ALIGNMENT);

  // Check whether the TMD view size is consistent.
  if (request.in_vectors[1].size != sizeof(u32) ||
      Memory::Read_U32(request.in_vectors[1].address) != request.io_vectors[0].size)
  {
    return GetDefaultReply(ES_PARAMETER_SIZE_OR_ALIGNMENT);
  }

  const bool has_tmd = request.in_vectors[0].size != 0;
  std::vector<u8> tmd_view;

  if (has_tmd)
  {
    std::vector<u8> tmd_bytes(request.in_vectors[0].size);
    Memory::CopyFromEmu(tmd_bytes.data(), request.in_vectors[0].address, tmd_bytes.size());
    const IOS::ES::TMDReader tmd{std::move(tmd_bytes)};

    if (!tmd.IsValid())
      return GetDefaultReply(ES_PARAMETER_SIZE_OR_ALIGNMENT);

    tmd_view = tmd.GetRawView();
  }
  else
  {
    // If no TMD was passed in and no title is active, IOS returns -1017.
    if (!s_title_context.active)
      return GetDefaultReply(ES_PARAMETER_SIZE_OR_ALIGNMENT);

    tmd_view = s_title_context.tmd.GetRawView();
  }

  if (tmd_view.size() != request.io_vectors[0].size)
    return GetDefaultReply(ES_PARAMETER_SIZE_OR_ALIGNMENT);

  Memory::CopyToEmu(request.io_vectors[0].address, tmd_view.data(), tmd_view.size());
  return GetDefaultReply(IPC_SUCCESS);
}

IPCCommandResult ES::GetConsumption(const IOCtlVRequest& request)
{
  if (!request.HasNumberOfValidVectors(1, 2))
    return GetDefaultReply(ES_PARAMETER_SIZE_OR_ALIGNMENT);

  // This is at least what crediar's ES module does
  Memory::Write_U32(0, request.io_vectors[1].address);
  INFO_LOG(IOS_ES, "IOCTL_ES_GETCONSUMPTION");
  return GetDefaultReply(IPC_SUCCESS);
}

static bool CanDeleteTitle(u64 title_id)
{
  // IOS only allows deleting non-system titles (or a system title higher than 00000001-00000101).
  return static_cast<u32>(title_id >> 32) != 0x00000001 || static_cast<u32>(title_id) > 0x101;
}

IPCCommandResult ES::DeleteTitle(const IOCtlVRequest& request)
{
  if (!request.HasNumberOfValidVectors(1, 0) || request.in_vectors[0].size != 8)
    return GetDefaultReply(ES_PARAMETER_SIZE_OR_ALIGNMENT);

  const u64 title_id = Memory::Read_U64(request.in_vectors[0].address);

  if (!CanDeleteTitle(title_id))
    return GetDefaultReply(ES_PARAMETER_SIZE_OR_ALIGNMENT);

  const std::string title_dir =
      StringFromFormat("%s/title/%08x/%08x/", RootUserPath(Common::FROM_SESSION_ROOT).c_str(),
                       static_cast<u32>(title_id >> 32), static_cast<u32>(title_id));
  if (!File::IsDirectory(title_dir) ||
      !DiscIO::CNANDContentManager::Access().RemoveTitle(title_id, Common::FROM_SESSION_ROOT))
  {
    return GetDefaultReply(FS_ENOENT);
  }

  if (!File::DeleteDirRecursively(title_dir))
  {
    ERROR_LOG(IOS_ES, "DeleteTitle: Failed to delete title directory: %s", title_dir.c_str());
    return GetDefaultReply(FS_EACCESS);
  }

  return GetDefaultReply(IPC_SUCCESS);
}

IPCCommandResult ES::DeleteTicket(const IOCtlVRequest& request)
{
  if (!request.HasNumberOfValidVectors(1, 0))
    return GetDefaultReply(ES_PARAMETER_SIZE_OR_ALIGNMENT);

  u64 TitleID = Memory::Read_U64(request.in_vectors[0].address);
  INFO_LOG(IOS_ES, "IOCTL_ES_DELETETICKET: title: %08x/%08x", (u32)(TitleID >> 32), (u32)TitleID);

  // Presumably return -1017 when delete fails
  if (!File::Delete(Common::GetTicketFileName(TitleID, Common::FROM_SESSION_ROOT)))
    return GetDefaultReply(ES_PARAMETER_SIZE_OR_ALIGNMENT);

  return GetDefaultReply(IPC_SUCCESS);
}

IPCCommandResult ES::DeleteTitleContent(const IOCtlVRequest& request)
{
  if (!request.HasNumberOfValidVectors(1, 0))
    return GetDefaultReply(ES_PARAMETER_SIZE_OR_ALIGNMENT);

  u64 TitleID = Memory::Read_U64(request.in_vectors[0].address);
  INFO_LOG(IOS_ES, "IOCTL_ES_DELETETITLECONTENT: title: %08x/%08x", (u32)(TitleID >> 32),
           (u32)TitleID);

  // Presumably return -1017 when title not installed TODO verify
  if (!DiscIO::CNANDContentManager::Access().RemoveTitle(TitleID, Common::FROM_SESSION_ROOT))
    return GetDefaultReply(ES_PARAMETER_SIZE_OR_ALIGNMENT);

  return GetDefaultReply(IPC_SUCCESS);
}

IPCCommandResult ES::GetStoredTMDSize(const IOCtlVRequest& request)
{
  if (!request.HasNumberOfValidVectors(1, 1))
    return GetDefaultReply(ES_PARAMETER_SIZE_OR_ALIGNMENT);

  u64 TitleID = Memory::Read_U64(request.in_vectors[0].address);
  const DiscIO::CNANDContentLoader& Loader = AccessContentDevice(TitleID);

  if (!Loader.IsValid() || !Loader.GetTMD().IsValid())
    return GetDefaultReply(FS_ENOENT);

  const u32 tmd_size = static_cast<u32>(Loader.GetTMD().GetRawTMD().size());
  Memory::Write_U32(tmd_size, request.io_vectors[0].address);

  INFO_LOG(IOS_ES, "IOCTL_ES_GETSTOREDTMDSIZE: title: %08x/%08x (view size %i)",
           (u32)(TitleID >> 32), (u32)TitleID, tmd_size);

  return GetDefaultReply(IPC_SUCCESS);
}

IPCCommandResult ES::GetStoredTMD(const IOCtlVRequest& request)
{
  if (!request.HasNumberOfValidVectors(2, 1))
    return GetDefaultReply(ES_PARAMETER_SIZE_OR_ALIGNMENT);

  u64 TitleID = Memory::Read_U64(request.in_vectors[0].address);
  // TODO: actually use this param in when writing to the outbuffer :/
  const u32 MaxCount = Memory::Read_U32(request.in_vectors[1].address);
  const DiscIO::CNANDContentLoader& Loader = AccessContentDevice(TitleID);

  if (!Loader.IsValid() || !Loader.GetTMD().IsValid())
    return GetDefaultReply(FS_ENOENT);

  const std::vector<u8> raw_tmd = Loader.GetTMD().GetRawTMD();
  if (raw_tmd.size() != request.io_vectors[0].size)
    return GetDefaultReply(ES_PARAMETER_SIZE_OR_ALIGNMENT);

  Memory::CopyToEmu(request.io_vectors[0].address, raw_tmd.data(), raw_tmd.size());

  INFO_LOG(IOS_ES, "IOCTL_ES_GETSTOREDTMD: title: %08x/%08x (buffer size: %i)",
           (u32)(TitleID >> 32), (u32)TitleID, MaxCount);
  return GetDefaultReply(IPC_SUCCESS);
}

IPCCommandResult ES::Encrypt(const IOCtlVRequest& request)
{
  if (!request.HasNumberOfValidVectors(3, 2))
    return GetDefaultReply(ES_PARAMETER_SIZE_OR_ALIGNMENT);

  u32 keyIndex = Memory::Read_U32(request.in_vectors[0].address);
  u8* IV = Memory::GetPointer(request.in_vectors[1].address);
  u8* source = Memory::GetPointer(request.in_vectors[2].address);
  u32 size = request.in_vectors[2].size;
  u8* newIV = Memory::GetPointer(request.io_vectors[0].address);
  u8* destination = Memory::GetPointer(request.io_vectors[1].address);

  mbedtls_aes_context AES_ctx;
  mbedtls_aes_setkey_enc(&AES_ctx, s_key_table[keyIndex], 128);
  memcpy(newIV, IV, 16);
  mbedtls_aes_crypt_cbc(&AES_ctx, MBEDTLS_AES_ENCRYPT, size, newIV, source, destination);

  _dbg_assert_msg_(IOS_ES, keyIndex == 6,
                   "IOCTL_ES_ENCRYPT: Key type is not SD, data will be crap");
  return GetDefaultReply(IPC_SUCCESS);
}

IPCCommandResult ES::Decrypt(const IOCtlVRequest& request)
{
  if (!request.HasNumberOfValidVectors(3, 2))
    return GetDefaultReply(ES_PARAMETER_SIZE_OR_ALIGNMENT);

  u32 keyIndex = Memory::Read_U32(request.in_vectors[0].address);
  u8* IV = Memory::GetPointer(request.in_vectors[1].address);
  u8* source = Memory::GetPointer(request.in_vectors[2].address);
  u32 size = request.in_vectors[2].size;
  u8* newIV = Memory::GetPointer(request.io_vectors[0].address);
  u8* destination = Memory::GetPointer(request.io_vectors[1].address);

  DecryptContent(keyIndex, IV, source, size, newIV, destination);

  _dbg_assert_msg_(IOS_ES, keyIndex == 6,
                   "IOCTL_ES_DECRYPT: Key type is not SD, data will be crap");
  return GetDefaultReply(IPC_SUCCESS);
}

IPCCommandResult ES::Launch(const IOCtlVRequest& request)
{
  if (!request.HasNumberOfValidVectors(2, 0))
    return GetDefaultReply(ES_PARAMETER_SIZE_OR_ALIGNMENT);

  u64 TitleID = Memory::Read_U64(request.in_vectors[0].address);
  u32 view = Memory::Read_U32(request.in_vectors[1].address);
  u64 ticketid = Memory::Read_U64(request.in_vectors[1].address + 4);
  u32 devicetype = Memory::Read_U32(request.in_vectors[1].address + 12);
  u64 titleid = Memory::Read_U64(request.in_vectors[1].address + 16);
  u16 access = Memory::Read_U16(request.in_vectors[1].address + 24);

  INFO_LOG(IOS_ES, "IOCTL_ES_LAUNCH %016" PRIx64 " %08x %016" PRIx64 " %08x %016" PRIx64 " %04x",
           TitleID, view, ticketid, devicetype, titleid, access);

  // IOS replies to the request through the mailbox on failure, and acks if the launch succeeds.
  // Note: Launch will potentially reset the whole IOS state -- including this ES instance.
  if (!LaunchTitle(TitleID))
    return GetDefaultReply(ES_INVALID_TMD);

  // Generate a "reply" to the IPC command.  ES_LAUNCH is unique because it
  // involves restarting IOS; IOS generates two acknowledgements in a row.
  // Note: If the launch succeeded, we should not write anything to the command buffer as
  // IOS does not even reply unless it failed.
  EnqueueCommandAcknowledgement(request.address, 0);
  return GetNoReply();
}

IPCCommandResult ES::LaunchBC(const IOCtlVRequest& request)
{
  if (!request.HasNumberOfValidVectors(0, 0))
    return GetDefaultReply(ES_PARAMETER_SIZE_OR_ALIGNMENT);

  // Here, IOS checks the clock speed and prevents ioctlv 0x25 from being used in GC mode.
  // An alternative way to do this is to check whether the current active IOS is MIOS.
  if (GetVersion() == 0x101)
    return GetDefaultReply(ES_PARAMETER_SIZE_OR_ALIGNMENT);

  if (!LaunchTitle(0x0000000100000100))
    return GetDefaultReply(ES_INVALID_TMD);

  EnqueueCommandAcknowledgement(request.address, 0);
  return GetNoReply();
}

IPCCommandResult ES::ExportTitleInit(const IOCtlVRequest& request)
{
  if (!request.HasNumberOfValidVectors(1, 1) || request.in_vectors[0].size != 8)
    return GetDefaultReply(ES_PARAMETER_SIZE_OR_ALIGNMENT);

  // No concurrent title import/export is allowed.
  if (m_export_title_context.valid)
    return GetDefaultReply(ES_PARAMETER_SIZE_OR_ALIGNMENT);

  const auto& content_loader = AccessContentDevice(Memory::Read_U64(request.in_vectors[0].address));
  if (!content_loader.IsValid())
    return GetDefaultReply(FS_ENOENT);
  if (!content_loader.GetTMD().IsValid())
    return GetDefaultReply(ES_INVALID_TMD);

  m_export_title_context.tmd = content_loader.GetTMD();

  const auto ticket = DiscIO::FindSignedTicket(m_export_title_context.tmd.GetTitleId());
  if (!ticket.IsValid())
    return GetDefaultReply(ES_NO_TICKET_INSTALLED);
  if (ticket.GetTitleId() != m_export_title_context.tmd.GetTitleId())
    return GetDefaultReply(ES_PARAMETER_SIZE_OR_ALIGNMENT);

  m_export_title_context.title_key = ticket.GetTitleKey();

  const auto& raw_tmd = m_export_title_context.tmd.GetRawTMD();
  if (request.io_vectors[0].size != raw_tmd.size())
    return GetDefaultReply(ES_PARAMETER_SIZE_OR_ALIGNMENT);

  Memory::CopyToEmu(request.io_vectors[0].address, raw_tmd.data(), raw_tmd.size());

  m_export_title_context.valid = true;
  return GetDefaultReply(IPC_SUCCESS);
}

IPCCommandResult ES::ExportContentBegin(const IOCtlVRequest& request)
{
  if (!request.HasNumberOfValidVectors(2, 0) || request.in_vectors[0].size != 8 ||
      request.in_vectors[1].size != 4)
    return GetDefaultReply(ES_PARAMETER_SIZE_OR_ALIGNMENT);

  const u64 title_id = Memory::Read_U64(request.in_vectors[0].address);
  const u32 content_id = Memory::Read_U32(request.in_vectors[1].address);

  if (!m_export_title_context.valid || m_export_title_context.tmd.GetTitleId() != title_id)
  {
    ERROR_LOG(IOS_ES, "Tried to use ExportContentBegin with an invalid title export context.");
    return GetDefaultReply(ES_PARAMETER_SIZE_OR_ALIGNMENT);
  }

  const auto& content_loader = AccessContentDevice(title_id);
  if (!content_loader.IsValid())
    return GetDefaultReply(FS_ENOENT);

  const auto* content = content_loader.GetContentByID(content_id);
  if (!content)
    return GetDefaultReply(ES_PARAMETER_SIZE_OR_ALIGNMENT);

  OpenedContent entry;
  entry.m_position = 0;
  entry.m_content = content->m_metadata;
  entry.m_title_id = title_id;
  content->m_Data->Open();

  u32 cid = 0;
  while (m_export_title_context.contents.find(cid) != m_export_title_context.contents.end())
    cid++;

  TitleExportContext::ExportContent content_export;
  content_export.content = std::move(entry);
  content_export.iv[0] = (content->m_metadata.index >> 8) & 0xFF;
  content_export.iv[1] = content->m_metadata.index & 0xFF;

  m_export_title_context.contents.emplace(cid, content_export);
  // IOS returns a content ID which is passed to further content calls.
  return GetDefaultReply(cid);
}

IPCCommandResult ES::ExportContentData(const IOCtlVRequest& request)
{
  if (!request.HasNumberOfValidVectors(1, 1) || request.in_vectors[0].size != 4 ||
      request.io_vectors[0].size == 0)
  {
    return GetDefaultReply(ES_PARAMETER_SIZE_OR_ALIGNMENT);
  }

  const u32 content_id = Memory::Read_U32(request.in_vectors[0].address);
  const u32 bytes_to_read = request.io_vectors[0].size;

  const auto iterator = m_export_title_context.contents.find(content_id);
  if (!m_export_title_context.valid || iterator == m_export_title_context.contents.end() ||
      iterator->second.content.m_position >= iterator->second.content.m_content.size)
  {
    return GetDefaultReply(ES_PARAMETER_SIZE_OR_ALIGNMENT);
  }

  auto& metadata = iterator->second.content;

  const auto& content_loader = AccessContentDevice(metadata.m_title_id);
  const auto* content = content_loader.GetContentByID(metadata.m_content.id);
  content->m_Data->Open();

  const u32 length =
      std::min(static_cast<u32>(metadata.m_content.size - metadata.m_position), bytes_to_read);
  std::vector<u8> buffer(length);

  if (!content->m_Data->GetRange(metadata.m_position, length, buffer.data()))
  {
    ERROR_LOG(IOS_ES, "ExportContentData: ES_READ_LESS_DATA_THAN_EXPECTED");
    return GetDefaultReply(ES_READ_LESS_DATA_THAN_EXPECTED);
  }

  // IOS aligns the buffer to 32 bytes. Since we also need to align it to 16 bytes,
  // let's just follow IOS here.
  buffer.resize(Common::AlignUp(buffer.size(), 32));
  std::vector<u8> output(buffer.size());

  mbedtls_aes_context aes_ctx;
  mbedtls_aes_setkey_enc(&aes_ctx, m_export_title_context.title_key.data(), 128);
  const int ret = mbedtls_aes_crypt_cbc(&aes_ctx, MBEDTLS_AES_ENCRYPT, buffer.size(),
                                        iterator->second.iv.data(), buffer.data(), output.data());
  if (ret != 0)
  {
    // XXX: proper error code when IOSC_Encrypt fails.
    ERROR_LOG(IOS_ES, "ExportContentData: Failed to encrypt content.");
    return GetDefaultReply(ES_PARAMETER_SIZE_OR_ALIGNMENT);
  }

  Memory::CopyToEmu(request.io_vectors[0].address, output.data(), output.size());
  metadata.m_position += length;
  return GetDefaultReply(IPC_SUCCESS);
}

IPCCommandResult ES::ExportContentEnd(const IOCtlVRequest& request)
{
  if (!request.HasNumberOfValidVectors(1, 0) || request.in_vectors[0].size != 4)
    return GetDefaultReply(ES_PARAMETER_SIZE_OR_ALIGNMENT);

  const u32 content_id = Memory::Read_U32(request.in_vectors[0].address);

  const auto iterator = m_export_title_context.contents.find(content_id);
  if (!m_export_title_context.valid || iterator == m_export_title_context.contents.end() ||
      iterator->second.content.m_position != iterator->second.content.m_content.size)
  {
    return GetDefaultReply(ES_PARAMETER_SIZE_OR_ALIGNMENT);
  }

  // XXX: Check the content hash, as IOS does?

  const auto& content_loader = AccessContentDevice(iterator->second.content.m_title_id);
  content_loader.GetContentByID(iterator->second.content.m_content.id)->m_Data->Close();

  m_export_title_context.contents.erase(iterator);
  return GetDefaultReply(IPC_SUCCESS);
}

IPCCommandResult ES::ExportTitleDone(const IOCtlVRequest& request)
{
  if (!m_export_title_context.valid)
    return GetDefaultReply(ES_PARAMETER_SIZE_OR_ALIGNMENT);

  m_export_title_context.valid = false;
  return GetDefaultReply(IPC_SUCCESS);
}

IPCCommandResult ES::CheckKoreaRegion(const IOCtlVRequest& request)
{
  if (!request.HasNumberOfValidVectors(0, 0))
    return GetDefaultReply(ES_PARAMETER_SIZE_OR_ALIGNMENT);

  // note by DacoTaco : name is unknown, I just tried to name it SOMETHING.
  // IOS70 has this to let system menu 4.2 check if the console is region changed. it returns
  // -1017
  // if the IOS didn't find the Korean keys and 0 if it does. 0 leads to a error 003
  INFO_LOG(IOS_ES, "IOCTL_ES_CHECKKOREAREGION: Title checked for Korean keys.");
  return GetDefaultReply(ES_PARAMETER_SIZE_OR_ALIGNMENT);
}

IPCCommandResult ES::GetDeviceCertificate(const IOCtlVRequest& request)
{
  if (!request.HasNumberOfValidVectors(0, 1) || request.io_vectors[0].size != 0x180)
    return GetDefaultReply(ES_PARAMETER_SIZE_OR_ALIGNMENT);

  INFO_LOG(IOS_ES, "IOCTL_ES_GETDEVICECERT");
  u8* destination = Memory::GetPointer(request.io_vectors[0].address);

  const EcWii& ec = EcWii::GetInstance();
  MakeNGCert(destination, ec.GetNGID(), ec.GetNGKeyID(), ec.GetNGPriv(), ec.GetNGSig());
  return GetDefaultReply(IPC_SUCCESS);
}

IPCCommandResult ES::Sign(const IOCtlVRequest& request)
{
  if (!request.HasNumberOfValidVectors(1, 2))
    return GetDefaultReply(ES_PARAMETER_SIZE_OR_ALIGNMENT);

  INFO_LOG(IOS_ES, "IOCTL_ES_SIGN");
  u8* ap_cert_out = Memory::GetPointer(request.io_vectors[1].address);
  u8* data = Memory::GetPointer(request.in_vectors[0].address);
  u32 data_size = request.in_vectors[0].size;
  u8* sig_out = Memory::GetPointer(request.io_vectors[0].address);

  if (!s_title_context.active)
    return GetDefaultReply(ES_PARAMETER_SIZE_OR_ALIGNMENT);

  const EcWii& ec = EcWii::GetInstance();
  MakeAPSigAndCert(sig_out, ap_cert_out, s_title_context.tmd.GetTitleId(), data, data_size,
                   ec.GetNGPriv(), ec.GetNGID());

  return GetDefaultReply(IPC_SUCCESS);
}

IPCCommandResult ES::GetBoot2Version(const IOCtlVRequest& request)
{
  if (!request.HasNumberOfValidVectors(0, 1))
    return GetDefaultReply(ES_PARAMETER_SIZE_OR_ALIGNMENT);

  INFO_LOG(IOS_ES, "IOCTL_ES_GETBOOT2VERSION");

  // as of 26/02/2012, this was latest bootmii version
  Memory::Write_U32(4, request.io_vectors[0].address);
  return GetDefaultReply(IPC_SUCCESS);
}

IPCCommandResult ES::DIGetTicketView(const IOCtlVRequest& request)
{
  if (!request.HasNumberOfValidVectors(1, 1) ||
      request.io_vectors[0].size != sizeof(IOS::ES::TicketView))
  {
    return GetDefaultReply(ES_PARAMETER_SIZE_OR_ALIGNMENT);
  }

  const bool has_ticket_vector = request.in_vectors[0].size == 0x2A4;

  // This ioctlv takes either a signed ticket or no ticket, in which case the ticket size must be 0.
  if (!has_ticket_vector && request.in_vectors[0].size != 0)
    return GetDefaultReply(ES_PARAMETER_SIZE_OR_ALIGNMENT);

  std::vector<u8> view;

  // If no ticket was passed in, IOS returns the ticket view for the current title.
  // Of course, this returns -1017 if no title is active and no ticket is passed.
  if (!has_ticket_vector)
  {
    if (!s_title_context.active)
      return GetDefaultReply(ES_PARAMETER_SIZE_OR_ALIGNMENT);

    view = s_title_context.ticket.GetRawTicketView(0);
  }
  else
  {
    std::vector<u8> ticket_bytes(request.in_vectors[0].size);
    Memory::CopyFromEmu(ticket_bytes.data(), request.in_vectors[0].address, ticket_bytes.size());
    const IOS::ES::TicketReader ticket{std::move(ticket_bytes)};

    view = ticket.GetRawTicketView(0);
  }

  Memory::CopyToEmu(request.io_vectors[0].address, view.data(), view.size());
  return GetDefaultReply(IPC_SUCCESS);
}

IPCCommandResult ES::GetOwnedTitleCount(const IOCtlVRequest& request)
{
  const std::vector<u64> titles = GetTitlesWithTickets();
  INFO_LOG(IOS_ES, "GetOwnedTitleCount: %zu titles", titles.size());
  return GetTitleCount(titles, request);
}

IPCCommandResult ES::GetOwnedTitles(const IOCtlVRequest& request)
{
  return GetTitles(GetTitlesWithTickets(), request);
}

const DiscIO::CNANDContentLoader& ES::AccessContentDevice(u64 title_id)
{
  // for WADs, the passed title id and the stored title id match; along with s_content_file
  // being set to the actual WAD file name. We cannot simply get a NAND Loader for the title id
  // in those cases, since the WAD need not be installed in the NAND, but it could be opened
  // directly from a WAD file anywhere on disk.
  if (s_title_context.active && s_title_context.tmd.GetTitleId() == title_id &&
      !s_content_file.empty())
  {
    return DiscIO::CNANDContentManager::Access().GetNANDLoader(s_content_file);
  }

  return DiscIO::CNANDContentManager::Access().GetNANDLoader(title_id, Common::FROM_SESSION_ROOT);
}

// This is technically an ioctlv in IOS's ES, but it is an internal API which cannot be
// used from the PowerPC (for unpatched IOSes anyway).
s32 ES::DIVerify(const IOS::ES::TMDReader& tmd, const IOS::ES::TicketReader& ticket)
{
  s_title_context.Clear();
  INFO_LOG(IOS_ES, "ES_DIVerify: Title context changed: (none)");

  if (!tmd.IsValid() || !ticket.IsValid())
    return ES_PARAMETER_SIZE_OR_ALIGNMENT;

  if (tmd.GetTitleId() != ticket.GetTitleId())
    return ES_PARAMETER_SIZE_OR_ALIGNMENT;

  std::string tmd_path = Common::GetTMDFileName(tmd.GetTitleId(), Common::FROM_SESSION_ROOT);

  File::CreateFullPath(tmd_path);
  File::CreateFullPath(Common::GetTitleDataPath(tmd.GetTitleId(), Common::FROM_SESSION_ROOT));

  if (!File::Exists(tmd_path))
  {
    File::IOFile tmd_file(tmd_path, "wb");
    const std::vector<u8>& tmd_bytes = tmd.GetRawTMD();
    if (!tmd_file.WriteBytes(tmd_bytes.data(), tmd_bytes.size()))
      ERROR_LOG(IOS_ES, "DIVerify failed to write disc TMD to NAND.");
  }
  DiscIO::cUIDsys uid_sys{Common::FromWhichRoot::FROM_SESSION_ROOT};
  uid_sys.AddTitle(tmd.GetTitleId());
  // DI_VERIFY writes to title.tmd, which is read and cached inside the NAND Content Manager.
  // clear the cache to avoid content access mismatches.
  DiscIO::CNANDContentManager::Access().ClearCache();

  s_title_context.Update(tmd, ticket);
  INFO_LOG(IOS_ES, "ES_DIVerify: Title context changed: %016" PRIx64, tmd.GetTitleId());
  return IPC_SUCCESS;
}
}  // namespace Device
}  // namespace HLE
}  // namespace IOS
