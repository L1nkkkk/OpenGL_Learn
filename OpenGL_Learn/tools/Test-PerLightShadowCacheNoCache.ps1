[CmdletBinding()]
param(
    [switch]$SkipBuild,
    [string]$MsBuildPath,
    [string]$ExecutablePath,
    [string]$ExperimentId =
        "per-light-cache-no-cache-vs-per-light-1080p-six-face-final",
    [ValidateRange(4, 5000)]
    [int]$MeasuredFrames = 1000,
    [ValidateRange(4, 1000)]
    [int]$ExternalWarmupFrames = 100,
    [ValidateRange(1, 300)]
    [int]$InternalWarmupFrames = 15,
    [ValidateRange(1, 3)]
    [int]$FormalRunsPerVariant = 3
)

$ErrorActionPreference = "Stop"

$harness = Join-Path $PSScriptRoot "Test-ShadowOptimizations.ps1"
if (-not (Test-Path -LiteralPath $harness)) {
    throw "Missing benchmark harness: $harness"
}

$commonEnvironment = @{
    OPENGL_LEARN_SHADOW_CASTER_CULLING = "1"
    OPENGL_LEARN_POINT_SHADOW_RENDER_PATH = "six-face"
    OPENGL_LEARN_POINT_SHADOW_FACE_CULLING = "1"
    OPENGL_LEARN_SHADOW_SPOT_CASTER_DEPTH_FIT = "0"
}

$noCacheEnvironment = $commonEnvironment.Clone()
$noCacheEnvironment["OPENGL_LEARN_SHADOW_CACHE"] = "none"
$noCacheEnvironment["OPENGL_LEARN_SHADOW_PER_LIGHT_CACHE"] = "0"

$perLightEnvironment = $commonEnvironment.Clone()
$perLightEnvironment["OPENGL_LEARN_SHADOW_CACHE"] = "revision"
$perLightEnvironment["OPENGL_LEARN_SHADOW_PER_LIGHT_CACHE"] = "1"

$arguments = @{
    ExperimentId = $ExperimentId
    VariantALabel = "no-cache"
    VariantBLabel = "per-light-revision"
    VariantAEnvironment = $noCacheEnvironment
    VariantBEnvironment = $perLightEnvironment
    Width = 1920
    Height = 1080
    MeasuredFrames = $MeasuredFrames
    ExternalWarmupFrames = $ExternalWarmupFrames
    InternalWarmupFrames = $InternalWarmupFrames
    FormalRunsPerVariant = $FormalRunsPerVariant
    MaximumPixelChannelDelta = 255
    MaximumChangedPixels = 32
    Workload = "move-point"
    Lights = "all"
    Mode = "hard"
    Sampling = "stable"
    RenderPath = "pbr-forward"
    SceneIds = @("sponza", "san-miguel")
}

if ($SkipBuild) {
    $arguments["SkipBuild"] = $true
}
if ($MsBuildPath) {
    $arguments["MsBuildPath"] = $MsBuildPath
}
if ($ExecutablePath) {
    $arguments["ExecutablePath"] = $ExecutablePath
}

& $harness @arguments
if ($LASTEXITCODE -ne 0) {
    exit $LASTEXITCODE
}
