/*
 *  Copyright (C) 2026 Team Jumpgate
 *  This file is part of Kodi - https://kodi.tv
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 *  See LICENSES/README.md for more information.
 */

#include "AndroidJumpgateSubtitleFileStore.h"

#include "utils/log.h"

#include <algorithm>
#include <array>
#include <condition_variable>
#include <deque>
#include <iomanip>
#include <limits>
#include <mutex>
#include <random>
#include <regex>
#include <sstream>
#include <type_traits>
#include <utility>
#include <vector>

#if defined(_WIN32)
#include <windows.h>
#else
#include <cerrno>

#include <dirent.h>
#include <fcntl.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <unistd.h>
#endif

using namespace std::chrono_literals;

namespace KODI::JUMPGATE
{
namespace
{
constexpr AndroidJumpgateCleanupBudget OWNED_DIRECTORY_CLEANUP_BUDGET{
    16, 24ULL * 1024ULL * 1024ULL, std::chrono::milliseconds{100}};
constexpr std::size_t MAX_DIRECTORY_CREATE_ATTEMPTS = 8;
constexpr std::size_t FILE_WRITE_CHUNK_BYTES = 64 * 1024;
constexpr std::size_t MAX_DISCARDED_COMPLETIONS = 4;
constexpr std::size_t MAX_CLEANUP_REQUESTS = 128;
constexpr std::size_t MAX_CLEANUP_ATTEMPTS = 4;
constexpr std::chrono::milliseconds CLEANUP_RETRY_WINDOW{250};
constexpr std::chrono::milliseconds CLEANUP_INITIAL_BACKOFF{5};
// Attempts and retry scheduling are strictly bounded. As documented on the store, an individual
// local-filesystem syscall cannot be preempted by this worker.

#if !defined(_WIN32)
class CScopedFd final
{
public:
  explicit CScopedFd(int fd = -1) : m_fd(fd) {}
  ~CScopedFd()
  {
    if (m_fd >= 0)
      close(m_fd);
  }
  CScopedFd(const CScopedFd&) = delete;
  CScopedFd& operator=(const CScopedFd&) = delete;
  CScopedFd(CScopedFd&& other) noexcept : m_fd(std::exchange(other.m_fd, -1)) {}
  CScopedFd& operator=(CScopedFd&& other) noexcept
  {
    if (this != &other)
    {
      if (m_fd >= 0)
        close(m_fd);
      m_fd = std::exchange(other.m_fd, -1);
    }
    return *this;
  }
  int Get() const { return m_fd; }
  int Release() { return std::exchange(m_fd, -1); }
  explicit operator bool() const { return m_fd >= 0; }

private:
  int m_fd{-1};
};

CScopedFd OpenPathDirectoryAtNoFollow(int parentFd, const std::string& name)
{
  const int fd = openat(parentFd, name.c_str(), O_PATH | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
  if (fd < 0)
    return CScopedFd{};
  struct stat status{};
  if (fstat(fd, &status) != 0 || !S_ISDIR(status.st_mode))
  {
    close(fd);
    return CScopedFd{};
  }
  return CScopedFd{fd};
}

CScopedFd OpenAncestorPathNoFollow(const std::filesystem::path& path)
{
  if (!path.is_absolute())
    return CScopedFd{};
  CScopedFd current{open("/", O_PATH | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW)};
  if (!current)
    return CScopedFd{};
  for (const std::filesystem::path& component : path.lexically_normal().relative_path())
  {
    const std::string name = component.string();
    if (name.empty() || name == ".")
      continue;
    if (name == "..")
      return CScopedFd{};
    CScopedFd next = OpenPathDirectoryAtNoFollow(current.Get(), name);
    if (!next)
      return CScopedFd{};
    current = std::move(next);
  }
  return current;
}

CScopedFd OpenAccessibleDirectoryAtNoFollow(int parentFd,
                                            const std::string& name,
                                            bool requireNoChildren)
{
  CScopedFd directory{
      openat(parentFd, name.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW)};
  if (!directory)
    return directory;
  struct stat status{};
  if (fstat(directory.Get(), &status) != 0 || !S_ISDIR(status.st_mode) ||
      (requireNoChildren && status.st_nlink != 2))
    return CScopedFd{};
  return directory;
}

CScopedFd OpenAccessibleLocalDirectoryNoFollow(const std::filesystem::path& path)
{
  const std::filesystem::path normalized = path.lexically_normal();
  if (!normalized.is_absolute() || normalized == normalized.root_path())
    return CScopedFd{};
  const std::string name = normalized.filename().string();
  const CScopedFd parent = OpenAncestorPathNoFollow(normalized.parent_path());
  if (!parent || name.empty() || name == "." || name == "..")
    return CScopedFd{};
  return OpenAccessibleDirectoryAtNoFollow(parent.Get(), name, false);
}

template<typename Value>
std::optional<std::uint64_t> CheckedIdentityValue(Value value)
{
  static_assert(std::is_integral_v<Value>);
  if constexpr (std::is_signed_v<Value>)
  {
    if (value < 0)
      return std::nullopt;
  }
  using UnsignedValue = std::make_unsigned_t<Value>;
  const UnsignedValue unsignedValue = static_cast<UnsignedValue>(value);
  if constexpr (sizeof(UnsignedValue) > sizeof(std::uint64_t))
  {
    if (unsignedValue > static_cast<UnsignedValue>(std::numeric_limits<std::uint64_t>::max()))
      return std::nullopt;
  }
  const std::uint64_t stored = static_cast<std::uint64_t>(unsignedValue);
  if (static_cast<UnsignedValue>(stored) != unsignedValue)
    return std::nullopt;
  return stored;
}

struct PosixObjectIdentity
{
  std::uint64_t device{0};
  std::uint64_t inode{0};

  bool operator==(const PosixObjectIdentity&) const = default;
};

std::optional<PosixObjectIdentity> CaptureIdentity(const struct stat& status)
{
  const auto device = CheckedIdentityValue(status.st_dev);
  const auto inode = CheckedIdentityValue(status.st_ino);
  if (!device || !inode)
    return std::nullopt;
  return PosixObjectIdentity{*device, *inode};
}

bool IsSameDirectoryEntry(int parentFd, const std::string& name, int directoryFd)
{
  struct stat opened{};
  struct stat current{};
  if (fstat(directoryFd, &opened) != 0 ||
      fstatat(parentFd, name.c_str(), &current, AT_SYMLINK_NOFOLLOW) != 0 ||
      !S_ISDIR(opened.st_mode) || !S_ISDIR(current.st_mode) || opened.st_nlink != 2 ||
      current.st_nlink != 2)
  {
    return false;
  }
  const auto openedIdentity = CaptureIdentity(opened);
  const auto currentIdentity = CaptureIdentity(current);
  return openedIdentity && currentIdentity && *openedIdentity == *currentIdentity;
}

bool IsSameDirectoryIdentity(int parentFd, const std::string& name, int directoryFd)
{
  struct stat opened{};
  struct stat current{};
  if (fstat(directoryFd, &opened) != 0 || !S_ISDIR(opened.st_mode) ||
      fstatat(parentFd, name.c_str(), &current, AT_SYMLINK_NOFOLLOW) != 0 ||
      !S_ISDIR(current.st_mode))
  {
    return false;
  }
  const auto openedIdentity = CaptureIdentity(opened);
  const auto currentIdentity = CaptureIdentity(current);
  return openedIdentity && currentIdentity && *openedIdentity == *currentIdentity;
}

CScopedFd WriteFileAt(int directoryFd,
                      const std::string& name,
                      const std::vector<std::uint8_t>& bytes,
                      const CJumpgateSubtitleCancellationToken& cancellation)
{
  if (bytes.empty() || cancellation.IsCancelled())
    return CScopedFd{};
  CScopedFd file{openat(directoryFd, name.c_str(),
                        O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC | O_NOFOLLOW, 0600)};
  if (!file)
    return CScopedFd{};
  struct stat status{};
  if (fstat(file.Get(), &status) != 0 || !S_ISREG(status.st_mode) || status.st_nlink != 1)
    return CScopedFd{};
  for (std::size_t offset = 0; offset < bytes.size();)
  {
    if (cancellation.IsCancelled())
      return CScopedFd{};
    const std::size_t length = std::min(FILE_WRITE_CHUNK_BYTES, bytes.size() - offset);
    const ssize_t written = write(file.Get(), bytes.data() + offset, length);
    if (written <= 0)
      return CScopedFd{};
    offset += static_cast<std::size_t>(written);
  }
  if (fsync(file.Get()) != 0 || cancellation.IsCancelled())
    return CScopedFd{};
  return file;
}

bool PublishFileAtNoReplace(int directoryFd,
                            const std::string& temporaryName,
                            const std::string& finalName,
                            int temporaryFd)
{
  struct stat opened{};
  struct stat temporary{};
  if (fstat(temporaryFd, &opened) != 0 || !S_ISREG(opened.st_mode) || opened.st_nlink != 1 ||
      fstatat(directoryFd, temporaryName.c_str(), &temporary, AT_SYMLINK_NOFOLLOW) != 0 ||
      !S_ISREG(temporary.st_mode) || temporary.st_nlink != 1)
  {
    return false;
  }
  const auto openedIdentity = CaptureIdentity(opened);
  const auto temporaryIdentity = CaptureIdentity(temporary);
  if (!openedIdentity || !temporaryIdentity || *openedIdentity != *temporaryIdentity)
    return false;
  bool renamed = false;
#if defined(SYS_renameat2)
  if (syscall(SYS_renameat2, directoryFd, temporaryName.c_str(), directoryFd, finalName.c_str(),
              1 /* RENAME_NOREPLACE */) == 0)
  {
    renamed = true;
  }
  else if (errno != ENOSYS && errno != EINVAL)
  {
    return false;
  }
#endif
  if (!renamed &&
      linkat(directoryFd, temporaryName.c_str(), directoryFd, finalName.c_str(), 0) != 0)
  {
    return false;
  }
  if (!renamed && unlinkat(directoryFd, temporaryName.c_str(), 0) != 0)
  {
    unlinkat(directoryFd, finalName.c_str(), 0);
    return false;
  }
  struct stat published{};
  if (fstatat(directoryFd, finalName.c_str(), &published, AT_SYMLINK_NOFOLLOW) != 0 ||
      !S_ISREG(published.st_mode) || published.st_nlink != 1)
  {
    unlinkat(directoryFd, finalName.c_str(), 0);
    return false;
  }
  const auto publishedIdentity = CaptureIdentity(published);
  if (!publishedIdentity || *publishedIdentity != *openedIdentity)
  {
    unlinkat(directoryFd, finalName.c_str(), 0);
    return false;
  }
  return true;
}

struct PosixPinnedFile
{
  std::string name;
  int fd{-1};
  PosixObjectIdentity identity;
};

CScopedFd OpenRegularFileAtNoFollow(int directoryFd,
                                    const std::string& name,
                                    PosixObjectIdentity& identity)
{
  CScopedFd file{openat(directoryFd, name.c_str(), O_RDONLY | O_CLOEXEC | O_NOFOLLOW)};
  struct stat opened{};
  struct stat current{};
  if (!file || fstat(file.Get(), &opened) != 0 || !S_ISREG(opened.st_mode) ||
      opened.st_nlink != 1 ||
      fstatat(directoryFd, name.c_str(), &current, AT_SYMLINK_NOFOLLOW) != 0 ||
      !S_ISREG(current.st_mode) || current.st_nlink != 1)
  {
    return CScopedFd{};
  }
  const auto openedIdentity = CaptureIdentity(opened);
  const auto currentIdentity = CaptureIdentity(current);
  if (!openedIdentity || !currentIdentity || *openedIdentity != *currentIdentity)
    return CScopedFd{};
  identity = *openedIdentity;
  return file;
}

class CPosixSubtitleArtifactAnchor final : public IAndroidJumpgateSubtitleArtifactAnchor
{
public:
  CPosixSubtitleArtifactAnchor(int directoryFd,
                               std::vector<PosixPinnedFile> files,
                               bool directoryRelativeInjection)
    : m_directoryFd(directoryFd),
      m_files(std::move(files)),
      m_directoryRelativeInjection(directoryRelativeInjection)
  {
    struct stat status{};
    const auto identity = m_directoryFd >= 0 && fstat(m_directoryFd, &status) == 0 &&
                                  S_ISDIR(status.st_mode) && status.st_nlink == 2
                              ? CaptureIdentity(status)
                              : std::nullopt;
    if (identity && flock(m_directoryFd, LOCK_SH | LOCK_NB) == 0)
      m_directoryIdentity = *identity;
  }

  ~CPosixSubtitleArtifactAnchor() override
  {
    for (PosixPinnedFile& file : m_files)
    {
      if (file.fd >= 0)
        close(file.fd);
    }
    if (m_directoryFd >= 0)
      close(m_directoryFd);
  }

  bool Validate() const override
  {
    struct stat directory{};
    if (m_directoryFd < 0 || fstat(m_directoryFd, &directory) != 0 || !S_ISDIR(directory.st_mode) ||
        directory.st_nlink != 2 || (directory.st_mode & 0222) != 0)
    {
      return false;
    }
    const auto directoryIdentity = CaptureIdentity(directory);
    if (!m_directoryIdentity || !directoryIdentity || *directoryIdentity != *m_directoryIdentity)
      return false;
    for (const PosixPinnedFile& expected : m_files)
    {
      struct stat opened{};
      struct stat current{};
      if (expected.fd < 0 || fstat(expected.fd, &opened) != 0 || !S_ISREG(opened.st_mode) ||
          opened.st_nlink != 1 || (opened.st_mode & 0222) != 0 ||
          fstatat(m_directoryFd, expected.name.c_str(), &current, AT_SYMLINK_NOFOLLOW) != 0 ||
          !S_ISREG(current.st_mode) || current.st_nlink != 1)
      {
        return false;
      }
      const auto openedIdentity = CaptureIdentity(opened);
      const auto currentIdentity = CaptureIdentity(current);
      if (!openedIdentity || !currentIdentity || *openedIdentity != expected.identity ||
          *currentIdentity != expected.identity)
        return false;
    }
    return !m_files.empty();
  }

  std::string InjectionPath(const std::string& fileName) const override
  {
    const auto file = std::find_if(m_files.begin(), m_files.end(),
                                   [&fileName](const auto& item) { return item.name == fileName; });
    if (!m_directoryIdentity || m_directoryFd < 0 || file == m_files.end() || file->fd < 0)
      return {};
    if (m_directoryRelativeInjection)
      return "/proc/self/fd/" + std::to_string(m_directoryFd) + "/" + fileName;
    return "/proc/self/fd/" + std::to_string(file->fd);
  }

private:
  int m_directoryFd{-1};
  std::optional<PosixObjectIdentity> m_directoryIdentity;
  std::vector<PosixPinnedFile> m_files;
  bool m_directoryRelativeInjection{false};
};
#else
class CScopedHandle final
{
public:
  explicit CScopedHandle(HANDLE handle = INVALID_HANDLE_VALUE) : m_handle(handle) {}
  ~CScopedHandle()
  {
    if (*this)
      CloseHandle(m_handle);
  }
  CScopedHandle(const CScopedHandle&) = delete;
  CScopedHandle& operator=(const CScopedHandle&) = delete;
  CScopedHandle(CScopedHandle&& other) noexcept
    : m_handle(std::exchange(other.m_handle, INVALID_HANDLE_VALUE))
  {
  }
  CScopedHandle& operator=(CScopedHandle&& other) noexcept
  {
    if (this != &other)
    {
      if (*this)
        CloseHandle(m_handle);
      m_handle = std::exchange(other.m_handle, INVALID_HANDLE_VALUE);
    }
    return *this;
  }
  HANDLE Get() const { return m_handle; }
  explicit operator bool() const { return m_handle != nullptr && m_handle != INVALID_HANDLE_VALUE; }

private:
  HANDLE m_handle{INVALID_HANDLE_VALUE};
};

CScopedHandle OpenWindowsEntryNoReparse(const std::filesystem::path& path,
                                        bool directory,
                                        DWORD access = FILE_READ_ATTRIBUTES)
{
  const DWORD flags = FILE_FLAG_OPEN_REPARSE_POINT | (directory ? FILE_FLAG_BACKUP_SEMANTICS : 0);
  CScopedHandle handle{CreateFileW(path.c_str(), access,
                                   FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                                   OPEN_EXISTING, flags, nullptr)};
  if (!handle)
    return CScopedHandle{};
  BY_HANDLE_FILE_INFORMATION information{};
  if (!GetFileInformationByHandle(handle.Get(), &information) ||
      (information.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0 ||
      information.nNumberOfLinks != 1 ||
      directory != ((information.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0))
  {
    return CScopedHandle{};
  }
  return handle;
}

bool IsSameWindowsEntry(const std::filesystem::path& path, HANDLE openedHandle, bool directory)
{
  CScopedHandle current = OpenWindowsEntryNoReparse(path, directory);
  if (!current)
    return false;
  BY_HANDLE_FILE_INFORMATION opened{};
  BY_HANDLE_FILE_INFORMATION resolved{};
  return GetFileInformationByHandle(openedHandle, &opened) &&
         GetFileInformationByHandle(current.Get(), &resolved) &&
         opened.dwVolumeSerialNumber == resolved.dwVolumeSerialNumber &&
         opened.nFileIndexHigh == resolved.nFileIndexHigh &&
         opened.nFileIndexLow == resolved.nFileIndexLow;
}

bool DeleteWindowsEntryByHandle(HANDLE handle)
{
  FILE_DISPOSITION_INFO disposition{};
  disposition.DeleteFile = TRUE;
  return SetFileInformationByHandle(handle, FileDispositionInfo, &disposition,
                                    sizeof(disposition)) != 0;
}

CScopedHandle OpenWindowsReparseEntry(const std::filesystem::path& path, bool directory)
{
  const DWORD flags = FILE_FLAG_OPEN_REPARSE_POINT | (directory ? FILE_FLAG_BACKUP_SEMANTICS : 0);
  CScopedHandle handle{CreateFileW(path.c_str(), DELETE | FILE_READ_ATTRIBUTES,
                                   FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                                   OPEN_EXISTING, flags, nullptr)};
  if (!handle)
    return CScopedHandle{};
  BY_HANDLE_FILE_INFORMATION information{};
  if (!GetFileInformationByHandle(handle.Get(), &information) ||
      (information.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) == 0 ||
      directory != ((information.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0))
  {
    return CScopedHandle{};
  }
  return handle;
}

#endif

bool SameBinding(const JumpgateSubtitleBinding& left, const JumpgateSubtitleBinding& right)
{
  return left.generation == right.generation && left.profileId == right.profileId &&
         left.deviceId == right.deviceId && left.bridgeOrigin == right.bridgeOrigin &&
         left.sessionId == right.sessionId;
}

std::string DefaultToken()
{
  std::random_device random;
  std::ostringstream token;
  token << std::hex << std::setfill('0');
  for (std::size_t index = 0; index < 4; ++index)
    token << std::setw(8) << static_cast<std::uint32_t>(random());
  return token.str();
}

#if !defined(_WIN32)
bool IsPrivateToken(const std::string& token)
{
  return token.size() == 32 &&
         std::all_of(token.begin(), token.end(), [](char item)
                     { return (item >= '0' && item <= '9') || (item >= 'a' && item <= 'f'); });
}
#endif

bool IsGenerationDirectoryName(const std::string& name)
{
  static const std::regex pattern{"^jg-[1-9][0-9]*-[a-f0-9]{32}$"};
  return std::regex_match(name, pattern);
}

#if !defined(_WIN32)
std::string Extension(const std::string& fileName)
{
  const std::size_t dot = fileName.rfind('.');
  return dot == std::string::npos ? std::string{} : fileName.substr(dot);
}

bool IsSupportedTextExtension(const std::string& extension)
{
  return extension == ".srt" || extension == ".vtt" || extension == ".ass" || extension == ".ssa" ||
         extension == ".smi" || extension == ".sub" || extension == ".txt";
}
#endif

} // namespace

void SecureClearAndroidJumpgateSubtitleCompletion(JumpgateSubtitleCompletion& completion)
{
  for (JumpgateSubtitleStagedPart& part : completion.artifact.parts)
  {
    volatile std::uint8_t* bytes = part.bytes.empty() ? nullptr : part.bytes.data();
    for (std::size_t index = 0; index < part.bytes.size(); ++index)
      bytes[index] = 0;
    part.bytes.clear();
    std::fill(part.sha256.begin(), part.sha256.end(), '\0');
    part.sha256.clear();
  }
  completion.artifact.parts.clear();
  completion.artifact.selected = {};
  completion.artifact.artifactId.clear();
  completion.artifact.expiresAt = 0;
}

bool IsWithinAndroidJumpgateArtifactCap(const JumpgateSubtitleCompletion& completion)
{
  if (completion.artifact.parts.empty() || completion.artifact.parts.size() > 2)
    return false;

  std::size_t aggregate = 0;
  for (const JumpgateSubtitleStagedPart& part : completion.artifact.parts)
  {
    if (part.bytes.empty() || part.bytes.size() > ANDROID_JUMPGATE_MAX_PART_BYTES ||
        aggregate > ANDROID_JUMPGATE_MAX_ARTIFACT_BYTES - part.bytes.size())
    {
      return false;
    }
    aggregate += part.bytes.size();
  }
  return aggregate <= ANDROID_JUMPGATE_MAX_ARTIFACT_BYTES;
}

bool CAndroidJumpgateArtifactBudget::Reserve(const std::string& artifactKey,
                                             const std::string& partKey,
                                             std::size_t maximumBytes)
{
  if (artifactKey.empty() || partKey.empty() || maximumBytes == 0 ||
      maximumBytes > ANDROID_JUMPGATE_MAX_PART_BYTES)
  {
    return false;
  }

  std::lock_guard lock(m_mutex);
  if (artifactKey != m_artifactKey)
  {
    m_artifactKey = artifactKey;
    m_parts.clear();
    m_reservedBytes = 0;
  }
  const auto existing = m_parts.find(partKey);
  if (existing != m_parts.end())
    return existing->second == maximumBytes;
  if (maximumBytes > ANDROID_JUMPGATE_MAX_ARTIFACT_BYTES - m_reservedBytes)
    return false;
  m_parts.emplace(partKey, maximumBytes);
  m_reservedBytes += maximumBytes;
  return true;
}

void CAndroidJumpgateArtifactBudget::Reset()
{
  std::lock_guard lock(m_mutex);
  m_artifactKey.clear();
  m_parts.clear();
  m_reservedBytes = 0;
}

struct CAndroidJumpgateSubtitleFileStore::CleanupTracker
{
  AndroidJumpgateCleanupBudget budget;
  AndroidJumpgateCleanupReport report;
  std::chrono::steady_clock::time_point started;
};

CAndroidJumpgateSubtitleFileStore::CAndroidJumpgateSubtitleFileStore(
    std::filesystem::path root,
    TokenGenerator tokenGenerator,
    Clock clock,
    PublishObserver publishObserver,
    SecurityObserver securityObserver)
  : m_root(std::filesystem::absolute(std::move(root)).lexically_normal()),
    m_tokenGenerator(tokenGenerator ? std::move(tokenGenerator) : DefaultToken),
    m_clock(clock ? std::move(clock) : [] { return std::chrono::steady_clock::now(); }),
    m_publishObserver(std::move(publishObserver)),
    m_securityObserver(std::move(securityObserver))
{
}

bool CAndroidJumpgateSubtitleFileStore::EnsureRoot()
{
#if !defined(_WIN32)
  const std::string rootName = m_root.filename().string();
  const CScopedFd parent = OpenAncestorPathNoFollow(m_root.parent_path());
  if (!parent || rootName.empty() || rootName == "." || rootName == "..")
    return false;

  struct stat status{};
  if (fstatat(parent.Get(), rootName.c_str(), &status, AT_SYMLINK_NOFOLLOW) != 0)
  {
    if (errno != ENOENT || mkdirat(parent.Get(), rootName.c_str(), 0700) != 0)
      return false;
  }
  const CScopedFd root = OpenAccessibleDirectoryAtNoFollow(parent.Get(), rootName, false);
  struct stat rootStatus{};
  return root && IsSameDirectoryIdentity(parent.Get(), rootName, root.Get()) &&
         fstat(root.Get(), &rootStatus) == 0 && rootStatus.st_uid == geteuid() &&
         (rootStatus.st_mode & 0077) == 0;
#else
  std::error_code error;
  std::filesystem::file_status status = std::filesystem::symlink_status(m_root, error);
  if (!error && std::filesystem::is_symlink(status))
    return false;
  if (error || !std::filesystem::exists(status))
  {
    error.clear();
    if (!std::filesystem::create_directories(m_root, error) && error)
      return false;
    status = std::filesystem::symlink_status(m_root, error);
  }
  if (error || !std::filesystem::is_directory(status) || std::filesystem::is_symlink(status))
    return false;
  return static_cast<bool>(OpenWindowsEntryNoReparse(m_root, true));
#endif
}

bool CAndroidJumpgateSubtitleFileStore::IsContainedGenerationDirectory(
    const std::filesystem::path& directory) const
{
  const std::filesystem::path normalized = std::filesystem::absolute(directory).lexically_normal();
  return normalized.parent_path() == m_root &&
         IsGenerationDirectoryName(normalized.filename().string());
}

std::optional<AndroidJumpgateStagedArtifact> CAndroidJumpgateSubtitleFileStore::Stage(
    const JumpgateSubtitleCompletion& completion,
    const CJumpgateSubtitleCancellationToken& cancellation)
{
#if defined(_WIN32)
  // Production staging is fail-closed until Windows has equivalent parent-relative handle APIs.
  static_cast<void>(completion);
  static_cast<void>(cancellation);
  return std::nullopt;
#else
  if (completion.status != JumpgateSubtitleResultStatus::Success ||
      !CJumpgateSubtitleClient::IsCanonicalLanguage(completion.artifact.selected.language) ||
      !IsWithinAndroidJumpgateArtifactCap(completion) || cancellation.IsCancelled())
  {
    return std::nullopt;
  }

  if (!EnsureRoot())
    return std::nullopt;

  const CScopedFd root = OpenAccessibleLocalDirectoryNoFollow(m_root);
  if (!root)
    return std::nullopt;

  std::string directoryName;
  for (std::size_t attempt = 0; attempt < MAX_DIRECTORY_CREATE_ATTEMPTS; ++attempt)
  {
    const std::string token = m_tokenGenerator();
    if (!IsPrivateToken(token))
      return std::nullopt;
    directoryName = "jg-" + std::to_string(completion.binding.generation) + "-" + token;
    if (mkdirat(root.Get(), directoryName.c_str(), 0700) == 0)
      break;
    if (errno != EEXIST)
      return std::nullopt;
    directoryName.clear();
  }
  if (directoryName.empty())
    return std::nullopt;

  const std::filesystem::path directory = m_root / directoryName;
  const std::string baseName = "jumpgate." + completion.artifact.selected.language;
  std::string injectionName;
  bool staged = false;
  CScopedFd directoryFd =
      root ? OpenAccessibleDirectoryAtNoFollow(root.Get(), directoryName, true) : CScopedFd{};
  if (!root || !directoryFd)
    return std::nullopt;
  if (m_securityObserver)
    m_securityObserver("generation-opened", directory);
  if (!IsSameDirectoryEntry(root.Get(), directoryName, directoryFd.Get()))
    return std::nullopt;

  if (completion.artifact.parts.size() == 1)
  {
    const JumpgateSubtitleStagedPart& part = completion.artifact.parts.front();
    const std::string extension = Extension(part.fileName);
    const std::string finalName = baseName + extension;
    const std::string temporaryName = finalName + ".part";
    CScopedFd temporaryFile;
    if (part.role == "subtitle" && IsSupportedTextExtension(extension))
      temporaryFile = WriteFileAt(directoryFd.Get(), temporaryName, part.bytes, cancellation);
    if (temporaryFile)
    {
      if (m_securityObserver)
        m_securityObserver("before-publish", directory / temporaryName);
      staged =
          IsSameDirectoryEntry(root.Get(), directoryName, directoryFd.Get()) &&
          PublishFileAtNoReplace(directoryFd.Get(), temporaryName, finalName, temporaryFile.Get());
      if (staged)
      {
        injectionName = finalName;
        if (m_publishObserver)
          m_publishObserver(directory / finalName);
      }
    }
  }
  else
  {
    const JumpgateSubtitleStagedPart& index = completion.artifact.parts[0];
    const JumpgateSubtitleStagedPart& sub = completion.artifact.parts[1];
    const std::string indexName = baseName + ".idx";
    const std::string subName = baseName + ".sub";
    const std::string temporaryIndex = indexName + ".part";
    const std::string temporarySub = subName + ".part";
    CScopedFd temporaryIndexFile;
    CScopedFd temporarySubFile;
    if (index.role == "index" && sub.role == "sub" && Extension(index.fileName) == ".idx" &&
        Extension(sub.fileName) == ".sub")
    {
      temporaryIndexFile =
          WriteFileAt(directoryFd.Get(), temporaryIndex, index.bytes, cancellation);
      temporarySubFile = WriteFileAt(directoryFd.Get(), temporarySub, sub.bytes, cancellation);
    }
    if (temporaryIndexFile && temporarySubFile &&
        IsSameDirectoryEntry(root.Get(), directoryName, directoryFd.Get()) &&
        PublishFileAtNoReplace(directoryFd.Get(), temporarySub, subName, temporarySubFile.Get()))
    {
      if (m_publishObserver)
        m_publishObserver(directory / subName);
      staged = PublishFileAtNoReplace(directoryFd.Get(), temporaryIndex, indexName,
                                      temporaryIndexFile.Get());
      if (staged)
      {
        if (m_publishObserver)
          m_publishObserver(directory / indexName);
        injectionName = indexName;
      }
    }
  }
  if (staged && !IsSameDirectoryEntry(root.Get(), directoryName, directoryFd.Get()))
    return std::nullopt;
  if (!staged || cancellation.IsCancelled())
  {
    CleanupTracker tracker{OWNED_DIRECTORY_CLEANUP_BUDGET, {}, m_clock()};
    ++tracker.report.entriesVisited;
    RemoveGenerationDirectoryAt(root.Get(), directoryName, tracker);
    return std::nullopt;
  }

  std::vector<PosixPinnedFile> pinnedFiles;
  const auto pinFile = [&directoryFd, &pinnedFiles](const std::string& name)
  {
    PosixObjectIdentity identity;
    CScopedFd file = OpenRegularFileAtNoFollow(directoryFd.Get(), name, identity);
    if (!file || fchmod(file.Get(), 0400) != 0)
      return false;
    pinnedFiles.emplace_back(PosixPinnedFile{name, file.Release(), identity});
    return true;
  };
  bool pinned = pinFile(injectionName);
  if (completion.artifact.parts.size() == 2)
    pinned = pinned && pinFile(baseName + ".sub");
  if (pinned)
    pinned = fsync(directoryFd.Get()) == 0 && fchmod(directoryFd.Get(), 0500) == 0;

  std::shared_ptr<CPosixSubtitleArtifactAnchor> anchor;
  bool filesTransferred = false;
  if (pinned)
  {
    anchor = std::make_shared<CPosixSubtitleArtifactAnchor>(
        directoryFd.Release(), std::move(pinnedFiles), completion.artifact.parts.size() == 2);
    filesTransferred = true;
    pinned = anchor->Validate();
  }
  if (!pinned)
  {
    anchor.reset();
    if (!filesTransferred)
    {
      for (PosixPinnedFile& file : pinnedFiles)
      {
        if (file.fd >= 0)
          close(file.fd);
      }
    }
    if (directoryFd)
      fchmod(directoryFd.Get(), 0700);
    CleanupTracker tracker{OWNED_DIRECTORY_CLEANUP_BUDGET, {}, m_clock()};
    ++tracker.report.entriesVisited;
    RemoveGenerationDirectoryAt(root.Get(), directoryName, tracker);
    return std::nullopt;
  }
  const std::string injectionPath = anchor->InjectionPath(injectionName);
  return AndroidJumpgateStagedArtifact{completion.binding,
                                       directory.string(),
                                       injectionName,
                                       injectionPath,
                                       completion.artifact.selected.language,
                                       completion.artifact.selected.format,
                                       std::move(anchor)};
#endif
}

AndroidJumpgateCleanupReport CAndroidJumpgateSubtitleFileStore::RemoveGenerationDirectory(
    const std::string& directory)
{
  CleanupTracker tracker{OWNED_DIRECTORY_CLEANUP_BUDGET, {}, m_clock()};
#if defined(_WIN32)
  // Path-based deletion cannot provide the parent-relative containment guarantee.
  static_cast<void>(directory);
  tracker.report.containmentRejected = true;
  return tracker.report;
#else
  return RemoveGenerationDirectory(std::filesystem::path{directory}, tracker);
#endif
}

AndroidJumpgateCleanupReport CAndroidJumpgateSubtitleFileStore::RemoveGenerationDirectory(
    const std::filesystem::path& directory, CleanupTracker& tracker)
{
  const std::filesystem::path normalized = std::filesystem::absolute(directory).lexically_normal();
  if (!IsContainedGenerationDirectory(normalized))
  {
    tracker.report.containmentRejected = true;
    return tracker.report;
  }
  if (tracker.report.entriesVisited >= tracker.budget.maximumEntries)
  {
    tracker.report.entryLimitHit = true;
    return tracker.report;
  }
  if (m_clock() - tracker.started >= tracker.budget.maximumElapsed)
  {
    tracker.report.timeLimitHit = true;
    return tracker.report;
  }
  ++tracker.report.entriesVisited;

#if defined(_WIN32)
  const auto canVisit = [this, &tracker]()
  {
    if (tracker.report.entriesVisited >= tracker.budget.maximumEntries)
    {
      tracker.report.entryLimitHit = true;
      return false;
    }
    if (m_clock() - tracker.started >= tracker.budget.maximumElapsed)
    {
      tracker.report.timeLimitHit = true;
      return false;
    }
    return true;
  };
  CScopedHandle rootHandle = OpenWindowsEntryNoReparse(m_root, true);
  if (!rootHandle)
  {
    tracker.report.containmentRejected = true;
    return tracker.report;
  }
  std::error_code error;
  const std::filesystem::file_status directoryStatus =
      std::filesystem::symlink_status(normalized, error);
  if (error || !std::filesystem::exists(directoryStatus))
    return tracker.report;
  if (std::filesystem::is_symlink(directoryStatus) ||
      !std::filesystem::is_directory(directoryStatus))
  {
    const DWORD attributes = GetFileAttributesW(normalized.c_str());
    if (attributes != INVALID_FILE_ATTRIBUTES && (attributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0)
    {
      CScopedHandle reparse =
          OpenWindowsReparseEntry(normalized, (attributes & FILE_ATTRIBUTE_DIRECTORY) != 0);
      if (!reparse || !IsSameWindowsEntry(m_root, rootHandle.Get(), true) ||
          !DeleteWindowsEntryByHandle(reparse.Get()))
      {
        tracker.report.containmentRejected = true;
      }
    }
    return tracker.report;
  }

  CScopedHandle directoryHandle =
      OpenWindowsEntryNoReparse(normalized, true, DELETE | FILE_READ_ATTRIBUTES);
  if (!directoryHandle || !IsSameWindowsEntry(m_root, rootHandle.Get(), true) ||
      !IsSameWindowsEntry(normalized, directoryHandle.Get(), true))
  {
    tracker.report.containmentRejected = true;
    return tracker.report;
  }

  std::filesystem::directory_iterator iterator{
      normalized, std::filesystem::directory_options::skip_permission_denied, error};
  const std::filesystem::directory_iterator end;
  for (; !error && iterator != end; iterator.increment(error))
  {
    if (!IsSameWindowsEntry(m_root, rootHandle.Get(), true))
    {
      tracker.report.containmentRejected = true;
      break;
    }
    if (!canVisit())
      break;
    if (!IsSameWindowsEntry(m_root, rootHandle.Get(), true) ||
        !IsSameWindowsEntry(normalized, directoryHandle.Get(), true))
    {
      tracker.report.containmentRejected = true;
      break;
    }
    const std::filesystem::path entry = iterator->path().lexically_normal();
    if (entry.parent_path() != normalized)
    {
      tracker.report.containmentRejected = true;
      break;
    }
    ++tracker.report.entriesVisited;
    const std::filesystem::file_status status = std::filesystem::symlink_status(entry, error);
    if (error)
      break;

    std::uintmax_t bytes = 0;
    CScopedHandle entryHandle;
    if (std::filesystem::is_regular_file(status))
    {
      entryHandle = OpenWindowsEntryNoReparse(entry, false, DELETE | FILE_READ_ATTRIBUTES);
      BY_HANDLE_FILE_INFORMATION information{};
      if (!entryHandle || !GetFileInformationByHandle(entryHandle.Get(), &information))
      {
        tracker.report.containmentRejected = true;
        break;
      }
      bytes =
          (static_cast<std::uintmax_t>(information.nFileSizeHigh) << 32) | information.nFileSizeLow;
      if (bytes > tracker.budget.maximumBytes - tracker.report.bytesRemoved)
      {
        tracker.report.byteLimitHit = true;
        break;
      }
    }

    error.clear();
    bool removed = false;
    if (std::filesystem::is_symlink(status))
    {
      const DWORD attributes = GetFileAttributesW(entry.c_str());
      entryHandle =
          OpenWindowsReparseEntry(entry, attributes != INVALID_FILE_ATTRIBUTES &&
                                             (attributes & FILE_ATTRIBUTE_DIRECTORY) != 0);
      removed = entryHandle && DeleteWindowsEntryByHandle(entryHandle.Get());
    }
    else if (std::filesystem::is_regular_file(status))
    {
      if (!entryHandle || !IsSameWindowsEntry(entry, entryHandle.Get(), false))
      {
        tracker.report.containmentRejected = true;
        break;
      }
      removed = DeleteWindowsEntryByHandle(entryHandle.Get());
    }
    else
    {
      tracker.report.containmentRejected = true;
      break;
    }
    if (removed && !error)
      tracker.report.bytesRemoved += bytes;
    if (!removed)
    {
      tracker.report.containmentRejected = true;
      break;
    }
    if (error)
      break;
  }

  if (!error && !tracker.report.entryLimitHit && !tracker.report.byteLimitHit &&
      !tracker.report.timeLimitHit && !tracker.report.containmentRejected)
  {
    if (!IsSameWindowsEntry(m_root, rootHandle.Get(), true) ||
        !IsSameWindowsEntry(normalized, directoryHandle.Get(), true) ||
        !DeleteWindowsEntryByHandle(directoryHandle.Get()))
    {
      tracker.report.containmentRejected = true;
      return tracker.report;
    }
  }
#else
  const CScopedFd root = OpenAccessibleLocalDirectoryNoFollow(m_root);
  if (!root)
  {
    tracker.report.containmentRejected = true;
    return tracker.report;
  }
  return RemoveGenerationDirectoryAt(root.Get(), normalized.filename().string(), tracker);
#endif
  return tracker.report;
}

#if !defined(_WIN32)
AndroidJumpgateCleanupReport CAndroidJumpgateSubtitleFileStore::RemoveGenerationDirectoryAt(
    int rootFd, const std::string& directoryName, CleanupTracker& tracker)
{
  const auto canVisit = [this, &tracker]()
  {
    if (tracker.report.entriesVisited >= tracker.budget.maximumEntries)
    {
      tracker.report.entryLimitHit = true;
      return false;
    }
    if (m_clock() - tracker.started >= tracker.budget.maximumElapsed)
    {
      tracker.report.timeLimitHit = true;
      return false;
    }
    return true;
  };

  struct stat directoryStatus{};
  if (fstatat(rootFd, directoryName.c_str(), &directoryStatus, AT_SYMLINK_NOFOLLOW) != 0)
    return tracker.report;
  if (S_ISLNK(directoryStatus.st_mode))
  {
    if (unlinkat(rootFd, directoryName.c_str(), 0) != 0)
      tracker.report.containmentRejected = true;
    return tracker.report;
  }
  if (!S_ISDIR(directoryStatus.st_mode) || directoryStatus.st_nlink != 2)
  {
    tracker.report.containmentRejected = true;
    return tracker.report;
  }

  const CScopedFd directoryFd = OpenAccessibleDirectoryAtNoFollow(rootFd, directoryName, true);
  if (!directoryFd || !IsSameDirectoryEntry(rootFd, directoryName, directoryFd.Get()))
  {
    tracker.report.containmentRejected = true;
    return tracker.report;
  }
  if (flock(directoryFd.Get(), LOCK_EX | LOCK_NB) != 0)
  {
    if (errno == EWOULDBLOCK || errno == EAGAIN)
      tracker.report.inUse = true;
    else
      tracker.report.containmentRejected = true;
    return tracker.report;
  }
  if (fchmod(directoryFd.Get(), 0700) != 0)
  {
    tracker.report.containmentRejected = true;
    return tracker.report;
  }
  const int streamFd = dup(directoryFd.Get());
  if (streamFd < 0)
    return tracker.report;
  DIR* stream = fdopendir(streamFd);
  if (!stream)
  {
    close(streamFd);
    return tracker.report;
  }
  errno = 0;
  while (const dirent* entry = readdir(stream))
  {
    const std::string name = entry->d_name;
    if (name == "." || name == "..")
      continue;
    if (!canVisit())
      break;
    ++tracker.report.entriesVisited;
    struct stat status{};
    if (fstatat(directoryFd.Get(), name.c_str(), &status, AT_SYMLINK_NOFOLLOW) != 0)
    {
      tracker.report.containmentRejected = true;
      break;
    }
    std::uintmax_t bytes = 0;
    if (S_ISREG(status.st_mode))
    {
      if (status.st_nlink != 1)
      {
        tracker.report.containmentRejected = true;
        break;
      }
      bytes = static_cast<std::uintmax_t>(status.st_size);
      if (bytes > tracker.budget.maximumBytes - tracker.report.bytesRemoved)
      {
        tracker.report.byteLimitHit = true;
        break;
      }
    }
    else if (!S_ISLNK(status.st_mode))
    {
      tracker.report.containmentRejected = true;
      break;
    }
    if (unlinkat(directoryFd.Get(), name.c_str(), 0) != 0)
    {
      tracker.report.containmentRejected = true;
      break;
    }
    tracker.report.bytesRemoved += bytes;
    errno = 0;
  }
  if (errno != 0)
    tracker.report.containmentRejected = true;
  closedir(stream);

  if (!tracker.report.entryLimitHit && !tracker.report.byteLimitHit &&
      !tracker.report.timeLimitHit && !tracker.report.containmentRejected &&
      IsSameDirectoryEntry(rootFd, directoryName, directoryFd.Get()) &&
      unlinkat(rootFd, directoryName.c_str(), AT_REMOVEDIR) != 0)
  {
    tracker.report.containmentRejected = true;
  }
  return tracker.report;
}
#endif

AndroidJumpgateCleanupReport CAndroidJumpgateSubtitleFileStore::SweepStartupOrphans(
    AndroidJumpgateCleanupBudget budget)
{
  CleanupTracker tracker{budget, {}, m_clock()};
#if defined(_WIN32)
  // Host-side behavior tests inject a fake backend; production Windows never mutates paths here.
  tracker.report.containmentRejected = true;
  return tracker.report;
#else
  if (budget.maximumEntries == 0 || budget.maximumBytes == 0 || budget.maximumElapsed <= 0ms ||
      !EnsureRoot())
  {
    return tracker.report;
  }

#if defined(_WIN32)
  const CScopedHandle rootHandle = OpenWindowsEntryNoReparse(m_root, true);
  if (!rootHandle)
  {
    tracker.report.containmentRejected = true;
    return tracker.report;
  }
  std::error_code error;
  std::filesystem::directory_iterator iterator{
      m_root, std::filesystem::directory_options::skip_permission_denied, error};
  const std::filesystem::directory_iterator end;
  for (; !error && iterator != end; iterator.increment(error))
  {
    if (!IsSameWindowsEntry(m_root, rootHandle.Get(), true))
    {
      tracker.report.containmentRejected = true;
      break;
    }
    if (tracker.report.entriesVisited >= budget.maximumEntries)
    {
      tracker.report.entryLimitHit = true;
      break;
    }
    if (m_clock() - tracker.started >= budget.maximumElapsed)
    {
      tracker.report.timeLimitHit = true;
      break;
    }
    const std::filesystem::path candidate = iterator->path().lexically_normal();
    if (candidate.parent_path() != m_root)
    {
      tracker.report.containmentRejected = true;
      break;
    }
    ++tracker.report.entriesVisited;
    if (!IsGenerationDirectoryName(candidate.filename().string()))
      continue;
    RemoveGenerationDirectory(candidate, tracker);
    if (!IsSameWindowsEntry(m_root, rootHandle.Get(), true))
      tracker.report.containmentRejected = true;
    if (tracker.report.entryLimitHit || tracker.report.byteLimitHit ||
        tracker.report.timeLimitHit || tracker.report.containmentRejected)
    {
      break;
    }
  }
#else
  const CScopedFd root = OpenAccessibleLocalDirectoryNoFollow(m_root);
  if (!root)
  {
    tracker.report.containmentRejected = true;
    return tracker.report;
  }
  const int streamFd = dup(root.Get());
  if (streamFd < 0)
    return tracker.report;
  DIR* stream = fdopendir(streamFd);
  if (!stream)
  {
    close(streamFd);
    return tracker.report;
  }
  errno = 0;
  while (const dirent* entry = readdir(stream))
  {
    const std::string name = entry->d_name;
    if (name == "." || name == ".." || !IsGenerationDirectoryName(name))
      continue;
    if (tracker.report.entriesVisited >= budget.maximumEntries)
    {
      tracker.report.entryLimitHit = true;
      break;
    }
    if (m_clock() - tracker.started >= budget.maximumElapsed)
    {
      tracker.report.timeLimitHit = true;
      break;
    }
    ++tracker.report.entriesVisited;
    RemoveGenerationDirectoryAt(root.Get(), name, tracker);
    if (tracker.report.entryLimitHit || tracker.report.byteLimitHit ||
        tracker.report.timeLimitHit || tracker.report.containmentRejected)
    {
      break;
    }
    errno = 0;
  }
  if (errno != 0)
    tracker.report.containmentRejected = true;
  closedir(stream);
#endif
  return tracker.report;
#endif
}

struct CAndroidJumpgateSubtitleStageWorker::WorkerState
{
  WorkerState(std::shared_ptr<IAndroidJumpgateSubtitleFileStore> store,
              ClearObserver completionClearObserver)
    : fileStore(std::move(store)),
      clearObserver(std::move(completionClearObserver))
  {
  }

  struct Job
  {
    JumpgateSubtitleCompletion completion;
    std::shared_ptr<CJumpgateSubtitleCancellationSource> cancellation;
  };

  struct CleanupRequest
  {
    std::string directory;
    std::chrono::steady_clock::time_point queuedAt{std::chrono::steady_clock::now()};
    std::size_t attempts{0};
  };

  bool QueueCleanupLocked(std::string directory)
  {
    if (directory.empty())
      return false;
    if (cleanupInFlight == directory ||
        std::any_of(cleanup.begin(), cleanup.end(), [&directory](const CleanupRequest& request)
                    { return request.directory == directory; }))
    {
      return true;
    }
    const std::size_t occupied = cleanup.size() + (cleanupInFlight.empty() ? 0U : 1U);
    if (occupied >= MAX_CLEANUP_REQUESTS)
      return false;
    cleanup.emplace_back(CleanupRequest{std::move(directory)});
    return true;
  }

  std::shared_ptr<IAndroidJumpgateSubtitleFileStore> fileStore;
  ClearObserver clearObserver;
  std::mutex mutex;
  std::condition_variable condition;
  std::condition_variable finishedCondition;
  std::optional<Job> pending;
  std::deque<JumpgateSubtitleCompletion> discarded;
  std::deque<CleanupRequest> cleanup;
  std::string cleanupInFlight;
  std::shared_ptr<CJumpgateSubtitleCancellationSource> activeCancellation;
  std::optional<JumpgateSubtitleBinding> latestBinding;
  std::optional<AndroidJumpgateStageCompletion> completion;
  bool startupSweepPending{false};
  bool stopping{false};
  bool finished{false};
};

CAndroidJumpgateSubtitleStageWorker::CAndroidJumpgateSubtitleStageWorker(
    std::shared_ptr<IAndroidJumpgateSubtitleFileStore> fileStore,
    std::shared_ptr<CJumpgateThreadRegistry> registry,
    ClearObserver clearObserver)
  : m_registry(registry ? std::move(registry) : CJumpgateThreadRegistry::Global())
{
  if (!fileStore)
    return;
  m_registryReservation = m_registry->Reserve();
  if (!m_registryReservation)
    return;
  m_state = std::make_shared<WorkerState>(std::move(fileStore), std::move(clearObserver));
  m_worker = std::thread([state = m_state] { Run(state); });
}

CAndroidJumpgateSubtitleStageWorker::~CAndroidJumpgateSubtitleStageWorker()
{
  Stop();
}

bool CAndroidJumpgateSubtitleStageWorker::SameBinding(const JumpgateSubtitleBinding& left,
                                                      const JumpgateSubtitleBinding& right)
{
  return KODI::JUMPGATE::SameBinding(left, right);
}

bool CAndroidJumpgateSubtitleStageWorker::Queue(JumpgateSubtitleCompletion&& completion)
{
  auto cancellation = std::make_shared<CJumpgateSubtitleCancellationSource>();
  std::lock_guard ownerLock(m_ownerMutex);
  const std::shared_ptr<WorkerState> state = m_state;
  if (!state)
    return false;
  {
    std::lock_guard lock(state->mutex);
    if (state->stopping || completion.binding.generation == 0 ||
        (state->latestBinding && completion.binding.generation <= state->latestBinding->generation))
    {
      if (state->stopping || state->discarded.size() >= MAX_DISCARDED_COMPLETIONS)
        return false;
      state->discarded.emplace_back(std::move(completion));
      state->condition.notify_all();
      return true;
    }
    if (state->activeCancellation)
      state->activeCancellation->Cancel();
    if (state->pending)
    {
      if (state->discarded.size() >= MAX_DISCARDED_COMPLETIONS)
        return false;
      state->pending->cancellation->Cancel();
      state->discarded.emplace_back(std::move(state->pending->completion));
      state->pending.reset();
    }
    if (state->completion && state->completion->artifact)
    {
      state->completion->artifact->anchor.reset();
      state->QueueCleanupLocked(std::move(state->completion->artifact->directory));
    }
    state->completion.reset();
    state->latestBinding = completion.binding;
    state->pending = WorkerState::Job{std::move(completion), std::move(cancellation)};
  }
  state->condition.notify_all();
  return true;
}

bool CAndroidJumpgateSubtitleStageWorker::Cancel(const JumpgateSubtitleBinding& binding)
{
  std::lock_guard ownerLock(m_ownerMutex);
  const std::shared_ptr<WorkerState> state = m_state;
  if (!state)
    return false;
  {
    std::lock_guard lock(state->mutex);
    if (state->stopping || !state->latestBinding || !SameBinding(*state->latestBinding, binding))
    {
      return false;
    }
    if (state->activeCancellation)
      state->activeCancellation->Cancel();
    if (state->pending && SameBinding(state->pending->completion.binding, binding))
      state->pending->cancellation->Cancel();
    if (state->completion && SameBinding(state->completion->binding, binding))
    {
      if (state->completion->artifact)
      {
        state->completion->artifact->anchor.reset();
        state->QueueCleanupLocked(std::move(state->completion->artifact->directory));
      }
      state->completion.reset();
    }
  }
  state->condition.notify_all();
  return true;
}

std::optional<AndroidJumpgateStageCompletion> CAndroidJumpgateSubtitleStageWorker::TakeCompletion(
    const JumpgateSubtitleBinding& binding)
{
  std::lock_guard ownerLock(m_ownerMutex);
  const std::shared_ptr<WorkerState> state = m_state;
  if (!state)
    return std::nullopt;
  std::lock_guard lock(state->mutex);
  if (state->stopping || !state->latestBinding || !SameBinding(*state->latestBinding, binding) ||
      !state->completion || !SameBinding(state->completion->binding, binding))
  {
    return std::nullopt;
  }
  std::optional<AndroidJumpgateStageCompletion> completion{std::move(*state->completion)};
  state->completion.reset();
  return completion;
}

bool CAndroidJumpgateSubtitleStageWorker::QueueCleanup(std::string directory)
{
  if (directory.empty())
    return false;
  std::lock_guard ownerLock(m_ownerMutex);
  const std::shared_ptr<WorkerState> state = m_state;
  if (!state)
    return false;
  {
    std::lock_guard lock(state->mutex);
    if (state->stopping || !state->QueueCleanupLocked(std::move(directory)))
      return false;
  }
  state->condition.notify_all();
  return true;
}

bool CAndroidJumpgateSubtitleStageWorker::QueueStartupSweep()
{
  std::lock_guard ownerLock(m_ownerMutex);
  const std::shared_ptr<WorkerState> state = m_state;
  if (!state)
    return false;
  {
    std::lock_guard lock(state->mutex);
    if (state->stopping || state->startupSweepPending)
      return false;
    state->startupSweepPending = true;
  }
  state->condition.notify_all();
  return true;
}

bool CAndroidJumpgateSubtitleStageWorker::IsOperational() const
{
  std::lock_guard ownerLock(m_ownerMutex);
  return static_cast<bool>(m_state);
}

std::size_t CAndroidJumpgateSubtitleStageWorker::PendingCleanupCount() const
{
  std::shared_ptr<WorkerState> state;
  {
    std::lock_guard ownerLock(m_ownerMutex);
    state = m_state;
  }
  if (!state)
    return 0;
  std::lock_guard lock(state->mutex);
  return state->cleanup.size() + (state->cleanupInFlight.empty() ? 0U : 1U);
}

bool CAndroidJumpgateSubtitleStageWorker::WaitForCleanupIdle(
    std::chrono::milliseconds timeout) const
{
  std::shared_ptr<WorkerState> state;
  {
    std::lock_guard ownerLock(m_ownerMutex);
    state = m_state;
  }
  if (!state)
    return true;
  std::unique_lock lock(state->mutex);
  return state->condition.wait_for(
      lock, std::max(timeout, std::chrono::milliseconds{0}),
      [&state] { return state->cleanup.empty() && state->cleanupInFlight.empty(); });
}

bool CAndroidJumpgateSubtitleStageWorker::Stop(std::chrono::milliseconds timeout)
{
  std::lock_guard ownerLock(m_ownerMutex);
  const std::shared_ptr<WorkerState> state = m_state;
  if (!state)
    return true;
  {
    std::lock_guard lock(state->mutex);
    state->stopping = true;
    state->startupSweepPending = false;
    if (state->activeCancellation)
      state->activeCancellation->Cancel();
    if (state->pending)
      state->pending->cancellation->Cancel();
    if (state->completion && state->completion->artifact)
    {
      state->completion->artifact->anchor.reset();
      state->QueueCleanupLocked(std::move(state->completion->artifact->directory));
    }
    state->completion.reset();
  }
  state->condition.notify_all();

  bool finished = false;
  {
    std::unique_lock lock(state->mutex);
    finished =
        state->finishedCondition.wait_for(lock, std::max(timeout, std::chrono::milliseconds{0}),
                                          [&state] { return state->finished; });
  }
  if (finished)
  {
    if (m_worker.joinable())
      m_worker.join();
    m_registryReservation.Reset();
  }
  else if (m_worker.joinable())
  {
    m_registry->Adopt(m_worker, std::move(m_registryReservation),
                      [state](std::chrono::milliseconds waitTime)
                      {
                        std::unique_lock lock(state->mutex);
                        return state->finishedCondition.wait_for(lock, waitTime, [&state]
                                                                 { return state->finished; });
                      });
  }
  m_state.reset();
  return finished;
}

void CAndroidJumpgateSubtitleStageWorker::Run(const std::shared_ptr<WorkerState>& state)
{
  while (true)
  {
    std::optional<WorkerState::Job> job;
    std::optional<JumpgateSubtitleCompletion> discarded;
    std::optional<WorkerState::CleanupRequest> cleanup;
    bool sweep = false;
    {
      std::unique_lock lock(state->mutex);
      state->condition.wait(lock,
                            [&state]
                            {
                              return state->stopping || state->pending ||
                                     !state->discarded.empty() || !state->cleanup.empty() ||
                                     state->startupSweepPending;
                            });
      if (!state->discarded.empty())
      {
        discarded = std::move(state->discarded.front());
        state->discarded.pop_front();
      }
      else if (!state->cleanup.empty())
      {
        cleanup = std::move(state->cleanup.front());
        state->cleanup.pop_front();
        state->cleanupInFlight = cleanup->directory;
      }
      else if (state->startupSweepPending && !state->stopping)
      {
        state->startupSweepPending = false;
        sweep = true;
      }
      else if (state->pending && !state->stopping)
      {
        job = std::move(state->pending);
        state->pending.reset();
        state->activeCancellation = job->cancellation;
      }
      else if (state->stopping)
      {
        if (state->pending)
        {
          discarded = std::move(state->pending->completion);
          state->pending.reset();
        }
        else
        {
          break;
        }
      }
    }

    if (discarded)
    {
      SecureClearAndroidJumpgateSubtitleCompletion(*discarded);
      if (state->clearObserver)
      {
        try
        {
          state->clearObserver(*discarded);
        }
        catch (...)
        {
        }
      }
      continue;
    }
    if (cleanup)
    {
      ++cleanup->attempts;
      const AndroidJumpgateCleanupReport report =
          state->fileStore->RemoveGenerationDirectory(cleanup->directory);
      const bool retryable = report.inUse && !report.containmentRejected && !report.entryLimitHit &&
                             !report.byteLimitHit && !report.timeLimitHit;
      const auto retryDeadline = cleanup->queuedAt + CLEANUP_RETRY_WINDOW;
      const auto now = std::chrono::steady_clock::now();
      bool retry = retryable && cleanup->attempts < MAX_CLEANUP_ATTEMPTS && now < retryDeadline;
      if (retry)
      {
        const auto multiplier = static_cast<std::int64_t>(1) << (cleanup->attempts - 1);
        const auto requestedBackoff = CLEANUP_INITIAL_BACKOFF * multiplier;
        const auto remaining =
            std::chrono::duration_cast<std::chrono::milliseconds>(retryDeadline - now);
        const auto backoff = std::min(requestedBackoff, remaining);
        std::unique_lock lock(state->mutex);
        state->condition.wait_for(lock, backoff, [&state] { return state->stopping; });
        retry = !state->stopping && std::chrono::steady_clock::now() < retryDeadline;
        if (retry)
          state->cleanup.emplace_back(std::move(*cleanup));
        state->cleanupInFlight.clear();
      }
      else
      {
        std::lock_guard lock(state->mutex);
        state->cleanupInFlight.clear();
      }
      state->condition.notify_all();
      const bool nonRetryableError = report.containmentRejected || report.entryLimitHit ||
                                     report.byteLimitHit || report.timeLimitHit;
      if (nonRetryableError)
      {
        CLog::Log(LOGWARNING, "CXBMCApp: Bridge subtitle cleanup failed safely without retry");
      }
      else if (report.inUse && !retry)
      {
        CLog::Log(LOGWARNING,
                  "CXBMCApp: Bridge subtitle cleanup remained in use; startup sweep retained as "
                  "fallback");
      }
      continue;
    }
    if (sweep)
    {
      state->fileStore->SweepStartupOrphans();
      continue;
    }
    if (!job)
      continue;

    std::optional<AndroidJumpgateStagedArtifact> artifact =
        state->fileStore->Stage(job->completion, job->cancellation->Token());
    SecureClearAndroidJumpgateSubtitleCompletion(job->completion);
    if (state->clearObserver)
    {
      try
      {
        state->clearObserver(job->completion);
      }
      catch (...)
      {
      }
    }

    {
      std::lock_guard lock(state->mutex);
      const bool current = !state->stopping && state->latestBinding &&
                           SameBinding(*state->latestBinding, job->completion.binding) &&
                           !job->cancellation->Token().IsCancelled();
      if (state->activeCancellation == job->cancellation)
        state->activeCancellation.reset();
      if (current)
      {
        state->completion =
            AndroidJumpgateStageCompletion{job->completion.binding, std::move(artifact)};
      }
      else if (artifact)
      {
        artifact->anchor.reset();
        state->QueueCleanupLocked(std::move(artifact->directory));
        artifact.reset();
      }
    }
    state->condition.notify_all();
  }

  {
    std::lock_guard lock(state->mutex);
    state->activeCancellation.reset();
    state->finished = true;
  }
  state->finishedCondition.notify_all();
}

} // namespace KODI::JUMPGATE
