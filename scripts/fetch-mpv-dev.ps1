<#
.SYNOPSIS
    Fetches the pinned libmpv development package described by deps/libmpv.lock.json
    into third_party/mpv/dev (or the extractDirectory from the lock).

.DESCRIPTION
    Windows libmpv is not stored in git (the runtime DLL is ~114 MB), so every fresh
    clone must run this before CMake configure. The script is idempotent: when the
    target already holds the extracted files whose hashes match the lock, it exits
    without touching the network. The archive is verified against the pinned
    SHA-256 before extraction, so a tampered or stale download never lands in the
    tree.

    This is the same source of truth the Windows CI job reads, which is what keeps
    local builds and release artifacts on byte-identical libmpv bits.

.EXAMPLE
    pwsh -NoProfile -File scripts/fetch-mpv-dev.ps1
    pwsh -NoProfile -File scripts/fetch-mpv-dev.ps1 -Force
#>
[CmdletBinding()]
param(
    # Re-download and re-extract even when the target already matches the lock.
    [switch]$Force
)

$ErrorActionPreference = 'Stop'

$repositoryRoot = Split-Path -Parent $PSScriptRoot
$lockPath = Join-Path $repositoryRoot 'deps\libmpv.lock.json'
if (-not (Test-Path $lockPath)) {
    throw "Missing dependency lock file: $lockPath"
}

$lock = Get-Content -Raw $lockPath | ConvertFrom-Json
$windows = $lock.windows

$assetName = $windows.asset
$assetSha256 = $windows.assetSha256.ToLowerInvariant()
$targetDirectory = Join-Path $repositoryRoot ($windows.extractDirectory -replace '/', '\')
$downloadUrl = "https://github.com/{0}/releases/download/{1}/{2}" -f `
    $windows.repository, $windows.tag, $assetName

function Get-FileSha256([string]$Path) {
    (Get-FileHash -Algorithm SHA256 -LiteralPath $Path).Hash.ToLowerInvariant()
}

function Test-ExtractionIsCurrent {
    # Every tracked member must exist and match its pinned extracted hash. A
    # member that is absent or differs (touched by hand, half-extracted, from an
    # older tag) forces a fresh download.
    foreach ($member in $windows.extracted.PSObject.Properties) {
        $extractedPath = Join-Path $targetDirectory ($member.Name -replace '/', '\')
        if (-not (Test-Path $extractedPath)) {
            return $false
        }
        if ((Get-FileSha256 $extractedPath) -ne $member.Value.ToLowerInvariant()) {
            Write-Warning ("{0} does not match the lock; re-fetching" -f $member.Name)
            return $false
        }
    }
    return $true
}

if (-not $Force -and (Test-Path $targetDirectory) -and (Test-ExtractionIsCurrent)) {
    Write-Host "libmpv already at pinned tag $($windows.tag); nothing to do."
    return
}

Write-Host "Fetching libmpv dev package"
Write-Host "  repository : $($windows.repository)"
Write-Host "  tag        : $($windows.tag)"
Write-Host "  asset      : $assetName"
Write-Host "  sha256     : $assetSha256"

$archive = Join-Path ([System.IO.Path]::GetTempPath()) $assetName
try {
    Write-Host "Downloading $downloadUrl"
    Invoke-WebRequest -Uri $downloadUrl -OutFile $archive -UseBasicParsing

    $actual = Get-FileSha256 $archive
    if ($actual -ne $assetSha256) {
        throw "Archive hash mismatch: expected $assetSha256, got $actual"
    }
    Write-Host "Archive hash verified."

    New-Item -ItemType Directory -Force $targetDirectory | Out-Null
    & 7z x $archive "-o$targetDirectory" -y | Out-Null
    if ($LASTEXITCODE -ne 0) {
        throw "7z extraction failed with exit code $LASTEXITCODE. Install 7-Zip (scoop install 7zip)."
    }

    # Re-verify each extracted member against the lock so a corrupted archive
    # cannot pass unnoticed into the build tree.
    foreach ($member in $windows.extracted.PSObject.Properties) {
        $extractedPath = Join-Path $targetDirectory ($member.Name -replace '/', '\')
        if (-not (Test-Path $extractedPath)) {
            throw "Extraction is missing $($member.Name)"
        }
        $extractedActual = Get-FileSha256 $extractedPath
        if ($extractedActual -ne $member.Value.ToLowerInvariant()) {
            throw "Extracted $($member.Name) hash mismatch: expected $($member.Value), got $extractedActual"
        }
    }
}
finally {
    Remove-Item -Force -ErrorAction SilentlyContinue $archive
}

Write-Host "libmpv dev package installed at $targetDirectory"
