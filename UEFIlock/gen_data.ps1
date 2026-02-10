param(
  [string]$InputEfi = "C:\\edk2\\release\\BOOTX64.EFI",
  [string]$OutDir = (Split-Path -Parent $MyInvocation.MyCommand.Path)
)

Set-StrictMode -Version Latest
$ErrorActionPreference = "Stop"

if (-not (Test-Path -LiteralPath $InputEfi)) {
  throw "Input file not found: $InputEfi"
}

$bytes = [System.IO.File]::ReadAllBytes($InputEfi)

$hPath = Join-Path $OutDir "data.h"
$cppPath = Join-Path $OutDir "data.cpp"

$h = @"
// data.h
// Auto-generated: embedded BOOTX64.EFI from $InputEfi
#pragma once

#include <cstddef>
#include <cstdint>

extern const std::uint8_t kEmbeddedBootx64Efi[];
extern const std::size_t kEmbeddedBootx64EfiSize;
"@

$sb = New-Object System.Text.StringBuilder
[void]$sb.AppendLine("// data.cpp")
[void]$sb.AppendLine("// Auto-generated: embedded BOOTX64.EFI from $InputEfi")
[void]$sb.AppendLine("")
[void]$sb.AppendLine('#include "data.h"')
[void]$sb.AppendLine("")
[void]$sb.AppendLine("const std::uint8_t kEmbeddedBootx64Efi[] = {")

for ($i = 0; $i -lt $bytes.Length; $i += 16) {
  $end = [Math]::Min($i + 16, $bytes.Length)
  $line = New-Object System.Text.StringBuilder
  [void]$line.Append("  ")
  for ($j = $i; $j -lt $end; $j++) {
    if ($j -ne $i) { [void]$line.Append(", ") }
    [void]$line.AppendFormat("0x{0:X2}", $bytes[$j])
  }
  if ($end -ne $bytes.Length) { [void]$line.Append(",") }
  [void]$sb.AppendLine($line.ToString())
}

[void]$sb.AppendLine("};")
[void]$sb.AppendLine("")
[void]$sb.AppendLine("const std::size_t kEmbeddedBootx64EfiSize = sizeof(kEmbeddedBootx64Efi);")

$utf8NoBom = New-Object System.Text.UTF8Encoding($false)
[System.IO.File]::WriteAllText($hPath, $h, $utf8NoBom)
[System.IO.File]::WriteAllText($cppPath, $sb.ToString(), $utf8NoBom)

Write-Host ("Wrote {0} and {1} ({2} bytes)" -f $hPath, $cppPath, $bytes.Length)

