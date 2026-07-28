[CmdletBinding()]
param(
    [switch]$SkipBuild,
    [string]$MsBuildPath,
    [string]$PythonPath,
    [switch]$SkipReport,
    [string]$ReportOutputDirectory,
    [string]$BatchId = "per-light-cache-motion-timeline",
    [ValidateRange(64, 16384)]
    [int]$Width = 1920,
    [ValidateRange(64, 16384)]
    [int]$Height = 1080,
    [ValidateRange(4, 5000)]
    [int]$MeasuredFrames = 1000,
    [ValidateRange(4, 1000)]
    [int]$ExternalWarmupFrames = 100,
    [ValidateRange(1, 300)]
    [int]$InternalWarmupFrames = 15,
    [ValidateRange(1, 3)]
    [int]$FormalRunsPerVariant = 3,
    [switch]$SkipExternalWarmup,
    [ValidateSet("sponza", "san-miguel")]
    [string[]]$SceneIds = @("sponza", "san-miguel"),
    [ValidateSet("point", "caster", "camera", "mixed")]
    [string[]]$Profiles = @("point", "camera", "caster", "mixed"),
    [ValidateRange(1, 1000)]
    [int]$TimelineFps = 60,
    [ValidateRange(4, 36000)]
    [int]$TimelineCycleFrames = 600
)

$ErrorActionPreference = "Stop"

function Resolve-ReportPython {
    $userProfileDirectory = [Environment]::GetFolderPath("UserProfile")
    $pytorchPython = Join-Path $userProfileDirectory (
        "Anaconda3\envs\pytorch_cuda\python.exe"
    )
    $bundledPython = Join-Path $userProfileDirectory (
        ".cache\codex-runtimes\codex-primary-runtime\" +
        "dependencies\python\python.exe"
    )
    $candidates = @($PythonPath, $pytorchPython, $bundledPython)
    $pythonCommand = Get-Command python -ErrorAction SilentlyContinue
    if ($pythonCommand) {
        $candidates += $pythonCommand.Source
    }
    $previousErrorActionPreference = $ErrorActionPreference
    foreach ($candidate in $candidates) {
        if (-not $candidate -or
            -not (Test-Path -LiteralPath $candidate)) {
            continue
        }
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

$completed = @()
for ($index = 0; $index -lt $Profiles.Count; ++$index) {
    $profile = $Profiles[$index]
    $workload = "timeline-$profile"
    $experimentId =
        "$BatchId-$profile-${Width}x${Height}"
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
        FormalRunsPerVariant = $FormalRunsPerVariant
        Workload = $workload
        Lights = "all"
        Mode = "hard"
        Sampling = "stable"
        RenderPath = "pbr-forward"
        SceneIds = $SceneIds
        TimelineFps = $TimelineFps
        TimelineCycleFrames = $TimelineCycleFrames
    }
    if ($SkipExternalWarmup) {
        $arguments.SkipExternalWarmup = $true
    }
    if ($SkipBuild -or $index -gt 0) {
        $arguments.SkipBuild = $true
    }
    if ($MsBuildPath) {
        $arguments.MsBuildPath = $MsBuildPath
    }

    Write-Host "Deterministic shadow timeline: $profile"
    & $runner @arguments
    $completed += [ordered]@{
        profile = $profile
        workload = $workload
        experimentId = $experimentId
        directory = (
            "benchmark-results/shadow-optimizations/$experimentId"
        )
    }
}

$manifest = [ordered]@{
    schemaVersion = 1
    batchId = $BatchId
    createdUtc = [DateTime]::UtcNow.ToString("o")
    resolution = @($Width, $Height)
    measuredFrames = $MeasuredFrames
    externalWarmupFrames = $ExternalWarmupFrames
    internalWarmupFrames = $InternalWarmupFrames
    formalRunsPerVariant = $FormalRunsPerVariant
    scenes = $SceneIds
    timeline = [ordered]@{
        fixedFramesPerSecond = $TimelineFps
        cycleFrames = $TimelineCycleFrames
    }
    experiments = $completed
}
$manifestPath = Join-Path (
    Join-Path $projectDirectory "benchmark-results\shadow-optimizations"
) "$BatchId-manifest.json"
$manifest |
    ConvertTo-Json -Depth 8 |
    Set-Content -LiteralPath $manifestPath -Encoding UTF8

Write-Host "Shadow motion timeline manifest: $manifestPath"

if (-not $SkipReport) {
    $python = Resolve-ReportPython
    if (-not $ReportOutputDirectory) {
        $ReportOutputDirectory = Join-Path $projectDirectory (
            "docs\benchmark-images\shadow-motion-timeline\$BatchId"
        )
    }
    $reportTool = Join-Path (
        $PSScriptRoot
    ) "generate_shadow_motion_timeline_report.py"
    & $python $reportTool `
        --manifest $manifestPath `
        --output-dir $ReportOutputDirectory
    if ($LASTEXITCODE -ne 0) {
        throw "Shadow motion timeline report generation failed."
    }
}
