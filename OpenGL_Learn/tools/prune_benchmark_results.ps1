param(
    [switch]$Execute,
    [int]$ExpectedDeleteCount = 0,
    [int]$ExpectedKeepCount = 0
)

$ErrorActionPreference = "Stop"

$projectRoot = [System.IO.Path]::GetFullPath((Join-Path $PSScriptRoot ".." )).TrimEnd('\')
$benchmarkRoot = [System.IO.Path]::GetFullPath((Join-Path $projectRoot "benchmark-results")).TrimEnd('\')

if ([System.IO.Path]::GetFileName($benchmarkRoot) -ne "benchmark-results") {
    throw "Refusing to operate on unexpected directory: $benchmarkRoot"
}
if (-not $benchmarkRoot.StartsWith($projectRoot + '\', [System.StringComparison]::OrdinalIgnoreCase)) {
    throw "Benchmark directory is outside the project: $benchmarkRoot"
}
if (-not (Test-Path -LiteralPath $benchmarkRoot -PathType Container)) {
    throw "Benchmark directory does not exist: $benchmarkRoot"
}

function Assert-InBenchmarkRoot([string]$Path) {
    $fullPath = [System.IO.Path]::GetFullPath($Path)
    if (-not $fullPath.StartsWith($benchmarkRoot + '\', [System.StringComparison]::OrdinalIgnoreCase)) {
        throw "Path escaped benchmark-results: $fullPath"
    }
    return $fullPath
}

$files = @(Get-ChildItem -LiteralPath $benchmarkRoot -Recurse -Force -File)
$keep = [System.Collections.Generic.HashSet[string]]::new([System.StringComparer]::OrdinalIgnoreCase)

# Preserve reports, compact summaries, compressed figures, and integrity records.
$alwaysKeepExtensions = @(
    '.md', '.png', '.jpg', '.jpeg', '.svg', '.gif',
    '.html', '.csv', '.txt', '.sha256'
)
$summaryNamePattern = '(?i)(aggregate|summary|manifest|metadata|verification|provenance|metrics|statistics|report|protocol|breakdown|result|config|scene|hardware|environment)'

foreach ($file in $files) {
    $extension = $file.Extension.ToLowerInvariant()
    if ($alwaysKeepExtensions -contains $extension) {
        [void]$keep.Add($file.FullName)
        continue
    }
    if ($extension -eq '.json' -and $file.BaseName -match $summaryNamePattern) {
        [void]$keep.Add($file.FullName)
    }
}

# A cleanup pass must never remove evidence that has already been committed.
$gitRoot = (& git -C $projectRoot rev-parse --show-toplevel).Trim()
if (-not $gitRoot) {
    throw "Unable to resolve the Git repository root."
}
$trackedBenchmarkFiles = @(
    git -C $gitRoot ls-files -- 'OpenGL_Learn/benchmark-results'
)
foreach ($relativePath in $trackedBenchmarkFiles) {
    $fullPath = Assert-InBenchmarkRoot (Join-Path $gitRoot $relativePath)
    if (Test-Path -LiteralPath $fullPath -PathType Leaf) {
        [void]$keep.Add($fullPath)
    }
}

# Keep one optimized/conditionally accepted RenderDoc capture per validated path.
$retainedRdcPaths = @(
    'point-light-grid-slice-count\grid-slice-count-formal-20260805\renderdoc\captures\cached-n0512-r060-s08_capture.rdc',
    'point-light-screen-routing\screen-bounds-scissor-analytic-20260804\renderdoc\captures\representative-0512-analytic-screen_capture.rdc',
    'point-light-stencil-clear-ab\stencil-clear-coalescing-ab-20260802\renderdoc\captures\representative-0512-coalesced_capture.rdc',
    'point-light-tile-cluster-runtime-boundary\tile-cluster-runtime-boundary-formal-20260805\renderdoc\captures\cached-n0512-r030-cluster16_capture.rdc',
    'ssao-renderdoc-evidence\renderdoc-1.45.0\captures\sponza-half64-bilateral_capture.rdc'
)
foreach ($relativePath in $retainedRdcPaths) {
    $fullPath = Assert-InBenchmarkRoot (Join-Path $benchmarkRoot $relativePath)
    if (Test-Path -LiteralPath $fullPath -PathType Leaf) {
        [void]$keep.Add($fullPath)
    }
}

# Keep one raw screenshot for every top-level experiment family as a fallback.
$ppmGroups = $files |
    Where-Object Extension -eq '.ppm' |
    Group-Object { $_.FullName.Substring($benchmarkRoot.Length + 1).Split('\')[0] }

foreach ($group in $ppmGroups) {
    $bestScreenshot = $group.Group |
        Sort-Object `
            @{ Expression = { if ($_.Name -match '(?i)(correctness|oracle|reference|final|representative|optimized|candidate|coalesced|analytic|half64)') { 0 } else { 1 } } }, `
            @{ Expression = 'Length' }, `
            @{ Expression = 'FullName' } |
        Select-Object -First 1
    if ($null -ne $bestScreenshot) {
        [void]$keep.Add($bestScreenshot.FullName)
    }
}

$filesToKeep = @($files | Where-Object { $keep.Contains($_.FullName) })
$filesToDelete = @($files | Where-Object { -not $keep.Contains($_.FullName) })
$originalBytes = ($files | Measure-Object Length -Sum).Sum
$keepBytes = ($filesToKeep | Measure-Object Length -Sum).Sum
$deleteBytes = ($filesToDelete | Measure-Object Length -Sum).Sum

[pscustomobject]@{
    Mode          = if ($Execute) { 'Execute' } else { 'DryRun' }
    OriginalFiles = $files.Count
    OriginalGiB   = [math]::Round($originalBytes / 1GB, 3)
    KeepFiles     = $filesToKeep.Count
    KeepGiB       = [math]::Round($keepBytes / 1GB, 3)
    DeleteFiles   = $filesToDelete.Count
    DeleteGiB     = [math]::Round($deleteBytes / 1GB, 3)
} | Format-List

if (-not $Execute) {
    return
}
if ($ExpectedDeleteCount -le 0 -or $ExpectedKeepCount -le 0) {
    throw "Execute mode requires positive ExpectedDeleteCount and ExpectedKeepCount."
}
if ($filesToDelete.Count -ne $ExpectedDeleteCount -or $filesToKeep.Count -ne $ExpectedKeepCount) {
    throw "Retention set changed. Expected delete/keep $ExpectedDeleteCount/$ExpectedKeepCount, got $($filesToDelete.Count)/$($filesToKeep.Count)."
}

$failures = [System.Collections.Generic.List[string]]::new()
foreach ($file in $filesToDelete) {
    $fullPath = Assert-InBenchmarkRoot $file.FullName
    try {
        Remove-Item -LiteralPath $fullPath -Force
    }
    catch {
        [void]$failures.Add("$fullPath :: $($_.Exception.Message)")
    }
}

$removedDirectoryCount = 0
$directories = Get-ChildItem -LiteralPath $benchmarkRoot -Recurse -Force -Directory |
    Sort-Object { $_.FullName.Length } -Descending
foreach ($directory in $directories) {
    $fullPath = Assert-InBenchmarkRoot $directory.FullName
    if (-not (Get-ChildItem -LiteralPath $fullPath -Force | Select-Object -First 1)) {
        Remove-Item -LiteralPath $fullPath -Force
        $removedDirectoryCount++
    }
}

[pscustomobject]@{
    DeletedFiles          = $filesToDelete.Count - $failures.Count
    ReclaimedGiB          = [math]::Round($deleteBytes / 1GB, 3)
    RemovedEmptyFolders   = $removedDirectoryCount
    FailedFileDeletions   = $failures.Count
} | Format-List

if ($failures.Count -gt 0) {
    $failures | Select-Object -First 20
    throw "Some files could not be deleted."
}
