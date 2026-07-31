param(
    [ValidateRange(1, 40000)]
    [int]$ObjectCount = 30000,

    [ValidateRange(0, 100)]
    [int]$DynamicPercent = 20,

    [ValidateRange(1, 64)]
    [int]$MaterialCount = 16,

    [ValidateRange(0, 1000000)]
    [int]$WarmupFrames = 300,

    [ValidateRange(1, 1000000)]
    [int]$SampleFrames = 800,

    [ValidateSet("legacy", "key-direct", "key-index")]
    [string]$OpaqueSortMode = "key-index",

    [ValidateSet("forward", "deferred")]
    [string]$RenderPath = "forward",

    [ValidateSet("quad", "mixed")]
    [string]$GeometrySet = "quad",

    [switch]$CollectionBreakdown,

    [string]$Label = "submission-stress",

    [string]$Output = "benchmark-results/submission-stress/submission-stress.json",

    [string]$CapturePath = ""
)

$projectDirectory = Split-Path -Parent $PSScriptRoot
$repositoryDirectory = Split-Path -Parent $projectDirectory
$executable = Join-Path $repositoryDirectory "x64\Release\OpenGL_Learn.exe"

if (-not (Test-Path -LiteralPath $executable -PathType Leaf)) {
    throw "Release executable not found: $executable"
}

Push-Location $projectDirectory
try {
    $arguments = @(
        "--submission-stress-scene",
        "--stress-object-count", $ObjectCount,
        "--stress-dynamic-percent", $DynamicPercent,
        "--stress-material-count", $MaterialCount,
        "--stress-width", 1920,
        "--stress-height", 1080,
        "--stress-seed", "0x5eed1234",
        "--opaque-sort-mode", $OpaqueSortMode,
        "--stress-render-path", $RenderPath,
        "--stress-geometry-set", $GeometrySet,
        "--performance-benchmark",
        "--benchmark-warmup-frames", $WarmupFrames,
        "--benchmark-sample-frames", $SampleFrames,
        "--benchmark-label", $Label,
        "--benchmark-output", $Output
    )
    if (-not [string]::IsNullOrWhiteSpace($CapturePath)) {
        $arguments += @("--stress-capture-path", $CapturePath)
    }
    if ($CollectionBreakdown) {
        $arguments += "--stress-collection-breakdown"
    }

    & $executable @arguments

    if ($LASTEXITCODE -ne 0) {
        throw "Submission stress benchmark failed with exit code $LASTEXITCODE"
    }
}
finally {
    Pop-Location
}
