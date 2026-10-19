# ---------------------------------------------------------------------------
#  SecondRevision: build -> deploy -> select the renderer -> start -> log.
#
#  The Running.ini cleanup (skipping the recovery dialog) is done here too.
#
#     run-dx12.ps1                       build and run for 40 s
#     run-dx12.ps1 -Wait 90              a longer run, to look at it
#     run-dx12.ps1 -Defines "RDX_BRINGUP_CLEAR=1"
#     run-dx12.ps1 -NoRun                build and install only
#     run-dx12.ps1 -NoInstall            build only
#     run-dx12.ps1 -NoRun -DlssDev       ... with NVIDIA's development DLLs of DLSS beside the host
#
#  A game that is already running is never touched: nothing is installed beside it (it holds the DLL, and it may be
#  somebody's), and it is not closed from here.
# ---------------------------------------------------------------------------
param(
    [int]$Wait = 40,
    [string]$Defines = "",   # normal build: no defines. RDX_BRINGUP_CLEAR was the default once - a bring-up leftover that cleared every frame to blue (D3D12 warning #820)
    [switch]$NoRun,
    [switch]$NoInstall,
    # NVIDIA's development builds of the DLSS DLLs beside the host instead of the release ones: they draw NGX's debug
    # overlays and take its hotkeys - for looking at what DLSS is given on an NVIDIA card. Never shipped: the SDK
    # forbids it.
    [switch]$DlssDev
)

$root = Split-Path $PSScriptRoot -Parent
. (Join-Path $PSScriptRoot 'game.ps1')
$sys  = $GameSystem
$log  = Join-Path $sys "Revision.log"

$msb = $null
foreach ($v in @('18','2022','2019')) {
    $p = "C:\Program Files (x86)\Microsoft Visual Studio\$v\BuildTools\MSBuild\Current\Bin\MSBuild.exe"
    if (-not $msb -and (Test-Path $p)) { $msb = $p }
}
# Not in Build Tools' usual place: whichever Visual Studio the installer knows of that has MSBuild.
$vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
if (-not $msb -and (Test-Path $vswhere)) {
    $msb = & $vswhere -latest -products * -requires Microsoft.Component.MSBuild -find 'MSBuild\**\Bin\MSBuild.exe' |
           Select-Object -First 1
}
if (-not $msb) { Write-Host "ERROR: no MSBuild (looked in Build Tools' usual place and asked vswhere)"; exit 1 }

$a = @("$root\src\RevisionDX12\RevisionDX12.vcxproj", '/p:Configuration=Release', '/p:Platform=Win32',
       '/t:Rebuild', '/v:minimal', '/nologo')
if ($Defines) { $a += "/p:RdxDefines=$Defines" }
# Only real errors are shown. The SDK headers pour out C4595 warnings on every build (UObject::operator delete is
# inline and may throw), and that is not ours to fix.
$build = & $msb @a 2>&1
$code  = $LASTEXITCODE
$errors = $build | Select-String -Pattern ': error'
# THE EXIT CODE decides; the ': error' pattern only shows WHAT went wrong. A failure the pattern does not catch
# (a missing project, an MSB error, a full disk) can then no longer install a stale DLL - which did happen once,
# and a whole measurement was drawn from the old DLL.
if ($code -ne 0 -or $errors) {
    if ($errors) { $errors | Select-Object -First 10 | ForEach-Object { Write-Host $_.Line.Trim() } }
    else         { $build  | Select-Object -Last  10 | ForEach-Object { Write-Host "$_".Trim() } }
    Write-Host "BUILD FAILED (exit code $code)"
    exit 1
}

# The 64-bit host the Upscaler key's Auto runs AMD's FSR DLLs in (docs/terv-fsr4.md). Its build copies the DLLs beside
# it. A failed host build stops the whole run: installing the renderer beside a host of another protocol would only
# show up in the game as the fallback.
$hb = & $msb "$root\src\Host64\Host64.vcxproj" '/p:Configuration=Release' '/p:Platform=x64' '/t:Rebuild' '/v:minimal' '/nologo' 2>&1
if ($LASTEXITCODE -ne 0) {
    $hb | Select-String -Pattern ': error' | Select-Object -First 10 | ForEach-Object { Write-Host $_.Line.Trim() }
    Write-Host "BUILD FAILED: SecondRevisionHost64.exe (exit code $LASTEXITCODE)"
    exit 1
}

if ($NoInstall) { Write-Host "built, not installed"; exit 0 }
if (Get-Process -Name Revision -ErrorAction SilentlyContinue) {
    Write-Host "the game is running - built, NOT installed (close the game, then install)"
    exit 2
}
Copy-Item "$root\build\Release\SecondRevision.dll" $sys -Force
Copy-Item "$root\src\RevisionDX12\SecondRevision.int" $sys -Force
# The notices travel with the binary: the zlib licence of the renderer this one descends from, and the MIT licence of
# the FidelityFX code built into it and of AMD's FSR DLLs beside the host, all require it.
Copy-Item "$root\THIRD-PARTY-NOTICES.md" "$sys\SecondRevision-THIRD-PARTY-NOTICES.md" -Force
# EZ-Tree's leaf cards (scripts/make_leaf_cards.py), which the grown trees' leaves are drawn in.
Copy-Item "$root\assets\SecondRevision-leafcards.bin" $sys -Force
# The bark photographed (scripts/make_bark.py).
Copy-Item "$root\assets\SecondRevision-bark.bin" $sys -Force
# The host and the DLLs the upscale and frame generation need, in a folder of their own: they are 64-bit, and nothing in
# the game's own folder should take them for its own. nvngx_dlss.dll and nvngx_dlssg.dll are NVIDIA's DLSS models - the
# upscaler's and frame generation's - for AntiAliasing=DLSS; libxess.dll is Intel's XeSS, for AntiAliasing=XeSS, with its
# licence and its third-party list beside it, as Intel's terms ask.
$hostDir = Join-Path $sys "SecondRevisionHost64"
New-Item -ItemType Directory -Force -Path $hostDir | Out-Null
foreach ($f in @('SecondRevisionHost64.exe', 'amd_fidelityfx_loader_dx12.dll', 'amd_fidelityfx_upscaler_dx12.dll',
                 'amd_fidelityfx_framegeneration_dx12.dll', 'nvngx_dlss.dll', 'nvngx_dlssg.dll', 'libxess.dll',
                 'libxess-LICENSE.txt', 'libxess-third-party-programs.txt')) {
    Copy-Item "$root\build\Release\$f" $hostDir -Force
}
if ($DlssDev) {
    $dev = "$root\external\DLSS-310.9.1\lib\Windows_x86_64\dev"
    foreach ($f in @('nvngx_dlss.dll', 'nvngx_dlssg.dll')) { Copy-Item "$dev\$f" $hostDir -Force }
    Write-Host "installed - with NVIDIA's DEVELOPMENT DLLs of DLSS beside the host (debug overlays; not for a release)"
}
else { Write-Host "installed" }

python "$root\scripts\ini.py" set "$sys\Revision.ini" GameRenderDevice "SecondRevision.RevisionRenderDevice" | Out-Null

if ($NoRun) { exit 0 }

if (Test-Path $log) { Copy-Item $log "$log.previous" -Force }

# Skip the recovery dialog - see docs/allapot-01-fuszttesz.md
$marker = Join-Path $sys "Running.ini"
if (Test-Path $marker) { Remove-Item $marker -Force -ErrorAction SilentlyContinue }

Start-Process -FilePath "$sys\Revision.exe" -WorkingDirectory $sys | Out-Null
Write-Host "running, waiting $Wait s..."
Start-Sleep -Seconds $Wait
Get-Process Revision -ErrorAction SilentlyContinue | Stop-Process -Force
Start-Sleep -Seconds 2

$txt = Get-Content $log -Raw -ErrorAction SilentlyContinue
Write-Host ""
($txt -split "`n") | Where-Object { $_ -match 'SecondRevision|Critical:' } |
    ForEach-Object { ($_ -replace '^Log: ', '').Trim() } | Select-Object -First 25
