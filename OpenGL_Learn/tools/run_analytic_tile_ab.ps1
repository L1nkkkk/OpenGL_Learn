param(
    [ValidateSet("All", "Capture", "Analyze", "Verify")]
    [string]$Mode = "All",
    [string]$RunDirectory = "",
    [string]$PythonExecutable = "",
    [ValidateRange(1, 1000000)][int]$WarmupFrames = 300,
    [ValidateRange(1, 1000000)][int]$SampleFrames = 600,
    [ValidateRange(1, 20)][int]$Rounds = 3,
    [switch]$Resume
)

Set-StrictMode -Version Latest
$ErrorActionPreference = "Stop"

$projectDirectory = Split-Path -Parent $PSScriptRoot
$repositoryDirectory = Split-Path -Parent $projectDirectory
$executable = Join-Path $repositoryDirectory "x64\Release\OpenGL_Learn.exe"
if ([string]::IsNullOrWhiteSpace($RunDirectory)) {
    $RunDirectory = Join-Path $projectDirectory `
        "benchmark-results\point-light-analytic-tile-ab\analytic-tile-ab-formal-20260806"
}
$RunDirectory = [IO.Path]::GetFullPath($RunDirectory)
$rawDirectory = Join-Path $RunDirectory "raw"
$captureDirectory = Join-Path $RunDirectory "captures"
$logDirectory = Join-Path $RunDirectory "logs"
$protocolPath = Join-Path $RunDirectory "PHASE0_FROZEN_PROTOCOL_CN.md"
$preCaptureManifestPath = Join-Path $RunDirectory "pre-capture-manifest.json"
$captureManifestPath = Join-Path $RunDirectory "capture-manifest.json"

foreach ($directory in @($RunDirectory, $rawDirectory, $captureDirectory, $logDirectory)) {
    New-Item -ItemType Directory -Force -Path $directory | Out-Null
}

function Resolve-Python {
    if (-not [string]::IsNullOrWhiteSpace($PythonExecutable)) {
        return [IO.Path]::GetFullPath($PythonExecutable)
    }
    foreach ($candidate in @(
        "C:\Users\Link\AppData\Local\Python\bin\python.exe",
        "C:\Users\Link\.cache\codex-runtimes\codex-primary-runtime\dependencies\python\python.exe",
        "python")) {
        try {
            & $candidate -c "import numpy, PIL, matplotlib" *> $null
            if ($LASTEXITCODE -eq 0) { return $candidate }
        }
        catch {}
    }
    throw "Python 3 with NumPy, Pillow, and Matplotlib was not found."
}

function Relative([string]$Path) {
    $base = $RunDirectory.TrimEnd([IO.Path]::DirectorySeparatorChar) + [IO.Path]::DirectorySeparatorChar
    $baseUri = [Uri]$base
    $pathUri = [Uri][IO.Path]::GetFullPath($Path)
    return [Uri]::UnescapeDataString($baseUri.MakeRelativeUri($pathUri).ToString()).Replace(
        '/', [IO.Path]::DirectorySeparatorChar)
}

function Write-JsonAtomic([string]$Path, $Value) {
    $temporary = "$Path.tmp"
    $Value | ConvertTo-Json -Depth 30 | Set-Content -LiteralPath $temporary -Encoding UTF8
    Move-Item -LiteralPath $temporary -Destination $Path -Force
}

function Radius-Token([double]$Radius) {
    return "{0:D3}" -f [int][Math]::Round($Radius * 10.0)
}

function Radius-Text([double]$Radius) {
    return $Radius.ToString("0.0############", [Globalization.CultureInfo]::InvariantCulture)
}

function Get-ZoneCount($Zones, [string]$Name) {
    $property = $Zones.PSObject.Properties[$Name]
    if ($null -eq $property -or $null -eq $property.Value) { return 0 }
    return @($property.Value).Count
}

function Assert-Result([string]$ResultPath, [string]$CapturePath, [string]$LogPath, $Run) {
    foreach ($path in @($ResultPath, $CapturePath, $LogPath)) {
        if (-not (Test-Path -LiteralPath $path -PathType Leaf)) { throw "Missing artifact: $path" }
    }
    $result = Get-Content -LiteralPath $ResultPath -Raw -Encoding UTF8 | ConvertFrom-Json
    $point = $result.pointLightStress
    $summary = $result.profiler.summary
    $samples = $result.profiler.samples
    $grid = $point.gridRuntime
    $isTile = [bool]$Run.tile
    $isMoving = [string]$Run.camera -eq "moving"
    $totalFrames = $WarmupFrames + $SampleFrames

    $checks = [ordered]@{
        success = [bool]$result.success
        release = [string]$result.buildConfiguration -eq "Release"
        architecture = [string]$result.architecture -eq "x64"
        resolution = [int]$result.resolution[0] -eq 1920 -and [int]$result.resolution[1] -eq 1080
        warmup = [int]$result.warmupFrames -eq $WarmupFrames
        samples = [int]$result.measuredFrames -eq $SampleFrames
        deferred = [bool]$result.settings.deferredRendering
        renderMode = [string]$point.renderMode -eq [string]$Run.renderMode
        renderModeExplicit = [bool]$point.renderModeExplicit
        offscreenCulling = -not [bool]$point.offscreenCulling -and [bool]$point.offscreenCullingExplicit
        lightCount = [int]$point.generatedLightCount -eq [int]$Run.lightCount
        radius = [Math]::Abs([double]$point.volumeRadius - [double]$Run.radius) -le 0.0001
        stressFrames = [int]$point.warmupFrames -eq $WarmupFrames -and [int]$point.sampleFrames -eq $SampleFrames
        noPointShadows = -not [bool]$point.pointShadowsEnabled
        wallSamples = @($samples.wallFrame).Count -eq $SampleFrames
        cpuSamples = @($samples.cpuFrame).Count -eq $SampleFrames
        gpuSamples = @($samples.gpuFrame).Count -eq $SampleFrames
        deferredPointCpuSamples = (Get-ZoneCount $samples.cpuZones "Deferred Point Lights") -eq $SampleFrames
        deferredPointGpuSamples = (Get-ZoneCount $samples.gpuZones "Deferred Point Lights") -eq $SampleFrames
        submittedLights = [int]$summary.pointLightsSubmitted.median -eq [int]$Run.lightCount
        culledLights = [int]$summary.pointLightsCulled.median -eq 0
        stencilDraws = [int]$summary.pointLightStencilDraws.median -eq 0
        volumeDraws = [int]$summary.pointLightLightingVolumeDraws.median -eq 0
    }

    if ($isMoving) {
        $checks.motionEnabled = [bool]$result.motionTimeline.enabled
        $checks.motionProfile = [string]$result.motionTimeline.profile -eq "camera"
        $checks.motionRate = [int]$result.motionTimeline.fixedFramesPerSecond -eq 60
        $checks.motionCycle = [int]$result.motionTimeline.cycleFrames -eq 600
        $checks.motionSamples = @($result.motionTimeline.samples).Count -eq $SampleFrames
        $checks.motionPositionAmplitude = [Math]::Abs(
            [double]$result.motionTimeline.amplitudeRatios.cameraPosition - 0.05) -le 0.000001
        $checks.motionTargetAmplitude = [Math]::Abs(
            [double]$result.motionTimeline.amplitudeRatios.cameraTarget - 0.01) -le 0.000001
    }
    else {
        $checks.motionDisabled = -not [bool]$result.motionTimeline.enabled
        $checks.motionProfile = [string]$result.motionTimeline.profile -eq "none"
        $checks.motionSamples = @($result.motionTimeline.samples).Count -eq 0
    }

    if ($isTile) {
        $expectedBuilds = if ($isMoving) { $totalFrames } else { 1 }
        $expectedHits = if ($isMoving) { 0 } else { $totalFrames - 1 }
        $checks.gridUpdate = [string]$point.gridUpdateMode -eq "cached" -and [bool]$point.gridUpdateModeExplicit
        $checks.sliceExplicit = [bool]$point.gridSliceCountExplicit -and [int]$point.gridSliceCountConfigured -eq 1
        $checks.gridValid = [bool]$grid.valid -and -not [bool]$grid.overflow -and
            [string]::IsNullOrEmpty([string]$grid.error)
        $checks.gridShape = [int]$grid.tileSize -eq 16 -and [int]$grid.sliceCount -eq 1 -and
            -not [bool]$grid.clustered -and [int]$grid.lightCount -eq [int]$Run.lightCount
        $checks.gridBuildCount = [int64]$grid.buildCount -eq $expectedBuilds
        $checks.gridUploadCount = [int64]$grid.uploadCount -eq $expectedBuilds
        $checks.gridCacheHits = [int64]$grid.cacheHitCount -eq $expectedHits
        $checks.screenDraws = [int]$summary.pointLightScreenDraws.median -eq 1
        $checks.gridLightingCpuSamples = (Get-ZoneCount $samples.cpuZones "Point Light Grid Lighting CPU") -eq $SampleFrames
        $checks.gridLightingGpuSamples = (Get-ZoneCount $samples.gpuZones "Point Light Grid Lighting GPU") -eq $SampleFrames
        $checks.gridBuildSamples = (Get-ZoneCount $samples.cpuZones "Point Light Grid Build") -eq $SampleFrames
        $checks.gridUploadSamples = (Get-ZoneCount $samples.cpuZones "Point Light Grid Upload") -eq $SampleFrames
        $checks.gridCacheSamples = (Get-ZoneCount $samples.cpuZones "Point Light Grid Cache Check") -eq $SampleFrames
    }
    else {
        $checks.screenDraws = [int]$summary.pointLightScreenDraws.median -eq [int]$Run.lightCount
        $checks.screenCpuSamples = (Get-ZoneCount $samples.cpuZones "Point Light Screen CPU") -eq $SampleFrames
        $checks.screenGpuSamples = (Get-ZoneCount $samples.gpuZones "Point Light Screen GPU") -eq $SampleFrames
    }

    $failed = @($checks.Keys | Where-Object { -not [bool]$checks[$_] })
    if ($failed.Count -gt 0) {
        throw "Frozen semantic validation failed for $($Run.stem): $($failed -join ', ')"
    }
    $logText = Get-Content -LiteralPath $LogPath -Raw -Encoding UTF8
    if ($logText -match "GL_INVALID|GL error|shader compilation failed|shader linking failed|failed to load shader") {
        throw "OpenGL/shader error found in log: $LogPath"
    }
}

$cells = @(
    [ordered]@{ name = "low-boundary"; lightCount = 16; radius = 1.5 },
    [ordered]@{ name = "low-radius"; lightCount = 64; radius = 1.5 },
    [ordered]@{ name = "wide-coverage"; lightCount = 64; radius = 12.0 },
    [ordered]@{ name = "representative"; lightCount = 256; radius = 6.0 },
    [ordered]@{ name = "heavy"; lightCount = 512; radius = 12.0 }
)
$paths = [ordered]@{
    "analytic-static" = [ordered]@{ name = "analytic-static"; renderMode = "analytic-screen"; camera = "static"; tile = $false }
    "tile-static" = [ordered]@{ name = "tile-static"; renderMode = "tile16"; camera = "static"; tile = $true }
    "analytic-moving" = [ordered]@{ name = "analytic-moving"; renderMode = "analytic-screen"; camera = "moving"; tile = $false }
    "tile-moving" = [ordered]@{ name = "tile-moving"; renderMode = "tile16"; camera = "moving"; tile = $true }
}
$pathOrders = @(
    @("analytic-static", "tile-static", "analytic-moving", "tile-moving"),
    @("tile-moving", "analytic-moving", "tile-static", "analytic-static"),
    @("tile-static", "analytic-static", "tile-moving", "analytic-moving")
)

$expected = @()
foreach ($round in 1..$Rounds) {
    if ($round -eq 1) { $roundCells = @($cells) }
    elseif ($round -eq 2) { $roundCells = @($cells[4], $cells[3], $cells[2], $cells[1], $cells[0]) }
    elseif ($round -eq 3) { $roundCells = @($cells[2], $cells[3], $cells[4], $cells[0], $cells[1]) }
    else {
        $offset = ($round - 1) % $cells.Count
        $roundCells = @()
        foreach ($index in 0..($cells.Count - 1)) { $roundCells += $cells[($index + $offset) % $cells.Count] }
    }
    $pathOrder = $pathOrders[($round - 1) % $pathOrders.Count]
    foreach ($cell in $roundCells) {
        foreach ($pathName in $pathOrder) {
            $path = $paths[$pathName]
            $stem = "{0}-n{1:D4}-r{2}-{3}-round{4}" -f `
                $cell.name, $cell.lightCount, (Radius-Token $cell.radius), $path.name, $round
            $expected += [ordered]@{
                stem = $stem
                cell = $cell.name
                lightCount = $cell.lightCount
                radius = [double]$cell.radius
                path = $path.name
                renderMode = $path.renderMode
                camera = $path.camera
                tile = [bool]$path.tile
                round = $round
                result = Relative (Join-Path $rawDirectory "$stem.json")
                capture = Relative (Join-Path $captureDirectory "$stem.ppm")
                log = Relative (Join-Path $logDirectory "$stem.log")
            }
        }
    }
}

if ($Mode -in @("All", "Capture")) {
    if (-not (Test-Path -LiteralPath $executable -PathType Leaf)) { throw "Missing Release executable: $executable" }
    if (-not (Test-Path -LiteralPath $protocolPath -PathType Leaf)) { throw "Missing frozen protocol: $protocolPath" }
    if (@(Get-Process -Name "OpenGL_Learn" -ErrorAction SilentlyContinue).Count -gt 0) {
        throw "OpenGL_Learn is already running; close it before formal capture."
    }

    $analyzerPath = Join-Path $PSScriptRoot "analyze_analytic_tile_ab.py"
    $verifierPath = Join-Path $PSScriptRoot "verify_analytic_tile_ab.py"
    foreach ($path in @($analyzerPath, $verifierPath)) {
        if (-not (Test-Path -LiteralPath $path -PathType Leaf)) { throw "Missing experiment tool: $path" }
    }
    $sourceFiles = [ordered]@{
        deferredCpp = Join-Path $projectDirectory "DeferRenderPass.cpp"
        deferredHeader = Join-Path $projectDirectory "DeferRenderPass.h"
        gridCpp = Join-Path $projectDirectory "PointLightGridRuntime.cpp"
        gridHeader = Join-Path $projectDirectory "PointLightGridRuntime.h"
        globalHeader = Join-Path $projectDirectory "Global.h"
        stressGenerator = Join-Path $projectDirectory "PointLightStressBenchmark.h"
        motionCpp = Join-Path $projectDirectory "BenchmarkMotionTimeline.cpp"
        motionHeader = Join-Path $projectDirectory "BenchmarkMotionTimeline.h"
        testDriver = Join-Path $projectDirectory "test.cpp"
        screenVertex = Join-Path $projectDirectory "shaders\lightVolumeFullscreenVertex.glsl"
        screenFragment = Join-Path $projectDirectory "shaders\lightVolumeFullscreenFragment.glsl"
        gridVertex = Join-Path $projectDirectory "shaders\pointLightGridVertex.glsl"
        gridFragment = Join-Path $projectDirectory "shaders\pointLightGridFragment.glsl"
        orchestrator = $PSCommandPath
        analyzer = $analyzerPath
        verifier = $verifierPath
    }
    $sourceHashes = [ordered]@{}
    foreach ($name in $sourceFiles.Keys) {
        if (-not (Test-Path -LiteralPath $sourceFiles[$name] -PathType Leaf)) { throw "Missing source: $($sourceFiles[$name])" }
        $sourceHashes[$name] = (Get-FileHash -LiteralPath $sourceFiles[$name] -Algorithm SHA256).Hash
    }
    $pre = [ordered]@{
        schemaVersion = 1
        experiment = "point-light-analytic-screen-vs-tile-s1"
        protocolFrozenBeforeCapture = $true
        createdUtc = [DateTime]::UtcNow.ToString("o")
        protocol = Relative $protocolPath
        protocolSha256 = (Get-FileHash -LiteralPath $protocolPath -Algorithm SHA256).Hash
        executable = $executable
        executableSha256 = (Get-FileHash -LiteralPath $executable -Algorithm SHA256).Hash
        buildConfiguration = "Release"
        architecture = "x64"
        resolution = @(1920, 1080)
        seed = "0x21D3F3A5"
        coverage = "representative"
        tileSize = 16
        sliceCount = 1
        warmupFrames = $WarmupFrames
        sampleFrames = $SampleFrames
        rounds = $Rounds
        cells = $cells
        cameraTimeline = [ordered]@{
            fixedFramesPerSecond = 60
            cycleFrames = 600
            positionAmplitudeRatio = 0.05
            targetAmplitudeRatio = 0.01
        }
        winnerThreshold = [ordered]@{
            absoluteMilliseconds = 0.05
            relativePercent = 3.0
            pairedDirectionAgreement = "$Rounds/$Rounds"
        }
        qualityGate = [ordered]@{
            maxChannelLsb = 2
            meanChannelLsb = 0.1
            p99ChannelLsb = 1
        }
        stopGate = [ordered]@{ singleProcessMinutes = 15; minimumFreeDiskGiB = 20 }
        sourceHashes = $sourceHashes
        expectedRuns = $expected
    }

    if (Test-Path -LiteralPath $preCaptureManifestPath -PathType Leaf) {
        if (-not $Resume) { throw "Pre-capture manifest exists; use -Resume to continue the frozen run." }
        $old = Get-Content -LiteralPath $preCaptureManifestPath -Raw -Encoding UTF8 | ConvertFrom-Json
        if ([string]$old.protocolSha256 -ne [string]$pre.protocolSha256 -or
            [string]$old.executableSha256 -ne [string]$pre.executableSha256 -or
            [int]$old.warmupFrames -ne $WarmupFrames -or [int]$old.sampleFrames -ne $SampleFrames -or
            [int]$old.rounds -ne $Rounds -or @($old.expectedRuns).Count -ne $expected.Count) {
            throw "Resume rejected: protocol, binary, or matrix changed."
        }
        foreach ($name in $sourceHashes.Keys) {
            if ([string]$old.sourceHashes.$name -ne [string]$sourceHashes[$name]) {
                throw "Resume rejected: source hash changed: $name"
            }
        }
        $oldStems = @($old.expectedRuns | ForEach-Object { [string]$_.stem }) -join "|"
        $newStems = @($expected | ForEach-Object { [string]$_.stem }) -join "|"
        if ($oldStems -ne $newStems) { throw "Resume rejected: run order changed." }
        $pre = $old
    }
    else {
        if (@(Get-ChildItem -LiteralPath $rawDirectory -File -ErrorAction SilentlyContinue).Count -gt 0 -or
            @(Get-ChildItem -LiteralPath $captureDirectory -File -ErrorAction SilentlyContinue).Count -gt 0 -or
            @(Get-ChildItem -LiteralPath $logDirectory -File -ErrorAction SilentlyContinue).Count -gt 0) {
            throw "Raw artifacts exist without a pre-capture manifest."
        }
        Write-JsonAtomic $preCaptureManifestPath $pre
    }

    $completed = @()
    $ordinal = 0
    foreach ($run in $expected) {
        ++$ordinal
        $resultPath = Join-Path $RunDirectory $run.result
        $capturePath = Join-Path $RunDirectory $run.capture
        $logPath = Join-Path $RunDirectory $run.log
        $existingCount = @(@($resultPath, $capturePath, $logPath) | Where-Object {
            Test-Path -LiteralPath $_ -PathType Leaf
        }).Count
        $resumeRun = $Resume -and $existingCount -eq 3
        if ($existingCount -gt 0 -and -not $resumeRun) {
            throw "Partial run would be overwritten: $($run.stem)"
        }

        if ($resumeRun) {
            Assert-Result $resultPath $capturePath $logPath $run
            Write-Host "[$ordinal/$($expected.Count)] SKIP $($run.stem)"
        }
        else {
            if ((Get-PSDrive -Name C).Free -lt 20GB) { throw "Frozen free-disk stop gate triggered." }
            $arguments = @(
                "--gbuffer-position", "explicit",
                "--point-light-render-mode", [string]$run.renderMode,
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
            if ([bool]$run.tile) {
                $arguments += @("--point-light-grid-update", "cached", "--point-light-grid-slices", "1")
            }
            if ([string]$run.camera -eq "moving") {
                $arguments += @(
                    "--classic-scene-deterministic-camera-timeline",
                    "--classic-scene-timeline-fps", "60",
                    "--classic-scene-timeline-cycle-frames", "600",
                    "--classic-scene-camera-timeline-position-radius-ratio", "0.05",
                    "--classic-scene-camera-timeline-target-radius-ratio", "0.01"
                )
            }
            Write-Host "[$ordinal/$($expected.Count)] RUN $($run.stem)"
            $timer = [Diagnostics.Stopwatch]::StartNew()
            Push-Location $projectDirectory
            try {
                & $executable @arguments *> $logPath
                $exitCode = $LASTEXITCODE
            }
            finally {
                Pop-Location
                $timer.Stop()
            }
            if ($exitCode -ne 0) { throw "$($run.stem) failed: exit=$exitCode log=$logPath" }
            if ($timer.Elapsed.TotalMinutes -gt 15) {
                throw "Frozen single-process time stop gate triggered after $($run.stem)."
            }
            Assert-Result $resultPath $capturePath $logPath $run
        }

        $completed += [ordered]@{
            stem = $run.stem
            resumed = [bool]$resumeRun
            resultSha256 = (Get-FileHash -LiteralPath $resultPath -Algorithm SHA256).Hash
            captureSha256 = (Get-FileHash -LiteralPath $capturePath -Algorithm SHA256).Hash
            logSha256 = (Get-FileHash -LiteralPath $logPath -Algorithm SHA256).Hash
        }
    }

    $manifest = [ordered]@{
        schemaVersion = 1
        experiment = "point-light-analytic-screen-vs-tile-s1"
        valid = $true
        completedUtc = [DateTime]::UtcNow.ToString("o")
        protocolSha256 = [string]$pre.protocolSha256
        preCaptureManifestSha256 = (Get-FileHash -LiteralPath $preCaptureManifestPath -Algorithm SHA256).Hash
        executableSha256 = [string]$pre.executableSha256
        expectedRunCount = $expected.Count
        completedRunCount = $completed.Count
        completedRuns = $completed
    }
    Write-JsonAtomic $captureManifestPath $manifest
}

$python = Resolve-Python
$analyzer = Join-Path $PSScriptRoot "analyze_analytic_tile_ab.py"
$verifier = Join-Path $PSScriptRoot "verify_analytic_tile_ab.py"
if ($Mode -in @("All", "Analyze")) {
    & $python $analyzer --run-dir $RunDirectory
    if ($LASTEXITCODE -ne 0) { throw "Analytic/Tile analysis failed with exit code $LASTEXITCODE" }
}
if ($Mode -in @("All", "Verify")) {
    & $python $verifier --run-dir $RunDirectory
    if ($LASTEXITCODE -ne 0) { throw "Analytic/Tile independent verification failed with exit code $LASTEXITCODE" }
    if ($Mode -eq "All") {
        & $python $analyzer --run-dir $RunDirectory
        if ($LASTEXITCODE -ne 0) { throw "Final report refresh failed with exit code $LASTEXITCODE" }
    }
}

Write-Host "[analytic-tile-ab] PASS mode=$Mode runDir=$RunDirectory"
