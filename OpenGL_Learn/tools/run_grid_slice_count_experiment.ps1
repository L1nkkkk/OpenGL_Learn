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
    $RunDirectory = Join-Path $projectDirectory "benchmark-results\point-light-grid-slice-count\grid-slice-count-formal-20260805"
}
$RunDirectory = [IO.Path]::GetFullPath($RunDirectory)
$rawDirectory = Join-Path $RunDirectory "raw"
$captureDirectory = Join-Path $RunDirectory "captures"
$logDirectory = Join-Path $RunDirectory "logs"
$protocolPath = Join-Path $RunDirectory "PHASE0_FROZEN_PROTOCOL_CN.md"
$preCaptureManifestPath = Join-Path $RunDirectory "pre-capture-manifest.json"
$captureManifestPath = Join-Path $RunDirectory "capture-manifest.json"
$correctnessManifestPath = Join-Path $RunDirectory "correctness-manifest.json"
$ledgerPath = Join-Path $RunDirectory "run-ledger.ndjson"
foreach ($directory in @($RunDirectory, $rawDirectory, $captureDirectory, $logDirectory)) {
    New-Item -ItemType Directory -Force -Path $directory | Out-Null
}

function Resolve-Python {
    if (-not [string]::IsNullOrWhiteSpace($PythonExecutable)) { return [IO.Path]::GetFullPath($PythonExecutable) }
    foreach ($candidate in @(
        "C:\Users\Link\AppData\Local\Python\bin\python.exe",
        "C:\Users\Link\.cache\codex-runtimes\codex-primary-runtime\dependencies\python\python.exe",
        "python")) {
        try { & $candidate --version *> $null; if ($LASTEXITCODE -eq 0) { return $candidate } } catch {}
    }
    throw "Python 3 with NumPy, Pillow, and Matplotlib was not found."
}

function Relative([string]$Path) {
    $base = $RunDirectory.TrimEnd([IO.Path]::DirectorySeparatorChar) + [IO.Path]::DirectorySeparatorChar
    $baseUri = [Uri]$base
    $pathUri = [Uri][IO.Path]::GetFullPath($Path)
    return [Uri]::UnescapeDataString($baseUri.MakeRelativeUri($pathUri).ToString()).Replace('/', [IO.Path]::DirectorySeparatorChar)
}

function Write-JsonAtomic([string]$Path, $Value) {
    $temporary = "$Path.tmp"
    $Value | ConvertTo-Json -Depth 20 | Set-Content -LiteralPath $temporary -Encoding UTF8
    Move-Item -LiteralPath $temporary -Destination $Path -Force
}

function Radius-Token([double]$Radius) { return "{0:D3}" -f [int][Math]::Round($Radius * 10.0) }
function Radius-Text([double]$Radius) { return $Radius.ToString("0.0############", [Globalization.CultureInfo]::InvariantCulture) }

function Assert-PerformanceResult([string]$ResultPath, [string]$CapturePath, $Run) {
    if (-not (Test-Path -LiteralPath $ResultPath -PathType Leaf) -or
        -not (Test-Path -LiteralPath $CapturePath -PathType Leaf)) { throw "Missing result/capture: $($Run.stem)" }
    $result = Get-Content -LiteralPath $ResultPath -Raw -Encoding UTF8 | ConvertFrom-Json
    $point = $result.pointLightStress
    $grid = $point.gridRuntime
    $expectedBuilds = if ($Run.regime -eq "rebuild") { $WarmupFrames + $SampleFrames } else { 1 }
    $expectedHits = if ($Run.regime -eq "cached") { $WarmupFrames + $SampleFrames - 1 } else { 0 }
    if (-not [bool]$result.success -or [string]$result.buildConfiguration -ne "Release" -or
        [string]$result.architecture -ne "x64" -or [int]$result.resolution[0] -ne 1920 -or
        [int]$result.resolution[1] -ne 1080 -or [int]$result.warmupFrames -ne $WarmupFrames -or
        [int]$result.measuredFrames -ne $SampleFrames -or [string]$point.renderMode -ne "cluster16" -or
        -not [bool]$point.renderModeExplicit -or [string]$point.gridUpdateMode -ne $Run.regime -or
        -not [bool]$point.gridUpdateModeExplicit -or -not [bool]$point.gridSliceCountExplicit -or
        [int]$point.gridSliceCountConfigured -ne [int]$Run.sliceCount -or [int]$grid.sliceCount -ne [int]$Run.sliceCount -or
        [bool]$grid.clustered -ne ([int]$Run.sliceCount -gt 1) -or [int]$point.generatedLightCount -ne [int]$Run.lightCount -or
        [Math]::Abs([double]$point.volumeRadius - [double]$Run.radius) -gt 0.0001 -or
        [bool]$point.offscreenCulling -or -not [bool]$point.offscreenCullingExplicit -or
        -not [bool]$grid.valid -or [bool]$grid.overflow -or -not [string]::IsNullOrEmpty([string]$grid.error) -or
        [int]$grid.lightCount -ne [int]$Run.lightCount -or [int64]$grid.buildCount -ne $expectedBuilds -or
        [int64]$grid.uploadCount -ne $expectedBuilds -or [int64]$grid.cacheHitCount -ne $expectedHits -or
        [int]$result.profiler.summary.wallFrame.count -ne $SampleFrames -or
        [int]$result.profiler.summary.cpuFrame.count -ne $SampleFrames -or
        [int]$result.profiler.summary.gpuFrame.count -ne $SampleFrames -or
        [int]$result.profiler.summary.gpuZones.'Point Light Grid Lighting GPU'.count -ne $SampleFrames -or
        [int]$result.profiler.summary.pointLightScreenDraws.median -ne 1 -or
        [int]$result.profiler.summary.pointLightStencilDraws.median -ne 0 -or
        [int]$result.profiler.summary.pointLightLightingVolumeDraws.median -ne 0) {
        throw "Result failed frozen validation: $ResultPath"
    }
}

$slices = @(1, 2, 4, 8, 16)
$cachedCounts = @(32, 64, 128, 256, 512)
$cachedRadii = @(1.5, 3.0, 6.0, 8.0, 12.0)
$rebuildCounts = @(32, 128, 256, 512)
$rebuildRadii = @(1.5, 3.0, 6.0, 12.0)
$expected = @()
foreach ($round in 1..$Rounds) {
    foreach ($regime in @("cached", "rebuild")) {
        $counts = if ($regime -eq "cached") { $cachedCounts } else { $rebuildCounts }
        $radii = if ($regime -eq "cached") { $cachedRadii } else { $rebuildRadii }
        $cells = @()
        foreach ($count in $counts) { foreach ($radius in $radii) { $cells += [ordered]@{ lightCount=$count; radius=[double]$radius } } }
        if (($round % 2) -eq 0) { [array]::Reverse($cells) }
        $sliceOrder = @($slices)
        if (($round % 2) -eq 0) { [array]::Reverse($sliceOrder) }
        foreach ($cell in $cells) {
            foreach ($sliceCount in $sliceOrder) {
                $stem = "{0}-n{1:D4}-r{2}-s{3:D2}-round{4}" -f $regime,$cell.lightCount,(Radius-Token $cell.radius),$sliceCount,$round
                $expected += [ordered]@{
                    stem=$stem; regime=$regime; lightCount=$cell.lightCount; radius=$cell.radius; sliceCount=$sliceCount; round=$round
                    result=Relative (Join-Path $rawDirectory "$stem.json")
                    capture=Relative (Join-Path $captureDirectory "$stem.ppm")
                    log=Relative (Join-Path $logDirectory "$stem.log")
                }
            }
        }
    }
}

$correctnessExpected = @()
foreach ($quality in @(
    [ordered]@{lightCount=512;radius=1.5;name="low"},
    [ordered]@{lightCount=256;radius=6.0;name="boundary"},
    [ordered]@{lightCount=512;radius=12.0;name="high"})) {
    foreach ($sliceCount in @(0,1,2,4,8,16)) {
        $kind = if ($sliceCount -eq 0) { "oracle" } else { "grid" }
        $stem = "quality-{0}-n{1:D4}-r{2}-{3}" -f $quality.name,$quality.lightCount,(Radius-Token $quality.radius),$(if($sliceCount -eq 0){"analytic"}else{"s{0:D2}" -f $sliceCount})
        $correctnessExpected += [ordered]@{
            stem=$stem; kind="oracle-quality"; lightCount=$quality.lightCount; radius=[double]$quality.radius
            coverage="representative"; sliceCount=$sliceCount; renderMode=$(if($sliceCount -eq 0){"analytic-screen"}else{"cluster16"})
            captureGBuffer=($quality.name -eq "high" -and $sliceCount -eq 1)
            result=Relative (Join-Path $rawDirectory "$stem.json"); capture=Relative (Join-Path $captureDirectory "$stem.ppm"); log=Relative (Join-Path $logDirectory "$stem.log")
        }
    }
}
foreach ($edge in @(
    [ordered]@{lightCount=0;coverage="representative";name="n0"},
    [ordered]@{lightCount=1;coverage="representative";name="n1"},
    [ordered]@{lightCount=16;coverage="edge-cases";name="fixtures"})) {
    foreach ($sliceCount in $slices) {
        $stem = "edge-{0}-s{1:D2}" -f $edge.name,$sliceCount
        $correctnessExpected += [ordered]@{
            stem=$stem; kind="edge"; lightCount=$edge.lightCount; radius=3.0; coverage=$edge.coverage; sliceCount=$sliceCount; renderMode="cluster16"; captureGBuffer=$false
            result=Relative (Join-Path $rawDirectory "$stem.json"); capture=Relative (Join-Path $captureDirectory "$stem.ppm"); log=Relative (Join-Path $logDirectory "$stem.log")
        }
    }
}
foreach ($compat in @(
    [ordered]@{name="tile-legacy";renderMode="tile16";sliceCount=0},
    [ordered]@{name="tile-explicit-s1";renderMode="tile16";sliceCount=1},
    [ordered]@{name="cluster-legacy";renderMode="cluster16";sliceCount=0},
    [ordered]@{name="cluster-explicit-s16";renderMode="cluster16";sliceCount=16})) {
    $stem = "compat-$($compat.name)"
    $correctnessExpected += [ordered]@{
        stem=$stem;kind="compat";lightCount=128;radius=3.0;coverage="representative";sliceCount=$compat.sliceCount;renderMode=$compat.renderMode;captureGBuffer=$false
        result=Relative (Join-Path $rawDirectory "$stem.json");capture=Relative (Join-Path $captureDirectory "$stem.ppm");log=Relative (Join-Path $logDirectory "$stem.log")
    }
}
$correctnessExpected += [ordered]@{
    stem="cache-slice-cycle";kind="slice-cycle";lightCount=128;radius=3.0;coverage="representative";sliceCount=1;renderMode="cluster16";captureGBuffer=$false
    result=Relative (Join-Path $rawDirectory "cache-slice-cycle.json");capture=Relative (Join-Path $captureDirectory "cache-slice-cycle.ppm");log=Relative (Join-Path $logDirectory "cache-slice-cycle.log")
}

if ($Mode -in @("All","Capture")) {
    if (-not (Test-Path $executable -PathType Leaf)) { throw "Missing Release executable: $executable" }
    if (-not (Test-Path $protocolPath -PathType Leaf)) { throw "Missing frozen protocol: $protocolPath" }
    $analyzerPath = Join-Path $PSScriptRoot "analyze_grid_slice_count_experiment.py"
    $verifierPath = Join-Path $PSScriptRoot "verify_grid_slice_count_experiment.py"
    foreach ($path in @($analyzerPath,$verifierPath)) { if (-not (Test-Path $path -PathType Leaf)) { throw "Missing tool: $path" } }
    $sourceHashes = [ordered]@{
        runtimeCpp=(Get-FileHash (Join-Path $projectDirectory "PointLightGridRuntime.cpp") -Algorithm SHA256).Hash
        runtimeHeader=(Get-FileHash (Join-Path $projectDirectory "PointLightGridRuntime.h") -Algorithm SHA256).Hash
        globalHeader=(Get-FileHash (Join-Path $projectDirectory "Global.h") -Algorithm SHA256).Hash
        deferredPass=(Get-FileHash (Join-Path $projectDirectory "DeferRenderPass.cpp") -Algorithm SHA256).Hash
        testDriver=(Get-FileHash (Join-Path $projectDirectory "test.cpp") -Algorithm SHA256).Hash
        generator=(Get-FileHash (Join-Path $projectDirectory "PointLightStressBenchmark.h") -Algorithm SHA256).Hash
        gridFragmentShader=(Get-FileHash (Join-Path $projectDirectory "shaders\pointLightGridFragment.glsl") -Algorithm SHA256).Hash
        orchestrator=(Get-FileHash $PSCommandPath -Algorithm SHA256).Hash
        analyzer=(Get-FileHash $analyzerPath -Algorithm SHA256).Hash
        verifier=(Get-FileHash $verifierPath -Algorithm SHA256).Hash
        fnvHelper=(Get-FileHash (Join-Path $PSScriptRoot "fnv1a64_stdin.cpp") -Algorithm SHA256).Hash
    }
    $pre = [ordered]@{
        schemaVersion=1;experiment="point-light-grid-slice-count-runtime";protocolFrozenBeforeCapture=$true;createdUtc=[DateTime]::UtcNow.ToString("o")
        protocol=Relative $protocolPath;protocolSha256=(Get-FileHash $protocolPath -Algorithm SHA256).Hash
        executable=$executable;executableSha256=(Get-FileHash $executable -Algorithm SHA256).Hash
        buildConfiguration="Release";architecture="x64";resolution=@(1920,1080);seed="0x21D3F3A5";tileSize=16;sliceCounts=$slices
        cachedCounts=$cachedCounts;cachedRadii=$cachedRadii;rebuildCounts=$rebuildCounts;rebuildRadii=$rebuildRadii
        warmupFrames=$WarmupFrames;sampleFrames=$SampleFrames;rounds=$Rounds
        winnerThreshold=[ordered]@{absoluteMilliseconds=0.05;relativePercent=3.0;pairedDirectionAgreement="$Rounds/$Rounds";engineeringTieBreak="smallest-slice-count"}
        stopGate=[ordered]@{singleProcessMinutes=15;minimumFreeDiskGiB=100}
        sourceHashes=$sourceHashes;expectedRuns=$expected;expectedCorrectnessRuns=$correctnessExpected
    }
    if (Test-Path $preCaptureManifestPath -PathType Leaf) {
        if (-not $Resume) { throw "Pre-capture manifest exists; use -Resume." }
        $old = Get-Content $preCaptureManifestPath -Raw -Encoding UTF8 | ConvertFrom-Json
        if ([string]$old.protocolSha256 -ne [string]$pre.protocolSha256 -or [string]$old.executableSha256 -ne [string]$pre.executableSha256 -or
            [int]$old.expectedRuns.Count -ne $expected.Count -or [int]$old.warmupFrames -ne $WarmupFrames -or [int]$old.sampleFrames -ne $SampleFrames) {
            throw "Resume rejected: frozen protocol/binary/matrix changed."
        }
        foreach($name in $sourceHashes.Keys){if([string]$old.sourceHashes.$name -ne [string]$sourceHashes[$name]){throw "Resume rejected: source hash changed: $name"}}
    } else {
        if (@(Get-ChildItem $rawDirectory -File -ErrorAction SilentlyContinue).Count -gt 0 -or @(Get-ChildItem $captureDirectory -File -ErrorAction SilentlyContinue).Count -gt 0) {
            throw "Raw artifacts exist without a pre-capture manifest."
        }
        Write-JsonAtomic $preCaptureManifestPath $pre
    }

    $completed = @(); $ordinal=0
    foreach($run in $expected){
        ++$ordinal; $resultPath=Join-Path $RunDirectory $run.result; $capturePath=Join-Path $RunDirectory $run.capture; $logPath=Join-Path $RunDirectory $run.log
        $resumeRun=$Resume -and (Test-Path $resultPath -PathType Leaf) -and (Test-Path $capturePath -PathType Leaf) -and (Test-Path $logPath -PathType Leaf)
        if($resumeRun){Assert-PerformanceResult $resultPath $capturePath $run; Write-Host "[$ordinal/$($expected.Count)] SKIP $($run.stem)"}
        else {
            foreach($path in @($resultPath,$capturePath,$logPath)){if(Test-Path $path){throw "Incomplete run would be overwritten: $path"}}
            if((Get-PSDrive C).Free -lt 100GB){throw "Frozen disk-space stop gate triggered."}
            $arguments=@("--gbuffer-position","explicit","--point-light-render-mode","cluster16","--point-light-grid-slices",[string]$run.sliceCount,
                "--point-light-grid-update",$run.regime,"--point-light-offscreen-culling","off","--point-light-stencil-clear-mode","coalesced-n-plus-one",
                "--point-light-stress","--point-light-count",[string]$run.lightCount,"--point-light-coverage","representative","--point-light-seed","0x21D3F3A5",
                "--point-light-target-radius",(Radius-Text $run.radius),"--point-light-width","1920","--point-light-height","1080",
                "--point-light-warmup-frames",[string]$WarmupFrames,"--point-light-sample-frames",[string]$SampleFrames,"--point-light-result",$resultPath,"--point-light-capture",$capturePath)
            Write-Host "[$ordinal/$($expected.Count)] RUN $($run.stem)"; $start=[DateTime]::UtcNow; $timer=[Diagnostics.Stopwatch]::StartNew()
            Push-Location $projectDirectory; try { & $executable @arguments *> $logPath; $exit=$LASTEXITCODE } finally { Pop-Location; $timer.Stop() }
            if($exit -ne 0){throw "$($run.stem) failed: exit=$exit log=$logPath"}
            if($timer.Elapsed.TotalMinutes -gt 15){throw "Frozen single-process time stop gate triggered after $($run.stem)."}
            Assert-PerformanceResult $resultPath $capturePath $run
        }
        $record=[ordered]@{stem=$run.stem;resumed=[bool]$resumeRun;exitCode=0;resultSha256=(Get-FileHash $resultPath -Algorithm SHA256).Hash;captureSha256=(Get-FileHash $capturePath -Algorithm SHA256).Hash;logSha256=(Get-FileHash $logPath -Algorithm SHA256).Hash}
        ($record|ConvertTo-Json -Compress)|Add-Content $ledgerPath -Encoding UTF8; $completed += $record
    }

    $correctnessCompleted=@();$co=0
    foreach($run in $correctnessExpected){
        ++$co;$resultPath=Join-Path $RunDirectory $run.result;$capturePath=Join-Path $RunDirectory $run.capture;$logPath=Join-Path $RunDirectory $run.log
        $positionPath=Join-Path $captureDirectory "truth-gbuffer-position.pfm";$validityPath=Join-Path $captureDirectory "truth-gbuffer-validity.pfm"
        $required=@($resultPath,$capturePath,$logPath);if($run.captureGBuffer){$required+=@($positionPath,$validityPath)}
        $resumeRun=[bool]$Resume;foreach($path in $required){if(-not(Test-Path $path -PathType Leaf)){$resumeRun=$false}}
        if(-not $resumeRun){
            foreach($path in $required){if(Test-Path $path){throw "Incomplete correctness run would be overwritten: $path"}}
            $arguments=@("--gbuffer-position","explicit","--point-light-render-mode",$run.renderMode,"--point-light-offscreen-culling","off",
                "--point-light-stencil-clear-mode","coalesced-n-plus-one","--point-light-stress","--point-light-count",[string]$run.lightCount,
                "--point-light-coverage",$run.coverage,"--point-light-seed","0x21D3F3A5","--point-light-target-radius",(Radius-Text $run.radius),
                "--point-light-width","1920","--point-light-height","1080","--point-light-warmup-frames","30","--point-light-sample-frames","1",
                "--point-light-result",$resultPath,"--point-light-capture",$capturePath)
            if($run.renderMode -ne "analytic-screen"){$arguments+=@("--point-light-grid-update","cached")}
            if([int]$run.sliceCount -gt 0){$arguments+=@("--point-light-grid-slices",[string]$run.sliceCount)}
            if($run.kind -eq "slice-cycle"){$arguments+=@("--point-light-grid-slice-cycle")}
            if($run.captureGBuffer){$arguments+=@("--classic-scene-gbuffer-position-capture",$positionPath,"--classic-scene-ssao-depth-capture",$validityPath)}
            Write-Host "[correctness $co/$($correctnessExpected.Count)] RUN $($run.stem)"
            Push-Location $projectDirectory;try{& $executable @arguments *> $logPath;$exit=$LASTEXITCODE}finally{Pop-Location};if($exit -ne 0){throw "$($run.stem) failed: $exit"}
        }
        $result=Get-Content $resultPath -Raw -Encoding UTF8|ConvertFrom-Json
        if(-not [bool]$result.success -or [int]$result.pointLightStress.generatedLightCount -ne [int]$run.lightCount -or [int]$result.measuredFrames -ne 1){throw "Correctness validation failed: $($run.stem)"}
        if($run.kind -eq "slice-cycle" -and ([int64]$result.pointLightStress.gridRuntime.buildCount -ne 31 -or [int64]$result.pointLightStress.gridRuntime.cacheHitCount -ne 0)){throw "Slice cache invalidation failed."}
        $record=[ordered]@{stem=$run.stem;resultSha256=(Get-FileHash $resultPath -Algorithm SHA256).Hash;captureSha256=(Get-FileHash $capturePath -Algorithm SHA256).Hash;logSha256=(Get-FileHash $logPath -Algorithm SHA256).Hash}
        if($run.captureGBuffer){$record.position=Relative $positionPath;$record.positionSha256=(Get-FileHash $positionPath -Algorithm SHA256).Hash;$record.validity=Relative $validityPath;$record.validitySha256=(Get-FileHash $validityPath -Algorithm SHA256).Hash}
        $correctnessCompleted += $record
    }
    Write-JsonAtomic $correctnessManifestPath ([ordered]@{schemaVersion=1;valid=$correctnessCompleted.Count -eq $correctnessExpected.Count;protocolSha256=$pre.protocolSha256;expectedRuns=$correctnessExpected;completedRuns=$correctnessCompleted})
    Write-JsonAtomic $captureManifestPath ([ordered]@{schemaVersion=1;experiment=$pre.experiment;valid=$completed.Count -eq $expected.Count;completedUtc=[DateTime]::UtcNow.ToString("o");preCaptureManifest=Relative $preCaptureManifestPath;preCaptureManifestSha256=(Get-FileHash $preCaptureManifestPath -Algorithm SHA256).Hash;protocolSha256=$pre.protocolSha256;executableSha256=$pre.executableSha256;expectedRunCount=$expected.Count;completedRunCount=$completed.Count;correctnessRunCount=$correctnessCompleted.Count;correctnessManifest=Relative $correctnessManifestPath;correctnessManifestSha256=(Get-FileHash $correctnessManifestPath -Algorithm SHA256).Hash;expectedRuns=$expected;completedRuns=$completed})
}

$python=Resolve-Python
if($Mode -in @("All","Analyze")){& $python (Join-Path $PSScriptRoot "analyze_grid_slice_count_experiment.py") --run-dir $RunDirectory;if($LASTEXITCODE -ne 0){throw "Analysis failed: $LASTEXITCODE"}}
if($Mode -in @("All","Verify")){& $python (Join-Path $PSScriptRoot "verify_grid_slice_count_experiment.py") --run-dir $RunDirectory;if($LASTEXITCODE -ne 0){throw "Verification failed: $LASTEXITCODE"}}
Write-Host "Grid slice-count experiment complete: $RunDirectory"
