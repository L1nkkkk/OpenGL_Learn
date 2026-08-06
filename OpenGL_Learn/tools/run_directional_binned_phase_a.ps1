param(
    [ValidateSet("All", "Capture", "Analyze")]
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
        "benchmark-results\directional-binned-lighting\directional-binned-phase-a-20260804"
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
    throw "Frozen Phase 0 protocol is missing: $protocolPath"
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
    throw "Python 3 with NumPy and Pillow was not found. Pass -PythonExecutable."
}

$cases = @(
    [ordered]@{ coverage = "small-local"; lightCount = 64 },
    [ordered]@{ coverage = "medium-local"; lightCount = 64 },
    [ordered]@{ coverage = "representative"; lightCount = 256 },
    [ordered]@{ coverage = "high-overlap"; lightCount = 256 }
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
        $ldrPath = Join-Path $caseDirectory "app-analytic-screen.ppm"
        $logPath = Join-Path $logDirectory "$stem.log"
        $capturePaths = [ordered]@{
            position = Join-Path $caseDirectory "position.pfm"
            validity = Join-Path $caseDirectory "position-validity-alpha.pfm"
            normal = Join-Path $caseDirectory "normal.pfm"
            albedo = Join-Path $caseDirectory "albedo.pfm"
            material = Join-Path $caseDirectory "material-rgb.pfm"
            materialAlpha = Join-Path $caseDirectory "material-alpha.pfm"
            emissive = Join-Path $caseDirectory "emissive.pfm"
        }
        $required = @($resultPath, $ldrPath) + @($capturePaths.Values)
        $canResume = $Resume
        foreach ($path in $required) {
            if (-not (Test-Path -LiteralPath $path -PathType Leaf)) {
                $canResume = $false
                break
            }
        }
        if ($canResume) {
            $existing = Get-Content -LiteralPath $resultPath -Encoding UTF8 -Raw |
                ConvertFrom-Json
            if (-not [bool]$existing.success -or
                [string]$existing.pointLightStress.coverage -ne $case.coverage -or
                [int]$existing.pointLightStress.generatedLightCount -ne $case.lightCount -or
                [string]$existing.pointLightStress.renderMode -ne "analytic-screen") {
                throw "Resume data is inconsistent: $resultPath"
            }
            Write-Host "SKIP existing $stem"
            $launches += [ordered]@{
                stem = $stem
                coverage = $case.coverage
                lightCount = $case.lightCount
                exitCode = 0
                resumed = $true
                result = $resultPath
                ldr = $ldrPath
                captures = $capturePaths
                log = $logPath
            }
            continue
        }

        $arguments = @(
            "--gbuffer-position", "explicit",
            "--point-light-stencil-clear-mode", "coalesced-n-plus-one",
            "--point-light-render-mode", "analytic-screen",
            "--point-light-offscreen-culling", "off",
            "--point-light-bounds-telemetry",
            "--point-light-stencil-lifecycle-check",
            "--point-light-stress",
            "--point-light-count", [string]$case.lightCount,
            "--point-light-coverage", [string]$case.coverage,
            "--point-light-seed", "0x21D3F3A5",
            "--point-light-width", "640",
            "--point-light-height", "360",
            "--point-light-warmup-frames", [string]$WarmupFrames,
            "--point-light-sample-frames", "1",
            "--point-light-result", $resultPath,
            "--point-light-capture", $ldrPath,
            "--classic-scene-gbuffer-position-capture", $capturePaths.position,
            "--classic-scene-ssao-depth-capture", $capturePaths.validity,
            "--classic-scene-gbuffer-normal-capture", $capturePaths.normal,
            "--classic-scene-gbuffer-albedo-capture", $capturePaths.albedo,
            "--classic-scene-gbuffer-material-capture", $capturePaths.material,
            "--classic-scene-gbuffer-material-alpha-capture", $capturePaths.materialAlpha,
            "--classic-scene-gbuffer-emissive-capture", $capturePaths.emissive
        )

        Write-Host "CAPTURE $stem"
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
        $result = Get-Content -LiteralPath $resultPath -Encoding UTF8 -Raw |
            ConvertFrom-Json
        if (-not [bool]$result.success -or
            [string]$result.pointLightStress.renderMode -ne "analytic-screen" -or
            -not [bool]$result.pointLightStress.renderModeExplicit -or
            [int]$result.pointLightStress.generatedLightCount -ne $case.lightCount) {
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
            width = 640
            height = 360
            warmupFrames = $WarmupFrames
            sampleFrames = 1
            startUtc = $startUtc.ToString("o")
            elapsedSeconds = [Math]::Round($stopwatch.Elapsed.TotalSeconds, 3)
            exitCode = $exitCode
            resumed = $false
            result = $resultPath
            ldr = $ldrPath
            captures = $capturePaths
            log = $logPath
        }
    }

    $manifest = [ordered]@{
        schemaVersion = 1
        experiment = "directional-binned-tile-cluster-phase-a"
        phase = "A-offline-diagnostic"
        protocolFrozenBeforeCapture = $true
        protocol = $protocolPath
        protocolSha256 = (Get-FileHash -LiteralPath $protocolPath -Algorithm SHA256).Hash
        executable = $executable
        executableSha256 = (Get-FileHash -LiteralPath $executable -Algorithm SHA256).Hash
        buildConfiguration = "Release"
        architecture = "x64"
        resolution = @(640, 360)
        seed = "0x21D3F3A5"
        renderMode = "analytic-screen"
        defaultChanged = $false
        cases = $launches
        sourceHashes = [ordered]@{
            pointLightGenerator = (Get-FileHash -LiteralPath `
                (Join-Path $projectDirectory "PointLightStressBenchmark.h") `
                -Algorithm SHA256).Hash
            pointLightShader = (Get-FileHash -LiteralPath `
                (Join-Path $projectDirectory "shaders\pointLightLighting.glsl") `
                -Algorithm SHA256).Hash
            analyticScreenShader = (Get-FileHash -LiteralPath `
                (Join-Path $projectDirectory "shaders\lightVolumeFullscreenFragment.glsl") `
                -Algorithm SHA256).Hash
        }
    }
    $manifest | ConvertTo-Json -Depth 8 |
        Set-Content -LiteralPath $manifestPath -Encoding UTF8
}

if ($Mode -in @("All", "Analyze")) {
    if (-not (Test-Path -LiteralPath $manifestPath -PathType Leaf)) {
        throw "Capture manifest is missing: $manifestPath"
    }
    $python = Resolve-Python
    & $python (Join-Path $PSScriptRoot "analyze_directional_binned_phase_a.py") `
        --run-dir $RunDirectory
    if ($LASTEXITCODE -ne 0) {
        throw "Directional-binned Phase A analysis failed: $LASTEXITCODE"
    }
}
