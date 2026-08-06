param(
    [ValidateSet("All", "Formal", "Smoke", "Zero", "Analyze")]
    [string]$Mode = "All",

    [string]$RunName = ("stencil-clear-ab-" + (Get-Date -Format "yyyyMMdd-HHmmss")),

    [ValidateRange(1, 10)]
    [int]$ProcessRuns = 3,

    [ValidateRange(1, 1000000)]
    [int]$WarmupFrames = 300,

    [ValidateRange(1, 1000000)]
    [int]$SampleFrames = 600,

    [ValidateRange(1, 1000000)]
    [int]$SmokeWarmupFrames = 30,

    [ValidateRange(1, 1000000)]
    [int]$SmokeSampleFrames = 60,

    [string]$PythonExecutable = "",

    [switch]$SkipBuild,
    [switch]$Resume
)

Set-StrictMode -Version Latest
$ErrorActionPreference = "Stop"

$projectDirectory = (Resolve-Path (Join-Path $PSScriptRoot "..")).Path
$repositoryDirectory = (Resolve-Path (Join-Path $projectDirectory "..")).Path
$executable = Join-Path $repositoryDirectory "x64\Release\OpenGL_Learn.exe"
$runDirectory = Join-Path $projectDirectory "benchmark-results\point-light-stencil-clear-ab\$RunName"
$rawDirectory = Join-Path $runDirectory "raw"
$imageDirectory = Join-Path $runDirectory "images"
$logDirectory = Join-Path $runDirectory "logs"
$manifestPath = Join-Path $runDirectory "manifest.json"
$orderPath = Join-Path $runDirectory "launch-order.jsonl"
$analyzer = Join-Path $PSScriptRoot "analyze_point_light_stencil_clear_ab.py"

if ((Test-Path -LiteralPath $runDirectory) -and -not $Resume -and $Mode -ne "Analyze") {
    throw "Refusing to overwrite existing A/B directory: $runDirectory. Use -Resume only to continue it."
}
foreach ($path in @($runDirectory, $rawDirectory, $imageDirectory, $logDirectory)) {
    New-Item -ItemType Directory -Force -Path $path | Out-Null
}

function Resolve-Python {
    $candidates = [System.Collections.Generic.List[string]]::new()
    if (-not [string]::IsNullOrWhiteSpace($PythonExecutable)) {
        $candidates.Add($PythonExecutable)
    }
    if (-not [string]::IsNullOrWhiteSpace($env:USERPROFILE)) {
        $candidates.Add((Join-Path $env:USERPROFILE ".cache\codex-runtimes\codex-primary-runtime\dependencies\python\python.exe"))
    }
    foreach ($name in @("python3.exe", "python.exe", "py.exe")) {
        $command = Get-Command $name -ErrorAction SilentlyContinue
        if ($command) { $candidates.Add($command.Source) }
    }
    foreach ($candidate in ($candidates | Select-Object -Unique)) {
        if (-not (Test-Path -LiteralPath $candidate -PathType Leaf)) { continue }
        & $candidate -c "import sys" 2>$null
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

$script:launchIndex = if (Test-Path -LiteralPath $orderPath) {
    @(Get-Content -LiteralPath $orderPath).Count
}
else { 0 }

function Invoke-PointLightCase {
    param(
        [Parameter(Mandatory)] [string]$Classification,
        [Parameter(Mandatory)] [string]$Coverage,
        [Parameter(Mandatory)] [int]$LightCount,
        [Parameter(Mandatory)] [ValidateSet("legacy-2n", "coalesced-n-plus-one")] [string]$ClearMode,
        [Parameter(Mandatory)] [int]$RunIndex,
        [Parameter(Mandatory)] [int]$Warmup,
        [Parameter(Mandatory)] [int]$Samples,
        [switch]$LifecycleCheck
    )

    $stem = "{0}-{1}-{2:D4}-{3}-run{4:D2}" -f $Classification, $Coverage, $LightCount, $ClearMode, $RunIndex
    $jsonPath = Join-Path $rawDirectory "$stem.json"
    $capturePath = Join-Path $imageDirectory "$stem.ppm"
    $logPath = Join-Path $logDirectory "$stem.log"
    if ($Resume -and (Test-Path -LiteralPath $jsonPath -PathType Leaf)) {
        $existing = Get-Content -Raw -LiteralPath $jsonPath | ConvertFrom-Json
        if ([bool]$existing.success -and
            [string]$existing.pointLightStress.stencilClearMode -eq $ClearMode -and
            [int]$existing.pointLightStress.generatedLightCount -eq $LightCount) {
            Write-Host "SKIP existing $stem"
            return
        }
        throw "Existing result is not valid for resume: $jsonPath"
    }

    $script:launchIndex++
    $startUtc = [DateTime]::UtcNow
    Write-Host ("[{0:D2}] {1:o} {2}" -f $script:launchIndex, $startUtc, $stem)
    $arguments = @(
        "--gbuffer-position", "explicit",
        "--point-light-stencil-clear-mode", $ClearMode,
        "--point-light-stress",
        "--point-light-count", [string]$LightCount,
        "--point-light-coverage", $Coverage,
        "--point-light-seed", "0x21D3F3A5",
        "--point-light-warmup-frames", [string]$Warmup,
        "--point-light-sample-frames", [string]$Samples,
        "--point-light-result", $jsonPath,
        "--point-light-capture", $capturePath
    )
    if ($LifecycleCheck) { $arguments += "--point-light-stencil-lifecycle-check" }

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
    $orderRecord = [ordered]@{
        launchIndex = $script:launchIndex
        startUtc = $startUtc.ToString("o")
        endUtc = $endUtc.ToString("o")
        elapsedSeconds = [Math]::Round($stopwatch.Elapsed.TotalSeconds, 3)
        classification = $Classification
        coverage = $Coverage
        lightCount = $LightCount
        stencilClearMode = $ClearMode
        run = $RunIndex
        warmupFrames = $Warmup
        sampleFrames = $Samples
        lifecycleCheck = [bool]$LifecycleCheck
        exitCode = $exitCode
        json = $jsonPath
        capture = $capturePath
        log = $logPath
    }
    ($orderRecord | ConvertTo-Json -Compress) | Add-Content -LiteralPath $orderPath -Encoding utf8
    if ($exitCode -ne 0) { throw "$stem failed with exit code $exitCode; log=$logPath" }
    if (-not (Test-Path -LiteralPath $jsonPath -PathType Leaf)) { throw "$stem did not produce JSON" }

    $result = Get-Content -Raw -LiteralPath $jsonPath | ConvertFrom-Json
    $expectedPointClears = if ($ClearMode -eq "legacy-2n") {
        2 * $LightCount
    }
    elseif ($LightCount -gt 0) { $LightCount + 1 }
    else { 0 }
    if (-not [bool]$result.success -or
        [int]$result.schemaVersion -lt 24 -or
        [string]$result.pointLightStress.stencilClearMode -ne $ClearMode -or
        -not [bool]$result.pointLightStress.stencilClearModeExplicit -or
        [int]$result.pointLightStress.generatedLightCount -ne $LightCount -or
        [string]$result.pointLightStress.coverage -ne $Coverage -or
        [int]$result.profiler.summary.gpuFrame.count -ne $Samples -or
        [int]$result.profiler.summary.gpuZones.'Deferred Point Lights'.count -ne $Samples -or
        [double]$result.profiler.summary.pointLightsSubmitted.median -ne $LightCount -or
        [double]$result.profiler.summary.pointLightsCulled.median -ne 0 -or
        [double]$result.profiler.summary.pointLightStencilClears.median -ne $expectedPointClears -or
        [double]$result.profiler.summary.fixedStencilClears.median -ne 3 -or
        [double]$result.profiler.summary.stencilClears.median -ne ($expectedPointClears + 3)) {
        throw "$stem failed post-run invariant validation"
    }
    if ($LifecycleCheck -and -not [bool]$result.pointLightStress.stencilLifecycleValidation.clean) {
        throw "$stem left non-zero stencil pixels after the point-light phase"
    }
}

if ($Mode -in @("All", "Formal")) {
    if ($ProcessRuns -ne 3) {
        throw "The formal balanced plan requires exactly 3 independent processes per mode/configuration."
    }
    $formalPlan = @(
        @(16,  1, "legacy-2n"),             @(16,  1, "coalesced-n-plus-one"),
        @(512, 1, "coalesced-n-plus-one"),  @(512, 1, "legacy-2n"),
        @(256, 1, "legacy-2n"),             @(256, 1, "coalesced-n-plus-one"),
        @(256, 2, "coalesced-n-plus-one"),  @(256, 2, "legacy-2n"),
        @(512, 2, "legacy-2n"),             @(512, 2, "coalesced-n-plus-one"),
        @(16,  2, "coalesced-n-plus-one"),  @(16,  2, "legacy-2n"),
        @(16,  3, "legacy-2n"),             @(16,  3, "coalesced-n-plus-one"),
        @(512, 3, "coalesced-n-plus-one"),  @(512, 3, "legacy-2n"),
        @(256, 3, "legacy-2n"),             @(256, 3, "coalesced-n-plus-one")
    )
    foreach ($entry in $formalPlan) {
        Invoke-PointLightCase -Classification "formal" -Coverage "representative" `
            -LightCount ([int]$entry[0]) -RunIndex ([int]$entry[1]) `
            -ClearMode ([string]$entry[2]) -Warmup $WarmupFrames -Samples $SampleFrames
    }
}

if ($Mode -in @("All", "Smoke")) {
    foreach ($coverage in @("high-overlap", "edge-cases")) {
        foreach ($clearMode in @("legacy-2n", "coalesced-n-plus-one")) {
            Invoke-PointLightCase -Classification "smoke" -Coverage $coverage `
                -LightCount 16 -RunIndex 1 -ClearMode $clearMode `
                -Warmup $SmokeWarmupFrames -Samples $SmokeSampleFrames -LifecycleCheck
        }
    }
}

if ($Mode -in @("All", "Zero")) {
    foreach ($clearMode in @("legacy-2n", "coalesced-n-plus-one")) {
        Invoke-PointLightCase -Classification "zero" -Coverage "representative" `
            -LightCount 0 -RunIndex 1 -ClearMode $clearMode `
            -Warmup 5 -Samples 10 -LifecycleCheck
    }
}

$cpu = Get-CimInstance Win32_Processor | Select-Object -First 1
$os = Get-CimInstance Win32_OperatingSystem | Select-Object -First 1
$gpu = Get-CimInstance Win32_VideoController |
    Where-Object { $_.Name -match "NVIDIA|AMD|Intel" } |
    Select-Object -First 1
$commit = (& git -C $repositoryDirectory rev-parse HEAD 2>$null)
$launchOrder = @()
if (Test-Path -LiteralPath $orderPath -PathType Leaf) {
    $launchOrder = @(Get-Content -LiteralPath $orderPath | ForEach-Object { $_ | ConvertFrom-Json })
}
$manifest = [ordered]@{
    schemaVersion = 1
    runName = $RunName
    generatedAtUtc = [DateTime]::UtcNow.ToString("o")
    projectDirectory = $projectDirectory
    runDirectory = $runDirectory
    executable = $executable
    executableBytes = (Get-Item -LiteralPath $executable).Length
    executableSha256 = (Get-FileHash -LiteralPath $executable -Algorithm SHA256).Hash
    gitCommit = if ($commit) { [string]$commit } else { "unavailable" }
    protocol = [ordered]@{
        resolution = @(1920, 1080)
        scene = "classic-scenes/sponza/sponza.obj"
        camera = "fixed point-light-heavy camera"
        seed = "0x21D3F3A5"
        gBufferPosition = "explicit"
        renderPath = "phong-deferred-volume"
        requestedSwapInterval = 0
        ssao = $false
        bloom = $false
        pointShadows = $false
        formalCounts = @(16, 256, 512)
        processRuns = 3
        warmupFrames = $WarmupFrames
        sampleFrames = $SampleFrames
        launchPolicy = "balanced interleaving; explicit mode on every process"
        passGate = "exact image/signature/draw correctness; exact 2N to N+1 clears; both 256 and 512 pooled point-light GPU median improve by >=0.10 ms and >=2%, with 3/3 paired process medians positive"
    }
    machine = [ordered]@{
        cpu = if ($cpu) { [string]$cpu.Name } else { "unavailable" }
        logicalProcessors = if ($cpu) { [int]$cpu.NumberOfLogicalProcessors } else { 0 }
        gpu = if ($gpu) { [string]$gpu.Name } else { "unavailable" }
        gpuDriver = if ($gpu) { [string]$gpu.DriverVersion } else { "unavailable" }
        os = if ($os) { "{0} {1} build {2}" -f $os.Caption, $os.Version, $os.BuildNumber } else { "unavailable" }
    }
    launchOrder = $launchOrder
}
$manifest | ConvertTo-Json -Depth 10 | Set-Content -LiteralPath $manifestPath -Encoding utf8

$python = Resolve-Python
& $python $analyzer --run-dir $runDirectory
if ($LASTEXITCODE -ne 0) { throw "A/B analysis failed with exit code $LASTEXITCODE" }

Write-Host "A/B run directory: $runDirectory"
Write-Host "Report: $(Join-Path $runDirectory 'REPORT_CN.md')"
