@echo off
setlocal

REM Boot via fallback path \EFI\BOOT\BOOTX64.EFI from a host dir exposed as FAT (vvfat).
REM BOOTX64.EFI is your chainloader; it should load GRUBX64.EFI (our app).
REM bg2.anim is read by our app from the same FAT volume.

set QEMU_EXE=C:\Program Files\qemu\qemu-system-x86_64.exe
set OVMF_CODE=C:\edk2\edk2\Build\OvmfX64\DEBUG_VS2022\FV\OVMF_CODE.fd
set OVMF_VARS_TEMPLATE=C:\edk2\edk2\Build\OvmfX64\DEBUG_VS2022\FV\OVMF_VARS.fd
set BOOTDIR=C:\edk2\run\gifboot
set VARS_COPY=C:\edk2\run\OVMF_VARS.fd

if not exist "%QEMU_EXE%" (
  echo QEMU not found: "%QEMU_EXE%"
  exit /b 1
)
if not exist "%OVMF_CODE%" (
  echo OVMF_CODE not found: "%OVMF_CODE%"
  exit /b 1
)
if not exist "%OVMF_VARS_TEMPLATE%" (
  echo OVMF_VARS not found: "%OVMF_VARS_TEMPLATE%"
  exit /b 1
)
if not exist "%BOOTDIR%\EFI\BOOT\BOOTX64.EFI" (
  echo BOOTX64.EFI not found in: "%BOOTDIR%\EFI\BOOT\BOOTX64.EFI"
  exit /b 1
)
if not exist "%BOOTDIR%\EFI\BOOT\GRUBX64.EFI" (
  echo GRUBX64.EFI not found in: "%BOOTDIR%\EFI\BOOT\GRUBX64.EFI"
  exit /b 1
)
if not exist "%BOOTDIR%\bg2.anim" (
  echo bg2.anim not found in: "%BOOTDIR%\bg2.anim"
  echo Converting C:\prog\bg2.gif ^> bg2.anim ...
  py -3 C:\edk2\edk2\GifAnimPkg\Tools\gif2anim.py C:\prog\bg2.gif "%BOOTDIR%\bg2.anim" --fps 12 --max-width 320 --max-height 480
  if errorlevel 1 exit /b 1
)

copy /y "%OVMF_VARS_TEMPLATE%" "%VARS_COPY%" >nul

REM vvfat: expose BOOTDIR as a FAT block device to the guest.
REM Use a normal display window; serial goes to a console window if needed.
"%QEMU_EXE%" ^
  -machine q35 ^
  -m 1024 ^
  -smp 2 ^
  -drive if=pflash,format=raw,readonly=on,file="%OVMF_CODE%" ^
  -drive if=pflash,format=raw,file="%VARS_COPY%" ^
  -drive if=virtio,format=raw,file=fat:rw:c:/edk2/run/gifboot ^
  -net none

endlocal
