# snapshot.ps1 - portable leakage probe (read-only)
# Dumps two things into $Out:
#   [SECTION A] full name list of every hermes-related host location (for set-diff)
#   [SECTION B] every file whose LastWriteTime >= $Watermark anywhere in the
#               host scan roots + the portable root (for mtime-diff)
# Never writes/deletes anything outside $Out.

param(
    [Parameter(Mandatory=$true)][string]$Watermark,
    [Parameter(Mandatory=$true)][string]$Out,
    [string]$PortableRoot = 'D:\text\HermesPortable',
    [switch]$SkipNewFileScan
)

$ErrorActionPreference = 'SilentlyContinue'

$wm = [datetime]::Parse($Watermark, [Globalization.CultureInfo]::InvariantCulture)

$hostHome = [Environment]::GetFolderPath('UserProfile')
$local    = $env:LOCALAPPDATA
$roam     = $env:APPDATA

# ---------- A. hermes-related locations on the HOST ----------
$keyRoots = @(
    (Join-Path $hostHome '.hermes'),
    (Join-Path $hostHome '.hermes-web-ui'),
    (Join-Path $hostHome '.ekko'),
    (Join-Path $hostHome '.cache'),
    (Join-Path $hostHome '.config'),
    (Join-Path $hostHome '.local'),
    (Join-Path $hostHome '.npm'),
    (Join-Path $hostHome '.uv'),
    (Join-Path $hostHome '.hermes-studio'),
    (Join-Path $local   'hermes'),
    (Join-Path $local   'hermes-web-ui'),
    (Join-Path $local   'ekko-studio'),
    (Join-Path $local   'uv'),
    (Join-Path $local   'pip'),
    (Join-Path $local   'npm-cache'),
    (Join-Path $roam    'hermes'),
    (Join-Path $roam    'hermes-web-ui'),
    (Join-Path $roam    'ekko-studio'),
    (Join-Path $roam    'npm')
)

$sw = [IO.StreamWriter]::new($Out, $false, [Text.UTF8Encoding]::new($false))
try {
    $sw.WriteLine("# leakcheck snapshot")
    $sw.WriteLine("# at              = $([datetime]::Now.ToString('o'))")
    $sw.WriteLine("# watermark       = $($wm.ToString('o'))")
    $sw.WriteLine("# hostHome        = $hostHome")
    $sw.WriteLine("# LOCALAPPDATA    = $local")
    $sw.WriteLine("# APPDATA         = $roam")
    $sw.WriteLine("# TEMP            = $env:TEMP")
    $sw.WriteLine("# TMP             = $env:TMP")
    $sw.WriteLine("# HOME(env)       = $env:HOME")
    $sw.WriteLine("# USERPROFILE(env)= $env:USERPROFILE")
    $sw.WriteLine("# PortableRoot    = $PortableRoot")
    $sw.WriteLine("")

    # ---- SECTION A ----
    $sw.WriteLine("[A] HOST KEY LOCATIONS (name list)")
    foreach ($r in $keyRoots) {
        if (-not (Test-Path -LiteralPath $r)) { continue }
        $sw.WriteLine("A|ROOT|$r")
        Get-ChildItem -LiteralPath $r -Recurse -Force -ErrorAction SilentlyContinue |
            ForEach-Object { $sw.WriteLine("A|$($_.FullName)") }
    }
    $sw.WriteLine("")

    # ---- SECTION A2 : stray top-level dirs in the host home ----
    $sw.WriteLine("[A2] HOST HOME TOP-LEVEL ENTRIES")
    Get-ChildItem -LiteralPath $hostHome -Force -ErrorAction SilentlyContinue |
        ForEach-Object { $sw.WriteLine("A2|$($_.Name)") }
    $sw.WriteLine("")

    # ---- SECTION B : mtime >= watermark, host scan roots ----
    if (-not $SkipNewFileScan) {
    $sw.WriteLine("[B] FILES NEWER THAN WATERMARK (host)")
    $scanRoots = @(
        $hostHome,
        $local,
        $roam,
        (Join-Path $env:SystemRoot 'Temp')
    ) | Where-Object { $_ -and (Test-Path -LiteralPath $_) } | Select-Object -Unique

    foreach ($r in $scanRoots) {
        Get-ChildItem -LiteralPath $r -Recurse -Force -File -ErrorAction SilentlyContinue |
            Where-Object { $_.LastWriteTime -ge $wm } |
            ForEach-Object {
                $sw.WriteLine("B|$($_.LastWriteTime.ToString('yyyy-MM-ddTHH:mm:ss'))|$($_.Length)|$($_.FullName)")
            }
    }
    $sw.WriteLine("")

    # ---- SECTION C : the portable itself (should be the ONLY place that grows) ----
    $sw.WriteLine("[C] FILES NEWER THAN WATERMARK (portable)")
    if (Test-Path -LiteralPath $PortableRoot) {
        Get-ChildItem -LiteralPath $PortableRoot -Recurse -Force -File -ErrorAction SilentlyContinue |
            Where-Object { $_.LastWriteTime -ge $wm } |
            ForEach-Object {
                $sw.WriteLine("C|$($_.LastWriteTime.ToString('yyyy-MM-ddTHH:mm:ss'))|$($_.Length)|$($_.FullName)")
            }
    }
    $sw.WriteLine("")
    }
    $sw.WriteLine("[END]")
}
finally {
    $sw.Close()
}
Write-Output "snapshot -> $Out"
