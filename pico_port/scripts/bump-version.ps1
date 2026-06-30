<#
.SYNOPSIS
    Auto-bumps FW_VERSION_PATCH in Firmware_Version.h when source files change.

.DESCRIPTION
    Hashes every .c/.h file under application/, relevant shared sevantica_drivers
    source/include folders, and the pico_port root, plus CMakeLists.txt files.
    Compares against a stored hash in pico_port/.last_build_hash.
    On change, increments FW_VERSION_PATCH by 1 and persists the new hash.

    Always exits 0 so it never blocks a build, even on unexpected errors.
#>

param()

$ErrorActionPreference = "Stop"

$scriptDir     = Split-Path -Parent $MyInvocation.MyCommand.Path
$projectDir    = Split-Path -Parent $scriptDir          # pico_port/
$appDir        = (Resolve-Path (Join-Path $projectDir "../application")).Path
$driversDirRaw = Join-Path $projectDir "../../../../Cross Project/VS Code Common/sevantica_drivers"
$driversDir    = if (Test-Path $driversDirRaw) { (Resolve-Path $driversDirRaw).Path } else { $null }
$versionHeader = Join-Path $appDir "Include\System\Firmware_Version.h"
$hashFile      = Join-Path $projectDir ".last_build_hash"

# ── Collect files to monitor ──────────────────────────────────────────────────
$skipPattern = [regex]'\\(build|cache|autosave|backup|__pycache__)\\?'

$driverFiles = @()
if ($driversDir) {
    $driverSearchRoots = @("Include", "Source", "FatFs") |
        ForEach-Object { Join-Path $driversDir $_ } |
        Where-Object { Test-Path $_ }

    $driverFiles = @(
        if ($driverSearchRoots.Count -gt 0) {
            Get-ChildItem -Path $driverSearchRoots -Recurse -Include "*.c","*.h" |
                Where-Object { $_.FullName -notmatch $skipPattern }
        }
        Get-Item -Path (Join-Path $driversDir "CMakeLists.txt") -ErrorAction SilentlyContinue
    )
}

$files = @(
    Get-ChildItem -Path $appDir    -Recurse -Include "*.c","*.h" |
        Where-Object { $_.FullName -notmatch $skipPattern } |
        Where-Object { $_.FullName -ne $versionHeader }     # exclude the version header itself
    Get-ChildItem -Path $projectDir -File   -Include "*.c","*.h","CMakeLists.txt"
    $driverFiles
) | Where-Object { $_ -ne $null } | Sort-Object FullName

if ($files.Count -eq 0) {
    Write-Warning "[version] No source files found - skipping bump."
    exit 0
}

# ── Compute combined hash (hash-of-hashes for efficiency) ────────────────────
$sha256    = [System.Security.Cryptography.SHA256]::Create()
$manifest  = [System.Text.StringBuilder]::new()

foreach ($f in $files) {
    $bytes  = [System.IO.File]::ReadAllBytes($f.FullName)
    $digest = $sha256.ComputeHash($bytes)
    $hex    = [System.BitConverter]::ToString($digest).Replace("-","").ToLower()
    $null   = $manifest.AppendLine("$hex  $($f.FullName)")
}

$manifestBytes = [System.Text.Encoding]::UTF8.GetBytes($manifest.ToString())
$finalBytes    = $sha256.ComputeHash($manifestBytes)
$currentHash   = [System.BitConverter]::ToString($finalBytes).Replace("-","").ToLower()

# ── Compare with stored hash ──────────────────────────────────────────────────
$storedHash = if (Test-Path $hashFile) { (Get-Content $hashFile -Raw).Trim() } else { "" }

if ($currentHash -eq $storedHash) {
    Write-Host "[version] No source changes detected - patch version unchanged."
    exit 0
}

# ── Bump FW_VERSION_PATCH ─────────────────────────────────────────────────────
try {
    $content = [System.IO.File]::ReadAllText($versionHeader)

    if ($content -notmatch '#define\s+FW_VERSION_PATCH\s+(\d+)') {
        Write-Warning "[version] Could not parse FW_VERSION_PATCH from $versionHeader - skipping bump."
        exit 0
    }

    $oldPatch = [int]$Matches[1]
    $newPatch = $oldPatch + 1
    # Plain string replacement — no regex back-reference complications
    $oldToken = "#define FW_VERSION_PATCH    $oldPatch"
    $newToken = "#define FW_VERSION_PATCH    $newPatch"
    if ($content.IndexOf($oldToken) -lt 0) {
        # Spacing may differ — fall back to a liberal regex with a Func<> MatchEvaluator
        $evaluator = [System.Text.RegularExpressions.MatchEvaluator](
            [System.Func[System.Text.RegularExpressions.Match, string]]{
                param($m) ($m.Value -replace "\d+$", "$newPatch")
            }
        )
        $content = [System.Text.RegularExpressions.Regex]::Replace(
            $content,
            '(?m)^#define\s+FW_VERSION_PATCH\s+\d+',
            $evaluator
        )
    } else {
        $content = $content.Replace($oldToken, $newToken)
    }

    [System.IO.File]::WriteAllText($versionHeader, $content)
    Write-Host "[version] FW_VERSION_PATCH bumped $oldPatch -> $newPatch"
} catch {
    Write-Warning "[version] Failed to update $versionHeader`: $_"
    exit 0
}

# ── Persist new hash ──────────────────────────────────────────────────────────
[System.IO.File]::WriteAllText($hashFile, $currentHash)
Write-Host "[version] Source hash stored."
