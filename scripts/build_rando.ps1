# pinned commit and puts it next to windfall.exe: <runtime dir>\randomizer\wwhd_rando.exe + data\ + LICENSE.
param([string]$Out = '')
$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot
if (-not $Out) { $Out = Join-Path $root 'build\release\runtime' }
$commit = 'cb7b1c979ac031c8eae91e23fb00fc5f4d042efa'
$src = Join-Path $root 'build\third_party\TWWHD-Randomizer'

if (-not (Get-Command git -ErrorAction SilentlyContinue)) { throw 'git is needed to fetch the randomizer' }
if (-not (Test-Path (Join-Path $src '.git')))
{
    New-Item -ItemType Directory -Force (Split-Path $src) | Out-Null
    git clone --quiet https://github.com/SuperDude88/TWWHD-Randomizer $src
    if ($LASTEXITCODE) { throw 'cloning the randomizer failed' }
}
git -C $src fetch --quiet origin $commit 2>$null
git -C $src checkout --quiet $commit
if ($LASTEXITCODE) { throw "checking out randomizer commit $commit failed" }
git -C $src submodule update --init --quiet
if ($LASTEXITCODE) { throw 'fetching the randomizer libraries failed' }

if (-not $env:VCINSTALLDIR)
{
    $env:PATH = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer;$env:PATH"
    $vs = & vswhere.exe -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
    cmd /c "`"$vs\VC\Auxiliary\Build\vcvars64.bat`" >nul && set" | ForEach-Object {
        if ($_ -match '^([^=]+)=(.*)$') { [Environment]::SetEnvironmentVariable($matches[1], $matches[2]) }
    }
}
if (Test-Path "$env:ProgramFiles\LLVM\bin") { $env:PATH = "$env:ProgramFiles\LLVM\bin;$env:PATH" }

$bld = Join-Path $src 'build'
$ErrorActionPreference = 'Continue'  # cmake writes warnings on stderr
cmake -S $src -B $bld -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_C_COMPILER=clang-cl -DCMAKE_CXX_COMPILER=clang-cl | Out-Null
if ($LASTEXITCODE) { throw 'configuring the randomizer failed' }
$log = Join-Path $bld 'build.log'
cmake --build $bld *> $log
if ($LASTEXITCODE) {
    Get-Content $log | Select-String -Pattern ': error|FAILED' | Select-Object -First 20 | ForEach-Object { Write-Host $_ }
    throw "building the randomizer failed (see $log)"
}
$ErrorActionPreference = 'Stop'

$dest = Join-Path $Out 'randomizer'
if (Test-Path $dest) { Remove-Item -Recurse -Force $dest }
New-Item -ItemType Directory -Force $dest | Out-Null
Copy-Item (Join-Path $bld 'wwhd_rando.exe') $dest
Copy-Item -Recurse (Join-Path $bld 'data') (Join-Path $dest 'data')
Copy-Item (Join-Path $src 'LICENSE.txt') (Join-Path $dest 'LICENSE.txt')
Write-Host "randomizer: $dest"
