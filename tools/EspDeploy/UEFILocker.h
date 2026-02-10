// UEFILocker.h
// 用于生成“UEFI 锁/保护程序”所需文件的辅助类：
// - psw.key 内容（XOR1:HEX）
// - 可选的信息文件路径（infor.txt）
//
// 注意：这里的 XOR 只是“混淆”，不是强加密。
// 目的只是避免明文存储与一些明显的绕过（例如 NUL 注入），并不能抵抗有权限的攻击者。

#pragma once

#include <algorithm>
#include <cstdint>
#include <cstddef>
#include <filesystem>
#include <iosfwd>
#include <string>
#include <vector>

class UEFILocker {
 public:
  static constexpr size_t kMaxPasswordLen = 32;

  // 面向“载入程序/配置式”的最简配置：全部用 std::string（UTF-8）。
  // 你可以直接修改 locker.cfg 里的字段，然后调用 DeployFromConfig()。
  struct Config {
    uint32_t diskNumber = 0;      // PhysicalDriveN
    char mountLetter = 'S';       // A-Z
    bool keepMount = true;        // 是否保留挂载盘符

    // 源 EFI 文件：支持绝对路径；若是相对路径，则以 EXE 同目录为基准解析。
    std::string protectEfi = "BOOTX64.EFI";

    // 密码：ASCII 可见字符，长度 1-32（UEFI 端键盘输入一致）。
    std::string password = "123456";

    // UEFI 端显示信息（infor.txt 内容，UTF-8；内部会写成 UTF-16LE(BOM) 以支持中文）。
    std::string infoText =
        "此设备已上锁。\r\n"
        "请输入密码以恢复启动文件。\r\n";

    // XOR 混淆密钥（需与 UEFI 端保持一致）。
    std::string xorKey = "yunchenqwq";
  };

  struct DeployPaths {
    // Paths are relative to ESP root. They may start with "\\" or "/".
    std::wstring bootEfiRel      = L"\\EFI\\BOOT\\BOOTX64.EFI";
    std::wstring winBootMgrRel   = L"\\EFI\\Microsoft\\Boot\\bootmgfw.efi";
    std::wstring infoRootRel     = L"\\infor.txt";
    std::wstring infoMsRel       = L"\\EFI\\Microsoft\\Boot\\infor.txt";
    std::wstring infoBootRel     = L"\\EFI\\BOOT\\infor.txt";
    std::wstring pswKeyRel       = L"\\EFI\\BOOT\\psw.key";
  };

  struct DeployOptions {
    DeployPaths paths{};
    bool keepMount = false;  // Keep the drive letter mounted after deployment.
  };

  // 默认加密密钥（需与 UEFI 端 InfoPrompt.c 保持一致）
  UEFILocker() : xorKey_("yunchenqwq") {}
  explicit UEFILocker(std::string xorKey) : xorKey_(std::move(xorKey)) {}

  // 你可以直接改 cfg，然后调用 DeployFromConfig()。
  Config cfg;

  void SetXorKey(std::string xorKey) { xorKey_ = std::move(xorKey); }

  void SetProtectEfiFile(std::filesystem::path efiFile) { protectEfiFile_ = std::move(efiFile); }
  void SetProtectEfiFile(const wchar_t* efiFile) { SetProtectEfiFile(std::filesystem::path(efiFile ? efiFile : L"")); }
  const std::filesystem::path& ProtectEfiFile() const { return protectEfiFile_; }

  void SetProtectEfiBytes(const void* data, std::size_t size) {
    protectEfiData_ = static_cast<const std::uint8_t*>(data);
    protectEfiSize_ = size;
  }
  bool HasProtectEfiBytes() const { return protectEfiData_ != nullptr && protectEfiSize_ != 0; }

  void SetInfoFile(std::filesystem::path infoFile) { infoFile_ = std::move(infoFile); }
  void SetInfoFile(const wchar_t* infoFile) { SetInfoFile(std::filesystem::path(infoFile ? infoFile : L"")); }
  const std::filesystem::path& InfoFile() const { return infoFile_; }

  // 直接设置 infor.txt 的显示内容（推荐用于包含中文：会以 UTF-16LE(BOM) 写入）。
  // 若设置了 InfoText，则部署时优先写入 InfoText，而不是拷贝 InfoFile。
  void SetInfoText(std::wstring text) { infoText_ = std::move(text); }
  void SetInfoText(const wchar_t* text) { SetInfoText(std::wstring(text ? text : L"")); }
  void ClearInfoText() { infoText_.clear(); }
  bool HasInfoText() const { return !infoText_.empty(); }
  const std::wstring& InfoText() const { return infoText_; }

  void ClearPswKeyContent() { pswKeyContent_.clear(); }
  bool HasPswKeyContent() const { return !pswKeyContent_.empty(); }
  const std::string& PswKeyContent() const { return pswKeyContent_; }

  bool HasPassword() const { return !password_.empty(); }
  const std::wstring& Password() const { return password_; }

  static bool IsValidPassword(const std::wstring& pw) {
    if (pw.empty() || pw.size() > kMaxPasswordLen) return false;
    // 与 UEFI 端键盘输入保持一致：只允许 ASCII 可见字符。
    for (wchar_t wc : pw) {
      if (wc < 0x20 || wc > 0x7E) return false;
    }
    return true;
  }

  bool SetPassword(const std::wstring& pw) {
    if (!IsValidPassword(pw)) return false;
    password_ = pw;
    return true;
  }

  // 交互式输入密码 + 生成 psw.key 内容，并缓存到对象内（pswKeyContent_）。
  // 成功返回 true；用户输入空行视为跳过，返回 false。
  template <typename WIn, typename WOut>
  bool PreparePswKeyContentFromPrompt(WIn& in, WOut& out) {
    std::string content;
    if (!PromptPassword(in, out)) {
      // password_ may contain user input; wipe it anyway.
      SecureWipe(password_);
      return false;
    }
    if (!BuildPswKeyContent(content)) {
      SecureWipe(password_);
      return false;
    }
    // Keep only obfuscated content; wipe plaintext asap.
    SecureWipe(password_);
    pswKeyContent_ = std::move(content);
    return true;
  }

  // 用固定密码直接生成 psw.key 内容，并缓存到对象内（pswKeyContent_）。
  // 适合做最小示例/自动化；会尽快擦除明文密码。
  bool PreparePswKeyContentFromPassword(const std::wstring& pw) {
    ClearPswKeyContent();
    if (!SetPassword(pw)) return false;
    std::string content;
    const bool ok = BuildPswKeyContent(content);
    SecureWipe(password_);
    if (!ok) return false;
    pswKeyContent_ = std::move(content);
    return true;
  }
  bool PreparePswKeyContentFromPassword(const wchar_t* pw) {
    return PreparePswKeyContentFromPassword(std::wstring(pw ? pw : L""));
  }

  // 更简单的版本：直接传 ASCII 密码字符串（不需要 wchar_t / 转码）。
  bool PreparePswKeyContentFromPasswordAscii(const std::string& pw);

  // 更简单的版本：直接传 UTF-8 提示信息字符串（内部写为 UTF-16LE(BOM)）。
  void SetInfoTextUtf8(const std::string& text);

  // 从 cfg 一键部署（默认不输出日志）。
  int DeployFromConfig(const DeployPaths& paths = DeployPaths{});

  // 部署到指定磁盘的 ESP，并根据当前对象状态写入/更新：
  // - 保护 EFI（ProtectEfiFile）
  // - 可选 infor.txt（InfoFile）
  // - 可选 psw.key（PswKeyContent）
  //
  // 返回：0 成功；非 0 失败（错误细节写到 err）。
  int DeployToDisk(uint32_t diskNumber,
                   wchar_t mountLetter,
                   const DeployOptions& opt,
                   std::wostream& out,
                   std::wostream& err) const;

  // 仅挂载指定磁盘的 ESP 到盘符（不做任何文件拷贝/写入），并保持挂载。
  // 返回：0 成功；非 0 失败（不输出任何提示信息）。
  int MountEspOnly(uint32_t diskNumber, wchar_t mountLetter) const;

  // 交互式两次确认输入。返回：
  // - true  : 密码已设置
  // - false : 用户选择跳过（空输入）或输入流结束
  template <typename WIn, typename WOut>
  bool PromptPassword(WIn& in, WOut& out) {
    while (true) {
      std::wstring p1, p2;
      out << L"Enter password (ASCII printable, 1-32 chars; echoed): ";
      out.flush();
      if (!std::getline(in, p1)) return false;
      if (p1.empty()) {
        // 空输入视为“跳过设置密码”
        SecureWipe(p1);
        return false;
      }
      out << L"Confirm password: ";
      out.flush();
      if (!std::getline(in, p2)) return false;
      if (p1 != p2) {
        out << L"[Hint] Passwords do not match. Try again.\n";
        SecureWipe(p1);
        SecureWipe(p2);
        continue;
      }
      if (!SetPassword(p1)) {
        out << L"[Hint] Invalid password (ASCII printable only, length 1-32).\n";
        SecureWipe(p1);
        SecureWipe(p2);
        continue;
      }
      SecureWipe(p1);
      SecureWipe(p2);
      return true;
    }
  }

  // 生成 psw.key 内容：
  //   XOR1:<HEX>\r\n
  bool BuildPswKeyContent(std::string& out) const {
    if (!HasPassword()) return false;
    if (xorKey_.empty()) return false;

    std::vector<std::uint8_t> bytes;
    bytes.reserve(password_.size());
    for (wchar_t wc : password_) {
      bytes.push_back(static_cast<std::uint8_t>(wc & 0xFF));
    }

    const size_t keyLen = xorKey_.size();
    for (size_t i = 0; i < bytes.size(); ++i) {
      bytes[i] = static_cast<std::uint8_t>(bytes[i] ^ static_cast<std::uint8_t>(xorKey_[i % keyLen]));
    }

    static constexpr char kHex[] = "0123456789ABCDEF";
    std::string hex;
    hex.reserve(bytes.size() * 2);
    for (std::uint8_t b : bytes) {
      hex.push_back(kHex[(b >> 4) & 0xF]);
      hex.push_back(kHex[b & 0xF]);
    }

    out.clear();
    out.append(kHeader);
    out.append(hex);
    out.append("\r\n");
    return true;
  }

 private:
  static constexpr const char* kHeader = "XOR1:";

  static void SecureWipe(std::wstring& s) {
    std::fill(s.begin(), s.end(), L'\0');
    s.clear();
  }

  std::string xorKey_;
  std::wstring password_;
  std::filesystem::path infoFile_;
  std::wstring infoText_;
  std::filesystem::path protectEfiFile_;
  const std::uint8_t* protectEfiData_ = nullptr;
  std::size_t protectEfiSize_ = 0;
  std::string pswKeyContent_;
};
