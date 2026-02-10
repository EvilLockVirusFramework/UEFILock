EspDeploy (Windows)

说明
- 这是一个“固定配置”的小工具：不提供命令行参数。
- 需要修改配置时，请直接改源码 `EspDeploy.cpp` 顶部的固定配置区，然后重新编译即可。

它做什么
- 在指定磁盘上找到 EFI System Partition(ESP)（GPT 分区类型 GUID = ESP GUID）
- 把这个 ESP 对应的 Windows Volume 挂载到一个盘符（例如 S:\）
- 把一个 EFI 文件复制到 ESP（默认 \EFI\BOOT\BOOTX64.EFI）
- 可选：复制一个资源文件（默认 \bg2.anim）
- 默认复制完成后自动卸载盘符（可在源码里配置保留挂载）

编译（Visual Studio 2022）
1) 打开 “x64 Native Tools Command Prompt for VS 2022”
2) cd C:\edk2\tools\EspDeploy
3) cmake -S . -B build -G "Visual Studio 17 2022" -A x64
4) cmake --build build --config Release
输出：
  build\Release\EspDeploy.exe

运行（必须管理员权限）
  build\Release\EspDeploy.exe

安全性
- 本工具不会修改分区类型/属性，只做挂载与文件拷贝。
- 如果盘符已占用，请修改源码里的 k挂载盘符。
