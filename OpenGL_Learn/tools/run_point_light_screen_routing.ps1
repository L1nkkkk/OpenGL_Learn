param(
    [ValidateSet("All", "Formal", "Correctness", "Analyze")]
    [string]$Mode = "All",
    [string]$RunDirectory = "",
    [ValidateRange(3, 5)]
    [int]$FormalRuns = 3,
    [ValidateRange(1, 1000000)]
    [int]$WarmupFrames = 300,
    [ValidateRange(1, 1000000)]
    [int]$SampleFrames = 600,
    [string]$PythonExecutable = "",
    [switch]$SkipBuild,
    [switch]$Resume
)

Set-StrictMode -Version Latest
$ErrorActionPreference = "Stop"

$projectDirectory = Split-Path -Parent $PSScriptRoot
$repositoryDirectory = Split-Path -Parent $projectDirectory
$executable = Join-Path $repositoryDirectory "x64\Release\OpenGL_Learn.exe"
if ([string]::IsNullOrWhiteSpace($RunDirectory)) {
    $stamp = Get-Date -Format "yyyyMMdd-HHmmss"
    $RunDirectory = Join-Path $projectDirectory "benchmark-results\point-light-screen-routing\screen-routing-$stamp"
}
$RunDirectory = [System.IO.Path]::GetFullPath($RunDirectory)
$rawDirectory = Join-Path $RunDirectory "raw"
$logDirectory = Join-Path $RunDirectory "logs"
$imageDirectory = Join-Path $RunDirectory "images"
$scriptDirectory = Join-Path $RunDirectory "scripts"
$launchOrderPath = Join-Path $RunDirectory "launch-order.jsonl"
$manifestPath = Join-Path $RunDirectory "manifest.json"

foreach ($directory in @($RunDirectory, $rawDirectory, $logDirectory, $imageDirectory, $scriptDirectory)) {
    New-Item -ItemType Directory -Force -Path $directory | Out-Null
}

function Resolve-Python {
    if (-not [string]::IsNullOrWhiteSpace($PythonExecutable)) {
        return $PythonExecutable
    }
    foreach ($candidate in @("python", "python3")) {
        & $candidate --version *> $null
        if ($LASTEXITCODE -eq 0) { return $candidate }
    }
    throw "Python 3 was not found. Pass -PythonExecutable explicitly."
}

if (-not $SkipBuild -and $Mode -ne "Analyze") {
    $vswhere = Join-Path ${env:ProgramFiles(x86)} "Microsoft Visual Studio\Installer\vswhere.exe"
    if (-not (Test-Path -LiteralPath $vswhere -PathType Leaf)) {
        throw "vswhere.exe was not found: $vswhere"
    }
    $msbuild = & $vswhere -latest -products * -requires Microsoft.Component.MSBuild -find "MSBuild\**\Bin\MSBuild.exe" |
        Select-Object -First 1
    if ([string]::IsNullOrWhiteSpace($msbuild)) { throw "MSBuild was not found" }
    & $msbuild (Join-Path $repositoryDirectory "OpenGL_Learn.sln") /m /p:Configuration=Release /p:Platform=x64 /v:minimal /nologo
    if ($LASTEXITCODE -ne 0) { throw "Release x64 build failed: $LASTEXITCODE" }
}
if (-not (Test-Path -LiteralPath $executable -PathType Leaf)) {
    throw "Release executable is missing: $executable"
}

$script:launchIndex = if (Test-Path -LiteralPath $launchOrderPath) {
    @(Get-Content -LiteralPath $launchOrderPath).Count
}
else { 0 }

function Invoke-PointLightCase {
    param(
        [Parameter(Mandatory)] [string]$Phase,
        [Parameter(Mandatory)] [string]$Classification,
        [Parameter(Mandatory)] [string]$Coverage,
        [Parameter(Mandatory)] [int]$LightCount,
        [Parameter(Mandatory)] [string]$RenderMode,
        [Parameter(Mandatory)] [ValidateSet("on", "off")] [string]$Culling,
        [Parameter(Mandatory)] [int]$RunIndex,
        [Parameter(Mandatory)] [int]$Warmup,
        [Parameter(Mandatory)] [int]$Samples,
        [int]$Width = 1920,
        [int]$Height = 1080,
        [switch]$BoundsTelemetry,
        [switch]$LifecycleCheck
    )

    $stem = "{0}-{1}-{2}-{3:D4}-{4}-cull-{5}-{6}x{7}-run{8:D2}" -f `
        $Classification, $Phase, $Coverage, $LightCount, $RenderMode, $Culling, $Width, $Height, $RunIndex
    $jsonPath = Join-Path $rawDirectory "$stem.json"
    $capturePath = Join-Path $imageDirectory "$stem.ppm"
    $logPath = Join-Path $logDirectory "$stem.log"
    if ($Resume -and (Test-Path -LiteralPath $jsonPath -PathType Leaf)) {
        $existing = Get-Content -Raw -LiteralPath $jsonPath | ConvertFrom-Json
        if ([bool]$existing.success -and
            [string]$existing.pointLightStress.renderMode -eq $RenderMode -and
            [int]$existing.pointLightStress.generatedLightCount -eq $LightCount) {
            Write-Host "SKIP existing $stem"
            return
        }
        throw "Existing result is invalid for resume: $jsonPath"
    }

    $arguments = @(
        "--gbuffer-position", "explicit",
        "--point-light-stencil-clear-mode", "coalesced-n-plus-one",
        "--point-light-render-mode", $RenderMode,
        "--point-light-offscreen-culling", $Culling,
        "--point-light-stress",
        "--point-light-count", [string]$LightCount,
        "--point-light-coverage", $Coverage,
        "--point-light-seed", "0x21D3F3A5",
        "--point-light-width", [string]$Width,
        "--point-light-height", [string]$Height,
        "--point-light-warmup-frames", [string]$Warmup,
        "--point-light-sample-frames", [string]$Samples,
        "--point-light-result", $jsonPath,
        "--point-light-capture", $capturePath
    )
    if ($BoundsTelemetry) { $arguments += "--point-light-bounds-telemetry" }
    if ($LifecycleCheck) { $arguments += "--point-light-stencil-lifecycle-check" }

    $script:launchIndex++
    $startUtc = [DateTime]::UtcNow
    Write-Host ("[{0:D3}] {1:o} {2}" -f $script:launchIndex, $startUtc, $stem)
    $stopwatch = [System.Diagnostics.Stopwatch]::StartNew()
    Push-Location $projectDirectory
    try {
        & $executable @arguments *> $logPath
        $exitCode = $LASTEXITCODE
    }
    finally {
        Pop-Location
        $stopwatch.Stop()
    }
    $endUtc = [DateTime]::UtcNow
    $record = [ordered]@{
        launchIndex = $script:launchIndex
        startUtc = $startUtc.ToString("o")
        endUtc = $endUtc.ToString("o")
        elapsedSeconds = [Math]::Round($stopwatch.Elapsed.TotalSeconds, 3)
        phase = $Phase
        classification = $Classification
        coverage = $Coverage
        lightCount = $LightCount
        renderMode = $RenderMode
        culling = $Culling
        width = $Width
        height = $Height
        run = $RunIndex
        warmupFrames = $Warmup
        sampleFrames = $Samples
        boundsTelemetry = [bool]$BoundsTelemetry
        lifecycleCheck = [bool]$LifecycleCheck
        exitCode = $exitCode
        json = $jsonPath
        capture = $capturePath
        log = $logPath
    }
    ($record | ConvertTo-Json -Compress) | Add-Content -LiteralPath $launchOrderPath -Encoding utf8
    if ($exitCode -ne 0) { throw "$stem failed with exit code $exitCode; log=$logPath" }
    $result = Get-Content -Raw -LiteralPath $jsonPath | ConvertFrom-Json
    if (-not [bool]$result.success) { throw "$stem wrote success=false" }
    if ([string]$result.pointLightStress.renderMode -ne $RenderMode -or
        -not [bool]$result.pointLightStress.renderModeExplicit -or
        -not [bool]$result.pointLightStress.stencilClearModeExplicit) {
        throw "$stem did not record the explicit requested modes"
    }
}

function Invoke-BalancedPair {
    param(
        [string]$Phase,
        [string]$Coverage,
        [int]$LightCount,
        [string]$ModeA,
        [string]$ModeB,
        [string]$CullA = "off",
        [string]$CullB = "off"
    )
    for ($run = 1; $run -le $FormalRuns; ++$run) {
        $pair = if (($run % 2) -eq 1) {
            @(@($ModeA, $CullA), @($ModeB, $CullB))
        }
        else {
            @(@($ModeB, $CullB), @($ModeA, $CullA))
        }
        foreach ($entry in $pair) {
            Invoke-PointLightCase -Phase $Phase -Classification "formal" `
                -Coverage $Coverage -LightCount $LightCount `
                -RenderMode $entry[0] -Culling $entry[1] -RunIndex $run `
                -Warmup $WarmupFrames -Samples $SampleFrames
        }
    }
}

if ($Mode -in @("All", "Correctness")) {
    foreach ($coverage in @("small-local", "medium-local", "representative", "high-overlap", "edge-cases")) {
        foreach ($renderMode in @("coalesced-volume", "scissored-volume", "analytic-volume", "analytic-screen", "analytic-fullscreen")) {
            Invoke-PointLightCase -Phase "quality" -Classification "quality" `
                -Coverage $coverage -LightCount 16 -RenderMode $renderMode `
                -Culling "off" -RunIndex 1 -Warmup 30 -Samples 1 `
                -BoundsTelemetry -LifecycleCheck
        }
    }
    foreach ($size in @(@(1280, 720), @(1280, 1024), @(3440, 1440))) {
        Invoke-PointLightCase -Phase "bounds-aspect" -Classification "quality" `
            -Coverage "edge-cases" -LightCount 16 -RenderMode "bounds-volume" `
            -Culling "off" -RunIndex 1 -Warmup 2 -Samples 1 `
            -Width $size[0] -Height $size[1] -BoundsTelemetry -LifecycleCheck
    }
}

if ($Mode -in @("All", "Formal")) {
    Invoke-BalancedPair -Phase "A-bounds-overhead" -Coverage "representative" `
        -LightCount 16 -ModeA "coalesced-volume" -ModeB "bounds-volume"
    Invoke-BalancedPair -Phase "A-offscreen-culling" -Coverage "edge-cases" `
        -LightCount 16 -ModeA "bounds-volume" -ModeB "bounds-volume" `
        -CullA "off" -CullB "on"
    foreach ($count in @(16, 256, 512)) {
        Invoke-BalancedPair -Phase "B-scissor" -Coverage "representative" `
            -LightCount $count -ModeA "coalesced-volume" -ModeB "scissored-volume"
    }
    Invoke-BalancedPair -Phase "C0-semantic-unification" -Coverage "representative" `
        -LightCount 256 -ModeA "scissored-volume" -ModeB "analytic-volume"
    foreach ($coverage in @("small-local", "medium-local", "representative", "high-overlap")) {
        Invoke-BalancedPair -Phase "C-fixed-path" -Coverage $coverage `
            -LightCount 256 -ModeA "analytic-volume" -ModeB "analytic-screen"
    }
}

$hash = (Get-FileHash -Algorithm SHA256 -LiteralPath $executable).Hash
$launchOrder = @()
if (Test-Path -LiteralPath $launchOrderPath) {
    $launchOrder = @(Get-Content -LiteralPath $launchOrderPath | ForEach-Object { $_ | ConvertFrom-Json })
}
$manifest = [ordered]@{
    schemaVersion = 1
    generatedUtc = [DateTime]::UtcNow.ToString("o")
    runDirectory = $RunDirectory
    executable = $executable
    executableSha256 = $hash
    protocol = [ordered]@{
        configuration = "Release"
        architecture = "x64"
        resolution = @(1920, 1080)
        warmupFrames = $WarmupFrames
        sampleFrames = $SampleFrames
        independentProcesses = $FormalRuns
        seed = "0x21D3F3A5"
        gBufferPosition = "explicit"
        deferred = $true
        requestedSwapInterval = 0
        ssao = $false
        bloom = $false
        pointShadows = $false
        performanceTelemetryCapture = $false
        qualityRunsSeparate = $true
    }
    launchOrder = $launchOrder
}
$manifest | ConvertTo-Json -Depth 10 | Set-Content -LiteralPath $manifestPath -Encoding utf8

foreach ($scriptName in @("run_point_light_screen_routing.ps1", "analyze_point_light_screen_routing.py")) {
    $source = Join-Path $PSScriptRoot $scriptName
    if (Test-Path -LiteralPath $source -PathType Leaf) {
        Copy-Item -LiteralPath $source -Destination (Join-Path $scriptDirectory $scriptName) -Force
    }
}

if ($Mode -in @("All", "Formal", "Correctness", "Analyze")) {
    $python = Resolve-Python
    & $python (Join-Path $PSScriptRoot "analyze_point_light_screen_routing.py") --run-dir $RunDirectory
    if ($LASTEXITCODE -ne 0) { throw "Analysis failed: $LASTEXITCODE" }
}

Write-Host "Point-light screen-routing run directory: $RunDirectory"
