@echo off
REM ---------------------------------------------------------------------------
REM  Tests for the renderer's engine-independent rules.
REM  Needs no window, no GPU and no game.
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

"%MSBUILD%" "%~dp0..\tests\RdxTests.vcxproj" /p:Configuration=Release /p:Platform=Win32 /v:quiet /nologo
if errorlevel 1 (
  echo BUILD ERROR
  exit /b 1
)

echo.
"%~dp0..\build\tests\RdxTests.exe"
exit /b %errorlevel%
