param(
    [ValidateSet("All", "Capture", "Analyze", "Verify")]
    [string]$Mode = "All",
    [string]$RunDirectory = "",
    [string]$PythonExecutable = "",
    [ValidateRange(1, 1000000)]
    [int]$WarmupFrames = 30,
    [switch]$Resume
)

Set-StrictMode -Version Latest
$ErrorActionPreference = "Stop"

$projectDirectory = Split-Path -Parent $PSScriptRoot
$repositoryDirectory = Split-Path -Parent $projectDirectory
$executable = Join-Path $repositoryDirectory "x64\Release\OpenGL_Learn.exe"
if ([string]::IsNullOrWhiteSpace($RunDirectory)) {
    $RunDirectory = Join-Path $projectDirectory `
        "benchmark-results\tile-vs-cluster\tile-cluster-phase-a-20260804"
}
$RunDirectory = [System.IO.Path]::GetFullPath($RunDirectory)
$captureDirectory = Join-Path $RunDirectory "captures"
$logDirectory = Join-Path $RunDirectory "logs"
$manifestPath = Join-Path $RunDirectory "capture-manifest.json"
$protocolPath = Join-Path $RunDirectory "PHASE0_FROZEN_PROTOCOL_CN.md"

foreach ($directory in @($RunDirectory, $captureDirectory, $logDirectory)) {
    New-Item -ItemType Directory -Force -Path $directory | Out-Null
}
if (-not (Test-Path -LiteralPath $protocolPath -PathType Leaf)) {
    throw "Frozen protocol is missing: $protocolPath"
}

function Resolve-Python {
    if (-not [string]::IsNullOrWhiteSpace($PythonExecutable)) {
        return [System.IO.Path]::GetFullPath($PythonExecutable)
    }
    $bundled = "C:\Users\Link\.cache\codex-runtimes\codex-primary-runtime\dependencies\python\python.exe"
    if (Test-Path -LiteralPath $bundled -PathType Leaf) {
        return $bundled
    }
    foreach ($candidate in @("python", "python3")) {
        & $candidate --version *> $null
        if ($LASTEXITCODE -eq 0) { return $candidate }
    }
    throw "Python 3 with NumPy and Pillow was not found; pass -PythonExecutable."
}

$cases = @(
    [ordered]@{ coverage = "small-local"; lightCount = 64 },
    [ordered]@{ coverage = "medium-local"; lightCount = 64 },
    [ordered]@{ coverage = "representative"; lightCount = 256 },
    [ordered]@{ coverage = "high-overlap"; lightCount = 512 }
)

if ($Mode -in @("All", "Capture")) {
    if (-not (Test-Path -LiteralPath $executable -PathType Leaf)) {
        throw "Release executable is missing: $executable"
    }
    $launches = @()
    foreach ($case in $cases) {
        $stem = "{0}-{1:D4}" -f $case.coverage, $case.lightCount
        $caseDirectory = Join-Path $captureDirectory $stem
        New-Item -ItemType Directory -Force -Path $caseDirectory | Out-Null
        $resultPath = Join-Path $caseDirectory "scene.json"
        $ldrPath = Join-Path $caseDirectory "app-exact-analytic-screen.ppm"
        $positionPath = Join-Path $caseDirectory "position.pfm"
        $validityPath = Join-Path $caseDirectory "position-validity-alpha.pfm"
        $logPath = Join-Path $logDirectory "$stem-renderer.log"
        $required = @($resultPath, $ldrPath, $positionPath, $validityPath)
        $canResume = [bool]$Resume
        foreach ($path in $required) {
            if (-not (Test-Path -LiteralPath $path -PathType Leaf)) {
                $canResume = $false
                break
            }
        }
        if ($canResume) {
            $existing = Get-Content -LiteralPath $resultPath -Raw -Encoding UTF8 | ConvertFrom-Json
            if (-not [bool]$existing.success -or
                [string]$existing.pointLightStress.coverage -ne $case.coverage -or
                [int]$existing.pointLightStress.generatedLightCount -ne $case.lightCount -or
                [string]$existing.pointLightStress.renderMode -ne "analytic-screen" -or
                -not [bool]$existing.pointLightStress.renderModeExplicit -or
                [int]$existing.resolution[0] -ne 1920 -or
                [int]$existing.resolution[1] -ne 1080) {
                throw "Resume data is inconsistent: $resultPath"
            }
            Write-Host "SKIP existing $stem"
            $launches += [ordered]@{
                stem = $stem
                coverage = $case.coverage
                lightCount = $case.lightCount
                width = 1920
                height = 1080
                warmupFrames = $WarmupFrames
                sampleFrames = 1
                exitCode = 0
                resumed = $true
                result = $resultPath
                ldr = $ldrPath
                captures = [ordered]@{ position = $positionPath; validity = $validityPath }
                log = $logPath
            }
            continue
        }

        $arguments = @(
            "--gbuffer-position", "explicit",
            "--point-light-render-mode", "analytic-screen",
            "--point-light-offscreen-culling", "off",
            "--point-light-stencil-clear-mode", "coalesced-n-plus-one",
            "--point-light-bounds-telemetry",
            "--point-light-stencil-lifecycle-check",
            "--point-light-stress",
            "--point-light-count", [string]$case.lightCount,
            "--point-light-coverage", [string]$case.coverage,
            "--point-light-seed", "0x21D3F3A5",
            "--point-light-width", "1920",
            "--point-light-height", "1080",
            "--point-light-warmup-frames", [string]$WarmupFrames,
            "--point-light-sample-frames", "1",
            "--point-light-result", $resultPath,
            "--point-light-capture", $ldrPath,
            "--classic-scene-gbuffer-position-capture", $positionPath,
            "--classic-scene-ssao-depth-capture", $validityPath
        )

        Write-Host "CAPTURE $stem (1920x1080)"
        $startUtc = [DateTime]::UtcNow
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
        if ($exitCode -ne 0) {
            throw "$stem failed with exit code $exitCode; log=$logPath"
        }
        $result = Get-Content -LiteralPath $resultPath -Raw -Encoding UTF8 | ConvertFrom-Json
        if (-not [bool]$result.success -or
            [string]$result.pointLightStress.renderMode -ne "analytic-screen" -or
            -not [bool]$result.pointLightStress.renderModeExplicit -or
            [int]$result.pointLightStress.generatedLightCount -ne $case.lightCount -or
            [int]$result.resolution[0] -ne 1920 -or
            [int]$result.resolution[1] -ne 1080) {
            throw "$stem wrote an invalid or mismatched result"
        }
        foreach ($path in $required) {
            if (-not (Test-Path -LiteralPath $path -PathType Leaf)) {
                throw "$stem did not create $path"
            }
        }
        $launches += [ordered]@{
            stem = $stem
            coverage = $case.coverage
            lightCount = $case.lightCount
            width = 1920
            height = 1080
            warmupFrames = $WarmupFrames
            sampleFrames = 1
            startUtc = $startUtc.ToString("o")
            elapsedSeconds = [Math]::Round($stopwatch.Elapsed.TotalSeconds, 3)
            exitCode = $exitCode
            resumed = $false
            result = $resultPath
            ldr = $ldrPath
            captures = [ordered]@{ position = $positionPath; validity = $validityPath }
            log = $logPath
        }
    }

    $manifest = [ordered]@{
        schemaVersion = 1
        experiment = "exact-tile-vs-cluster-light-list-phase-a"
        protocolFrozenBeforeCapture = $true
        protocol = $protocolPath
        protocolSha256 = (Get-FileHash -LiteralPath $protocolPath -Algorithm SHA256).Hash
        captureManifestWrittenUtc = [DateTime]::UtcNow.ToString("o")
        executable = $executable
        executableSha256 = (Get-FileHash -LiteralPath $executable -Algorithm SHA256).Hash
        buildConfiguration = "Release"
        architecture = "x64"
        resolution = @(1920, 1080)
        seed = "0x21D3F3A5"
        renderMode = "analytic-screen"
        renderModeExplicit = $true
        pointShadows = $false
        tileSize = 16
        primaryClusterSlices = 16
        sensitivityClusterSlices = @(8, 16, 24, 32)
        runtimeCandidateImplemented = $false
        defaultChanged = $false
        cases = $launches
        sourceHashes = [ordered]@{
            pointLightGenerator = (Get-FileHash -LiteralPath `
                (Join-Path $projectDirectory "PointLightStressBenchmark.h") -Algorithm SHA256).Hash
            pointLightShader = (Get-FileHash -LiteralPath `
                (Join-Path $projectDirectory "shaders\pointLightLighting.glsl") -Algorithm SHA256).Hash
            analyticScreenShader = (Get-FileHash -LiteralPath `
                (Join-Path $projectDirectory "shaders\lightVolumeFullscreenFragment.glsl") -Algorithm SHA256).Hash
            analyzer = (Get-FileHash -LiteralPath `
                (Join-Path $PSScriptRoot "analyze_tile_cluster_phase_a.py") -Algorithm SHA256).Hash
            independentVerifier = (Get-FileHash -LiteralPath `
                (Join-Path $PSScriptRoot "verify_tile_cluster_phase_a.py") -Algorithm SHA256).Hash
            orchestrator = (Get-FileHash -LiteralPath $PSCommandPath -Algorithm SHA256).Hash
        }
    }
    $manifest | ConvertTo-Json -Depth 9 | Set-Content -LiteralPath $manifestPath -Encoding UTF8
}

$python = Resolve-Python
if ($Mode -in @("All", "Analyze")) {
    if (-not (Test-Path -LiteralPath $manifestPath -PathType Leaf)) {
        throw "Capture manifest is missing: $manifestPath"
    }
    $analysisLog = Join-Path $logDirectory "analysis.log"
    & $python (Join-Path $PSScriptRoot "analyze_tile_cluster_phase_a.py") `
        --run-dir $RunDirectory *> $analysisLog
    $exitCode = $LASTEXITCODE
    Get-Content -LiteralPath $analysisLog | Select-Object -Last 40
    if ($exitCode -ne 0) {
        throw "Tile/Cluster Phase A analysis failed: $exitCode; log=$analysisLog"
    }
}

if ($Mode -in @("All", "Verify")) {
    $verifyLog = Join-Path $logDirectory "independent-verification.log"
    & $python (Join-Path $PSScriptRoot "verify_tile_cluster_phase_a.py") `
        --run-dir $RunDirectory *> $verifyLog
    $exitCode = $LASTEXITCODE
    Get-Content -LiteralPath $verifyLog | Select-Object -Last 20
    if ($exitCode -ne 0) {
        throw "Independent Tile/Cluster verification failed: $exitCode; log=$verifyLog"
    }
    & $python (Join-Path $PSScriptRoot "analyze_tile_cluster_phase_a.py") `
        --run-dir $RunDirectory --report-only
    if ($LASTEXITCODE -ne 0) {
        throw "Final report regeneration failed: $LASTEXITCODE"
    }
}

Write-Host "Tile/Cluster Phase A run directory: $RunDirectory"
