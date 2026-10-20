# Build the player-facing release zip from the Vulkan build. Contains the
# launcher, the recompiled game, the runtime DLLs, port config, the file
# manifest imports are checked against, the changelog and the VC++ runtime
# (app-local, as Microsoft's redistribution terms allow). No game data of any
# kind.
#
# usage: $env:FM2_ONLINE_SERVER='host:port'; .\build-vulkan.bat
#        tools\package_release.ps1 [-Version x.y.z] [-SdkThirdParty <dir>]
# The version defaults to the newest "## x.y.z" heading in CHANGELOG.md.
param([string]$Version, [string]$Preset = "win-amd64-vulkan", [string]$SdkThirdParty)
$ErrorActionPreference = "Stop"
$root = Split-Path $PSScriptRoot
if (-not $Version) {
  $Version = (Select-String -Path (Join-Path $root "CHANGELOG.md") -Pattern '^## (\d+\.\d+\.\d+)' |
    Select-Object -First 1).Matches[0].Groups[1].Value
}
# The patched Vulkan SDK the build uses (see build-vulkan.bat); its license files
# are appended to the notices.
if (-not $SdkThirdParty) {
  $SdkThirdParty = Join-Path $root "..\..\fm4-recomp\work\vulkan-trial\shiftglue-sdk\thirdparty"
}
$build = Join-Path $root "out\build\$Preset"
$name = "Forza2-Native-$Version-windows-x64"
$dist = Join-Path $root "dist"
$stage = Join-Path $dist $name

$files = "Forza2Launcher.exe", "forza2.exe", "rexruntime.dll", "rexgpu-fh1.dll", "forza2.toml", "game_files.sha256"
foreach ($f in $files) {
  if (-not (Test-Path (Join-Path $build $f))) { throw "missing $f in $build - build first (build-vulkan.bat)" }
}

Remove-Item $stage -Recurse -Force -ErrorAction SilentlyContinue
New-Item -ItemType Directory -Force $stage | Out-Null
foreach ($f in $files) { Copy-Item (Join-Path $build $f) $stage }
# Box art, only when a local launcher\data\cover.png exists (not in the repository).
$cover = Join-Path $root "launcher\data\cover.png"
if (Test-Path $cover) { Copy-Item $cover $stage }

# VC++ 2015-2022 runtime, app-local.
$vs = & "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe" -products * -latest -property installationPath
$crt = Get-ChildItem "$vs\VC\Redist\MSVC\*\x64\Microsoft.VC14*.CRT" -Directory | Sort-Object FullName | Select-Object -Last 1
if (-not $crt) { throw "VC++ redistributable folder not found under $vs" }
Copy-Item "$crt\*.dll" $stage

Copy-Item (Join-Path $root "docs\PLAYERS.md") (Join-Path $stage "LEEME.md")
Copy-Item (Join-Path $root "CHANGELOG.md") $stage
# Licences of bundled third-party code (BSD/MIT/zlib/LGPL require them with
# binaries), plus the full texts of the libraries inside the Vulkan runtime.
$notices = [IO.File]::ReadAllText((Join-Path $root "docs\THIRD_PARTY_NOTICES.txt"))
$licenses = [ordered]@{
  "glslang (rexgpu-fh1.dll)" = "glslang\LICENSE.txt"
  "Vulkan Memory Allocator (rexgpu-fh1.dll)" = "vulkan-memory-allocator\LICENSE.txt"
  "spdlog (rexruntime.dll)" = "spdlog\LICENSE"
  "{fmt} (rexruntime.dll)" = "fmt\LICENSE"
}
foreach ($entry in $licenses.GetEnumerator()) {
  $text = [IO.File]::ReadAllText((Join-Path $SdkThirdParty $entry.Value))
  $notices += "`n" + ("-" * 72) + "`n$($entry.Key)`n" + ("-" * 72) + "`n$($text.TrimEnd())`n"
}
[IO.File]::WriteAllText((Join-Path $stage "THIRD_PARTY_NOTICES.txt"), $notices.Replace("`r`n", "`n"),
  (New-Object Text.UTF8Encoding $false))

# Guard: nothing from the game may end up in the package.
$leak = Get-ChildItem $stage -Recurse -File | Where-Object { $_.Extension -in ".xex", ".zip", ".iso", ".xds", ".fsb", ".bik" }
if ($leak) { throw "game data in package: $($leak.Name -join ', ')" }

$zip = Join-Path $dist "$name.zip"
Remove-Item $zip -ErrorAction SilentlyContinue
Compress-Archive -Path "$stage\*" -DestinationPath $zip
$hash = (Get-FileHash $zip -Algorithm SHA256).Hash.ToLower()
"$hash  $name.zip" | Set-Content -Encoding ascii "$zip.sha256"
"package: $zip"
"sha256:  $hash"
Get-ChildItem $stage | Select-Object Name, Length | Format-Table -AutoSize | Out-String
