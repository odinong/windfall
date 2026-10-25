param([ValidateSet('release', 'debug')][string]$Preset = 'release', [switch]$KeepGoing)
$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot

if (-not $env:VCINSTALLDIR) {
    $installer = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer"
    $vswhere = "$installer\vswhere.exe"
    $env:PATH = "$installer;$env:PATH"  # vcvars64.bat calls vswhere by name
    $vs = & $vswhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
    if (-not $vs) { throw 'Visual Studio 2022 with the C++ x64 tools is required.' }
    cmd /c "`"$vs\VC\Auxiliary\Build\vcvars64.bat`" >nul && set" | ForEach-Object {
        if ($_ -match '^([^=]+)=(.*)$') { [Environment]::SetEnvironmentVariable($matches[1], $matches[2]) }
    }
}

foreach ($p in "$env:ProgramFiles\LLVM\bin", "$env:LOCALAPPDATA\Microsoft\WinGet\Links") {
    if ((Test-Path $p) -and ($env:PATH -notlike "*$p*")) { $env:PATH = "$p;$env:PATH" }
}
if (-not $env:VULKAN_SDK) {
    $sdk = Get-ChildItem 'C:\VulkanSDK' -Directory -ErrorAction SilentlyContinue | Sort-Object Name | Select-Object -Last 1
    if ($sdk) { $env:VULKAN_SDK = $sdk.FullName }
}

Push-Location $root
$ErrorActionPreference = 'Continue'
try {
    cmake --preset $Preset
    if ($LASTEXITCODE) { throw "configure failed" }
    if ($KeepGoing) { cmake --build --preset $Preset -- -k 0 } else { cmake --build --preset $Preset }
    if ($LASTEXITCODE) { throw "build failed" }
} finally { Pop-Location }
