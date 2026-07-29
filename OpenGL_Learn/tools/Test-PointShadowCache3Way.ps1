[CmdletBinding()]
param(
    [switch]$SkipBuild,
    [string]$MsBuildPath,
    [string]$PythonPath,
    [switch]$SkipReport,
    [switch]$SkipCorrectnessAudit,
    [switch]$SkipExternalWarmup,
    [switch]$AllowDirtySource,
    [string]$BatchId = "point-shadow-cache-3way-1080p-final",
    [string]$ReportOutputDirectory,
    [string]$ReportPath,
    [ValidateRange(64, 16384)]
    [int]$Width = 1920,
    [ValidateRange(64, 16384)]
    [int]$Height = 1080,
    [ValidateRange(30, 5000)]
    [int]$MeasuredFrames = 1800,
    [ValidateRange(4, 1000)]
    [int]$ExternalWarmupFrames = 300,
    [ValidateRange(1, 300)]
    [int]$InternalWarmupFrames = 300,
    [ValidateRange(1, 3)]
    [int]$FormalRunsPerVariant = 3,
    [ValidateSet("sponza", "san-miguel")]
    [string[]]$SceneIds = @("sponza", "san-miguel"),
    [ValidateRange(1, 1000)]
    [int]$TimelineFps = 60,
    [ValidateRange(30, 36000)]
    [int]$TimelineCycleFrames = 1800
)

$ErrorActionPreference = "Stop"

function Resolve-ReportPython {
    $userProfileDirectory = [Environment]::GetFolderPath("UserProfile")
    $bundledPython = Join-Path $userProfileDirectory (
        ".cache\codex-runtimes\codex-primary-runtime\" +
        "dependencies\python\python.exe"
    )
    $pytorchPython = Join-Path $userProfileDirectory (
        "Anaconda3\envs\pytorch_cuda\python.exe"
    )
    $candidates = @($PythonPath, $bundledPython, $pytorchPython)
    $pythonCommand = Get-Command python -ErrorAction SilentlyContinue
    if ($pythonCommand) {
        $candidates += $pythonCommand.Source
    }
    foreach ($candidate in $candidates) {
        if (-not $candidate -or
            -not (Test-Path -LiteralPath $candidate)) {
            continue
        }
        $previousErrorActionPreference = $ErrorActionPreference
        try {
            $ErrorActionPreference = "SilentlyContinue"
            & $candidate -c "import matplotlib; import PIL" *> $null
            $probeExitCode = $LASTEXITCODE
        }
        finally {
            $ErrorActionPreference = $previousErrorActionPreference
        }
        if ($probeExitCode -eq 0) {
            return $candidate
        }
    }
    throw (
        "Python with matplotlib and Pillow was not found. " +
        "Pass -PythonPath <python.exe> or use -SkipReport."
    )
}

if ($BatchId -notmatch "^[A-Za-z0-9][A-Za-z0-9._-]*$") {
    throw "BatchId may contain only letters, numbers, dot, underscore, and dash."
}

$runner = Join-Path $PSScriptRoot "Test-ShadowOptimizations.ps1"
$projectDirectory = Split-Path -Parent $PSScriptRoot
$resultRoot = Join-Path (
    $projectDirectory
) "benchmark-results\shadow-optimizations"

$sharedEnvironment = @{
    OPENGL_LEARN_POINT_SHADOW_RENDER_PATH = "six-face"
    OPENGL_LEARN_POINT_SHADOW_FACE_CULLING = "1"
    OPENGL_LEARN_SHADOW_SPOT_CASTER_DEPTH_FIT = "0"
}
$globalDirtyEnvironment = $sharedEnvironment.Clone()
$globalDirtyEnvironment.OPENGL_LEARN_SHADOW_CACHE = "none"
$globalDirtyEnvironment.OPENGL_LEARN_SHADOW_PER_LIGHT_CACHE = "0"
$globalDirtyEnvironment.OPENGL_LEARN_SHADOW_SPATIAL_CASTER_CACHE = "0"
$globalDirtyEnvironment.OPENGL_LEARN_POINT_SHADOW_PER_FACE_CACHE = "0"
$globalDirtyEnvironment.OPENGL_LEARN_POINT_SHADOW_FORCE_ALL_REQUIRED = "0"

$perLightEnvironment = $sharedEnvironment.Clone()
$perLightEnvironment.OPENGL_LEARN_SHADOW_CACHE = "revision"
$perLightEnvironment.OPENGL_LEARN_SHADOW_PER_LIGHT_CACHE = "1"
$perLightEnvironment.OPENGL_LEARN_SHADOW_SPATIAL_CASTER_CACHE = "0"
$perLightEnvironment.OPENGL_LEARN_POINT_SHADOW_PER_FACE_CACHE = "0"
$perLightEnvironment.OPENGL_LEARN_POINT_SHADOW_FORCE_ALL_REQUIRED = "0"

$perFaceEnvironment = $sharedEnvironment.Clone()
$perFaceEnvironment.OPENGL_LEARN_SHADOW_CACHE = "revision"
$perFaceEnvironment.OPENGL_LEARN_SHADOW_PER_LIGHT_CACHE = "1"
$perFaceEnvironment.OPENGL_LEARN_SHADOW_SPATIAL_CASTER_CACHE = "1"
$perFaceEnvironment.OPENGL_LEARN_POINT_SHADOW_PER_FACE_CACHE = "1"
$perFaceEnvironment.OPENGL_LEARN_POINT_SHADOW_FORCE_ALL_REQUIRED = "0"

$globalVsPerLightId = "$BatchId-global-vs-per-light"
$perLightVsPerFaceId = "$BatchId-per-light-vs-per-face"
$correctnessId = "$BatchId-six-face-correctness"

function Invoke-PerformancePair {
    param(
        [string]$ExperimentId,
        [string]$VariantALabel,
        [string]$VariantBLabel,
        [hashtable]$VariantAEnvironment,
        [hashtable]$VariantBEnvironment,
        [switch]$BuildAlreadyCompleted
    )
    $arguments = @{
        ExperimentId = $ExperimentId
        VariantALabel = $VariantALabel
        VariantBLabel = $VariantBLabel
        VariantAEnvironment = $VariantAEnvironment
        VariantBEnvironment = $VariantBEnvironment
        Width = $Width
        Height = $Height
        MeasuredFrames = $MeasuredFrames
        ExternalWarmupFrames = $ExternalWarmupFrames
        InternalWarmupFrames = $InternalWarmupFrames
        FormalRunsPerVariant = $FormalRunsPerVariant
        MaximumPixelChannelDelta = 0
        MaximumChangedPixels = 0
        Workload = "timeline-cache-3way"
        Lights = "all"
        Mode = "hard"
        Sampling = "stable"
        RenderPath = "pbr-forward"
        SceneIds = $SceneIds
        TimelineFps = $TimelineFps
        TimelineCycleFrames = $TimelineCycleFrames
    }
    if ($SkipBuild -or $BuildAlreadyCompleted) {
        $arguments.SkipBuild = $true
    }
    if ($SkipExternalWarmup) {
        $arguments.SkipExternalWarmup = $true
    }
    if ($MsBuildPath) {
        $arguments.MsBuildPath = $MsBuildPath
    }
    & $runner @arguments
}

Write-Host "Three-way shadow cache: global dirty vs Per-Light"
Invoke-PerformancePair `
    -ExperimentId $globalVsPerLightId `
    -VariantALabel "A-global-dirty" `
    -VariantBLabel "B-per-light" `
    -VariantAEnvironment $globalDirtyEnvironment `
    -VariantBEnvironment $perLightEnvironment

Write-Host "Three-way shadow cache: Per-Light vs Point Per-Face"
Invoke-PerformancePair `
    -ExperimentId $perLightVsPerFaceId `
    -VariantALabel "B-per-light" `
    -VariantBLabel "C-per-light-face" `
    -VariantAEnvironment $perLightEnvironment `
    -VariantBEnvironment $perFaceEnvironment `
    -BuildAlreadyCompleted

if (-not $SkipCorrectnessAudit) {
    $auditControlEnvironment = $perLightEnvironment.Clone()
    $auditControlEnvironment.OPENGL_LEARN_POINT_SHADOW_FORCE_ALL_REQUIRED = "1"
    $auditCandidateEnvironment = $perFaceEnvironment.Clone()
    $auditCandidateEnvironment.OPENGL_LEARN_POINT_SHADOW_FORCE_ALL_REQUIRED = "1"
    $auditArguments = @{
        SkipBuild = $true
        SkipExternalWarmup = $true
        ExperimentId = $correctnessId
        VariantALabel = "B-six-face-oracle"
        VariantBLabel = "C-per-face-materialized"
        VariantAEnvironment = $auditControlEnvironment
        VariantBEnvironment = $auditCandidateEnvironment
        Width = $Width
        Height = $Height
        MeasuredFrames = 30
        InternalWarmupFrames = 10
        FormalRunsPerVariant = 1
        MaximumPixelChannelDelta = 0
        MaximumChangedPixels = 0
        Workload = "timeline-cache-3way"
        Lights = "all"
        Mode = "pcss"
        Sampling = "stable"
        RenderPath = "pbr-forward"
        SceneIds = $SceneIds
        TimelineFps = $TimelineFps
        TimelineCycleFrames = 60
    }
    Write-Host "Point Per-Face full-cubemap correctness audit"
    & $runner @auditArguments
}

function Read-ExperimentMetadata {
    param([string]$ExperimentId)
    $path = Join-Path $resultRoot "$ExperimentId\metadata.json"
    if (-not (Test-Path -LiteralPath $path -PathType Leaf)) {
        throw "Missing experiment metadata: $path"
    }
    return Get-Content -LiteralPath $path -Raw | ConvertFrom-Json
}

$primaryMetadata = Read-ExperimentMetadata $globalVsPerLightId
$secondaryMetadata = Read-ExperimentMetadata $perLightVsPerFaceId
$metadataSet = @($primaryMetadata, $secondaryMetadata)
if (-not $SkipCorrectnessAudit) {
    $metadataSet += Read-ExperimentMetadata $correctnessId
}
$gitHeads = @(
    $metadataSet |
        ForEach-Object { [string]$_.source.gitHead } |
        Sort-Object -Unique
)
$executableHashes = @(
    $metadataSet |
        ForEach-Object {
            [string]$_.executables.A.sha256
            [string]$_.executables.B.sha256
        } |
        Sort-Object -Unique
)
if ($gitHeads.Count -ne 1) {
    throw "Three-way experiments were not produced from one Git commit."
}
if ($executableHashes.Count -ne 1) {
    throw "Three-way experiments did not use one identical executable."
}
if (-not $AllowDirtySource -and
    @($metadataSet | Where-Object { [bool]$_.source.gitDirty }).Count -gt 0) {
    throw (
        "Formal three-way results require gitDirty=false. " +
        "Run from the clean frozen worktree, or use -AllowDirtySource only for smoke tests."
    )
}
if (-not $SkipCorrectnessAudit) {
    $auditSummaryPath = Join-Path $resultRoot "$correctnessId\summary.json"
    $auditSummary =
        Get-Content -LiteralPath $auditSummaryPath -Raw |
            ConvertFrom-Json
    foreach ($scene in @($auditSummary.scenes)) {
        foreach ($capture in @($scene.correctness.captureComparisons)) {
            if (-not [bool]$capture.exact) {
                throw "$($scene.displayName) Force-All final screenshot differs."
            }
        }
        foreach ($cube in @($scene.correctness.pointShadowCubeComparisons)) {
            if (-not [bool]$cube.exact -or
                @($cube.faces | Where-Object { -not [bool]$_.exact }).Count -gt 0) {
                throw "$($scene.displayName) Force-All six-face hash differs."
            }
        }
    }
}

$manifest = [ordered]@{
    schemaVersion = 2
    batchId = $BatchId
    createdUtc = [DateTime]::UtcNow.ToString("o")
    resolution = @($Width, $Height)
    measuredFrames = $MeasuredFrames
    externalWarmupFrames = $ExternalWarmupFrames
    internalWarmupFrames = $InternalWarmupFrames
    formalRunsPerVariant = $FormalRunsPerVariant
    scenes = $SceneIds
    workload = "timeline-cache-3way"
    timeline = [ordered]@{
        fixedFramesPerSecond = $TimelineFps
        cycleFrames = $TimelineCycleFrames
        phases = @(
            "point+camera",
            "local-caster+camera",
            "camera-only"
        )
    }
    variants = [ordered]@{
        A = "global-dirty"
        B = "per-light"
        C = "per-light-point-per-face"
    }
    provenance = [ordered]@{
        gitHead = $gitHeads[0]
        gitDirty = [bool]$primaryMetadata.source.gitDirty
        executableSha256 = $executableHashes[0]
        formalOrder = @($primaryMetadata.order)
        requestedSwapInterval = 0
    }
    experiments = [ordered]@{
        globalVsPerLight = $globalVsPerLightId
        perLightVsPerFace = $perLightVsPerFaceId
        sixFaceCorrectness = if ($SkipCorrectnessAudit) {
            $null
        }
        else {
            $correctnessId
        }
    }
}
$manifestPath = Join-Path $resultRoot "$BatchId-manifest.json"
$manifest |
    ConvertTo-Json -Depth 8 |
    Set-Content -LiteralPath $manifestPath -Encoding UTF8
Write-Host "Three-way manifest: $manifestPath"

if (-not $SkipReport) {
    $python = Resolve-ReportPython
    if (-not $ReportOutputDirectory) {
        $ReportOutputDirectory = Join-Path $projectDirectory (
            "docs\benchmark-images\shadow-optimizations\$BatchId"
        )
    }
    if (-not $ReportPath) {
        $ReportPath = Join-Path $projectDirectory (
            "POINT_SHADOW_CACHE_3WAY_REPORT_CN.md"
        )
    }
    $reportTool = Join-Path (
        $PSScriptRoot
    ) "generate_point_shadow_cache_3way_report.py"
    & $python $reportTool `
        --manifest $manifestPath `
        --output-dir $ReportOutputDirectory `
        --report $ReportPath
    if ($LASTEXITCODE -ne 0) {
        throw "Three-way Point shadow cache report generation failed."
    }
}
