// example.cpp
// 最简“载入程序”：只改配置，不需要 wchar_t / 类型转换。
// - locker 内部会处理 UTF-8 转换、路径解析、挂载/部署。
// - 默认不输出任何日志，靠返回码判断成功失败。

#include <Windows.h>

#include <cstdio>

#include "data.h"
#include "UEFILocker.h"

int main() {
  // Make UTF-8 narrow strings show correctly in Windows console (best effort).
  (void)SetConsoleOutputCP(CP_UTF8);
  (void)SetConsoleCP(CP_UTF8);

  UEFILocker locker;

  // ---- 配置：只改这里 ----
  locker.cfg.diskNumber = 0;          // PhysicalDrive0/1/2...你的磁盘驱动器号
  locker.cfg.mountLetter = 'S';       // A-Z
  locker.cfg.keepMount = true;        // true=保留挂载
  // 把 C:\\edk2\\release\\BOOTX64.EFI 直接内置到程序里，不需要额外拷贝文件。
  locker.SetProtectEfiBytes(kEmbeddedBootx64Efi, kEmbeddedBootx64EfiSize);
  locker.cfg.protectEfi.clear();      // 清空表示不走“从文件加载”路径
  locker.cfg.password = "123456";     // ASCII 可见字符，1-32 位
  // 来自 C:\edk2\release\infor.txt 的内容（直接内置，UTF-8）。
  locker.cfg.infoText = R"INFO(
  .-'      '-.            =====================================
 /            \                    SYSTEM COMPROMISED
|              |          =====================================
|,  .-.  .-.  ,|
| )(__/  \__)( |          This is a boot recovery test program.
|/     /\     \|          If you see this screen:
(_     ^^     _)           - Enter the password to recover boot files
 \__|IIIIII|__/            - The system will reboot automatically
  | \IIIIII/ |             - Press ESC to cancel
  \          /
   `--------`

  Notes:
  - Do not power off during recovery.
  - Keep your password file safe (EFI\BOOT\psw.key).

)INFO";
  // locker.cfg.xorKey = "yunchenqwq"; // 不改默认就不用写

  const int rc = locker.DeployFromConfig();
  std::printf("DeployFromConfig() 返回码 = %d\n", rc);
  switch (rc) {
    case 0:
      std::puts("成功");
      break;
    case 1:
      std::puts("失败：部署失败（挂载/复制/写入/卸载 过程中出错）。");
      break;
    case 2:
      std::puts("失败：配置无效（密码不合法，或源 EFI 缺失）。");
      break;
    default:
      std::puts("失败：未知错误。");
      break;
  }
  return rc;
}
