<# : batch part (runs the PowerShell below; double-click it or run: build.bat [options])
@echo off
setlocal
set "WF_BUILD_ARGS=%*"
set "WF_BUILD_FILE=%~f0"
powershell -NoProfile -ExecutionPolicy Bypass -Command "$f=$env:WF_BUILD_FILE; $sb=[scriptblock]::Create((Get-Content -LiteralPath $f -Raw)); Invoke-Expression ('& $sb ' + $env:WF_BUILD_ARGS)"
set code=%errorlevel%
if "%WF_BUILD_NOPAUSE%"=="" pause
exit /b %code%
#>
param([string]$Game = '', [switch]$Debug, [switch]$Package, [switch]$SkipTests, [switch]$NoInstall, [switch]$NoRando)
$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $f
Set-Location $root
$preset = if ($Debug) { 'debug' } else { 'release' }
$kRpxSha1 = 'bdcbc4f815ae4c9b0a1f4a16238f96c197b38239'

function Step($text) { Write-Host ''; Write-Host "== $text" -ForegroundColor Cyan }
function Fail($text) { Write-Host ''; Write-Host "BUILD FAILED: $text" -ForegroundColor Red; exit 1 }
function Ok($text) { Write-Host "   $text" -ForegroundColor Green }

Step 'Checking the tools'
function Refresh-Path {
    $env:PATH = [Environment]::GetEnvironmentVariable('PATH', 'Machine') + ';' + [Environment]::GetEnvironmentVariable('PATH', 'User')
    foreach ($p in "$env:ProgramFiles\LLVM\bin", "$env:ProgramFiles\CMake\bin", "$env:LOCALAPPDATA\Microsoft\WinGet\Links") {
        if ((Test-Path $p) -and ($env:PATH -notlike "*$p*")) { $env:PATH = "$p;$env:PATH" }
    }
    if (-not $env:VULKAN_SDK -or -not (Test-Path $env:VULKAN_SDK)) {
        $sdk = Get-ChildItem 'C:\VulkanSDK' -Directory -ErrorAction SilentlyContinue | Sort-Object Name | Select-Object -Last 1
        if ($sdk) { $env:VULKAN_SDK = $sdk.FullName }
    }
}
Refresh-Path
$vswhere = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"
function Find-VS {
    if (-not (Test-Path $vswhere)) { return $null }
    & $vswhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
}
function Version-Of($exe, $pattern) {
    try { $out = (& $exe --version 2>&1 | Out-String) } catch { return $null }
    if ($out -match $pattern) { return [version]$matches[1] } else { return $null }
}
$tools = @(
    @{ Name = 'Visual Studio 2022 C++ tools (MSVC, Windows SDK)'; Check = { [bool](Find-VS) }; Winget = 'Microsoft.VisualStudio.2022.BuildTools';
       Args = @('--override', '--quiet --wait --add Microsoft.VisualStudio.Workload.VCTools --includeRecommended') },
    @{ Name = 'LLVM (clang-cl)'; Check = { [bool](Get-Command clang-cl -ErrorAction SilentlyContinue) }; Winget = 'LLVM.LLVM' },
    @{ Name = 'CMake 3.25+'; Check = { $v = Version-Of 'cmake' 'cmake version (\d+\.\d+)'; $v -and $v -ge [version]'3.25' }; Winget = 'Kitware.CMake' },
    @{ Name = 'Ninja'; Check = { [bool](Get-Command ninja -ErrorAction SilentlyContinue) }; Winget = 'Ninja-build.Ninja' },
    @{ Name = 'Python 3.10+'; Check = { $v = Version-Of 'python' 'Python (\d+\.\d+)'; $v -and $v -ge [version]'3.10' }; Winget = 'Python.Python.3.12' },
    @{ Name = 'Vulkan SDK'; Check = { $env:VULKAN_SDK -and (Test-Path "$env:VULKAN_SDK\Include\vulkan\vulkan.h") }; Winget = 'KhronosGroup.VulkanSDK' }
)
$missing = @()
foreach ($t in $tools) {
    if (& $t.Check) { Ok "$($t.Name): found" } else { Write-Host "   $($t.Name): MISSING" -ForegroundColor Yellow; $missing += $t }
}
if ($missing.Count) {
    $winget = Get-Command winget -ErrorAction SilentlyContinue
    if ($NoInstall -or -not $winget) {
        Fail ("missing: " + (($missing | ForEach-Object { $_.Name }) -join ', ') + ". Install them and run build.bat again.")
    }
    $answer = Read-Host "Install the missing tools with winget now? (y/n)"
    if ($answer -notmatch '^[yY]') { Fail 'missing tools' }
    foreach ($t in $missing) {
        Write-Host "   installing $($t.Name)..."
        $wargs = @('install', '--id', $t.Winget, '-e', '--accept-package-agreements', '--accept-source-agreements')
        if ($t.Args) { $wargs += $t.Args }
        & winget @wargs
    }
    Refresh-Path
    foreach ($t in $missing) { if (-not (& $t.Check)) { Fail "$($t.Name) still not found (open a new terminal after installing, then run build.bat again)" } }
}

Step 'Checking your game dump'
$saved = Join-Path $root '.local\dump_path.txt'
if (-not $Game) {
    $ini = Join-Path $env:APPDATA 'WWHDRecomp\settings.ini'
    if (Test-Path -LiteralPath $ini) {
        $line = Get-Content -LiteralPath $ini | Where-Object { $_ -match '^\s*paths\.game\s*=\s*(.+)$' } | Select-Object -First 1
        if ($line -match '=\s*(.+)$') { $Game = $matches[1].Trim() }
    }
}
if (-not $Game -and (Test-Path -LiteralPath $saved)) { $Game = (Get-Content -LiteralPath $saved -Raw).Trim() }
while ($true) {
    if (-not $Game) {
        $typed = Read-Host 'Path to your Wind Waker HD dump (the folder with code, content and meta)'
        $Game = if ($typed) { $typed.Trim('" ') } else { '' }
    }
    if (-not $Game) { Fail 'no game folder given' }
    $rpx = Join-Path $Game 'code\cking.rpx'
    if (-not (Test-Path -LiteralPath $rpx)) {
        $sub = Get-ChildItem -LiteralPath $Game -Directory -ErrorAction SilentlyContinue | Where-Object { Test-Path -LiteralPath (Join-Path $_.FullName 'code\cking.rpx') } | Select-Object -First 1
        if ($sub) { $Game = $sub.FullName; $rpx = Join-Path $Game 'code\cking.rpx' }
    }
    if (-not (Test-Path -LiteralPath $rpx)) { Write-Host "   no code\cking.rpx in $Game" -ForegroundColor Yellow; $Game = ''; continue }
    $sha = (Get-FileHash -Algorithm SHA1 -LiteralPath $rpx).Hash.ToLower()
    if ($sha -ne $kRpxSha1) {
        Write-Host "   code\cking.rpx isn't the supported revision (USA v0); SHA-1 $sha" -ForegroundColor Yellow; $Game = ''; continue
    }
    break
}
Ok "dump: $Game (USA v0, verified)"
New-Item -ItemType Directory -Force (Join-Path $root '.local') | Out-Null
Set-Content -Encoding utf8 $saved $Game

Step 'Cleaning the old build and recompiled sources'
foreach ($d in "build\$preset", 'generated') {
    $p = Join-Path $root $d
    if (Test-Path -LiteralPath $p) { Remove-Item -LiteralPath $p -Recurse -Force; Ok "removed $d" }
}

Step 'Recompiling the game code from your dump (a few minutes)'
python (Join-Path $root 'tools\recomp\recomp.py') $rpx
if ($LASTEXITCODE) { Fail 'the recompiler failed' }
if (-not $SkipTests) {
    Step 'Testing the recompiler'
    python (Join-Path $root 'tools\recomp\test_semantics.py')
    if ($LASTEXITCODE) { Fail 'the recompiler tests failed' }
}

Step "Building windfall.exe ($preset)"
if (-not $env:VCINSTALLDIR) {
    $vs = Find-VS
    $env:PATH = "$(Split-Path $vswhere);$env:PATH"  # vcvars64.bat calls vswhere by name
    cmd /c "`"$vs\VC\Auxiliary\Build\vcvars64.bat`" >nul && set" | ForEach-Object {
        if ($_ -match '^([^=]+)=(.*)$') { [Environment]::SetEnvironmentVariable($matches[1], $matches[2]) }
    }
    Refresh-Path
    foreach ($p in "$env:ProgramFiles\LLVM\bin") { if (Test-Path $p) { $env:PATH = "$p;$env:PATH" } }
}
$ErrorActionPreference = 'Continue'  # cmake writes warnings on stderr; failures come from exit codes
cmake --preset $preset
if ($LASTEXITCODE) { Fail 'cmake configure failed' }
cmake --build --preset $preset
if ($LASTEXITCODE) { Fail 'the build failed' }
$ErrorActionPreference = 'Stop'
$exe = Join-Path $root "build\$preset\runtime\windfall.exe"
if (-not (Test-Path $exe)) { Fail 'windfall.exe was not produced' }

if (-not $NoRando) {
    Step 'Building the randomizer (TWWHD Randomizer seed generator)'
    try { & (Join-Path $root 'scripts\build_rando.ps1') -Out (Split-Path $exe) }
    catch { Write-Host "   randomizer not built: $_ (the game still works; the Randomizer tab can't make seeds)" -ForegroundColor Yellow }
}

if ($Package) {
    Step 'Packaging'
    & (Join-Path $root 'scripts\package.ps1') -NoBuild
}

Write-Host ''
Write-Host "Done: $exe" -ForegroundColor Green
if ($Package) { Write-Host "Package: $(Join-Path $root 'dist\Windfall.zip')" -ForegroundColor Green }
exit 0
