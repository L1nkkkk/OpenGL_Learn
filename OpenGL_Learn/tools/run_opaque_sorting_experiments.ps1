param(
    [ValidateRange(0, 1000000)]
    [int]$WarmupFrames = 300,

    [ValidateRange(1, 1000000)]
    [int]$SampleFrames = 600,

    [string]$OutputDirectory =
        "benchmark-results/opaque-sorting/object-heavy-20260730",

    [switch]$Resume
)

$projectDirectory = Split-Path -Parent (Split-Path -Parent $PSCommandPath)
$repositoryDirectory = Split-Path -Parent $projectDirectory
$runStress = Join-Path $PSScriptRoot "run_submission_stress.ps1"
$rawDirectory = Join-Path $projectDirectory (Join-Path $OutputDirectory "raw")
$captureDirectory =
    Join-Path $projectDirectory (Join-Path $OutputDirectory "captures")
$manifestPath =
    Join-Path $projectDirectory (Join-Path $OutputDirectory "run-manifest.json")

New-Item -ItemType Directory -Force -Path $rawDirectory | Out-Null
New-Item -ItemType Directory -Force -Path $captureDirectory | Out-Null

$runRecords = [System.Collections.Generic.List[object]]::new()

function Test-ValidBenchmark {
    param([string]$Path)
    if (-not (Test-Path -LiteralPath $Path -PathType Leaf)) {
        return $false
    }
    try {
        $report = Get-Content -Raw -LiteralPath $Path | ConvertFrom-Json
        return [bool]$report.capture.valid
    }
    catch {
        return $false
    }
}

function Invoke-Experiment {
    param(
        [Parameter(Mandatory)]
        [string]$Name,

        [Parameter(Mandatory)]
        [ValidateSet("legacy", "key-direct", "key-index")]
        [string]$SortMode,

        [int]$ObjectCount = 30000,
        [int]$DynamicPercent = 20,
        [ValidateSet("forward", "deferred")]
        [string]$RenderPath = "forward",
        [ValidateSet("quad", "mixed")]
        [string]$GeometrySet = "quad",
        [switch]$CollectionBreakdown,
        [switch]$Capture
    )

    $outputRelative = "$OutputDirectory/raw/$Name.json"
    $outputAbsolute = Join-Path $projectDirectory $outputRelative
    $captureRelative = ""
    if ($Capture) {
        $captureRelative = "$OutputDirectory/captures/$Name.ppm"
    }

    if ($Resume -and (Test-ValidBenchmark -Path $outputAbsolute)) {
        Write-Host "Reuse valid run: $Name" -ForegroundColor DarkGreen
    }
    else {
        Write-Host "Run: $Name" -ForegroundColor Cyan
        $arguments = @{
            ObjectCount = $ObjectCount
            DynamicPercent = $DynamicPercent
            MaterialCount = 16
            WarmupFrames = $WarmupFrames
            SampleFrames = $SampleFrames
            OpaqueSortMode = $SortMode
            RenderPath = $RenderPath
            GeometrySet = $GeometrySet
            Label = $Name
            Output = $outputRelative
        }
        if ($CollectionBreakdown) {
            $arguments.CollectionBreakdown = $true
        }
        if ($Capture) {
            $arguments.CapturePath = $captureRelative
        }
        & $runStress @arguments
        if ($LASTEXITCODE -ne 0) {
            throw "Benchmark failed: $Name (exit $LASTEXITCODE)"
        }
        if (-not (Test-ValidBenchmark -Path $outputAbsolute)) {
            throw "Benchmark did not produce a valid report: $outputAbsolute"
        }
    }

    $runRecords.Add([ordered]@{
        name = $Name
        sortMode = $SortMode
        objectCount = $ObjectCount
        dynamicPercent = $DynamicPercent
        renderPath = $RenderPath
        geometrySet = $GeometrySet
        collectionBreakdown = [bool]$CollectionBreakdown
        report = $outputRelative.Replace("\", "/")
        capture = if ($Capture) {
            $captureRelative.Replace("\", "/")
        }
        else {
            $null
        }
    })
}

# Primary same-binary A/B. Three fresh processes per variant, balanced order.
$primaryOrder = @(
    @{ Mode = "legacy"; Index = 1 },
    @{ Mode = "key-index"; Index = 1 },
    @{ Mode = "key-index"; Index = 2 },
    @{ Mode = "legacy"; Index = 2 },
    @{ Mode = "legacy"; Index = 3 },
    @{ Mode = "key-index"; Index = 3 }
)
foreach ($entry in $primaryOrder) {
    $name = "primary-30k-d20-$($entry.Mode)-r$($entry.Index)"
    Invoke-Experiment `
        -Name $name `
        -SortMode $entry.Mode `
        -Capture
}

# Keep the direct-DrawItem key comparator as an independently measured
# candidate rather than assuming compact indices always win.
foreach ($repeat in 1..3) {
    Invoke-Experiment `
        -Name "candidate-30k-d20-key-direct-r$repeat" `
        -SortMode "key-direct"
}

# Dynamic-percent screening. The primary key-index repetitions supply 20%.
foreach ($dynamicPercent in @(0, 100)) {
    foreach ($repeat in 1..3) {
        Invoke-Experiment `
            -Name "dynamic-30k-d$dynamicPercent-key-index-r$repeat" `
            -SortMode "key-index" `
            -DynamicPercent $dynamicPercent
    }
}

# Total-object scaling. Primary key-index repetitions supply the 30k point.
foreach ($objectCount in @(1000, 5000, 10000)) {
    foreach ($repeat in 1..3) {
        Invoke-Experiment `
            -Name "scale-${objectCount}-d20-key-index-r$repeat" `
            -SortMode "key-index" `
            -ObjectCount $objectCount
    }
}

# Coarse, outer-pass collection attribution. This benchmark-only replay starts
# one timer per stage, never one timer per object or mesh.
foreach ($dynamicPercent in @(0, 20, 100)) {
    Invoke-Experiment `
        -Name "collection-probe-30k-d$dynamicPercent-key-index" `
        -SortMode "key-index" `
        -DynamicPercent $dynamicPercent `
        -CollectionBreakdown
}

# Non-identical geometry and lower object count guard against a result that
# exists only for 30k copies of one quad.
$mixedOrder = @(
    @{ Mode = "legacy"; Index = 1 },
    @{ Mode = "key-index"; Index = 1 },
    @{ Mode = "key-index"; Index = 2 },
    @{ Mode = "legacy"; Index = 2 },
    @{ Mode = "legacy"; Index = 3 },
    @{ Mode = "key-index"; Index = 3 }
)
foreach ($entry in $mixedOrder) {
    $name = "mixed-10k-d20-$($entry.Mode)-r$($entry.Index)"
    Invoke-Experiment `
        -Name $name `
        -SortMode $entry.Mode `
        -ObjectCount 10000 `
        -GeometrySet "mixed" `
        -Capture:($entry.Index -eq 1)
}

# Forward is the performance target. Deferred gets a controlled execution and
# image/order-equivalence check for both A/B paths.
Invoke-Experiment `
    -Name "deferred-10k-d20-legacy" `
    -SortMode "legacy" `
    -ObjectCount 10000 `
    -RenderPath "deferred" `
    -Capture
Invoke-Experiment `
    -Name "deferred-10k-d20-key-index" `
    -SortMode "key-index" `
    -ObjectCount 10000 `
    -RenderPath "deferred" `
    -Capture

$cpu = Get-CimInstance Win32_Processor | Select-Object -First 1
$os = Get-CimInstance Win32_OperatingSystem
$gpu = Get-CimInstance Win32_VideoController |
    Where-Object { $_.Name -like "*NVIDIA*" } |
    Select-Object -First 1
$executable =
    Join-Path $repositoryDirectory "x64\Release\OpenGL_Learn.exe"
$gitCommit = (& git -C $repositoryDirectory rev-parse HEAD).Trim()
$gitBranch = (& git -C $repositoryDirectory branch --show-current).Trim()
$gitStatus = @(& git -C $repositoryDirectory status --short)
$executableHash =
    (Get-FileHash -Algorithm SHA256 -LiteralPath $executable).Hash

$manifest = [ordered]@{
    schemaVersion = 1
    generatedAtUtc = [DateTime]::UtcNow.ToString("yyyy-MM-ddTHH:mm:ssZ")
    protocol = [ordered]@{
        warmupFrames = $WarmupFrames
        sampleFrames = $SampleFrames
        resolution = "1920x1080"
        materialCount = 16
        seed = "0x5eed1234"
        requestedSwapInterval = 0
        primaryOrder = "A-B-B-A-A-B"
        percentileMethod = "nearest-rank"
    }
    source = [ordered]@{
        gitCommit = $gitCommit
        gitBranch = $gitBranch
        worktreeDirty = $gitStatus.Count -gt 0
        releaseExecutableSha256 = $executableHash
    }
    system = [ordered]@{
        os = $os.Caption
        osVersion = $os.Version
        cpu = $cpu.Name
        logicalProcessors = $cpu.NumberOfLogicalProcessors
        gpu = $gpu.Name
        gpuDriverVersion = $gpu.DriverVersion
    }
    runs = $runRecords
}

$manifest |
    ConvertTo-Json -Depth 8 |
    Set-Content -LiteralPath $manifestPath -Encoding utf8

Write-Host "Completed $($runRecords.Count) benchmark runs." -ForegroundColor Green
Write-Host "Manifest: $manifestPath" -ForegroundColor Green
