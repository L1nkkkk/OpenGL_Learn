param(
    [ValidateSet("All", "Capture", "Analyze", "Verify")]
    [string]$Mode = "All",
    [string]$RunDirectory = "",
    [string]$PythonExecutable = "",
    [ValidateRange(1, 1000000)]
    [int]$WarmupFrames = 300,
    [ValidateRange(1, 1000000)]
    [int]$SampleFrames = 600,
    [ValidateRange(1, 20)]
    [int]$Rounds = 3,
    [switch]$Resume
)

Set-StrictMode -Version Latest
$ErrorActionPreference = "Stop"

$projectDirectory = Split-Path -Parent $PSScriptRoot
$repositoryDirectory = Split-Path -Parent $projectDirectory
$executable = Join-Path $repositoryDirectory "x64\Release\OpenGL_Learn.exe"
if ([string]::IsNullOrWhiteSpace($RunDirectory)) {
    $RunDirectory = Join-Path $projectDirectory `
        "benchmark-results\point-light-tile-cluster-runtime-boundary\tile-cluster-runtime-boundary-formal-20260805"
}
$RunDirectory = [System.IO.Path]::GetFullPath($RunDirectory)
$rawDirectory = Join-Path $RunDirectory "raw"
$captureDirectory = Join-Path $RunDirectory "captures"
$logDirectory = Join-Path $RunDirectory "logs"
$protocolPath = Join-Path $RunDirectory "PHASE0_FROZEN_PROTOCOL_CN.md"
$preCaptureManifestPath = Join-Path $RunDirectory "pre-capture-manifest.json"
$captureManifestPath = Join-Path $RunDirectory "capture-manifest.json"
$ledgerPath = Join-Path $RunDirectory "run-ledger.ndjson"
$correctnessManifestPath = Join-Path $RunDirectory "correctness-manifest.json"

foreach ($directory in @($RunDirectory, $rawDirectory, $captureDirectory, $logDirectory)) {
    New-Item -ItemType Directory -Force -Path $directory | Out-Null
}

function Resolve-Python {
    if (-not [string]::IsNullOrWhiteSpace($PythonExecutable)) {
        return [System.IO.Path]::GetFullPath($PythonExecutable)
    }
    foreach ($candidate in @(
        "C:\Users\Link\AppData\Local\Python\bin\python.exe",
        "C:\Users\Link\.cache\codex-runtimes\codex-primary-runtime\dependencies\python\python.exe",
        "python"
    )) {
        try {
            & $candidate --version *> $null
            if ($LASTEXITCODE -eq 0) { return $candidate }
        }
        catch {}
    }
    throw "Python 3 with NumPy, Pillow, and Matplotlib was not found."
}

function Get-RelativePath([string]$Path) {
    $base = $RunDirectory.TrimEnd([System.IO.Path]::DirectorySeparatorChar) +
        [System.IO.Path]::DirectorySeparatorChar
    $baseUri = New-Object System.Uri($base)
    $pathUri = New-Object System.Uri([System.IO.Path]::GetFullPath($Path))
    return [System.Uri]::UnescapeDataString(
        $baseUri.MakeRelativeUri($pathUri).ToString()).Replace(
            [System.IO.Path]::AltDirectorySeparatorChar,
            [System.IO.Path]::DirectorySeparatorChar)
}

function Write-JsonAtomic([string]$Path, $Value) {
    $temporary = "$Path.tmp"
    $Value | ConvertTo-Json -Depth 12 | Set-Content -LiteralPath $temporary -Encoding UTF8
    Move-Item -LiteralPath $temporary -Destination $Path -Force
}

function Radius-Token([double]$Radius) {
    return "{0:D3}" -f [int][Math]::Round($Radius * 10.0)
}

function Radius-Text([double]$Radius) {
    return $Radius.ToString("0.0############", [System.Globalization.CultureInfo]::InvariantCulture)
}

function Assert-Result(
    [string]$ResultPath,
    [string]$CapturePath,
    [string]$Regime,
    [int]$LightCount,
    [double]$Radius,
    [string]$RenderMode
) {
    if (-not (Test-Path -LiteralPath $ResultPath -PathType Leaf)) {
        throw "Missing result: $ResultPath"
    }
    if (-not (Test-Path -LiteralPath $CapturePath -PathType Leaf)) {
        throw "Missing capture: $CapturePath"
    }
    $result = Get-Content -LiteralPath $ResultPath -Raw -Encoding UTF8 | ConvertFrom-Json
    $point = $result.pointLightStress
    $grid = $point.gridRuntime
    if (-not [bool]$result.success -or
        [string]$result.buildConfiguration -ne "Release" -or
        [string]$result.architecture -ne "x64" -or
        [int]$result.resolution[0] -ne 1920 -or
        [int]$result.resolution[1] -ne 1080 -or
        [int]$result.warmupFrames -ne $WarmupFrames -or
        [int]$result.measuredFrames -ne $SampleFrames -or
        [string]$point.renderMode -ne $RenderMode -or
        -not [bool]$point.renderModeExplicit -or
        [string]$point.gridUpdateMode -ne $Regime -or
        -not [bool]$point.gridUpdateModeExplicit -or
        [bool]$point.offscreenCulling -or
        -not [bool]$point.offscreenCullingExplicit -or
        [int]$point.generatedLightCount -ne $LightCount -or
        [Math]::Abs([double]$point.volumeRadius - $Radius) -gt 0.0001 -or
        -not [bool]$grid.valid -or [bool]$grid.overflow -or
        -not [string]::IsNullOrEmpty([string]$grid.error) -or
        [int]$grid.lightCount -ne $LightCount -or
        [int]$result.profiler.summary.wallFrame.count -ne $SampleFrames -or
        [int]$result.profiler.summary.cpuFrame.count -ne $SampleFrames -or
        [int]$result.profiler.summary.gpuFrame.count -ne $SampleFrames -or
        [int]$result.profiler.summary.gpuZones.'Point Light Grid Lighting GPU'.count -ne $SampleFrames) {
        throw "Result failed frozen-protocol validation: $ResultPath"
    }
}

$counts = @(32, 64, 128, 256, 512)
$cachedRadii = @(1.5, 2.0, 2.5, 3.0, 4.0, 5.0, 6.0, 8.0, 10.0, 12.0)
$rebuildRadii = @(1.5, 3.0, 6.0, 12.0)
$expected = @()
foreach ($round in 1..$Rounds) {
    foreach ($regime in @("cached", "rebuild")) {
        $radii = if ($regime -eq "cached") { $cachedRadii } else { $rebuildRadii }
        $cells = @()
        foreach ($count in $counts) {
            foreach ($radius in $radii) {
                $cells += [ordered]@{ lightCount = $count; radius = [double]$radius }
            }
        }
        if (($round % 2) -eq 0) { [array]::Reverse($cells) }
        $renderModes = if (($round % 2) -eq 1) {
            @("tile16", "cluster16")
        }
        else {
            @("cluster16", "tile16")
        }
        foreach ($cell in $cells) {
            foreach ($renderMode in $renderModes) {
                $stem = "{0}-n{1:D4}-r{2}-round{3}-{4}" -f `
                    $regime, $cell.lightCount, (Radius-Token $cell.radius), $round, $renderMode
                $resultPath = Join-Path $rawDirectory "$stem.json"
                $capturePath = Join-Path $captureDirectory "$stem.ppm"
                $logPath = Join-Path $logDirectory "$stem.log"
                $expected += [ordered]@{
                    stem = $stem
                    regime = $regime
                    lightCount = $cell.lightCount
                    radius = $cell.radius
                    round = $round
                    renderMode = $renderMode
                    result = Get-RelativePath $resultPath
                    capture = Get-RelativePath $capturePath
                    log = Get-RelativePath $logPath
                }
            }
        }
    }
}

$correctnessExpected = @()
foreach ($qualityCase in @(
    [ordered]@{ lightCount = 512; radius = 1.5 },
    [ordered]@{ lightCount = 128; radius = 10.0 },
    [ordered]@{ lightCount = 512; radius = 12.0 }
)) {
    foreach ($renderMode in @("analytic-screen", "tile16", "cluster16")) {
        $stem = "quality-n{0:D4}-r{1}-{2}" -f `
            $qualityCase.lightCount, (Radius-Token $qualityCase.radius), $renderMode
        $correctnessExpected += [ordered]@{
            stem = $stem
            kind = "oracle-quality"
            coverage = "representative"
            lightCount = $qualityCase.lightCount
            radius = $qualityCase.radius
            renderMode = $renderMode
            result = Get-RelativePath (Join-Path $rawDirectory "$stem.json")
            capture = Get-RelativePath (Join-Path $captureDirectory "$stem.ppm")
            log = Get-RelativePath (Join-Path $logDirectory "$stem.log")
            captureGBuffer = $renderMode -eq "tile16" -and
                $qualityCase.lightCount -eq 512 -and $qualityCase.radius -eq 12.0
        }
    }
}
foreach ($edgeCase in @(
    [ordered]@{ lightCount = 0; coverage = "representative" },
    [ordered]@{ lightCount = 1; coverage = "representative" },
    [ordered]@{ lightCount = 16; coverage = "edge-cases" }
)) {
    foreach ($renderMode in @("tile16", "cluster16")) {
        $stem = "edge-n{0:D4}-{1}" -f $edgeCase.lightCount, $renderMode
        $correctnessExpected += [ordered]@{
            stem = $stem
            kind = "edge"
            coverage = $edgeCase.coverage
            lightCount = $edgeCase.lightCount
            radius = 3.0
            renderMode = $renderMode
            result = Get-RelativePath (Join-Path $rawDirectory "$stem.json")
            capture = Get-RelativePath (Join-Path $captureDirectory "$stem.ppm")
            log = Get-RelativePath (Join-Path $logDirectory "$stem.log")
            captureGBuffer = $false
        }
    }
}

if ($Mode -in @("All", "Capture")) {
    if (-not (Test-Path -LiteralPath $executable -PathType Leaf)) {
        throw "Release executable is missing: $executable"
    }
    if (-not (Test-Path -LiteralPath $protocolPath -PathType Leaf)) {
        throw "Frozen protocol is missing: $protocolPath"
    }
    $analyzerPath = Join-Path $PSScriptRoot "analyze_tile_cluster_runtime_boundary.py"
    $verifierPath = Join-Path $PSScriptRoot "verify_tile_cluster_runtime_boundary.py"
    foreach ($path in @($analyzerPath, $verifierPath)) {
        if (-not (Test-Path -LiteralPath $path -PathType Leaf)) {
            throw "Required frozen tool is missing: $path"
        }
    }

    $sourceHashes = [ordered]@{
        runtimeCpp = (Get-FileHash -LiteralPath (Join-Path $projectDirectory "PointLightGridRuntime.cpp") -Algorithm SHA256).Hash
        runtimeHeader = (Get-FileHash -LiteralPath (Join-Path $projectDirectory "PointLightGridRuntime.h") -Algorithm SHA256).Hash
        deferredPass = (Get-FileHash -LiteralPath (Join-Path $projectDirectory "DeferRenderPass.cpp") -Algorithm SHA256).Hash
        generator = (Get-FileHash -LiteralPath (Join-Path $projectDirectory "PointLightStressBenchmark.h") -Algorithm SHA256).Hash
        gridVertexShader = (Get-FileHash -LiteralPath (Join-Path $projectDirectory "shaders\pointLightGridVertex.glsl") -Algorithm SHA256).Hash
        gridFragmentShader = (Get-FileHash -LiteralPath (Join-Path $projectDirectory "shaders\pointLightGridFragment.glsl") -Algorithm SHA256).Hash
        orchestrator = (Get-FileHash -LiteralPath $PSCommandPath -Algorithm SHA256).Hash
        analyzer = (Get-FileHash -LiteralPath $analyzerPath -Algorithm SHA256).Hash
        verifier = (Get-FileHash -LiteralPath $verifierPath -Algorithm SHA256).Hash
        fnvHelper = (Get-FileHash -LiteralPath (Join-Path $PSScriptRoot "fnv1a64_stdin.cpp") -Algorithm SHA256).Hash
    }
    $preCapture = [ordered]@{
        schemaVersion = 1
        experiment = "actual-tile16-vs-cluster16-runtime-boundary"
        protocolFrozenBeforeCapture = $true
        createdUtc = [DateTime]::UtcNow.ToString("o")
        protocol = Get-RelativePath $protocolPath
        protocolSha256 = (Get-FileHash -LiteralPath $protocolPath -Algorithm SHA256).Hash
        executable = $executable
        executableSha256 = (Get-FileHash -LiteralPath $executable -Algorithm SHA256).Hash
        buildConfiguration = "Release"
        architecture = "x64"
        resolution = @(1920, 1080)
        seed = "0x21D3F3A5"
        tileSize = 16
        clusterSlices = 16
        warmupFrames = $WarmupFrames
        sampleFrames = $SampleFrames
        rounds = $Rounds
        winnerThreshold = [ordered]@{
            absoluteMilliseconds = 0.05
            relativePercent = 3.0
            pairedDirectionAgreement = "$Rounds/$Rounds"
        }
        sourceHashes = $sourceHashes
        expectedRuns = $expected
        expectedCorrectnessRuns = $correctnessExpected
    }

    if (Test-Path -LiteralPath $preCaptureManifestPath -PathType Leaf) {
        $existing = Get-Content -LiteralPath $preCaptureManifestPath -Raw -Encoding UTF8 | ConvertFrom-Json
        if (-not $Resume) {
            throw "Pre-capture manifest already exists; use -Resume only after checking the frozen run."
        }
        if ([string]$existing.protocolSha256 -ne [string]$preCapture.protocolSha256 -or
            [string]$existing.executableSha256 -ne [string]$preCapture.executableSha256 -or
            [int]$existing.warmupFrames -ne $WarmupFrames -or
            [int]$existing.sampleFrames -ne $SampleFrames -or
            [int]$existing.rounds -ne $Rounds -or
            [int]$existing.expectedRuns.Count -ne $expected.Count -or
            [int]$existing.expectedCorrectnessRuns.Count -ne $correctnessExpected.Count) {
            throw "Resume rejected: frozen protocol, binary, or matrix changed."
        }
        foreach ($name in $sourceHashes.Keys) {
            if ([string]$existing.sourceHashes.$name -ne [string]$sourceHashes[$name]) {
                throw "Resume rejected: source hash changed for $name."
            }
        }
    }
    else {
        $existingRaw = @(Get-ChildItem -LiteralPath $rawDirectory -File -ErrorAction SilentlyContinue)
        $existingCaptures = @(Get-ChildItem -LiteralPath $captureDirectory -File -ErrorAction SilentlyContinue)
        if ($existingRaw.Count -gt 0 -or $existingCaptures.Count -gt 0) {
            throw "Formal raw/capture files exist without a pre-capture manifest; refusing to overwrite."
        }
        Write-JsonAtomic $preCaptureManifestPath $preCapture
    }

    $completed = @()
    $ordinal = 0
    foreach ($run in $expected) {
        ++$ordinal
        $resultPath = Join-Path $RunDirectory $run.result
        $capturePath = Join-Path $RunDirectory $run.capture
        $logPath = Join-Path $RunDirectory $run.log
        $canResume = $Resume -and
            (Test-Path -LiteralPath $resultPath -PathType Leaf) -and
            (Test-Path -LiteralPath $capturePath -PathType Leaf) -and
            (Test-Path -LiteralPath $logPath -PathType Leaf)
        if ($canResume) {
            Assert-Result $resultPath $capturePath $run.regime $run.lightCount $run.radius $run.renderMode
            Write-Host "[$ordinal/$($expected.Count)] SKIP verified $($run.stem)"
            $completed += [ordered]@{
                stem = $run.stem
                resumed = $true
                exitCode = 0
                resultSha256 = (Get-FileHash -LiteralPath $resultPath -Algorithm SHA256).Hash
                captureSha256 = (Get-FileHash -LiteralPath $capturePath -Algorithm SHA256).Hash
                logSha256 = (Get-FileHash -LiteralPath $logPath -Algorithm SHA256).Hash
            }
            continue
        }
        foreach ($path in @($resultPath, $capturePath, $logPath)) {
            if (Test-Path -LiteralPath $path) {
                throw "Incomplete existing run would be overwritten: $path"
            }
        }

        $arguments = @(
            "--gbuffer-position", "explicit",
            "--point-light-render-mode", $run.renderMode,
            "--point-light-grid-update", $run.regime,
            "--point-light-offscreen-culling", "off",
            "--point-light-stencil-clear-mode", "coalesced-n-plus-one",
            "--point-light-stress",
            "--point-light-count", [string]$run.lightCount,
            "--point-light-coverage", "representative",
            "--point-light-seed", "0x21D3F3A5",
            "--point-light-target-radius", (Radius-Text $run.radius),
            "--point-light-width", "1920",
            "--point-light-height", "1080",
            "--point-light-warmup-frames", [string]$WarmupFrames,
            "--point-light-sample-frames", [string]$SampleFrames,
            "--point-light-result", $resultPath,
            "--point-light-capture", $capturePath
        )
        Write-Host "[$ordinal/$($expected.Count)] RUN $($run.stem)"
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
            throw "$($run.stem) failed with exit code $exitCode; log=$logPath"
        }
        Assert-Result $resultPath $capturePath $run.regime $run.lightCount $run.radius $run.renderMode
        $record = [ordered]@{
            stem = $run.stem
            startUtc = $startUtc.ToString("o")
            endUtc = [DateTime]::UtcNow.ToString("o")
            elapsedSeconds = [Math]::Round($stopwatch.Elapsed.TotalSeconds, 3)
            exitCode = $exitCode
            resumed = $false
            resultSha256 = (Get-FileHash -LiteralPath $resultPath -Algorithm SHA256).Hash
            captureSha256 = (Get-FileHash -LiteralPath $capturePath -Algorithm SHA256).Hash
            logSha256 = (Get-FileHash -LiteralPath $logPath -Algorithm SHA256).Hash
        }
        ($record | ConvertTo-Json -Compress) | Add-Content -LiteralPath $ledgerPath -Encoding UTF8
        $completed += $record
    }

    $correctnessCompleted = @()
    $correctnessOrdinal = 0
    foreach ($run in $correctnessExpected) {
        ++$correctnessOrdinal
        $resultPath = Join-Path $RunDirectory $run.result
        $capturePath = Join-Path $RunDirectory $run.capture
        $logPath = Join-Path $RunDirectory $run.log
        $gbufferPositionPath = Join-Path $captureDirectory "quality-n0512-r120-gbuffer-position.pfm"
        $gbufferValidityPath = Join-Path $captureDirectory "quality-n0512-r120-gbuffer-validity.pfm"
        $required = @($resultPath, $capturePath, $logPath)
        if ([bool]$run.captureGBuffer) {
            $required += @($gbufferPositionPath, $gbufferValidityPath)
        }
        $canResume = [bool]$Resume
        foreach ($path in $required) {
            if (-not (Test-Path -LiteralPath $path -PathType Leaf)) {
                $canResume = $false
                break
            }
        }
        if ($canResume) {
            $result = Get-Content -LiteralPath $resultPath -Raw -Encoding UTF8 | ConvertFrom-Json
            if (-not [bool]$result.success -or
                [string]$result.pointLightStress.renderMode -ne [string]$run.renderMode -or
                [int]$result.pointLightStress.generatedLightCount -ne [int]$run.lightCount -or
                [int]$result.measuredFrames -ne 1) {
                throw "Invalid correctness resume result: $resultPath"
            }
            Write-Host "[correctness $correctnessOrdinal/$($correctnessExpected.Count)] SKIP verified $($run.stem)"
        }
        else {
            foreach ($path in $required) {
                if (Test-Path -LiteralPath $path) {
                    throw "Incomplete correctness run would be overwritten: $path"
                }
            }
            $arguments = @(
                "--gbuffer-position", "explicit",
                "--point-light-render-mode", $run.renderMode,
                "--point-light-offscreen-culling", "off",
                "--point-light-stencil-clear-mode", "coalesced-n-plus-one",
                "--point-light-stress",
                "--point-light-count", [string]$run.lightCount,
                "--point-light-coverage", $run.coverage,
                "--point-light-seed", "0x21D3F3A5",
                "--point-light-target-radius", (Radius-Text $run.radius),
                "--point-light-width", "1920",
                "--point-light-height", "1080",
                "--point-light-warmup-frames", "30",
                "--point-light-sample-frames", "1",
                "--point-light-result", $resultPath,
                "--point-light-capture", $capturePath
            )
            if ($run.renderMode -ne "analytic-screen") {
                $arguments += @("--point-light-grid-update", "cached")
            }
            if ([bool]$run.captureGBuffer) {
                $arguments += @(
                    "--classic-scene-gbuffer-position-capture", $gbufferPositionPath,
                    "--classic-scene-ssao-depth-capture", $gbufferValidityPath
                )
            }
            Write-Host "[correctness $correctnessOrdinal/$($correctnessExpected.Count)] RUN $($run.stem)"
            Push-Location $projectDirectory
            try {
                & $executable @arguments *> $logPath
                $exitCode = $LASTEXITCODE
            }
            finally { Pop-Location }
            if ($exitCode -ne 0) {
                throw "$($run.stem) failed with exit code $exitCode; log=$logPath"
            }
        }
        $result = Get-Content -LiteralPath $resultPath -Raw -Encoding UTF8 | ConvertFrom-Json
        if (-not [bool]$result.success -or
            [string]$result.pointLightStress.renderMode -ne [string]$run.renderMode -or
            [int]$result.pointLightStress.generatedLightCount -ne [int]$run.lightCount -or
            [Math]::Abs([double]$result.pointLightStress.volumeRadius - [double]$run.radius) -gt 0.0001 -or
            [int]$result.measuredFrames -ne 1) {
            throw "Correctness result failed validation: $resultPath"
        }
        $record = [ordered]@{
            stem = $run.stem
            resultSha256 = (Get-FileHash -LiteralPath $resultPath -Algorithm SHA256).Hash
            captureSha256 = (Get-FileHash -LiteralPath $capturePath -Algorithm SHA256).Hash
            logSha256 = (Get-FileHash -LiteralPath $logPath -Algorithm SHA256).Hash
        }
        if ([bool]$run.captureGBuffer) {
            $record.gbufferPosition = Get-RelativePath $gbufferPositionPath
            $record.gbufferPositionSha256 = (Get-FileHash -LiteralPath $gbufferPositionPath -Algorithm SHA256).Hash
            $record.gbufferValidity = Get-RelativePath $gbufferValidityPath
            $record.gbufferValiditySha256 = (Get-FileHash -LiteralPath $gbufferValidityPath -Algorithm SHA256).Hash
        }
        $correctnessCompleted += $record
    }
    $correctnessManifest = [ordered]@{
        schemaVersion = 1
        valid = $correctnessCompleted.Count -eq $correctnessExpected.Count
        protocolSha256 = $preCapture.protocolSha256
        expectedRuns = $correctnessExpected
        completedRuns = $correctnessCompleted
    }
    Write-JsonAtomic $correctnessManifestPath $correctnessManifest

    $finalManifest = [ordered]@{
        schemaVersion = 1
        experiment = $preCapture.experiment
        valid = $completed.Count -eq $expected.Count
        completedUtc = [DateTime]::UtcNow.ToString("o")
        preCaptureManifest = Get-RelativePath $preCaptureManifestPath
        preCaptureManifestSha256 = (Get-FileHash -LiteralPath $preCaptureManifestPath -Algorithm SHA256).Hash
        protocolSha256 = $preCapture.protocolSha256
        executableSha256 = $preCapture.executableSha256
        expectedRunCount = $expected.Count
        completedRunCount = $completed.Count
        correctnessRunCount = $correctnessCompleted.Count
        correctnessManifest = Get-RelativePath $correctnessManifestPath
        correctnessManifestSha256 = (Get-FileHash -LiteralPath $correctnessManifestPath -Algorithm SHA256).Hash
        expectedRuns = $expected
        completedRuns = $completed
    }
    Write-JsonAtomic $captureManifestPath $finalManifest
}

$python = Resolve-Python
if ($Mode -in @("All", "Analyze")) {
    & $python (Join-Path $PSScriptRoot "analyze_tile_cluster_runtime_boundary.py") `
        --run-dir $RunDirectory
    if ($LASTEXITCODE -ne 0) { throw "Runtime-boundary analysis failed: $LASTEXITCODE" }
}
if ($Mode -in @("All", "Verify")) {
    & $python (Join-Path $PSScriptRoot "verify_tile_cluster_runtime_boundary.py") `
        --run-dir $RunDirectory
    if ($LASTEXITCODE -ne 0) { throw "Runtime-boundary verification failed: $LASTEXITCODE" }
}

Write-Host "Tile/Cluster runtime-boundary workflow complete: $RunDirectory"
