/*
 *  Copyright (C) 2026 Team Jumpgate
 *  This file is part of Kodi - https://kodi.tv
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 *  See LICENSES/README.md for more information.
 */

#include "JumpgateProfileStorage.h"

#include "filesystem/File.h"
#include "filesystem/SpecialProtocol.h"

#include <atomic>
#include <cerrno>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

namespace KODI::JUMPGATE
{
namespace
{
constexpr size_t MAX_PROFILE_METADATA_BYTES = 4 * 1024 * 1024;
std::atomic<uint64_t> s_tempSequence{0};

bool WriteAll(int descriptor, const char* data, size_t size)
{
  size_t offset = 0;
  while (offset < size)
  {
    const ssize_t written = write(descriptor, data + offset, size - offset);
    if (written > 0)
    {
      offset += static_cast<size_t>(written);
      continue;
    }
    if (written < 0 && errno == EINTR)
      continue;
    return false;
  }
  return true;
}

void RemoveTemp(const std::string& path)
{
  if (!path.empty())
    unlink(path.c_str());
}

std::string ParentDirectory(const std::string& path)
{
  const size_t separator = path.find_last_of('/');
  if (separator == std::string::npos)
    return ".";
  if (separator == 0)
    return "/";
  return path.substr(0, separator);
}

bool SyncDirectory(const std::string& path)
{
  int descriptor;
  do
  {
    descriptor = open(ParentDirectory(path).c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC);
  } while (descriptor < 0 && errno == EINTR);
  if (descriptor < 0)
    return false;

  int result;
  do
  {
    result = fsync(descriptor);
  } while (result != 0 && errno == EINTR);
  const bool closed = close(descriptor) == 0;
  return result == 0 && closed;
}

} // namespace

CJumpgateProfileStorage::CJumpgateProfileStorage(std::string specialPath)
  : m_specialPath(std::move(specialPath))
{
}

bool CJumpgateProfileStorage::Read(std::string& contents, bool& exists, std::string& error)
{
  contents.clear();
  error.clear();
  const std::string path = CSpecialProtocol::TranslatePath(m_specialPath);
  exists = XFILE::CFile::Exists(path);
  if (!exists)
    return true;

  XFILE::CFile file;
  std::vector<uint8_t> bytes;
  const ssize_t size = file.LoadFile(path, bytes);
  if (size < 0 || static_cast<size_t>(size) > MAX_PROFILE_METADATA_BYTES)
  {
    error = "Jumpgate profile metadata could not be read safely";
    return false;
  }
  contents.assign(bytes.begin(), bytes.end());
  return true;
}

bool CJumpgateProfileStorage::WriteAtomic(const std::string& contents, std::string& error)
{
  error.clear();
  if (contents.empty() || contents.size() > MAX_PROFILE_METADATA_BYTES)
  {
    error = "Jumpgate profile metadata has an invalid size";
    return false;
  }

  const std::string path = CSpecialProtocol::TranslatePath(m_specialPath);
  const std::string tempPath =
      path + ".tmp." + std::to_string(getpid()) + "." +
      std::to_string(s_tempSequence.fetch_add(1, std::memory_order_relaxed));

  const int descriptor = open(tempPath.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0600);
  if (descriptor < 0)
  {
    error = "Jumpgate profile metadata temporary file could not be created";
    return false;
  }

  bool success = WriteAll(descriptor, contents.data(), contents.size());
  if (success)
    success = fsync(descriptor) == 0;
  if (close(descriptor) != 0)
    success = false;

  if (!success)
  {
    RemoveTemp(tempPath);
    error = "Jumpgate profile metadata temporary write failed";
    return false;
  }

  // Both paths are in the profile directory, so rename is an atomic replacement on Android.
  if (rename(tempPath.c_str(), path.c_str()) != 0)
  {
    RemoveTemp(tempPath);
    error = "Jumpgate profile metadata atomic replacement failed";
    return false;
  }
  if (!SyncDirectory(path))
  {
    // The file itself is fsynced and the atomic rename is already visible. A
    // hard failure here would make the caller delete the newly written
    // credential while this metadata references it. Preserve consistency and
    // report the reduced crash-durability guarantee as a warning.
    error = "Jumpgate profile metadata committed; directory sync was not confirmed";
  }
  return true;
}

} // namespace KODI::JUMPGATE
