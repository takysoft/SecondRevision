# Puts the Deus Ex SDK headers and import libraries where the build expects them: external\Games\DeusEx.
#
# The SDK is NOT part of this repository. Its files carry "Copyright 1997-1999 Epic Games, Inc. All Rights Reserved."
# and no licence that would let us redistribute them, so the repository refers to them instead of shipping them
# (docs/szabaly-licenc-tisztasag.md). This script only unpacks a copy you already have, and checks it is complete.
#
#   .\scripts\setup-sdk.ps1 -Zip <path to the SDK zip>   unpacks it into place
#   .\scripts\setup-sdk.ps1                              only checks what is there
[CmdletBinding()]
param(
    [string]$Zip,
    [switch]$Force
)

$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot
$dest = Join-Path $root 'external\Games\DeusEx'

# What the build needs. The five import libraries and the headers beside them: Core and Engine carry the renderer
# interface, the other three come with the game's own native packages.
$needed = @(
    'Core\Inc\Core.h', 'Core\Lib\Core.lib',
    'Engine\Inc\Engine.h', 'Engine\Lib\Engine.lib',
    'Window\Lib\Window.lib', 'Extension\Lib\Extension.lib', 'DeusEx\Lib\DeusEx.lib'
)

function Test-Sdk([string]$Path) {
    foreach ($f in $needed) {
        if (-not (Test-Path (Join-Path $Path $f))) { return $false }
    }
    return $true
}

if ((Test-Sdk $dest) -and -not $Force) {
    Write-Host "SDK ok: $dest"
    exit 0
}

if (-not $Zip) {
    Write-Host "The Deus Ex SDK is missing from: $dest"
    Write-Host ''
    Write-Host 'It is not shipped with this repository. Get the official Deus Ex SDK (Ion Storm released it for'
    Write-Host 'mod making; renderer packages for the game bundle the same headers), then run:'
    Write-Host ''
    Write-Host '    .\scripts\setup-sdk.ps1 -Zip <path to the zip>'
    Write-Host ''
    Write-Host 'The build needs this layout under external\Games\DeusEx:'
    $needed | ForEach-Object { Write-Host "    $_" }
    exit 1
}

if (-not (Test-Path $Zip)) { throw "no such file: $Zip" }

$temp = Join-Path ([System.IO.Path]::GetTempPath()) ('rdx-sdk-' + [System.IO.Path]::GetRandomFileName())
New-Item -ItemType Directory -Path $temp | Out-Null
try {
    Expand-Archive -Path $Zip -DestinationPath $temp -Force

    # The folders may sit anywhere inside the zip: find the one that has Core\Inc\Core.h under it.
    $core = Get-ChildItem -Path $temp -Recurse -Filter 'Core.h' -File |
            Where-Object { $_.FullName -match '\\Core\\Inc\\Core\.h$' } | Select-Object -First 1
    if (-not $core) { throw "this zip has no Core\Inc\Core.h in it - is it the SDK?" }
    $src = Split-Path -Parent (Split-Path -Parent (Split-Path -Parent $core.FullName))

    New-Item -ItemType Directory -Path $dest -Force | Out-Null
    foreach ($folder in @('Core', 'Engine', 'Window', 'Extension', 'DeusEx')) {
        $from = Join-Path $src $folder
        if (Test-Path $from) { Copy-Item $from $dest -Recurse -Force }
    }

    if (Test-Sdk $dest) {
        Write-Host "SDK unpacked: $dest"
    } else {
        Write-Host "SDK unpacked, but something is missing from it:"
        $needed | Where-Object { -not (Test-Path (Join-Path $dest $_)) } | ForEach-Object { Write-Host "    $_" }
        exit 1
    }
} finally {
    Remove-Item $temp -Recurse -Force -ErrorAction SilentlyContinue
}
