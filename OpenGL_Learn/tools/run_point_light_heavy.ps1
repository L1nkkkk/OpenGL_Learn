param(
    [ValidateSet("All", "Formal", "Smoke", "Analyze")]
    [string]$Mode = "All",

    [int[]]$FormalCounts = @(16, 64, 256, 512),

    [ValidateRange(1, 10)]
    [int]$ProcessRuns = 3,

    [ValidateRange(1, 1000000)]
    [int]$WarmupFrames = 300,

    [ValidateRange(1, 1000000)]
    [int]$SampleFrames = 600,

    [ValidateRange(1, 1000000)]
    [int]$SmokeWarmupFrames = 10,

    [ValidateRange(1, 1000000)]
    [int]$SmokeSampleFrames = 30,

    [string]$RunName = ("legacy-baseline-" + (Get-Date -Format "yyyyMMdd-HHmmss")),

    [string]$PythonExecutable = "",

    [switch]$SkipBuild
)

Set-StrictMode -Version Latest
$ErrorActionPreference = "Stop"

$projectDirectory = Split-Path -Parent $PSScriptRoot
$repositoryDirectory = Split-Path -Parent $projectDirectory
$executable = Join-Path $repositoryDirectory "x64\Release\OpenGL_Learn.exe"
$runDirectory = Join-Path $projectDirectory "benchmark-results\point-light-heavy\$RunName"
$imageDirectory = Join-Path $projectDirectory "docs\benchmark-images\point-light-heavy\$RunName"
$reportPath = Join-Path $projectDirectory "POINT_LIGHT_HEAVY_BASELINE_CN.md"
$chartPath = Join-Path $imageDirectory "legacy-point-light-scaling.svg"
$analyzer = Join-Path $PSScriptRoot "analyze_point_light_heavy.py"
$manifestPath = Join-Path $runDirectory "manifest.json"

New-Item -ItemType Directory -Force -Path $runDirectory, $imageDirectory | Out-Null

if (-not $SkipBuild -and $Mode -ne "Analyze") {
    $vswhere = Join-Path ${env:ProgramFiles(x86)} "Microsoft Visual Studio\Installer\vswhere.exe"
    if (-not (Test-Path -LiteralPath $vswhere -PathType Leaf)) {
        throw "vswhere not found: $vswhere"
    }
    $msbuild = & $vswhere -latest -products * -requires Microsoft.Component.MSBuild -find "MSBuild\**\Bin\MSBuild.exe" | Select-Object -First 1
    if ([string]::IsNullOrWhiteSpace($msbuild)) {
        throw "MSBuild not found"
    }
    & $msbuild (Join-Path $repositoryDirectory "OpenGL_Learn.sln") /m /p:Configuration=Release /p:Platform=x64 /v:minimal
    if ($LASTEXITCODE -ne 0) {
        throw "Release build failed with exit code $LASTEXITCODE"
    }
}

if (-not (Test-Path -LiteralPath $executable -PathType Leaf)) {
    throw "Release executable not found: $executable"
}

$results = [System.Collections.Generic.List[object]]::new()

function Resolve-PythonExecutable {
    $candidates = [System.Collections.Generic.List[string]]::new()
    if (-not [string]::IsNullOrWhiteSpace($PythonExecutable)) {
        $candidates.Add($PythonExecutable)
    }
    $candidates.Add((Join-Path $repositoryDirectory ".venv\Scripts\python.exe"))
    $candidates.Add((Join-Path $projectDirectory ".venv\Scripts\python.exe"))
    if (-not [string]::IsNullOrWhiteSpace($env:USERPROFILE)) {
        $candidates.Add((Join-Path $env:USERPROFILE ".cache\codex-runtimes\codex-primary-runtime\dependencies\python\python.exe"))
    }
    foreach ($commandName in @("python3.exe", "python.exe", "py.exe")) {
        $command = Get-Command $commandName -ErrorAction SilentlyContinue
        if ($command) {
            $candidates.Add($command.Source)
        }
    }

    foreach ($candidate in ($candidates | Select-Object -Unique)) {
        if (-not (Test-Path -LiteralPath $candidate -PathType Leaf)) {
            continue
        }
        & $candidate -c "import sys" 2>$null
        if ($LASTEXITCODE -eq 0) {
            return $candidate
        }
    }
    throw "Python 3 was not found. Pass -PythonExecutable with an explicit python.exe path."
}

function Invoke-PointLightRun {
    param(
        [Parameter(Mandatory)] [string]$Classification,
        [Parameter(Mandatory)] [string]$Coverage,
        [Parameter(Mandatory)] [int]$LightCount,
        [Parameter(Mandatory)] [int]$RunIndex,
        [Parameter(Mandatory)] [int]$Warmup,
        [Parameter(Mandatory)] [int]$Samples
    )

    if ($LightCount -notin @(16, 64, 256, 512)) {
        throw "Unsupported light count: $LightCount"
    }
    $stem = "{0}-{1}-{2:D4}-run{3:D2}" -f $Classification.ToLowerInvariant(), $Coverage, $LightCount, $RunIndex
    $jsonPath = Join-Path $runDirectory "$stem.json"
    $logPath = Join-Path $runDirectory "$stem.log"
    $captureStem = "{0}-{1}-{2:D4}" -f $Classification.ToLowerInvariant(), $Coverage, $LightCount
    $capturePath = Join-Path $imageDirectory "$captureStem.ppm"
    $arguments = @(
        "--point-light-stencil-clear-mode", "legacy-2n",
        "--point-light-stress",
        "--point-light-count", $LightCount,
        "--point-light-coverage", $Coverage,
        "--point-light-seed", "0x21D3F3A5",
        "--point-light-warmup-frames", $Warmup,
        "--point-light-sample-frames", $Samples,
        "--point-light-result", $jsonPath,
        "--point-light-capture", $capturePath
    )

    Write-Host ("[{0}] {1}/{2} lights={3} run={4} warmup={5} samples={6}" -f (Get-Date -Format "HH:mm:ss"), $Classification, $Coverage, $LightCount, $RunIndex, $Warmup, $Samples)
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
    if (-not (Test-Path -LiteralPath $jsonPath -PathType Leaf)) {
        throw "$stem did not produce JSON"
    }
    $result = Get-Content -Raw -LiteralPath $jsonPath | ConvertFrom-Json
    if (-not [bool]$result.success -or [int]$result.schemaVersion -lt 23) {
        throw "$stem produced an invalid result"
    }
    if ([int]$result.pointLightStress.generatedLightCount -ne $LightCount -or
        [string]$result.pointLightStress.coverage -ne $Coverage -or
        [string]$result.pointLightStress.stencilClearMode -ne "legacy-2n" -or
        -not [bool]$result.pointLightStress.stencilClearModeExplicit -or
        [int]$result.profiler.summary.gpuFrame.count -ne $Samples -or
        [int]$result.profiler.summary.gpuZones.'Deferred Point Lights'.count -ne $Samples) {
        throw "$stem failed invariant validation"
    }
    $results.Add([ordered]@{
        classification = $Classification.ToLowerInvariant()
        coverage = $Coverage
        lightCount = $LightCount
        run = $RunIndex
        warmupFrames = $Warmup
        sampleFrames = $Samples
        elapsedSeconds = [Math]::Round($stopwatch.Elapsed.TotalSeconds, 3)
        sceneSignature = [string]$result.pointLightStress.sceneSignature
        submissionSignature = [string]$result.pointLightStress.submissionSignature
        json = $jsonPath
        log = $logPath
        capture = $capturePath
    })
}

if ($Mode -in @("All", "Smoke")) {
    foreach ($coverage in @("representative", "high-overlap", "edge-cases")) {
        Invoke-PointLightRun -Classification "smoke" -Coverage $coverage -LightCount 16 -RunIndex 1 -Warmup $SmokeWarmupFrames -Samples $SmokeSampleFrames
    }
}

if ($Mode -in @("All", "Formal")) {
    foreach ($lightCount in $FormalCounts) {
        foreach ($runIndex in 1..$ProcessRuns) {
            Invoke-PointLightRun -Classification "formal" -Coverage "representative" -LightCount $lightCount -RunIndex $runIndex -Warmup $WarmupFrames -Samples $SampleFrames
        }
    }
}

$cpu = Get-CimInstance Win32_Processor | Select-Object -First 1
$os = Get-CimInstance Win32_OperatingSystem | Select-Object -First 1
$gpu = Get-CimInstance Win32_VideoController |
    Where-Object { $_.Name -match "NVIDIA|AMD|Intel" } |
    Select-Object -First 1
$commit = (& git -C $repositoryDirectory rev-parse HEAD 2>$null)
$runMap = [ordered]@{}
if (Test-Path -LiteralPath $manifestPath -PathType Leaf) {
    $existingManifest = Get-Content -LiteralPath $manifestPath -Raw | ConvertFrom-Json
    foreach ($existingRun in @($existingManifest.runs)) {
        $runMap[[string]$existingRun.json] = $existingRun
    }
}
foreach ($newRun in $results) {
    $runMap[[string]$newRun.json] = $newRun
}

$manifest = [ordered]@{
    schemaVersion = 1
    runName = $RunName
    generatedAtUtc = [DateTime]::UtcNow.ToString("o")
    projectDirectory = $projectDirectory
    executable = $executable
    executableSha256 = (Get-FileHash -LiteralPath $executable -Algorithm SHA256).Hash
    gitCommit = if ($commit) { [string]$commit } else { "unavailable" }
    protocol = [ordered]@{
        resolution = @(1920, 1080)
        seed = "0x21D3F3A5"
        formalCounts = @($FormalCounts)
        processRuns = $ProcessRuns
        warmupFrames = $WarmupFrames
        sampleFrames = $SampleFrames
        smokeWarmupFrames = $SmokeWarmupFrames
        smokeSampleFrames = $SmokeSampleFrames
    }
    machine = [ordered]@{
        cpu = if ($cpu) { [string]$cpu.Name } else { "unavailable" }
        logicalProcessors = if ($cpu) { [int]$cpu.NumberOfLogicalProcessors } else { 0 }
        gpu = if ($gpu) { [string]$gpu.Name } else { "unavailable" }
        gpuDriver = if ($gpu) { [string]$gpu.DriverVersion } else { "unavailable" }
        os = if ($os) { "{0} {1} build {2}" -f $os.Caption, $os.Version, $os.BuildNumber } else { "unavailable" }
    }
    runs = @($runMap.Values)
}
$manifest | ConvertTo-Json -Depth 8 | Set-Content -LiteralPath $manifestPath -Encoding utf8

$resolvedPython = Resolve-PythonExecutable
& $resolvedPython $analyzer --run-dir $runDirectory --report $reportPath --chart $chartPath
if ($LASTEXITCODE -ne 0) {
    throw "Point-light analysis failed with exit code $LASTEXITCODE"
}

Write-Host "Run directory: $runDirectory"
Write-Host "Report: $reportPath"
Write-Host "Chart: $chartPath"
