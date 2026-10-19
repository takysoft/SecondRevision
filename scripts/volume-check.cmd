@echo off
REM ---------------------------------------------------------------------------
REM  The renderer's volume walk - smoke, flames, waterfalls - run by itself:
REM  builds tools\VolumeCheck, runs it on the software device, and turns the
REM  pictures it writes into PNGs. Needs no game and none of the graphics
REM  card's time.   volume-check.cmd [name] [-gpu] [...]
REM  The pictures go to _shots\<date-time>-<name>: every folder under _shots
REM  begins with its date, so that old ones are easy to clear away.
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
if not defined MSBUILD (
  echo ERROR: MSBuild.exe not found.
  exit /b 1
)

"%MSBUILD%" "%~dp0..\tools\VolumeCheck\VolumeCheck.vcxproj" /p:Configuration=Release /p:Platform=Win32 /v:quiet /nologo
if errorlevel 1 (
  echo BUILD ERROR
  exit /b 1
)

set "NAME=%~1"
if "%NAME%"=="" set "NAME=volume-check"
for /f %%T in ('powershell -NoProfile -Command "Get-Date -Format yyyyMMdd-HHmmss"') do set "STAMP=%%T"
set "OUT=%~dp0..\_shots\%STAMP%-%NAME%"
if not exist "%OUT%" mkdir "%OUT%"

echo.
"%~dp0..\build\Release\VolumeCheck.exe" -out "%OUT%" %2 %3 %4 %5 %6
set "RESULT=%errorlevel%"
python "%~dp0volume_check_images.py" "%OUT%"
exit /b %RESULT%
