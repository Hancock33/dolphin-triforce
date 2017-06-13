// Copyright 2008 Dolphin Emulator Project
// Licensed under GPLv2+
// Refer to the license.txt file included.

#pragma once

#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "Common/CommonTypes.h"
#include "DiscIO/Volume.h"

namespace DiscIO
{
// TODO: eww
class FileInfoGCWii;

// file info of an FST entry
class FileInfo
{
public:
  virtual ~FileInfo();

  // Not guaranteed to return a meaningful value for directories
  virtual u64 GetOffset() const = 0;
  // Not guaranteed to return a meaningful value for directories
  virtual u64 GetSize() const = 0;
  virtual bool IsDirectory() const = 0;
  virtual const std::string& GetName() const = 0;
};

class FileSystem
{
public:
  FileSystem(const Volume* _rVolume, const Partition& partition);

  virtual ~FileSystem();
  virtual bool IsValid() const = 0;
  // TODO: Should only return FileInfo, not FileInfoGCWii
  virtual const std::vector<FileInfoGCWii>& GetFileList() = 0;
  virtual u64 GetFileSize(const std::string& _rFullPath) = 0;
  virtual u64 ReadFile(const std::string& _rFullPath, u8* _pBuffer, u64 _MaxBufferSize,
                       u64 _OffsetInFile = 0) = 0;
  virtual bool ExportFile(const std::string& _rFullPath, const std::string& _rExportFilename) = 0;
  virtual bool ExportApploader(const std::string& _rExportFolder) const = 0;
  virtual bool ExportDOL(const std::string& _rExportFolder) const = 0;
  virtual std::string GetPath(u64 _Address) = 0;
  virtual std::string GetPathFromFSTOffset(size_t file_info_offset) = 0;
  virtual std::optional<u64> GetBootDOLOffset() const = 0;
  virtual std::optional<u32> GetBootDOLSize(u64 dol_offset) const = 0;

  virtual const Partition GetPartition() const { return m_partition; }
protected:
  const Volume* const m_rVolume;
  const Partition m_partition;
};

std::unique_ptr<FileSystem> CreateFileSystem(const Volume* volume, const Partition& partition);

}  // namespace
