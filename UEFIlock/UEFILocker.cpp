// UEFILocker.cpp
// 部署“保护/解锁”EFI 到目标磁盘的 ESP，并可选写入 infor.txt / psw.key。

#include "UEFILocker.h"

#include <Windows.h>
#include <winioctl.h>
#include <strsafe.h>

#include <cstdint>
#include <filesystem>
#include <optional>
#include <ostream>
#include <sstream>
#include <string>
#include <system_error>
#include <vector>

namespace {

// GPT ESP partition type GUID.
const GUID kGptEspGuid = {0xC12A7328, 0xF81F, 0x11D2, {0xBA, 0x4B, 0x00, 0xA0, 0xC9, 0x3E, 0xC9, 0x3B}};

std::wstring Win32ErrorMessage(DWORD err) {
  LPWSTR buf = nullptr;
  DWORD flags = FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS;
  DWORD n = FormatMessageW(flags, nullptr, err, 0, (LPWSTR)&buf, 0, nullptr);
  std::wstring s = (n && buf) ? std::wstring(buf, n) : L"(no message)";
  if (buf) LocalFree(buf);
  while (!s.empty() && (s.back() == L'\r' || s.back() == L'\n')) s.pop_back();
  return s;
}

void PrintLastError(std::wostream& errOut, const wchar_t* what, DWORD err) {
  errOut << L"[错误] " << what << L": " << err << L" " << Win32ErrorMessage(err) << L"\n";
}

struct EspPartition {
  DWORD diskNumber = 0;
  ULONGLONG startingOffset = 0;
  ULONGLONG length = 0;
};

bool IEqualsGuid(const GUID& a, const GUID& b) {
  return !!IsEqualGUID(a, b);
}

std::optional<EspPartition> FindEspPartitionOnDisk(std::wostream& errOut, DWORD diskNumber) {
  wchar_t diskPath[64];
  StringCchPrintfW(diskPath, 64, L"\\\\.\\PhysicalDrive%lu", static_cast<unsigned long>(diskNumber));

  HANDLE hDisk = CreateFileW(
      diskPath, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, 0, nullptr);
  if (hDisk == INVALID_HANDLE_VALUE) {
    PrintLastError(errOut, L"打开磁盘失败", GetLastError());
    return std::nullopt;
  }

  std::vector<std::uint8_t> buf(64 * 1024);
  DWORD bytes = 0;
  BOOL ok = DeviceIoControl(hDisk,
                            IOCTL_DISK_GET_DRIVE_LAYOUT_EX,
                            nullptr,
                            0,
                            buf.data(),
                            static_cast<DWORD>(buf.size()),
                            &bytes,
                            nullptr);
  CloseHandle(hDisk);
  if (!ok) {
    PrintLastError(errOut, L"获取磁盘分区布局失败(IOCTL_DISK_GET_DRIVE_LAYOUT_EX)", GetLastError());
    return std::nullopt;
  }

  if (bytes < sizeof(DRIVE_LAYOUT_INFORMATION_EX)) {
    errOut << L"[错误] 分区布局缓冲区过小。\n";
    return std::nullopt;
  }

  auto* layout = reinterpret_cast<DRIVE_LAYOUT_INFORMATION_EX*>(buf.data());
  if (layout->PartitionStyle != PARTITION_STYLE_GPT) {
    errOut << L"[错误] 该磁盘不是 GPT (PartitionStyle=" << layout->PartitionStyle << L").\n";
    return std::nullopt;
  }

  for (DWORD i = 0; i < layout->PartitionCount; ++i) {
    const PARTITION_INFORMATION_EX& p = layout->PartitionEntry[i];
    if (p.PartitionStyle != PARTITION_STYLE_GPT) continue;
    if (!IEqualsGuid(p.Gpt.PartitionType, kGptEspGuid)) continue;
    if (p.PartitionLength.QuadPart <= 0) continue;
    if (p.StartingOffset.QuadPart < 0) continue;

    EspPartition esp;
    esp.diskNumber = diskNumber;
    esp.startingOffset = static_cast<ULONGLONG>(p.StartingOffset.QuadPart);
    esp.length = static_cast<ULONGLONG>(p.PartitionLength.QuadPart);
    return esp;
  }

  errOut << L"[错误] 在磁盘 " << diskNumber << L" 上未找到 ESP 分区。\n";
  return std::nullopt;
}

std::optional<std::wstring> FindVolumeForDiskOffset(std::wostream& errOut, DWORD diskNumber, ULONGLONG startingOffset) {
  wchar_t volName[MAX_PATH];
  HANDLE hFind = FindFirstVolumeW(volName, MAX_PATH);
  if (hFind == INVALID_HANDLE_VALUE) {
    PrintLastError(errOut, L"枚举卷失败(FindFirstVolumeW)", GetLastError());
    return std::nullopt;
  }

  auto closeFind = [&]() { FindVolumeClose(hFind); };

  do {
    // Volume name looks like: \\?\Volume{GUID}\ (with a trailing backslash).
    std::wstring volumeName = volName;
    if (volumeName.size() < 5) continue;
    if (volumeName.back() != L'\\') continue;

    // CreateFile needs the trailing backslash removed.
    std::wstring volumeDevice = volumeName.substr(0, volumeName.size() - 1);
    HANDLE hVol = CreateFileW(
        volumeDevice.c_str(), 0, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, 0, nullptr);
    if (hVol == INVALID_HANDLE_VALUE) {
      continue;
    }

    // VOLUME_DISK_EXTENTS is variable-length; retry with a bigger buffer on ERROR_MORE_DATA.
    std::vector<std::uint8_t> extBuf(4096);
    DWORD bytes = 0;
    BOOL ok = FALSE;
    for (int attempt = 0; attempt < 4; ++attempt) {
      bytes = 0;
      ok = DeviceIoControl(hVol,
                           IOCTL_VOLUME_GET_VOLUME_DISK_EXTENTS,
                           nullptr,
                           0,
                           extBuf.data(),
                           static_cast<DWORD>(extBuf.size()),
                           &bytes,
                           nullptr);
      if (ok) break;
      DWORD e = GetLastError();
      if (e == ERROR_MORE_DATA) {
        extBuf.resize(extBuf.size() * 2);
        continue;
      }
      break;
    }
    CloseHandle(hVol);

    if (!ok || bytes < sizeof(VOLUME_DISK_EXTENTS)) {
      continue;
    }

    auto* ext = reinterpret_cast<VOLUME_DISK_EXTENTS*>(extBuf.data());
    for (DWORD i = 0; i < ext->NumberOfDiskExtents; ++i) {
      const DISK_EXTENT& de = ext->Extents[i];
      if (de.DiskNumber != diskNumber) continue;
      if (static_cast<ULONGLONG>(de.StartingOffset.QuadPart) != startingOffset) continue;
      return volumeName;  // Keep trailing backslash for SetVolumeMountPointW.
    }
  } while (FindNextVolumeW(hFind, volName, MAX_PATH));

  DWORD last = GetLastError();
  closeFind();
  if (last != ERROR_NO_MORE_FILES) {
    PrintLastError(errOut, L"枚举卷失败(FindNextVolumeW)", last);
  }
  errOut << L"[错误] 没有找到与磁盘 " << diskNumber << L" 起始偏移 " << startingOffset
         << L" 匹配的 Windows 卷 (可能 ESP 没有分配卷, 或权限不足).\n";
  return std::nullopt;
}

bool EnsureParentDirExists(std::wostream& errOut, const std::filesystem::path& filePath) {
  std::error_code ec;
  auto parent = filePath.parent_path();
  if (parent.empty()) return true;
  std::filesystem::create_directories(parent, ec);
  if (ec) {
    errOut << L"[错误] 创建目录失败: " << parent.wstring() << L" (" << ec.value() << L")\n";
    return false;
  }
  return true;
}

std::filesystem::path JoinMountAndRel(const std::wstring& mountPoint, std::wstring rel) {
  // rel may be "\\EFI\\BOOT\\BOOTX64.EFI" or "EFI\\BOOT\\BOOTX64.EFI".
  while (!rel.empty() && (rel.front() == L'\\' || rel.front() == L'/')) rel.erase(rel.begin());
  std::filesystem::path p(mountPoint);
  p /= std::filesystem::path(rel);
  return p;
}

std::wstring NowTimestampForFilename() {
  SYSTEMTIME st{};
  GetLocalTime(&st);
  wchar_t buf[32];
  // YYYYMMDD-HHMMSS
  StringCchPrintfW(buf,
                   32,
                   L"%04u%02u%02u-%02u%02u%02u",
                   st.wYear,
                   st.wMonth,
                   st.wDay,
                   st.wHour,
                   st.wMinute,
                   st.wSecond);
  return std::wstring(buf);
}

std::string Utf16LeWithBomBytes(const std::wstring& s) {
  // InfoPrompt.c supports UTF-16LE with BOM (0xFF,0xFE), otherwise treats bytes as ASCII.
  std::string out;
  out.reserve(2 + s.size() * 2);
  out.push_back(static_cast<char>(0xFF));
  out.push_back(static_cast<char>(0xFE));
  for (wchar_t wc : s) {
    const uint16_t u = static_cast<uint16_t>(wc);
    out.push_back(static_cast<char>(u & 0xFF));
    out.push_back(static_cast<char>((u >> 8) & 0xFF));
  }
  return out;
}

std::wstring Utf8ToWide(const std::string& s) {
  if (s.empty()) return {};
  const int need = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), static_cast<int>(s.size()), nullptr, 0);
  if (need <= 0) return {};
  std::wstring w(static_cast<size_t>(need), L'\0');
  MultiByteToWideChar(CP_UTF8, 0, s.c_str(), static_cast<int>(s.size()), w.data(), need);
  return w;
}

bool IsAbsPathW(const std::wstring& p) {
  if (p.size() >= 2 && p[1] == L':') return true;                  // C:\...
  if (p.size() >= 2 && p[0] == L'\\' && p[1] == L'\\') return true; // UNC
  return false;
}

std::wstring ExeDirW() {
  wchar_t buf[MAX_PATH];
  DWORD n = GetModuleFileNameW(nullptr, buf, MAX_PATH);
  if (n == 0 || n >= MAX_PATH) return L".";
  std::wstring full(buf, n);
  const size_t pos = full.find_last_of(L"\\/");
  if (pos == std::wstring::npos) return L".";
  return full.substr(0, pos);
}

std::filesystem::path ResolveFromExeDirUtf8(const std::string& relOrAbsUtf8) {
  const std::wstring w = Utf8ToWide(relOrAbsUtf8);
  if (w.empty()) return {};
  if (IsAbsPathW(w)) return std::filesystem::path(w);
  const std::wstring base = ExeDirW();
  if (base.empty() || base == L".") return std::filesystem::path(w);
  return std::filesystem::path(base + L"\\" + w);
}

wchar_t NormalizeDriveLetter(char c) {
  if (c >= 'a' && c <= 'z') c = static_cast<char>(c - 'a' + 'A');
  if (c < 'A' || c > 'Z') return L'S';
  return static_cast<wchar_t>(c);
}

bool CopyFileWithDirs(std::wostream& errOut,
                      const std::filesystem::path& src,
                      const std::filesystem::path& dst,
                      bool overwrite) {
  if (!EnsureParentDirExists(errOut, dst)) return false;
  if (!CopyFileW(src.wstring().c_str(), dst.wstring().c_str(), overwrite ? FALSE : TRUE)) {
    PrintLastError(errOut, L"拷贝文件失败", GetLastError());
    errOut << L"[信息] 源: " << src.wstring() << L"\n";
    errOut << L"[信息] 目标: " << dst.wstring() << L"\n";
    return false;
  }
  return true;
}

bool WriteBytesWithDirs(std::wostream& errOut, const std::filesystem::path& dst, const void* data, size_t size) {
  if (!EnsureParentDirExists(errOut, dst)) return false;
  HANDLE h = CreateFileW(dst.wstring().c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
  if (h == INVALID_HANDLE_VALUE) {
    PrintLastError(errOut, L"创建文件失败", GetLastError());
    errOut << L"[信息] 目标: " << dst.wstring() << L"\n";
    return false;
  }
  if (size > MAXDWORD) {
    errOut << L"[错误] 写入数据过大(size=" << size << L")。\n";
    CloseHandle(h);
    return false;
  }
  const DWORD want = static_cast<DWORD>(size);
  DWORD written = 0;
  BOOL ok = WriteFile(h, data, want, &written, nullptr);
  FlushFileBuffers(h);
  CloseHandle(h);
  if (!ok || written != want) {
    PrintLastError(errOut, L"写入文件失败", GetLastError());
    errOut << L"[信息] 目标: " << dst.wstring() << L"\n";
    return false;
  }
  return true;
}

void BackupIfExists(std::wostream& out,
                    std::wostream& errOut,
                    const std::filesystem::path& originalPath,
                    const wchar_t* displayName) {
  std::error_code ec;
  if (!std::filesystem::exists(originalPath, ec) || ec) {
    out << L"[跳过] 备份 " << displayName << L": 未找到 " << originalPath.wstring() << L"\n";
    return;
  }

  // Create xxx.orig only once.
  {
    std::filesystem::path origPath = originalPath;
    origPath += L".orig";
    std::error_code ecOrig;
    if (!std::filesystem::exists(origPath, ecOrig) && !ecOrig) {
      if (CopyFileWithDirs(errOut, originalPath, origPath, /*overwrite*/ false)) {
        out << L"[成功] 已创建 .orig 备份: " << origPath.wstring() << L"\n";
      } else {
        errOut << L"[警告] 创建 .orig 备份失败 (不影响继续部署).\n";
      }
    }
  }

  const std::wstring ts = NowTimestampForFilename();
  std::filesystem::path backupPath = originalPath;
  backupPath += L".bak.";
  backupPath += ts;

  // Avoid very unlikely same-second collisions.
  for (int i = 0; i < 100; ++i) {
    std::error_code ec2;
    if (!std::filesystem::exists(backupPath, ec2)) break;
    backupPath = originalPath;
    backupPath += L".bak.";
    backupPath += ts;
    backupPath += L".";
    backupPath += std::to_wstring(i + 1);
  }

  if (!CopyFileWithDirs(errOut, originalPath, backupPath, /*overwrite*/ false)) {
    errOut << L"[错误] 备份失败: " << displayName << L"\n";
    return;
  }

  out << L"[成功] 已备份 " << displayName << L": " << originalPath.wstring() << L" -> " << backupPath.wstring()
      << L"\n";
}

class MountGuard {
 public:
  MountGuard(std::wstring mountPoint, bool keepMount, std::wostream& out, std::wostream& errOut)
      : mountPoint_(std::move(mountPoint)), keepMount_(keepMount), out_(out), errOut_(errOut) {}

  bool Mount(const std::wstring& volumeName) {
    // If the mount point is already mounted, make repeated runs idempotent.
    // This commonly happens when keepMount=true and the program is executed again.
    {
      wchar_t existing[MAX_PATH + 1] = {};
      if (GetVolumeNameForVolumeMountPointW(mountPoint_.c_str(), existing, MAX_PATH)) {
        std::wstring existingVol(existing);
        if (existingVol == volumeName) {
          mounted_ = true;
          volumeName_ = volumeName;
          out_ << L"[成功] ESP 已挂载: " << volumeName_ << L" -> " << mountPoint_ << L"\n";
          return true;
        }
        errOut_ << L"[错误] 挂载点 " << mountPoint_ << L" 已被其它卷占用: " << existingVol << L"\n";
        return false;
      }
    }

    if (!SetVolumeMountPointW(mountPoint_.c_str(), volumeName.c_str())) {
      PrintLastError(errOut_,
                     L"挂载 ESP 失败(SetVolumeMountPointW)(是否以管理员运行? 盘符是否被占用?)",
                     GetLastError());
      return false;
    }
    mounted_ = true;
    volumeName_ = volumeName;
    out_ << L"[成功] 已挂载 ESP 卷 " << volumeName_ << L" 到 " << mountPoint_ << L"\n";
    return true;
  }

  bool Unmount() {
    if (!mounted_) return true;
    if (keepMount_) {
      out_ << L"[成功] 保留挂载在 " << mountPoint_ << L"\n";
      mounted_ = false;
      return true;
    }
    if (!DeleteVolumeMountPointW(mountPoint_.c_str())) {
      PrintLastError(errOut_, L"卸载盘符失败(DeleteVolumeMountPointW)", GetLastError());
      return false;
    }
    out_ << L"[成功] 已卸载 " << mountPoint_ << L"\n";
    mounted_ = false;
    return true;
  }

  ~MountGuard() {
    (void)Unmount();
  }

  const std::wstring& MountPoint() const { return mountPoint_; }

 private:
  std::wstring mountPoint_;
  bool keepMount_ = false;
  bool mounted_ = false;
  std::wstring volumeName_;
  std::wostream& out_;
  std::wostream& errOut_;
};

}  // namespace

bool UEFILocker::PreparePswKeyContentFromPasswordAscii(const std::string& pw) {
  if (pw.empty() || pw.size() > kMaxPasswordLen) return false;
  for (unsigned char c : pw) {
    if (c < 0x20 || c > 0x7E) return false;
  }
  // ASCII -> wchar_t (1:1)
  std::wstring w;
  w.reserve(pw.size());
  for (unsigned char c : pw) w.push_back(static_cast<wchar_t>(c));
  return PreparePswKeyContentFromPassword(w);
}

void UEFILocker::SetInfoTextUtf8(const std::string& text) {
  SetInfoText(Utf8ToWide(text));
}

int UEFILocker::DeployFromConfig(const DeployPaths& paths) {
  // Apply config values.
  SetXorKey(cfg.xorKey);
  if (!HasProtectEfiBytes() && !cfg.protectEfi.empty()) {
    SetProtectEfiFile(ResolveFromExeDirUtf8(cfg.protectEfi));
  }
  SetInfoTextUtf8(cfg.infoText);

  if (!PreparePswKeyContentFromPasswordAscii(cfg.password)) {
    return 2;
  }

  DeployOptions opt;
  opt.keepMount = cfg.keepMount;
  opt.paths = paths;

  // Default: silent.
  std::wostringstream nullOut;
  std::wostringstream nullErr;
  return DeployToDisk(cfg.diskNumber, NormalizeDriveLetter(cfg.mountLetter), opt, nullOut, nullErr);
}

int UEFILocker::DeployToDisk(uint32_t diskNumber,
                             wchar_t mountLetter,
                             const DeployOptions& opt,
                             std::wostream& out,
                             std::wostream& err) const {
  std::error_code ec;
  if (!HasProtectEfiBytes()) {
    if (protectEfiFile_.empty() || !std::filesystem::exists(protectEfiFile_, ec) || ec) {
      err << L"[错误] 源 EFI 不存在: " << protectEfiFile_.wstring() << L"\n";
      return 2;
    }
  }

  const DWORD disk = static_cast<DWORD>(diskNumber);

  auto esp = FindEspPartitionOnDisk(err, disk);
  if (!esp) {
    err << L"\n[失败] 未能定位 ESP 分区。\n";
    return 1;
  }

  auto vol = FindVolumeForDiskOffset(err, esp->diskNumber, esp->startingOffset);
  if (!vol) {
    err << L"\n[失败] 未能定位 ESP 对应的 Windows Volume。\n";
    return 1;
  }

  std::wstring mountPoint;
  mountPoint.push_back(mountLetter);
  mountPoint.append(L":\\");

  MountGuard mount(mountPoint, opt.keepMount, out, err);
  if (!mount.Mount(*vol)) {
    return 1;
  }

  int exitCode = 0;

  // Backup targets before overwriting (so manual recover is easier).
  BackupIfExists(out, err, JoinMountAndRel(mount.MountPoint(), opt.paths.winBootMgrRel), L"Windows bootmgfw.efi");
  BackupIfExists(out, err, JoinMountAndRel(mount.MountPoint(), opt.paths.bootEfiRel),    L"\\EFI\\BOOT\\BOOTX64.EFI");
  BackupIfExists(out, err, JoinMountAndRel(mount.MountPoint(), opt.paths.infoRootRel),   L"原 \\infor.txt");
  BackupIfExists(out, err, JoinMountAndRel(mount.MountPoint(), opt.paths.infoMsRel),     L"原 \\EFI\\Microsoft\\Boot\\infor.txt");
  BackupIfExists(out, err, JoinMountAndRel(mount.MountPoint(), opt.paths.infoBootRel),   L"原 \\EFI\\BOOT\\infor.txt");

  // Copy "protect/unlock" EFI to both locations.
  {
    std::filesystem::path dstBoot = JoinMountAndRel(mount.MountPoint(), opt.paths.bootEfiRel);
    if (HasProtectEfiBytes()) {
      if (!WriteBytesWithDirs(err, dstBoot, protectEfiData_, protectEfiSize_)) {
        exitCode = 1;
      } else {
        out << L"[成功] 已写入 EFI: " << dstBoot.wstring() << L"\n";
      }
    } else {
      if (!CopyFileWithDirs(err, protectEfiFile_, dstBoot, /*overwrite*/ true)) {
        exitCode = 1;
      } else {
        out << L"[成功] 已复制 EFI: " << protectEfiFile_.wstring() << L" -> " << dstBoot.wstring() << L"\n";
      }
    }
  }
  {
    std::filesystem::path dstWinBoot = JoinMountAndRel(mount.MountPoint(), opt.paths.winBootMgrRel);
    if (HasProtectEfiBytes()) {
      if (!WriteBytesWithDirs(err, dstWinBoot, protectEfiData_, protectEfiSize_)) {
        exitCode = 1;
      } else {
        out << L"[成功] 已写入 bootmgfw.efi: " << dstWinBoot.wstring() << L"\n";
      }
    } else {
      if (!CopyFileWithDirs(err, protectEfiFile_, dstWinBoot, /*overwrite*/ true)) {
        exitCode = 1;
      } else {
        out << L"[成功] 已复制 bootmgfw.efi: " << protectEfiFile_.wstring() << L" -> " << dstWinBoot.wstring() << L"\n";
      }
    }
  }

  // Optional info file.
  {
    const std::filesystem::path dstInfoRoot = JoinMountAndRel(mount.MountPoint(), opt.paths.infoRootRel);
    const std::filesystem::path dstInfoMs   = JoinMountAndRel(mount.MountPoint(), opt.paths.infoMsRel);
    const std::filesystem::path dstInfoBoot = JoinMountAndRel(mount.MountPoint(), opt.paths.infoBootRel);

    if (!infoText_.empty()) {
      const std::string bytes = Utf16LeWithBomBytes(infoText_);
      if (!WriteBytesWithDirs(err, dstInfoRoot, bytes.data(), bytes.size())) {
        exitCode = 1;
      } else {
        out << L"[成功] 已写入 INFO 文本: " << dstInfoRoot.wstring() << L"\n";
      }
      (void)WriteBytesWithDirs(err, dstInfoMs, bytes.data(), bytes.size());
      (void)WriteBytesWithDirs(err, dstInfoBoot, bytes.data(), bytes.size());
    } else if (!infoFile_.empty()) {
      std::error_code ecInfo;
      if (!std::filesystem::exists(infoFile_, ecInfo) || ecInfo) {
        out << L"[跳过] 未找到源 INFO 文件: " << infoFile_.wstring() << L"\n";
      } else {
        if (!CopyFileWithDirs(err, infoFile_, dstInfoRoot, /*overwrite*/ true)) {
          exitCode = 1;
        } else {
          out << L"[成功] 已复制 INFO: " << infoFile_.wstring() << L" -> " << dstInfoRoot.wstring() << L"\n";
        }
        // Best-effort extra copies (to match different boot paths).
        (void)CopyFileWithDirs(err, infoFile_, dstInfoMs, /*overwrite*/ true);
        (void)CopyFileWithDirs(err, infoFile_, dstInfoBoot, /*overwrite*/ true);
      }
    }
  }

  // Optional psw.key.
  if (!pswKeyContent_.empty()) {
    const std::filesystem::path dstKey = JoinMountAndRel(mount.MountPoint(), opt.paths.pswKeyRel);
    BackupIfExists(out, err, dstKey, L"psw.key");
    if (!WriteBytesWithDirs(err, dstKey, pswKeyContent_.data(), pswKeyContent_.size())) {
      err << L"[错误] 写入 psw.key 失败。\n";
      exitCode = 1;
    } else {
      out << L"[成功] 已写入 psw.key: " << dstKey.wstring() << L"\n";
    }
  }

  // Ensure unmount happens before returning.
  if (!mount.Unmount() && exitCode == 0) {
    // Leaving a mounted drive letter is not fatal for the deployment result itself,
    // but signal non-zero so automation can notice.
    exitCode = 1;
  }

  return exitCode;
}

int UEFILocker::MountEspOnly(uint32_t diskNumber, wchar_t mountLetter) const {
  const DWORD disk = static_cast<DWORD>(diskNumber);

  // Reuse the same locating logic as DeployToDisk, but do not print anything.
  // We intentionally ignore detailed errors here to satisfy "no prompts/output".
  std::wostringstream nullErr;
  auto esp = FindEspPartitionOnDisk(nullErr, disk);
  if (!esp) return 1;

  auto vol = FindVolumeForDiskOffset(nullErr, esp->diskNumber, esp->startingOffset);
  if (!vol) return 1;

  std::wstring mountPoint;
  mountPoint.push_back(mountLetter);
  mountPoint.append(L":\\");

  if (!SetVolumeMountPointW(mountPoint.c_str(), vol->c_str())) {
    return 1;
  }
  return 0;
}
