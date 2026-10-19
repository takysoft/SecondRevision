@echo off
REM ---------------------------------------------------------------------------
REM  The 64-bit host's pipeline, tried without the game (docs/terv-fsr4.md, M1).
REM  Builds SecondRevisionHost64.exe (x64) and Host64Probe.exe (Win32), then runs
REM  the probe. The probe uses the GPU, so it does not run while the game does:
REM  a probe gone wrong could reset the card under somebody's game.
REM
REM     host64-probe.cmd              build, then run 300 frames
REM     host64-probe.cmd build        build only
REM     host64-probe.cmd run 1000     run only, 1000 frames
REM     host64-probe.cmd run 300 --debug --rays
REM                                   the probe's own switches after the frames:
REM                                   the D3D12 debug layer, the rays' part alone
REM ---------------------------------------------------------------------------
setlocal

set "MSBUILD="
for %%V in (18 2022 2019) do (
  if not defined MSBUILD (
    if exist "C:\Program Files (x86)\Microsoft Visual Studio\%%V\BuildTools\MSBuild\Current\Bin\MSBuild.exe" (
      set "MSBUILD=C:\Program Files (x86)\Microsoft Visual Studio\%%V\BuildTools\MSBuild\Current\Bin\MSBuild.exe"
    )
  )
)
REM Not in Build Tools' usual place: whichever Visual Studio the installer knows of that has MSBuild.
set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
set "VSASK=-latest -products * -requires Microsoft.Component.MSBuild -find MSBuild\**\Bin\MSBuild.exe"
if not defined MSBUILD if exist "%VSWHERE%" (
  for /f "usebackq delims=" %%P in (`"%VSWHERE%" %VSASK%`) do if not defined MSBUILD set "MSBUILD=%%P"
)
if not defined MSBUILD (
  echo ERROR: MSBuild.exe not found.
  exit /b 1
)

if /i "%~1"=="run" goto run

"%MSBUILD%" "%~dp0..\src\Host64\Host64.vcxproj" /p:Configuration=Release /p:Platform=x64 /v:quiet /nologo
if errorlevel 1 (
  echo BUILD ERROR: SecondRevisionHost64.exe
  exit /b 1
)
"%MSBUILD%" "%~dp0..\tests\Host64Probe\Host64Probe.vcxproj" /p:Configuration=Release /p:Platform=Win32 /v:quiet /nologo
if errorlevel 1 (
  echo BUILD ERROR: Host64Probe.exe
  exit /b 1
)
echo built: build\Release\SecondRevisionHost64.exe, build\tests\Host64Probe.exe
if /i "%~1"=="build" exit /b 0

:run
tasklist /FI "IMAGENAME eq Revision.exe" 2>nul | find /i "Revision.exe" >nul
if not errorlevel 1 (
  echo the game is running - the probe does not run beside it
  exit /b 3
)
set "FRAMES=%~2"
if not defined FRAMES set "FRAMES=300"
"%~dp0..\build\tests\Host64Probe.exe" %FRAMES% %3 %4
exit /b %errorlevel%
