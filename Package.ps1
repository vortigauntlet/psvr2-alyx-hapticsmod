# Builds and assembles a self-contained release folder, then zips it.
#
#   .\Package.ps1
#
# Produces .\release\ containing everything a user needs - one executable, one
# optional config file, and three double-clickable shortcuts - and
# .\dist\psvr2-alyx-haptics-<version>.zip ready to upload.
#
# There is nothing to inject and no Valve binary is modified. One line is added
# to game\hlvr\cfg\skill_manifest.cfg, which Uninstall.bat removes.

$ErrorActionPreference = "Stop"
$root = $PSScriptRoot

# CMake is frequently NOT on PATH on a machine that only has the Visual Studio
# Build Tools, which is the common case for someone building this for the first
# time. Look where it actually lives before giving up.
$candidates = @(
    "C:\Program Files\CMake\bin\cmake.exe",
    "C:\Program Files (x86)\Microsoft Visual Studio\18\BuildTools\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe",
    "C:\Program Files\Microsoft Visual Studio\2022\Community\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe",
    "C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe"
)
$cmake = $null
foreach ($c in $candidates) { if (Test-Path $c) { $cmake = $c; break } }
if (-not $cmake) {
    $found = Get-Command cmake -ErrorAction SilentlyContinue
    if ($found) { $cmake = $found.Source }
}
if (-not $cmake) {
    throw "CMake not found. Install CMake, or Visual Studio Build Tools with the C++ CMake component."
}
Write-Host "CMake: $cmake" -ForegroundColor DarkGray

Write-Host "Configuring..." -ForegroundColor Cyan
& $cmake -S "$root\app" -B "$root\app\build" -A x64 | Out-Null
if ($LASTEXITCODE -ne 0) { throw "Configure failed." }

Write-Host "Building..." -ForegroundColor Cyan
& $cmake --build "$root\app\build" --config Release | Out-Null
if ($LASTEXITCODE -ne 0) { throw "Build failed." }

$exe = "$root\app\build\Release\psvr2_alyx_haptics.exe"
if (-not (Test-Path $exe)) { throw "Build produced no executable." }

# Take the version from the binary itself rather than keeping a second copy of
# it here, which would drift.
$versionLine = & $exe --version | Select-Object -First 1
if ($versionLine -match '(\d+\.\d+)') { $version = $Matches[1] } else { $version = "dev" }
Write-Host "Version: $version" -ForegroundColor DarkGray

$release = "$root\release"

# Files the USER creates in release\ and would be furious to lose. A blanket
# Remove-Item here used to delete a recorded --record session and any
# hand-tuned haptic_profiles.cfg - a genuinely destructive thing for a build
# script to do, and it happened to a real recording mid-session. Preserve them
# across the rebuild.
$preserve = @("*.log", "haptic_profiles.cfg", "*.session")
$stash = Join-Path ([System.IO.Path]::GetTempPath()) ("psvr2_pkg_" + [guid]::NewGuid().ToString("N"))
$saved = @()
if (Test-Path $release) {
    New-Item -ItemType Directory -Path $stash | Out-Null
    foreach ($pattern in $preserve) {
        foreach ($f in (Get-ChildItem -Path $release -Filter $pattern -File -ErrorAction SilentlyContinue)) {
            Copy-Item $f.FullName -Destination $stash
            $saved += $f.Name
        }
    }
    Remove-Item -Recurse -Force $release
}
New-Item -ItemType Directory -Path $release | Out-Null

Copy-Item $exe $release
Copy-Item "$root\config\psvr2_haptics.cfg" $release
Copy-Item "$root\README.md" "$release\README.md"
Copy-Item "$root\LICENSE" "$release\LICENSE"
Copy-Item "$root\THIRD_PARTY_NOTICES.txt" "$release\THIRD_PARTY_NOTICES.txt"

# Ship the docs too. VERIFIED.md in particular travels with the build on
# purpose: it is the record of what has actually been proven and at what level,
# and a release that arrives without it invites exactly the confident-sounding
# claims this project exists to avoid.
New-Item -ItemType Directory -Path "$release\docs" | Out-Null
Copy-Item "$root\docs\*.md" "$release\docs\"

# Plain ASCII, no BOM, so the shell never chokes on them.
$utf8 = New-Object System.Text.UTF8Encoding($false)

[System.IO.File]::WriteAllText("$release\Start Haptics.bat", @"
@echo off
title PSVR2 Alyx Haptics
cd /d "%~dp0"
psvr2_alyx_haptics.exe --launch
echo.
pause
"@, $utf8)

[System.IO.File]::WriteAllText("$release\Test Haptics.bat", @"
@echo off
title PSVR2 Alyx Haptics - self test
cd /d "%~dp0"
echo Hold both Sense controllers. Each signature is announced before it plays.
echo.
psvr2_alyx_haptics.exe --test
echo.
pause
"@, $utf8)

[System.IO.File]::WriteAllText("$release\Uninstall.bat", @"
@echo off
title PSVR2 Alyx Haptics - uninstall
cd /d "%~dp0"
psvr2_alyx_haptics.exe --uninstall
echo.
pause
"@, $utf8)

# No headset needed. This is the one a sceptical downloader can run to see what
# the mod actually produces before trusting it with their game folder.
[System.IO.File]::WriteAllText("$release\Measure Haptics (no headset).bat", @"
@echo off
title PSVR2 Alyx Haptics - offline measurement
cd /d "%~dp0"
echo Rendering every tactile signature offline. No headset required.
echo.
psvr2_alyx_haptics.exe --analyze
echo.
pause
"@, $utf8)

# Half-Life 2 VR. Shipped separately rather than as a switch on the Alyx
# launcher, because the two need different things installed and a user who owns
# one of the games should never be told to run something for the other.
#
# The measurement one is listed first on purpose: the Half-Life 2 VR game side
# needs a plugin that has to be built from the Source SDK, so until that exists
# the offline path is the only one that does anything, and it does a great deal.
[System.IO.File]::WriteAllText("$release\Measure HL2VR Haptics (no headset).bat", @"
@echo off
title PSVR2 Haptics - Half-Life 2 VR offline measurement
cd /d "%~dp0"
echo Rendering every Half-Life 2 VR signature offline.
echo No headset and no game required.
echo.
psvr2_alyx_haptics.exe --game hl2vr --verify
echo.
psvr2_alyx_haptics.exe --game hl2vr --analyze
echo.
pause
"@, $utf8)

[System.IO.File]::WriteAllText("$release\Start HL2VR Haptics.bat", @"
@echo off
title PSVR2 Haptics - Half-Life 2 VR
cd /d "%~dp0"
echo Half-Life 2 VR needs a game-side plugin that is built separately.
echo If events never arrive, read docs\HL2VR.md first.
echo.
psvr2_alyx_haptics.exe --game hl2vr --launch
echo.
pause
"@, $utf8)

# --- zip + checksum ------------------------------------------------------
# An unsigned executable downloaded from a forum deserves a checksum people can
# actually verify, and a zip is what both Reddit and a GitHub release want.
$dist = "$root\dist"
if (-not (Test-Path $dist)) { New-Item -ItemType Directory -Path $dist | Out-Null }
$zip = "$dist\psvr2-alyx-haptics-$version.zip"
if (Test-Path $zip) { Remove-Item -Force $zip }
Compress-Archive -Path "$release\*" -DestinationPath $zip -CompressionLevel Optimal

$hash = (Get-FileHash $zip -Algorithm SHA256).Hash
$exeHash = (Get-FileHash "$release\psvr2_alyx_haptics.exe" -Algorithm SHA256).Hash
[System.IO.File]::WriteAllText("$dist\psvr2-alyx-haptics-$version.sha256",
    "$hash  psvr2-alyx-haptics-$version.zip`n$exeHash  psvr2_alyx_haptics.exe`n", $utf8)

if ($saved.Count -gt 0) {
    Copy-Item (Join-Path $stash "*") -Destination $release
    Write-Host ("Preserved: " + ($saved -join ", ")) -ForegroundColor DarkGray
}
if (Test-Path $stash) { Remove-Item -Recurse -Force $stash }

Write-Host ""
Write-Host "Release ready: $release" -ForegroundColor Green
Get-ChildItem $release | Select-Object Name, @{n = "KB"; e = { [math]::Round($_.Length / 1KB, 1) } } | Format-Table -AutoSize | Out-Host
Write-Host "Zip:    $zip" -ForegroundColor Green
Write-Host "SHA256: $hash" -ForegroundColor Green
