# windfall.exe, a playtest launcher, README, LICENSE, the randomizer and the toolchain that builds the game code on the
param([switch]$NoBuild)
$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot
Set-Location $root

if (-not $NoBuild) { & "$PSScriptRoot\build.ps1" }

$exe = Join-Path $root 'build\release\runtime\windfall.exe'
if (-not (Test-Path $exe)) { throw "windfall.exe not found; build first" }

$dist = Join-Path $root 'dist\Windfall'
if (Test-Path $dist) { Remove-Item -Recurse -Force $dist }
New-Item -ItemType Directory -Force $dist | Out-Null
Copy-Item $exe $dist
Copy-Item (Join-Path $root 'README.md') $dist
Copy-Item (Join-Path $root 'LICENSE') $dist
Set-Content -Encoding ascii (Join-Path $dist 'Windfall Playtest.bat') "@start `"`" `"%~dp0windfall.exe`" --playtest"

$rando = Join-Path (Split-Path $exe) 'randomizer'
if (Test-Path (Join-Path $rando 'wwhd_rando.exe')) { Copy-Item -Recurse $rando (Join-Path $dist 'randomizer') }

$tc = Join-Path $dist 'toolchain'
$third = Join-Path $root 'build\third_party'
New-Item -ItemType Directory -Force $third | Out-Null
function Get-Pinned([string]$url, [string]$file, [string]$sha256)
{
    $path = Join-Path $third $file
    if (-not (Test-Path $path)) { Invoke-WebRequest -UseBasicParsing $url -OutFile $path }
    if ((Get-FileHash -Algorithm SHA256 $path).Hash -ne $sha256) { Remove-Item $path; throw "$file doesn't match its pinned hash" }
    return $path
}
# Python (PSF license, its LICENSE.txt is inside)
$py = Get-Pinned 'https://www.python.org/ftp/python/3.12.10/python-3.12.10-embed-amd64.zip' 'python-3.12.10-embed-amd64.zip' `
    '4ACBED6DD1C744B0376E3B1CF57CE906F9DC9E95E68824584C8099A63025A3C3'
Expand-Archive $py (Join-Path $tc 'python')
$llvmBin = Join-Path $env:ProgramFiles 'LLVM\bin'
$llvmVersion = '21.1.8'
if (-not ((& (Join-Path $llvmBin 'clang.exe') --version) -match "clang version $([regex]::Escape($llvmVersion))")) { throw "LLVM $llvmVersion is needed in $llvmBin" }
New-Item -ItemType Directory -Force (Join-Path $tc 'llvm') | Out-Null
Copy-Item (Join-Path $llvmBin 'clang.exe'), (Join-Path $llvmBin 'lld-link.exe') (Join-Path $tc 'llvm')
$llvmLicense = Get-Pinned "https://raw.githubusercontent.com/llvm/llvm-project/llvmorg-$llvmVersion/llvm/LICENSE.TXT" 'LLVM-LICENSE.TXT' `
    '8D85C1057D742E597985C7D4E6320B015A9139385CFF4CBAE06FFC0EBE89AFEE'
Copy-Item $llvmLicense (Join-Path $tc 'llvm\LICENSE.TXT')
$rec = Join-Path $tc 'recomp'
New-Item -ItemType Directory -Force (Join-Path $rec 'tools\recomp'), (Join-Path $rec 'config'), (Join-Path $rec 'runtime\include\wf') | Out-Null
Get-ChildItem (Join-Path $root 'tools\recomp\*.py') | Where-Object { $_.Name -notlike 'test_*' } | Copy-Item -Destination (Join-Path $rec 'tools\recomp')
Copy-Item (Join-Path $root 'config\game.json'), (Join-Path $root 'config\hooks.txt'), (Join-Path $root 'config\probes.txt') (Join-Path $rec 'config')
Copy-Item (Join-Path $root 'runtime\include\wf\*.h') (Join-Path $rec 'runtime\include\wf')
$buildId = (& (Join-Path $tc 'python\python.exe') -I (Join-Path $rec 'tools\recomp\recomp.py') --build-id).Trim()
$devId = (python (Join-Path $root 'tools\recomp\recomp.py') --build-id).Trim()
if (-not $buildId -or $buildId -ne $devId) { throw "the packaged recompiler's build id ($buildId) differs from the repository's ($devId)" }
[IO.File]::WriteAllText((Join-Path $tc 'BUILD_ID'), $buildId)

python (Join-Path $root 'tools\guard\check_no_game_data.py') --dir $dist --allow-big windfall.exe --allow-big wwhd_rando.exe `
    --allow-big clang.exe --allow-big lld-link.exe
if ($LASTEXITCODE) { Remove-Item -Recurse -Force $dist; throw "the package contains game data; nothing was packaged" }

$zip = Join-Path $root 'dist\Windfall.zip'
if (Test-Path $zip) { Remove-Item -Force $zip }
Compress-Archive -Path "$dist\*" -DestinationPath $zip
Write-Host "packaged: $dist and $zip"
