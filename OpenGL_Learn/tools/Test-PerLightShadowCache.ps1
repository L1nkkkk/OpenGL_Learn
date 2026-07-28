[CmdletBinding()]
param(
    [switch]$SkipBuild,
    [string]$MsBuildPath,
    [ValidateRange(64, 16384)]
    [int]$Width = 1920,
    [ValidateRange(64, 16384)]
    [int]$Height = 1080,
    [ValidateRange(4, 300)]
    [int]$MeasuredFrames = 4,
    [ValidateRange(4, 300)]
    [int]$ExternalWarmupFrames = 4,
    [ValidateRange(1, 60)]
    [int]$InternalWarmupFrames = 4,
    [ValidateSet("sponza", "san-miguel")]
    [string[]]$SceneIds = @("sponza", "san-miguel"),
    [string[]]$WorkloadIds = @()
)

$ErrorActionPreference = "Stop"
$runner = Join-Path $PSScriptRoot "Test-ShadowOptimizations.ps1"
$sharedEnvironment = @{
    OPENGL_LEARN_POINT_SHADOW_RENDER_PATH = "adaptive"
    OPENGL_LEARN_POINT_SHADOW_FACE_CULLING = "1"
    OPENGL_LEARN_SHADOW_SPOT_CASTER_DEPTH_FIT = "0"
}
$baselineEnvironment = $sharedEnvironment.Clone()
$baselineEnvironment.OPENGL_LEARN_SHADOW_CACHE = "none"
$baselineEnvironment.OPENGL_LEARN_SHADOW_PER_LIGHT_CACHE = "0"
$candidateEnvironment = $sharedEnvironment.Clone()
$candidateEnvironment.OPENGL_LEARN_SHADOW_CACHE = "revision"
$candidateEnvironment.OPENGL_LEARN_SHADOW_PER_LIGHT_CACHE = "1"
$allWorkloads = @(
    "static-hit",
    "force-update",
    "move-directional",
    "move-point",
    "move-spot",
    "move-caster",
    "change-caster-material",
    "reload-shadow-2d",
    "reload-shadow-point",
    "resize-point-shadow",
    "replace-point-shadow-target",
    "toggle-caster"
)
$workloads = @(
    if ($WorkloadIds.Count -gt 0) {
        $WorkloadIds
    }
    else {
        $allWorkloads
    }
)
$unknownWorkloads = @($workloads | Where-Object { $_ -notin $allWorkloads })
if ($unknownWorkloads.Count -gt 0) {
    throw "Unknown invalidation workloads: $($unknownWorkloads -join ', ')"
}

$completed = @()
$auditRecords = @()
for ($index = 0; $index -lt $workloads.Count; ++$index) {
    $workload = $workloads[$index]
    $experimentId =
        "per-light-cache-invalidation-$workload-${Width}x${Height}"
    $arguments = @{
        ExperimentId = $experimentId
        VariantALabel = "no-cache-oracle"
        VariantBLabel = "per-light-revision"
        VariantAEnvironment = $baselineEnvironment
        VariantBEnvironment = $candidateEnvironment
        Width = $Width
        Height = $Height
        MeasuredFrames = $MeasuredFrames
        ExternalWarmupFrames = $ExternalWarmupFrames
        InternalWarmupFrames = $InternalWarmupFrames
        FormalRunsPerVariant = 1
        SkipExternalWarmup = $true
        Workload = $workload
        Lights = "all"
        Mode = "hard"
        Sampling = "stable"
        RenderPath = "pbr-forward"
        SceneIds = $SceneIds
    }
    if ($workload -eq "change-caster-material") {
        $arguments.VariantAArguments = @("--classic-scene-untextured")
        $arguments.VariantBArguments = @("--classic-scene-untextured")
    }
    if ($SkipBuild -or $index -gt 0) {
        $arguments.SkipBuild = $true
    }
    if ($MsBuildPath) {
        $arguments.MsBuildPath = $MsBuildPath
    }

    Write-Host "Per-Light invalidation audit: $workload"
    & $runner @arguments
    foreach ($sceneId in $SceneIds) {
        $formalDirectory = Join-Path (
            Split-Path -Parent $PSScriptRoot
        ) (
            "benchmark-results\shadow-optimizations\$experimentId" +
            "\formal\$sceneId"
        )
        $beforeResult =
            Get-Content -LiteralPath (
                Join-Path $formalDirectory "A1.json"
            ) -Raw |
                ConvertFrom-Json
        $afterResult =
            Get-Content -LiteralPath (
                Join-Path $formalDirectory "B1.json"
            ) -Raw |
                ConvertFrom-Json
        $expectedPointPath = "six-face"
        $pointUpdates =
            [int64]$afterResult.shadow.measuredPointLightUpdateCount
        $pointEmptyClears = if ($afterResult.shadow.PSObject.Properties[
                "measuredEmptyShadowClearCount"
            ]) {
            [int64]$afterResult.shadow.measuredEmptyShadowClearCount / 3L
        }
        else {
            0L
        }
        $renderedPointUpdates = $pointUpdates - $pointEmptyClears
        if ($renderedPointUpdates -gt 0) {
            $actualPathUpdates = if ($expectedPointPath -eq "six-face") {
                [int64]$afterResult.shadow.measuredPointShadowSixFaceUpdateCount
            }
            else {
                [int64]$afterResult.shadow.measuredPointShadowLayeredUpdateCount
            }
            if ($actualPathUpdates -ne $renderedPointUpdates) {
                throw (
                    "$sceneId $workload did not exercise the expected " +
                    "$expectedPointPath point-shadow path."
                )
            }
        }
        $auditRecords += [pscustomobject][ordered]@{
            workload = $workload
            scene = $sceneId
            expectedPointPath = $expectedPointPath
            pointUpdates = $pointUpdates
            beforeUpdatedLights =
                [int64]$beforeResult.shadow.measuredUpdatedLightCount
            afterUpdatedLights =
                [int64]$afterResult.shadow.measuredUpdatedLightCount
            afterLightCacheHits =
                [int64]$afterResult.shadow.measuredLightCacheHitCount
            emptyShadowClears =
                [int64]$afterResult.shadow.measuredEmptyShadowClearCount
            resourceFailures =
                [int64]$afterResult.shadow.measuredShadowResourceFailureCount
            conservativeFallbacks =
                [int64]$afterResult.shadow.measuredConservativeShadowFallbackCount
        }
    }
    $completed += $experimentId
}

$manifestRoot = Join-Path (
    Split-Path -Parent $PSScriptRoot
) "benchmark-results\shadow-optimizations"
$manifestPath = Join-Path $manifestRoot "per-light-cache-invalidation-matrix.json"
$previousManifest = if (Test-Path -LiteralPath $manifestPath) {
    Get-Content -LiteralPath $manifestPath -Raw | ConvertFrom-Json
}
else {
    $null
}
$mergedExperiments = @(
    @(
        if ($previousManifest) {
            $previousManifest.experiments
        }
        $completed
    ) |
        Select-Object -Unique
)
$currentAuditKeys = @(
    $auditRecords |
        ForEach-Object { "$($_.workload)|$($_.scene)" }
)
$retainedAuditRecords = @(
    if ($previousManifest -and $previousManifest.auditRecords) {
        $previousManifest.auditRecords |
            Where-Object {
                "$($_.workload)|$($_.scene)" -notin $currentAuditKeys
            }
    }
)
$mergedAuditRecords = @($retainedAuditRecords) + @($auditRecords)
$manifest = [ordered]@{
    schemaVersion = 3
    createdUtc = [DateTime]::UtcNow.ToString("o")
    resolution = @($Width, $Height)
    measuredFrames = $MeasuredFrames
    externalWarmupFrames = $ExternalWarmupFrames
    internalWarmupFrames = $InternalWarmupFrames
    formalRunsPerVariant = 1
    externalWarmupPerformed = $false
    variantA = "no-cache-oracle"
    variantB = "per-light-revision"
    sceneIds = $SceneIds
    experiments = $mergedExperiments
    auditRecords = $mergedAuditRecords
}
$manifest |
    ConvertTo-Json -Depth 5 |
    Set-Content -LiteralPath $manifestPath -Encoding UTF8

Write-Host "Per-Light shadow-cache invalidation matrix passed."
Write-Host "Manifest: $manifestPath"
