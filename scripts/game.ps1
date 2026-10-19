# ---------------------------------------------------------------------------
#  Where the game is on this machine: the one place every script asks.
#
#  In this order: the RDX_GAME_ROOT environment variable; the first line of game-path.txt in the project's root (this
#  machine's own file, not in the repository); Windows' own list of installed programs, whichever shop or installer put
#  the game there; and Steam's libraries, for a library Windows has no entry for. Where none of them has it the script
#  stops and says how to name the folder - nothing is guessed. The folder is the game's own: the one that holds
#  Revision\System\Revision.exe. scripts\game_path.py says the same for the Python scripts.
#
#     . (Join-Path $PSScriptRoot 'game.ps1')     # then $GameRoot, $GameSystem, $GameSaves and $GameFoundBy are there
# ---------------------------------------------------------------------------
function Find-RdxGameRoot {
    # Asked of .NET, not of PowerShell's own path commands: those stop on a folder whose drive is not there, and the
    # lists below may well name one (a disk since unplugged).
    $holds = { param($p) $p -and [System.IO.File]::Exists("$p\Revision\System\Revision.exe") }

    if (& $holds $env:RDX_GAME_ROOT) { return $env:RDX_GAME_ROOT.TrimEnd('\'), 'the RDX_GAME_ROOT variable' }

    $file = Join-Path (Split-Path $PSScriptRoot -Parent) 'game-path.txt'
    if (Test-Path -LiteralPath $file) {
        $line = Get-Content -LiteralPath $file -TotalCount 1
        if ($line) { $line = $line.Trim().Trim('"').TrimEnd('\') }
        if (& $holds $line) { return $line, 'game-path.txt' }
    }

    # Every installed program's folder, in both halves of the registry and for this user alone: an entry may name the
    # game's folder or the Revision folder inside it.
    foreach ($view in 'Registry64', 'Registry32') {
        foreach ($hive in 'LocalMachine', 'CurrentUser') {
            $list = $null
            try {
                $list = [Microsoft.Win32.RegistryKey]::OpenBaseKey($hive, $view).OpenSubKey(
                    'SOFTWARE\Microsoft\Windows\CurrentVersion\Uninstall')
            } catch { }
            if (-not $list) { continue }
            foreach ($name in $list.GetSubKeyNames()) {
                $at = $null
                try { $at = $list.OpenSubKey($name).GetValue('InstallLocation') } catch { }
                if (-not $at) { continue }
                $at = "$at".Trim().Trim('"').TrimEnd('\')
                if (-not $at) { continue }
                $above = $null
                try { $above = [System.IO.Path]::GetDirectoryName($at) } catch { }
                foreach ($folder in $at, $above) {
                    if (& $holds $folder) { return $folder, 'the list of installed programs' }
                }
            }
        }
    }

    $steam = $null
    foreach ($key in 'HKCU:\Software\Valve\Steam', 'HKLM:\SOFTWARE\WOW6432Node\Valve\Steam') {
        $at = Get-ItemProperty -Path $key -ErrorAction SilentlyContinue
        if ($at -and $at.SteamPath)   { $steam = $at.SteamPath;   break }
        if ($at -and $at.InstallPath) { $steam = $at.InstallPath; break }
    }
    if ($steam) {
        $steam = ($steam -replace '/', '\').TrimEnd('\')
        $libraries = @($steam)
        $list = "$steam\steamapps\libraryfolders.vdf"
        if ([System.IO.File]::Exists($list)) {
            foreach ($l in Get-Content -LiteralPath $list) {
                if ($l -match '"path"\s+"([^"]+)"') { $libraries += ($Matches[1] -replace '\\\\', '\').TrimEnd('\') }
            }
        }
        foreach ($library in $libraries) {
            $game = "$library\steamapps\common\Deus Ex"
            if (& $holds $game) { return $game, 'Steam''s libraries' }
        }
    }

    throw ("the game was not found on this machine. Say where it is: put the folder that holds " +
           "Revision\System\Revision.exe into the first line of $file, or into the RDX_GAME_ROOT environment variable.")
}

$GameRoot, $GameFoundBy = Find-RdxGameRoot
$GameSystem = Join-Path $GameRoot 'Revision\System'
$GameSaves  = Join-Path $GameRoot 'Revision\Save'
