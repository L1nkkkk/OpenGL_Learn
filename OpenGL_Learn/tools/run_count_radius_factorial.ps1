param(
    [ValidateSet("All", "Build", "Capture", "Analyze", "Timing", "Verify", "Finalize")]
    [string]$Mode = "All",
    [string]$RunDirectory = "",
    [string]$PythonExecutable = "",
    [switch]$Resume
)

Set-StrictMode -Version Latest
$ErrorActionPreference = "Stop"

$projectDirectory = Split-Path -Parent $PSScriptRoot
$repositoryDirectory = Split-Path -Parent $projectDirectory
$solutionPath = Join-Path $repositoryDirectory "OpenGL_Learn.sln"
$executable = Join-Path $repositoryDirectory "x64\Release\OpenGL_Learn.exe"
$msbuild = "C:\Program Files\Microsoft Visual Studio\2022\Community\MSBuild\Current\Bin\MSBuild.exe"
if ([string]::IsNullOrWhiteSpace($RunDirectory)) {
    $RunDirectory = Join-Path $projectDirectory `
        "benchmark-results\point-light-count-radius-factorial\count-radius-factorial-20260805"
}
$RunDirectory = [System.IO.Path]::GetFullPath($RunDirectory)
$protocolPath = Join-Path $RunDirectory "PHASE0_FROZEN_PROTOCOL_CN.md"
$captureDirectory = Join-Path $RunDirectory "captures"
$captureLogDirectory = Join-Path $RunDirectory "logs\capture"
$timingDirectory = Join-Path $RunDirectory "timing"
$timingLogDirectory = Join-Path $RunDirectory "logs\timing"
$captureManifestPath = Join-Path $RunDirectory "capture-manifest.json"
$timingManifestPath = Join-Path $RunDirectory "timing-manifest.json"

$counts = @(32, 64, 128, 256, 512)
$radii = @(1.5, 3.0, 6.0, 12.0)
$seeds = @(
    [ordered]@{ ordinal = 0; text = "0x21D3F3A5"; value = [Convert]::ToUInt32("21D3F3A5", 16) },
    [ordered]@{ ordinal = 1; text = "0xA511E9B3"; value = [Convert]::ToUInt32("A511E9B3", 16) },
    [ordered]@{ ordinal = 2; text = "0xC0FFEE11"; value = [Convert]::ToUInt32("C0FFEE11", 16) }
)

foreach ($directory in @(
    $RunDirectory, $captureDirectory, $captureLogDirectory,
    $timingDirectory, $timingLogDirectory)) {
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
    if (Test-Path -LiteralPath $bundled -PathType Leaf) { return $bundled }
    foreach ($candidate in @("python", "python3")) {
        & $candidate --version *> $null
        if ($LASTEXITCODE -eq 0) { return $candidate }
    }
    throw "Python 3 with NumPy and Pillow was not found."
}

function Format-Radius([double]$Radius) {
    return $Radius.ToString("0.0", [Globalization.CultureInfo]::InvariantCulture)
}

function Get-CaseStem([int]$SeedOrdinal, [int]$Count, [double]$Radius) {
    $radiusCode = [int][Math]::Round($Radius * 10.0)
    return "s{0}-n{1:D4}-r{2:D3}" -f $SeedOrdinal, $Count, $radiusCode
}

function Write-JsonFile([string]$Path, $Value) {
    $Value | ConvertTo-Json -Depth 20 | Set-Content -LiteralPath $Path -Encoding UTF8
}

function Get-DirectoryTreeSha256([string]$Root) {
    $lines = @()
    foreach ($file in Get-ChildItem -LiteralPath $Root -File -Recurse | Sort-Object FullName) {
        $relative = $file.FullName.Substring($projectDirectory.Length).TrimStart('\')
        $hash = (Get-FileHash -LiteralPath $file.FullName -Algorithm SHA256).Hash
        $lines += "$relative|$hash"
    }
    $bytes = [Text.Encoding]::UTF8.GetBytes(($lines -join "`n"))
    $sha = [Security.Cryptography.SHA256]::Create()
    try {
        return ([BitConverter]::ToString($sha.ComputeHash($bytes))).Replace("-", "")
    }
    finally {
        $sha.Dispose()
    }
}

function Get-ShaderTreeSha256 {
    return (Get-DirectoryTreeSha256 -Root (Join-Path $projectDirectory "shaders"))
}

function Get-SponzaTreeSha256 {
    return (Get-DirectoryTreeSha256 -Root (Join-Path $projectDirectory "classic-scenes\sponza"))
}

function Invoke-ReleaseBuild {
    if (-not (Test-Path -LiteralPath $msbuild -PathType Leaf)) {
        throw "MSBuild is missing: $msbuild"
    }
    $logPath = Join-Path $RunDirectory "logs\release-build.log"
    & $msbuild $solutionPath /t:Build /p:Configuration=Release /p:Platform=x64 /m /v:minimal *> $logPath
    if ($LASTEXITCODE -ne 0) {
        Get-Content -LiteralPath $logPath | Select-Object -Last 80
        throw "Release build failed: $LASTEXITCODE"
    }
    if (-not (Test-Path -LiteralPath $executable -PathType Leaf)) {
        throw "Release executable is missing after build: $executable"
    }
    Write-Host "Release build passed: $executable"
}

function Invoke-Renderer(
    [string]$ResultPath,
    [string]$CapturePath,
    [string]$LogPath,
    [int]$Count,
    [double]$Radius,
    [string]$SeedText,
    [int]$Warmup,
    [int]$Samples,
    [bool]$CaptureGBuffer,
    [string]$PositionPath,
    [string]$ValidityPath,
    [bool]$ExplicitRenderMode,
    [bool]$ExplicitTargetRadius)
{
    $arguments = @(
        "--gbuffer-position", "explicit",
        "--point-light-offscreen-culling", "off",
        "--point-light-stencil-clear-mode", "coalesced-n-plus-one",
        "--point-light-bounds-telemetry",
        "--point-light-stress",
        "--point-light-count", [string]$Count,
        "--point-light-coverage", "representative",
        "--point-light-seed", $SeedText,
        "--point-light-width", "1920",
        "--point-light-height", "1080",
        "--point-light-warmup-frames", [string]$Warmup,
        "--point-light-sample-frames", [string]$Samples,
        "--point-light-result", $ResultPath,
        "--point-light-capture", $CapturePath
    )
    if ($ExplicitRenderMode) {
        $arguments = @("--point-light-render-mode", "analytic-screen") + $arguments
    }
    if ($ExplicitTargetRadius) {
        $arguments += @("--point-light-target-radius", (Format-Radius $Radius))
    }
    if ($CaptureGBuffer) {
        $arguments += @(
            "--classic-scene-gbuffer-position-capture", $PositionPath,
            "--classic-scene-ssao-depth-capture", $ValidityPath
        )
    }
    $startUtc = [DateTime]::UtcNow
    $stopwatch = [Diagnostics.Stopwatch]::StartNew()
    Push-Location $projectDirectory
    try {
        & $executable @arguments *> $LogPath
        $exitCode = $LASTEXITCODE
    }
    finally {
        Pop-Location
        $stopwatch.Stop()
    }
    return [ordered]@{
        exitCode = $exitCode
        startUtc = $startUtc.ToString("o")
        elapsedSeconds = [Math]::Round($stopwatch.Elapsed.TotalSeconds, 3)
        arguments = $arguments
    }
}

function Assert-FormalResult(
    [string]$ResultPath,
    [int]$Count,
    [double]$Radius,
    [uint32]$Seed,
    [int]$ExpectedSamples)
{
    if (-not (Test-Path -LiteralPath $ResultPath -PathType Leaf)) {
        throw "Renderer result is missing: $ResultPath"
    }
    $result = Get-Content -LiteralPath $ResultPath -Raw -Encoding UTF8 | ConvertFrom-Json
    $point = $result.pointLightStress
    if (-not [bool]$result.success -or
        [string]$result.buildConfiguration -ne "Release" -or
        [string]$result.architecture -ne "x64" -or
        [string]$point.coverage -ne "representative" -or
        [string]$point.renderMode -ne "analytic-screen" -or
        -not [bool]$point.renderModeExplicit -or
        [bool]$point.offscreenCulling -or
        -not [bool]$point.offscreenCullingExplicit -or
        [string]$point.stencilClearMode -ne "coalesced-n-plus-one" -or
        -not [bool]$point.stencilClearModeExplicit -or
        [string]$point.generatorVersion -ne "point-light-count-radius-xorshift32-v2" -or
        -not [bool]$point.targetRadiusExplicit -or
        [int]$point.generatedLightCount -ne $Count -or
        [uint32]$point.seed -ne $Seed -or
        [Math]::Abs([double]$point.requestedRadius - $Radius) -gt 0.0001 -or
        [Math]::Abs([double]$point.volumeRadius - $Radius) -gt 0.0001 -or
        [double]$point.radiusAbsoluteError -gt 0.0001 -or
        [int]$result.resolution[0] -ne 1920 -or
        [int]$result.resolution[1] -ne 1080 -or
        [string]$result.gBuffer.positionMode -ne "explicit" -or
        [bool]$result.settings.bloom -or
        [bool]$result.ssao.enabled -or
        [bool]$point.pointShadowsEnabled -or
        [bool]$point.lightMarkersEnabled -or
        [bool]$point.renderDocMarkersEnabled -or
        [int]$result.settings.activeDirectionLights -ne 0 -or
        [int]$result.settings.activeSpotLights -ne 0 -or
        [Math]::Abs([double]$result.camera.position[0] + 6.0) -gt 0.000001 -or
        [Math]::Abs([double]$result.camera.position[1] + 1.5) -gt 0.000001 -or
        [Math]::Abs([double]$result.camera.position[2]) -gt 0.000001 -or
        [Math]::Abs([double]$result.camera.target[0] - 6.0) -gt 0.000001 -or
        [Math]::Abs([double]$result.camera.target[1] + 0.8) -gt 0.000001 -or
        [Math]::Abs([double]$result.camera.target[2]) -gt 0.000001 -or
        [Math]::Abs([double]$result.camera.fovDegrees - 55.0) -gt 0.000001 -or
        -not [bool]$result.profiler.gpuTimingSupported -or
        [int]$result.profiler.summary.cpuFrame.count -ne $ExpectedSamples -or
        [int]$result.profiler.summary.gpuFrame.count -ne $ExpectedSamples -or
        [int]$result.profiler.summary.gpuZones.'Deferred Point Lights'.count -ne $ExpectedSamples) {
        throw "Renderer result failed formal validation: $ResultPath"
    }
    if (-not [bool]$point.boundsTelemetry.requested -or
        -not [bool]$point.boundsTelemetry.executed -or
        [int]$point.boundsTelemetry.records.Count -ne $Count) {
        throw "Bounds telemetry count mismatch: $ResultPath"
    }
    foreach ($record in $point.boundsTelemetry.records) {
        if ([Math]::Abs([double]$record.radius - $Radius) -gt 0.0001) {
            throw "Per-light effective radius mismatch: $ResultPath"
        }
    }
    return $result
}

function Invoke-CaptureMatrix {
    if (-not (Test-Path -LiteralPath $executable -PathType Leaf)) {
        throw "Release executable is missing: $executable"
    }
    if ((Test-Path -LiteralPath $captureManifestPath) -and -not $Resume) {
        throw "Capture manifest already exists; use -Resume or a new run directory."
    }
    $protocolHash = (Get-FileHash -LiteralPath $protocolPath -Algorithm SHA256).Hash
    $currentExeHash = (Get-FileHash -LiteralPath $executable -Algorithm SHA256).Hash
    $currentGeneratorHash = (Get-FileHash -LiteralPath (Join-Path $projectDirectory "PointLightStressBenchmark.h") -Algorithm SHA256).Hash
    $currentWriterHash = (Get-FileHash -LiteralPath (Join-Path $projectDirectory "test.cpp") -Algorithm SHA256).Hash
    $currentShaderTreeHash = Get-ShaderTreeSha256
    $currentSponzaTreeHash = Get-SponzaTreeSha256
    $currentOrchestratorHash = (Get-FileHash -LiteralPath $PSCommandPath -Algorithm SHA256).Hash
    $currentAnalyzerHash = (Get-FileHash -LiteralPath (Join-Path $PSScriptRoot "analyze_count_radius_factorial.py") -Algorithm SHA256).Hash
    $currentVerifierHash = (Get-FileHash -LiteralPath (Join-Path $PSScriptRoot "verify_count_radius_factorial.py") -Algorithm SHA256).Hash
    $trustedCaptureResume = $false
    if ($Resume -and (Test-Path -LiteralPath $captureManifestPath -PathType Leaf)) {
        $oldManifest = Get-Content -LiteralPath $captureManifestPath -Raw -Encoding UTF8 | ConvertFrom-Json
        if ([string]$oldManifest.protocolSha256 -ne $protocolHash -or
            [string]$oldManifest.executableSha256 -ne $currentExeHash -or
            [string]$oldManifest.sourceHashes.generator -ne $currentGeneratorHash -or
            [string]$oldManifest.sourceHashes.resultWriter -ne $currentWriterHash -or
            [string]$oldManifest.sourceHashes.runtimeShaderTree -ne $currentShaderTreeHash -or
            [string]$oldManifest.sourceHashes.sponzaAssetTree -ne $currentSponzaTreeHash -or
            [string]$oldManifest.sourceHashes.orchestrator -ne $currentOrchestratorHash -or
            [string]$oldManifest.sourceHashes.analyzer -ne $currentAnalyzerHash -or
            [string]$oldManifest.sourceHashes.independentVerifier -ne $currentVerifierHash) {
            throw "Resume refused: protocol, executable, or source hash changed."
        }
        if (-not (Test-Path -LiteralPath $oldManifest.sharedCaptures.position -PathType Leaf) -or
            -not (Test-Path -LiteralPath $oldManifest.sharedCaptures.validity -PathType Leaf) -or
            (Get-FileHash -LiteralPath $oldManifest.sharedCaptures.position -Algorithm SHA256).Hash -ne [string]$oldManifest.sharedCaptures.positionSha256 -or
            (Get-FileHash -LiteralPath $oldManifest.sharedCaptures.validity -Algorithm SHA256).Hash -ne [string]$oldManifest.sharedCaptures.validitySha256) {
            throw "Resume refused: shared G-buffer capture changed."
        }
        $trustedCaptureResume = $true
    }
    $sharedPosition = Join-Path $captureDirectory "shared-position.pfm"
    $sharedValidity = Join-Path $captureDirectory "shared-position-validity-alpha.pfm"
    $launches = @()
    $allResumed = $trustedCaptureResume
    $firstCapture = $true
    foreach ($seed in $seeds) {
        foreach ($count in $counts) {
            foreach ($radius in $radii) {
                $stem = Get-CaseStem $seed.ordinal $count $radius
                $caseDirectory = Join-Path $captureDirectory $stem
                New-Item -ItemType Directory -Force -Path $caseDirectory | Out-Null
                $resultPath = Join-Path $caseDirectory "scene.json"
                $capturePath = Join-Path $caseDirectory "exact-analytic-screen.ppm"
                $logPath = Join-Path $captureLogDirectory "$stem.log"
                $canResume = $trustedCaptureResume -and
                    (Test-Path -LiteralPath $resultPath -PathType Leaf) -and
                    (Test-Path -LiteralPath $capturePath -PathType Leaf) -and
                    (Test-Path -LiteralPath $logPath -PathType Leaf) -and
                    (Test-Path -LiteralPath $sharedPosition -PathType Leaf) -and
                    (Test-Path -LiteralPath $sharedValidity -PathType Leaf)
                if ($canResume) {
                    $result = Assert-FormalResult $resultPath $count $radius $seed.value 1
                    Write-Host "SKIP capture $stem"
                    $launch = [ordered]@{
                        stem = $stem; seedOrdinal = $seed.ordinal; seedText = $seed.text
                        seed = [uint64]$seed.value; lightCount = $count; requestedRadius = $radius
                        effectiveRadius = [double]$result.pointLightStress.volumeRadius
                        quadratic = [double]$result.pointLightStress.quadratic
                        sceneSignature = [string]$result.pointLightStress.sceneSignature
                        submissionSignature = [string]$result.pointLightStress.submissionSignature
                        positionPrefixSignature = [string]$result.pointLightStress.positionPrefixSignature
                        colorPrefixSignature = [string]$result.pointLightStress.colorPrefixSignature
                        result = $resultPath; ldr = $capturePath
                        captures = [ordered]@{ position = $sharedPosition; validity = $sharedValidity }
                        log = $logPath; resumed = $true; exitCode = 0
                    }
                }
                else {
                    $allResumed = $false
                    Write-Host "CAPTURE $stem"
                    $launchInfo = Invoke-Renderer $resultPath $capturePath $logPath `
                        $count $radius $seed.text 30 1 $firstCapture `
                        $sharedPosition $sharedValidity $true $true
                    if ($launchInfo.exitCode -ne 0) {
                        throw "$stem capture failed: exit=$($launchInfo.exitCode), log=$logPath"
                    }
                    $result = Assert-FormalResult $resultPath $count $radius $seed.value 1
                    if ($firstCapture) {
                        foreach ($path in @($sharedPosition, $sharedValidity)) {
                            if (-not (Test-Path -LiteralPath $path -PathType Leaf)) {
                                throw "Shared G-buffer capture is missing: $path"
                            }
                        }
                    }
                    $launch = [ordered]@{
                        stem = $stem; seedOrdinal = $seed.ordinal; seedText = $seed.text
                        seed = [uint64]$seed.value; lightCount = $count; requestedRadius = $radius
                        effectiveRadius = [double]$result.pointLightStress.volumeRadius
                        quadratic = [double]$result.pointLightStress.quadratic
                        sceneSignature = [string]$result.pointLightStress.sceneSignature
                        submissionSignature = [string]$result.pointLightStress.submissionSignature
                        positionPrefixSignature = [string]$result.pointLightStress.positionPrefixSignature
                        colorPrefixSignature = [string]$result.pointLightStress.colorPrefixSignature
                        result = $resultPath; ldr = $capturePath
                        captures = [ordered]@{ position = $sharedPosition; validity = $sharedValidity }
                        log = $logPath; resumed = $false; exitCode = 0
                        startUtc = $launchInfo.startUtc; elapsedSeconds = $launchInfo.elapsedSeconds
                    }
                }
                $launches += $launch
                $firstCapture = $false
            }
        }
    }
    if ($launches.Count -ne 60) { throw "Capture matrix size mismatch: $($launches.Count)" }
    if ($trustedCaptureResume -and $allResumed) {
        Write-Host "Capture matrix already complete; preserving original manifest byte-for-byte."
        return
    }
    $manifest = [ordered]@{
        schemaVersion = 1
        experiment = "point-light-count-radius-factorial-phase-a"
        protocolFrozenBeforeCapture = $true
        protocol = $protocolPath
        protocolSha256 = $protocolHash
        writtenUtc = [DateTime]::UtcNow.ToString("o")
        executable = $executable
        executableSha256 = $currentExeHash
        buildConfiguration = "Release"
        architecture = "x64"
        resolution = @(1920, 1080)
        renderMode = "analytic-screen"
        renderModeExplicit = $true
        coverage = "representative"
        counts = $counts
        radii = $radii
        seeds = $seeds
        tileSize = 16
        primaryClusterSlices = 16
        cases = $launches
        sharedCaptures = [ordered]@{
            position = $sharedPosition
            validity = $sharedValidity
            positionSha256 = (Get-FileHash -LiteralPath $sharedPosition -Algorithm SHA256).Hash
            validitySha256 = (Get-FileHash -LiteralPath $sharedValidity -Algorithm SHA256).Hash
        }
        runtimeCandidateImplemented = $false
        defaultChanged = $false
        sourceHashes = [ordered]@{
            generator = $currentGeneratorHash
            resultWriter = $currentWriterHash
            pointLightShader = (Get-FileHash -LiteralPath (Join-Path $projectDirectory "shaders\pointLightLighting.glsl") -Algorithm SHA256).Hash
            analyticScreenShader = (Get-FileHash -LiteralPath (Join-Path $projectDirectory "shaders\lightVolumeFullscreenFragment.glsl") -Algorithm SHA256).Hash
            runtimeShaderTree = $currentShaderTreeHash
            sponzaAssetTree = $currentSponzaTreeHash
            orchestrator = $currentOrchestratorHash
            analyzer = $currentAnalyzerHash
            independentVerifier = $currentVerifierHash
        }
    }
    Write-JsonFile $captureManifestPath $manifest
    Write-Host "Capture matrix complete: 60 cases"
}

function Get-TimingSchedule {
    $canonical = @()
    foreach ($count in $counts) {
        foreach ($radius in $radii) {
            $canonical += ,([ordered]@{ lightCount = $count; requestedRadius = $radius })
        }
    }
    $round1 = @($canonical)
    $round2 = @($canonical[($canonical.Count - 1)..0])
    $rotated = @($canonical[10..($canonical.Count - 1)] + $canonical[0..9])
    $round3 = @()
    for ($index = 0; $index -lt $rotated.Count; $index += 2) {
        $round3 += ,$rotated[$index + 1]
        $round3 += ,$rotated[$index]
    }
    return @(
        [pscustomobject]@{ cells = $round1 },
        [pscustomobject]@{ cells = $round2 },
        [pscustomobject]@{ cells = $round3 }
    )
}

function Invoke-TimingMatrix {
    if (-not (Test-Path -LiteralPath $captureManifestPath -PathType Leaf)) {
        throw "Capture manifest is required before timing."
    }
    if ((Test-Path -LiteralPath $timingManifestPath) -and -not $Resume) {
        throw "Timing manifest already exists; use -Resume or a new run directory."
    }
    $captureManifest = Get-Content -LiteralPath $captureManifestPath -Raw -Encoding UTF8 | ConvertFrom-Json
    $exeHash = (Get-FileHash -LiteralPath $executable -Algorithm SHA256).Hash
    if ([string]$captureManifest.executableSha256 -ne $exeHash) {
        throw "Timing executable hash differs from Capture executable."
    }
    if ([string]$captureManifest.sourceHashes.runtimeShaderTree -ne (Get-ShaderTreeSha256)) {
        throw "Timing runtime shader tree differs from Capture."
    }
    if ([string]$captureManifest.sourceHashes.sponzaAssetTree -ne (Get-SponzaTreeSha256) -or
        [string]$captureManifest.sourceHashes.orchestrator -ne (Get-FileHash -LiteralPath $PSCommandPath -Algorithm SHA256).Hash) {
        throw "Timing scene assets or orchestrator differ from Capture."
    }
    $trustedTimingResume = $false
    if ($Resume -and (Test-Path -LiteralPath $timingManifestPath -PathType Leaf)) {
        $oldTiming = Get-Content -LiteralPath $timingManifestPath -Raw -Encoding UTF8 | ConvertFrom-Json
        if ([string]$oldTiming.executableSha256 -ne $exeHash -or
            [string]$oldTiming.protocolSha256 -ne (Get-FileHash -LiteralPath $protocolPath -Algorithm SHA256).Hash -or
            [string]$oldTiming.captureManifestSha256 -ne (Get-FileHash -LiteralPath $captureManifestPath -Algorithm SHA256).Hash) {
            throw "Timing resume refused: protocol, executable, or capture manifest changed."
        }
        $trustedTimingResume = $true
    }
    $schedule = Get-TimingSchedule
    $runs = @()
    for ($round = 0; $round -lt 3; ++$round) {
        $order = 0
        foreach ($cell in $schedule[$round].cells) {
            $stem = "round-{0}-n{1:D4}-r{2:D3}" -f ($round + 1), $cell.lightCount, ([int][Math]::Round($cell.requestedRadius * 10.0))
            $caseDirectory = Join-Path $timingDirectory $stem
            New-Item -ItemType Directory -Force -Path $caseDirectory | Out-Null
            $resultPath = Join-Path $caseDirectory "result.json"
            $capturePath = Join-Path $caseDirectory "final.ppm"
            $logPath = Join-Path $timingLogDirectory "$stem.log"
            $canResume = $trustedTimingResume -and
                (Test-Path -LiteralPath $resultPath -PathType Leaf) -and
                (Test-Path -LiteralPath $capturePath -PathType Leaf) -and
                (Test-Path -LiteralPath $logPath -PathType Leaf)
            if ($canResume) {
                $result = Assert-FormalResult $resultPath $cell.lightCount `
                    $cell.requestedRadius $seeds[0].value 600
                Write-Host "SKIP timing $stem"
                $launchInfo = [ordered]@{
                    exitCode = 0; resumed = $true
                    startUtc = $null; elapsedSeconds = $null
                }
            }
            else {
                Write-Host "TIMING $stem"
                $launchInfo = Invoke-Renderer $resultPath $capturePath $logPath `
                    $cell.lightCount $cell.requestedRadius $seeds[0].text `
                    300 600 $false "" "" $true $true
                if ($launchInfo.exitCode -ne 0) {
                    throw "$stem timing failed: exit=$($launchInfo.exitCode), log=$logPath"
                }
                $result = Assert-FormalResult $resultPath $cell.lightCount `
                    $cell.requestedRadius $seeds[0].value 600
            }
            $runs += [ordered]@{
                stem = $stem; round = $round + 1; order = $order
                lightCount = $cell.lightCount; requestedRadius = $cell.requestedRadius
                result = $resultPath; capture = $capturePath; log = $logPath
                exitCode = 0; resumed = $canResume
                startUtc = $launchInfo.startUtc; elapsedSeconds = $launchInfo.elapsedSeconds
                cpuFrameMedianMs = [double]$result.profiler.summary.cpuFrame.median
                cpuFrameP95Ms = [double]$result.profiler.summary.cpuFrame.p95
                cpuFrameP99Ms = [double]$result.profiler.summary.cpuFrame.p99
                gpuFrameMedianMs = [double]$result.profiler.summary.gpuFrame.median
                gpuPointLightsMedianMs = [double]$result.profiler.summary.gpuZones.'Deferred Point Lights'.median
                gpuPointLightsP95Ms = [double]$result.profiler.summary.gpuZones.'Deferred Point Lights'.p95
                gpuPointLightsP99Ms = [double]$result.profiler.summary.gpuZones.'Deferred Point Lights'.p99
                cpuPointLightsMedianMs = [double]$result.profiler.summary.cpuZones.'Deferred Point Lights'.median
                drawCallsMedian = [double]$result.profiler.summary.drawCalls.median
                submittedLightsMedian = [double]$result.profiler.summary.pointLightsSubmitted.median
                rectPixelAreaMedian = [double]$result.profiler.summary.pointLightRectPixelArea.median
            }
            ++$order
        }
    }
    if ($runs.Count -ne 60) { throw "Timing matrix size mismatch: $($runs.Count)" }
    $manifest = [ordered]@{
        schemaVersion = 1
        experiment = "point-light-count-radius-factorial-analytic-screen-oracle-timing"
        label = "Measured Release analytic-screen timing; not Tile/Cluster runtime"
        protocolSha256 = (Get-FileHash -LiteralPath $protocolPath -Algorithm SHA256).Hash
        captureManifestSha256 = (Get-FileHash -LiteralPath $captureManifestPath -Algorithm SHA256).Hash
        writtenUtc = [DateTime]::UtcNow.ToString("o")
        executable = $executable
        executableSha256 = $exeHash
        seed = $seeds[0]
        warmupFrames = 300
        sampleFrames = 600
        independentProcessesPerCell = 3
        scheduleDefinition = "round1 canonical N/R ascending; round2 reverse; round3 canonical rotate-left-10 then adjacent-pair swap"
        runs = $runs
    }
    Write-JsonFile $timingManifestPath $manifest
    Write-Host "Timing matrix complete: 60 independent processes"
}

function Invoke-Analysis {
    $python = Resolve-Python
    $logPath = Join-Path $RunDirectory "logs\analysis.log"
    & $python (Join-Path $PSScriptRoot "analyze_count_radius_factorial.py") `
        --run-dir $RunDirectory *> $logPath
    $exitCode = $LASTEXITCODE
    Get-Content -LiteralPath $logPath | Select-Object -Last 40
    if ($exitCode -ne 0) { throw "Factorial analysis failed: $exitCode" }
}

function Invoke-Verification {
    $python = Resolve-Python
    $logPath = Join-Path $RunDirectory "logs\independent-verification.log"
    & $python (Join-Path $PSScriptRoot "verify_count_radius_factorial.py") `
        --run-dir $RunDirectory *> $logPath
    $exitCode = $LASTEXITCODE
    Get-Content -LiteralPath $logPath | Select-Object -Last 40
    if ($exitCode -ne 0) { throw "Independent verification failed: $exitCode" }
    & $python (Join-Path $PSScriptRoot "analyze_count_radius_factorial.py") `
        --run-dir $RunDirectory --report-only
    if ($LASTEXITCODE -ne 0) { throw "Final report regeneration failed" }
}

function Invoke-Finalize {
    Invoke-ReleaseBuild
    $finalHash = (Get-FileHash -LiteralPath $executable -Algorithm SHA256).Hash
    $captureManifest = Get-Content -LiteralPath $captureManifestPath -Raw -Encoding UTF8 | ConvertFrom-Json
    if ([string]$captureManifest.executableSha256 -ne $finalHash) {
        throw "Final Release EXE differs from formal Capture/Timing EXE."
    }
    if ([string]$captureManifest.sourceHashes.runtimeShaderTree -ne (Get-ShaderTreeSha256)) {
        throw "Final runtime shader tree differs from formal Capture/Timing."
    }
    if ([string]$captureManifest.sourceHashes.sponzaAssetTree -ne (Get-SponzaTreeSha256) -or
        [string]$captureManifest.sourceHashes.orchestrator -ne (Get-FileHash -LiteralPath $PSCommandPath -Algorithm SHA256).Hash) {
        throw "Final scene assets or orchestrator differ from formal Capture/Timing."
    }
    $smokeDirectory = Join-Path $RunDirectory "verification\default-smoke"
    New-Item -ItemType Directory -Force -Path $smokeDirectory | Out-Null
    $resultPath = Join-Path $smokeDirectory "result.json"
    $capturePath = Join-Path $smokeDirectory "capture.ppm"
    $logPath = Join-Path $smokeDirectory "run.log"
    $launch = Invoke-Renderer $resultPath $capturePath $logPath 16 0.0 `
        $seeds[0].text 3 3 $false "" "" $false $false
    if ($launch.exitCode -ne 0) { throw "Default smoke failed: $($launch.exitCode)" }
    $result = Get-Content -LiteralPath $resultPath -Raw -Encoding UTF8 | ConvertFrom-Json
    if (-not [bool]$result.success -or
        [string]$result.pointLightStress.renderMode -ne "analytic-screen" -or
        [bool]$result.pointLightStress.renderModeExplicit -or
        [bool]$result.pointLightStress.targetRadiusExplicit -or
        [string]$result.pointLightStress.generatorVersion -ne "point-light-heavy-xorshift32-v1" -or
        [int]$result.profiler.summary.gpuFrame.count -ne 3) {
        throw "Default smoke changed legacy/default behavior."
    }
    $rendererProcesses = @(Get-Process -Name "OpenGL_Learn" -ErrorAction SilentlyContinue)
    $renderDocProcesses = @(Get-Process -ErrorAction SilentlyContinue | Where-Object {
        $_.ProcessName -match "renderdoc|qrenderdoc"
    })
    if ($rendererProcesses.Count -ne 0 -or $renderDocProcesses.Count -ne 0) {
        throw "Residual renderer/RenderDoc processes remain."
    }
    $verification = [ordered]@{
        schemaVersion = 1
        releaseBuild = [ordered]@{ passed = $true; executableSha256 = $finalHash }
        defaultSmoke = [ordered]@{
            passed = $true; result = $resultPath; log = $logPath
            renderMode = [string]$result.pointLightStress.renderMode
            renderModeExplicit = [bool]$result.pointLightStress.renderModeExplicit
            targetRadiusExplicit = [bool]$result.pointLightStress.targetRadiusExplicit
            generatorVersion = [string]$result.pointLightStress.generatorVersion
            glErrorCount = 0
        }
        residualProcesses = [ordered]@{
            renderer = $rendererProcesses.Count; renderDoc = $renderDocProcesses.Count
        }
        runtimeCandidateImplemented = $false
        defaultChanged = $false
    }
    Write-JsonFile (Join-Path $RunDirectory "verification\final-verification.json") $verification
    $python = Resolve-Python
    & $python (Join-Path $PSScriptRoot "analyze_count_radius_factorial.py") `
        --run-dir $RunDirectory --report-only
    if ($LASTEXITCODE -ne 0) { throw "Final report/artifact regeneration failed" }
    Write-Host "Finalize passed; EXE SHA-256=$finalHash"
}

switch ($Mode) {
    "Build" { Invoke-ReleaseBuild }
    "Capture" { Invoke-CaptureMatrix }
    "Analyze" { Invoke-Analysis }
    "Timing" { Invoke-TimingMatrix }
    "Verify" { Invoke-Verification }
    "Finalize" { Invoke-Finalize }
    "All" {
        Invoke-ReleaseBuild
        Invoke-CaptureMatrix
        Invoke-Analysis
        Invoke-TimingMatrix
        Invoke-Analysis
        Invoke-Verification
        Invoke-Finalize
    }
}

Write-Host "Count/Radius factorial run directory: $RunDirectory"
