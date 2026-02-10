// EspDeploy：把 EFI 程序（以及可选资源文件）复制到 EFI System Partition(ESP)。
//
// 设计目标：
// - 不做命令行参数解析（所有配置都在源码顶部固定写死，改完重新编译即可）
// - 运行在 Windows 虚拟机里（需要管理员权限，否则挂载 ESP 通常会失败）
//
// 实现方式：
// - 通过 GPT 分区类型 GUID 找到 ESP 分区
// - 在系统里枚举 Volume，匹配“磁盘号 + 起始偏移”，定位到该 ESP 对应的 Volume
// - 使用 SetVolumeMountPointW 把这个 Volume 挂载到一个盘符（比如 S:\）
// - 用 CopyFileW 把 .efi / .anim 拷贝到 ESP 里
//
// 部署策略：
// - 直接用“保护/解锁 EFI”覆盖两套启动路径：
//     \EFI\BOOT\BOOTX64.EFI
//     \EFI\Microsoft\Boot\bootmgfw.efi
// - 原文件会备份为 .orig（只创建一次）以及 .bak.TIMESTAMP（每次部署都会生成）
//
// 安全性说明：
// - 本程序不会修改分区类型/属性（不会把 ESP “改成基本数据分区”）
// - 只做挂载与文件拷贝/写入 psw.key

#include <Windows.h>

#include <filesystem>
#include <iostream>
#include <optional>
#include <string>
#include <locale>

#include "UEFILocker.h"
#include "data.h"

// =========================
// 固定配置（按需修改这里）
// =========================
//
// 说明：磁盘号/盘符改为运行时由用户输入（交互式），这里不再固定写死。
// 要拷贝到 ESP 的文件（使用“相对路径”）。
// 路径相对于本 EXE 所在目录，而不是当前工作目录（避免从别的目录运行导致找不到文件）。
static const wchar_t* kSourceProtectEfiRel = L"BOOTX64.EFI";   // 我们的“保护/解锁”EFI（直接覆盖 bootmgfw.efi / BOOTX64.EFI）
static const wchar_t* kSourceInfoRel    = L"infor.txt";     // 信息文件（可为空字符串表示不拷贝）
// 拷贝完是否保留挂载（不卸载盘符）
static constexpr bool kKeepMount = false;

static void InitWideIoLocale() {
  // 避免默认 "C" locale 导致中文输出失败。
  // 注意：后续输出字符串尽量不要使用 CP936/GBK 无法表示的字符（例如中文引号、全角问号等），
  // 否则 wcout 可能在遇到无法转换的字符后进入 fail 状态，导致后续不再输出。
  try {
    std::locale::global(std::locale(""));
  } catch (...) {
    // ignore
  }
  std::wcout.imbue(std::locale());
  std::wcin.imbue(std::locale());
  std::wcerr.imbue(std::locale());
}

static bool IsRunningAsAdmin() {
  BOOL isAdmin = FALSE;
  PSID adminGroup = nullptr;
  SID_IDENTIFIER_AUTHORITY ntAuthority = SECURITY_NT_AUTHORITY;
  if (!AllocateAndInitializeSid(
          &ntAuthority,
          2,
          SECURITY_BUILTIN_DOMAIN_RID,
          DOMAIN_ALIAS_RID_ADMINS,
          0, 0, 0, 0, 0, 0,
          &adminGroup))
  {
    return false;
  }
  (void)CheckTokenMembership(nullptr, adminGroup, &isAdmin);
  FreeSid(adminGroup);
  return isAdmin != FALSE;
}

static void WaitForEnter(const wchar_t* prompt) {
  if (prompt) {
    std::wcout << prompt;
  }
  std::wcout << L"\n按回车继续...";
  std::wcout.flush();
  std::wstring dummy;
  std::getline(std::wcin, dummy);
}

static std::optional<DWORD> PromptDiskNumber() {
  while (true) {
    std::wcout << L"\n请输入目标磁盘号(对应 \\\\.\\PhysicalDriveN, 例如 0): ";
    std::wcout.flush();
    std::wstring s;
    if (!std::getline(std::wcin, s)) {
      return std::nullopt;
    }
    if (s.empty()) {
      std::wcout << L"[提示] 不能为空。\n";
      continue;
    }
    try {
      std::size_t idx = 0;
      unsigned long v = std::stoul(s, &idx, 10);
      if (idx != s.size()) {
        std::wcout << L"[提示] 输入包含非数字字符，请重试。\n";
        continue;
      }
      return static_cast<DWORD>(v);
    } catch (...) {
      std::wcout << L"[提示] 不是有效数字，请重试。\n";
    }
  }
}

static bool IsDriveLetterAvailable(wchar_t letter) {
  if (letter >= L'a' && letter <= L'z') letter = static_cast<wchar_t>(letter - L'a' + L'A');
  if (letter < L'A' || letter > L'Z') return false;
  const DWORD mask = 1u << (letter - L'A');
  return (GetLogicalDrives() & mask) == 0;
}

static std::optional<wchar_t> PromptMountLetter(wchar_t defaultLetter) {
  while (true) {
    std::wcout << L"请输入挂载盘符(A-Z, 默认 " << defaultLetter << L"): ";
    std::wcout.flush();
    std::wstring s;
    if (!std::getline(std::wcin, s)) {
      return std::nullopt;
    }
    if (s.empty()) {
      if (!IsDriveLetterAvailable(defaultLetter)) {
        std::wcout << L"[提示] 默认盘符 " << defaultLetter << L": 已被占用，请手动输入其他盘符。\n";
        continue;
      }
      return defaultLetter;
    }
    wchar_t c = s[0];
    if (c >= L'a' && c <= L'z') c = static_cast<wchar_t>(c - L'a' + L'A');
    if (c < L'A' || c > L'Z') {
      std::wcout << L"[提示] 盘符必须是 A-Z。\n";
      continue;
    }
    if (!IsDriveLetterAvailable(c)) {
      std::wcout << L"[提示] 盘符 " << c << L": 已被占用，请换一个。\n";
      continue;
    }
    return c;
  }
}

static std::filesystem::path GetExeDir() {
  wchar_t buf[MAX_PATH];
  DWORD n = GetModuleFileNameW(nullptr, buf, MAX_PATH);
  if (n == 0 || n >= MAX_PATH) {
    // 兜底：用当前目录（极少发生）
    return std::filesystem::current_path();
  }
  std::filesystem::path exePath(buf);
  return exePath.parent_path();
}

static std::filesystem::path ResolveFromExeDir(const wchar_t* relOrEmpty) {
  if (!relOrEmpty) return {};
  if (wcslen(relOrEmpty) == 0) return {};
  std::filesystem::path p(relOrEmpty);
  if (p.is_absolute()) return p;
  return GetExeDir() / p;
}

int wmain() {
  InitWideIoLocale();

  // 使用 UTF-16 控制台 API 的环境下，iostream 一般能正常显示中文；这里尽量保证不“闪退”。
  std::wcout << L"EspDeploy: 把 EFI 文件复制到 ESP(交互式)\n";
  std::wcout << L"注意: 请用管理员权限运行, 否则挂载 ESP 通常会失败.\n";
  if (!IsRunningAsAdmin()) {
    std::wcout << L"[警告] 当前进程看起来不是管理员权限, 可能会失败.\n";
  }

  std::filesystem::path srcProtect = ResolveFromExeDir(kSourceProtectEfiRel);
  {
    // Be tolerant to different build output names in the EXE directory.
    std::error_code ec;
    if (!std::filesystem::exists(srcProtect, ec) || ec) {
      const std::filesystem::path alt1 = ResolveFromExeDir(L"InfoPrompt.efi");
      const std::filesystem::path alt2 = ResolveFromExeDir(L"BOOTX64.EFI");
      if (std::filesystem::exists(alt1, ec) && !ec) {
        srcProtect = alt1;
      } else if (std::filesystem::exists(alt2, ec) && !ec) {
        srcProtect = alt2;
      }
    }
  }
  const std::filesystem::path srcInfo = ResolveFromExeDir(kSourceInfoRel);
  std::wcout << L"EXE 目录: " << GetExeDir().wstring() << L"\n";
  std::wcout << L"源 EFI : " << srcProtect.wstring() << L"\n";
  if (!srcInfo.empty()) std::wcout << L"源 INFO: " << srcInfo.wstring() << L"\n";

  // Optional: create "lock" artifacts (password + info file + psw.key).
  UEFILocker locker;
  // Prefer external EFI file if present; otherwise fallback to embedded BOOTX64.EFI.
  {
    std::error_code ec;
    if (!srcProtect.empty() && std::filesystem::exists(srcProtect, ec) && !ec) {
      locker.SetProtectEfiFile(srcProtect);
    } else {
      locker.SetProtectEfiBytes(kEmbeddedBootx64Efi, kEmbeddedBootx64EfiSize);
    }
  }
  locker.SetInfoFile(srcInfo);

  // psw.key content (obfuscated only; keep plaintext as short-lived as possible).
  locker.ClearPswKeyContent();
  std::wcout << L"\n是否写入/更新 psw.key? (y/n, 默认 y): ";
  std::wcout.flush();
  {
    std::wstring ans;
    if (!std::getline(std::wcin, ans)) return 2;
    bool doWriteKey = true;
    if (!ans.empty() && (ans[0] == L'n' || ans[0] == L'N')) doWriteKey = false;
    if (doWriteKey) {
      if (!locker.PreparePswKeyContentFromPrompt(std::wcin, std::wcout)) {
        std::wcout << L"[Skip] Password not set; psw.key will not be written.\n";
      }
    }
  }

  UEFILocker::DeployOptions deployOpt;
  deployOpt.keepMount = kKeepMount;

  // 主循环：失败不立即退出，便于看清错误信息并重试。
  while (true) {
    auto diskOpt = PromptDiskNumber();
    if (!diskOpt) {
      WaitForEnter(L"\n输入结束。");
      return 2;
    }

    auto letterOpt = PromptMountLetter(L'S');
    if (!letterOpt) {
      WaitForEnter(L"\n输入结束。");
      return 2;
    }

    const DWORD   diskNumber = *diskOpt;
    const wchar_t mountLetter = *letterOpt;

    std::wcout << L"\n目标磁盘: PhysicalDrive" << diskNumber << L"\n";
    std::wcout << L"挂载盘符: " << mountLetter << L":\\\n";

    int exitCode = locker.DeployToDisk(static_cast<uint32_t>(diskNumber),
                                       mountLetter,
                                       deployOpt,
                                       std::wcout,
                                       std::wcerr);
    if (exitCode == 0) {
      WaitForEnter(L"\n部署完成。");
    } else {
      std::wcout << L"\n[失败] 部署未完全成功 (exitCode=" << exitCode << L")。\n";
      WaitForEnter(L"\n部署完成，但有错误。");
    }

    // 询问是否继续
    std::wcout << L"\n是否继续部署其他磁盘? (y/n): ";
    std::wcout.flush();
    std::wstring ans;
    if (!std::getline(std::wcin, ans)) {
      return exitCode;
    }
    if (!ans.empty() && (ans[0] == L'y' || ans[0] == L'Y')) {
      continue;
    }
    return exitCode;
  }
}
