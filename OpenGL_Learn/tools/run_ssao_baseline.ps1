[CmdletBinding()]
param(
    [ValidateSet("Smoke", "Formal")]
    [string]$Preset = "Smoke",

    [string]$BatchId = ("ssao-baseline-{0}" -f (Get-Date -Format "yyyyMMdd-HHmmss")),

    [ValidateSet("sponza", "san-miguel")]
    [string[]]$SceneIds = @("sponza", "san-miguel"),

    [int[]]$SampleCounts = @(0, 8, 16, 32, 64),

    [ValidateRange(0, 10000000)]
    [int]$WarmupFrames = 0,

    [ValidateRange(0, 10000000)]
    [int]$MeasuredFrames = 0,

    [ValidateRange(0, 100)]
    [int]$Repeats = 0,

    [switch]$SkipBuild,
    [switch]$Resume,
    [switch]$ReportOnly,

    [ValidateSet("pending", "go", "no-go")]
    [string]$Decision = "pending",

    [string]$DecisionReason = "",
    [string]$PythonPath,
    [string]$MsBuildPath
)

$ErrorActionPreference = "Stop"
$ProgressPreference = "SilentlyContinue"

$toolsDirectory = $PSScriptRoot
$projectDirectory = Split-Path -Parent $toolsDirectory
$repositoryDirectory = Split-Path -Parent $projectDirectory
$manifestPath = Join-Path $projectDirectory "classic-scenes.manifest.json"
$sourceManifest = Get-Content -LiteralPath $manifestPath -Raw | ConvertFrom-Json
$assetRoot = Join-Path $projectDirectory $sourceManifest.assetRoot
$executablePath = Join-Path $repositoryDirectory "x64\Release\OpenGL_Learn.exe"
$presetName = $Preset.ToLowerInvariant()
$experimentRoot = Join-Path $projectDirectory (
    "benchmark-results\ssao-baseline\$BatchId\$presetName")
$rawRoot = Join-Path $experimentRoot "raw"
$captureRoot = Join-Path $experimentRoot "captures"
$logRoot = Join-Path $experimentRoot "logs"
$runManifestPath = Join-Path $experimentRoot "run-manifest.json"
$reportScript = Join-Path $toolsDirectory "generate_ssao_baseline_report.py"

if ($WarmupFrames -eq 0) {
    $WarmupFrames = if ($Preset -eq "Formal") { 300 } else { 30 }
}
if ($MeasuredFrames -eq 0) {
    $MeasuredFrames = if ($Preset -eq "Formal") { 2000 } else { 120 }
}
if ($Repeats -eq 0) {
    $Repeats = if ($Preset -eq "Formal") { 3 } else { 1 }
}

$allowedSamples = @(0, 8, 16, 32, 64)
foreach ($sampleCount in $SampleCounts) {
    if ($allowedSamples -notcontains $sampleCount) {
        throw "SampleCounts may only contain 0, 8, 16, 32, and 64."
    }
}
$SampleCounts = @($SampleCounts | Select-Object -Unique)
if ($SampleCounts.Count -eq 0) {
    throw "At least one SSAO sample configuration is required."
}
if ((@($SampleCounts | Sort-Object) -join ",") -ne "0,8,16,32,64") {
    throw "$Preset runs must include SSAO Off, 8, 16, 32, and 64 samples."
}
if ($Preset -eq "Formal" -and $Repeats -ne 3) {
    throw "Formal runs require exactly three independent processes per configuration."
}

function Resolve-Python {
    $candidates = @(
        $PythonPath,
        (Join-Path $HOME ".cache\codex-runtimes\codex-primary-runtime\dependencies\python\python.exe")
    )
    $pythonCommand = Get-Command python -ErrorAction SilentlyContinue
    if ($pythonCommand) {
        $candidates += $pythonCommand.Source
    }
    foreach ($candidate in $candidates) {
        if (-not $candidate -or
            -not (Test-Path -LiteralPath $candidate -PathType Leaf)) {
            continue
        }
        & $candidate -c "from PIL import Image, ImageDraw, ImageFont" 2>$null
        if ($LASTEXITCODE -eq 0) {
            return (Resolve-Path -LiteralPath $candidate).Path
        }
    }
    throw "Python with Pillow was not found. Pass -PythonPath <python.exe>."
}

function Resolve-MsBuild {
    $candidates = @(
        $MsBuildPath,
        "C:\Program Files\Microsoft Visual Studio\2022\Community\MSBuild\Current\Bin\MSBuild.exe",
        "C:\Program Files\Microsoft Visual Studio\2022\Professional\MSBuild\Current\Bin\MSBuild.exe",
        "C:\Program Files\Microsoft Visual Studio\2022\Enterprise\MSBuild\Current\Bin\MSBuild.exe",
        "C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\MSBuild\Current\Bin\MSBuild.exe"
    )
    foreach ($candidate in $candidates) {
        if ($candidate -and
            (Test-Path -LiteralPath $candidate -PathType Leaf)) {
            return (Resolve-Path -LiteralPath $candidate).Path
        }
    }
    throw "MSBuild 2022 was not found. Pass -MsBuildPath <MSBuild.exe>."
}

function Format-Invariant {
    param([double]$Value)
    return $Value.ToString(
        "R",
        [System.Globalization.CultureInfo]::InvariantCulture)
}

function Get-RelativeProjectPath {
    param([string]$Path)
    $projectPrefix =
        [System.IO.Path]::GetFullPath($projectDirectory).TrimEnd("\") + "\"
    $fullPath = [System.IO.Path]::GetFullPath($Path)
    if (-not $fullPath.StartsWith(
            $projectPrefix,
            [System.StringComparison]::OrdinalIgnoreCase)) {
        throw "Path is outside the project directory: $fullPath"
    }
    return $fullPath.Substring($projectPrefix.Length).Replace("\", "/")
}

function Get-ConfigurationName {
    param([int]$Samples)
    if ($Samples -eq 0) {
        return "off"
    }
    return "full-$Samples"
}

function Get-RunOrder {
    param([int]$Repeat)
    $base = @($SampleCounts)
    if (($Repeat % 3) -eq 2) {
        [array]::Reverse($base)
        return $base
    }
    if (($Repeat % 3) -eq 0 -and $base.Count -gt 1) {
        $offset = [Math]::Min(2, $base.Count - 1)
        return @($base[$offset..($base.Count - 1)] + $base[0..($offset - 1)])
    }
    return $base
}

function Test-ValidResult {
    param(
        [string]$Path,
        [int]$Samples,
        [int]$ExpectedFrames
    )
    if (-not (Test-Path -LiteralPath $Path -PathType Leaf)) {
        return $false
    }
    try {
        $result = Get-Content -LiteralPath $Path -Raw | ConvertFrom-Json
        if (-not [bool]$result.success -or
            [int]$result.schemaVersion -lt 20 -or
            [string]$result.buildConfiguration -ne "Release" -or
            [int]$result.resolution[0] -ne 1920 -or
            [int]$result.resolution[1] -ne 1080 -or
            [string]$result.frameMeasurement -ne "cpu-submission-wall" -or
            [int]$result.ssao.requestedSamples -ne $Samples -or
            [int]$result.profiler.summary.cpuFrame.count -ne $ExpectedFrames -or
            [int]$result.profiler.summary.gpuFrame.count -ne $ExpectedFrames -or
            [int]$result.profiler.summary.cpuZones.'Deferred Pass'.count -ne $ExpectedFrames -or
            [int]$result.profiler.summary.gpuZones.'Deferred Pass'.count -ne $ExpectedFrames -or
            [int]$result.profiler.summary.drawCalls.count -ne $ExpectedFrames) {
            return $false
        }
        if (@($result.profiler.samples.cpuFrame).Count -ne $ExpectedFrames -or
            @($result.profiler.samples.gpuFrame).Count -ne $ExpectedFrames -or
            @($result.profiler.samples.drawCalls).Count -ne $ExpectedFrames -or
            @($result.profiler.samples.cpuZones.'Deferred Pass').Count -ne
                $ExpectedFrames -or
            @($result.profiler.samples.gpuZones.'Deferred Pass').Count -ne
                $ExpectedFrames) {
            return $false
        }
        $expectedSsaoFrames = if ($Samples -eq 0) { 0 } else { $ExpectedFrames }
        $cpuSsaoCount = if ($result.profiler.summary.cpuZones.'SSAO Pass') {
            [int]$result.profiler.summary.cpuZones.'SSAO Pass'.count
        }
        else {
            0
        }
        $gpuSsaoCount = if ($result.profiler.summary.gpuZones.'SSAO Pass') {
            [int]$result.profiler.summary.gpuZones.'SSAO Pass'.count
        }
        else {
            0
        }
        if ($cpuSsaoCount -ne $expectedSsaoFrames -or
            $gpuSsaoCount -ne $expectedSsaoFrames) {
            return $false
        }
        if (@($result.profiler.samples.cpuZones.'SSAO Pass').Count -ne
                $expectedSsaoFrames -or
            @($result.profiler.samples.gpuZones.'SSAO Pass').Count -ne
                $expectedSsaoFrames) {
            return $false
        }
        if ($Samples -gt 0 -and
            ([int]$result.ssao.kernelSize -ne $Samples -or
                -not [bool]$result.ssao.fullResolution -or
                [string]$result.ssao.outputInternalFormatName -ne "GL_R16F")) {
            return $false
        }
        return $true
    }
    catch {
        return $false
    }
}

$python = Resolve-Python

if (-not $ReportOnly) {
    if ((Test-Path -LiteralPath $experimentRoot) -and -not $Resume) {
        throw "Output already exists: $experimentRoot. Use a new BatchId or -Resume."
    }
    New-Item -ItemType Directory -Path $rawRoot -Force | Out-Null
    New-Item -ItemType Directory -Path $captureRoot -Force | Out-Null
    New-Item -ItemType Directory -Path $logRoot -Force | Out-Null

    if (-not $SkipBuild) {
        $msbuild = Resolve-MsBuild
        Write-Host "Building Release x64..." -ForegroundColor Cyan
        & $msbuild (Join-Path $repositoryDirectory "OpenGL_Learn.sln") `
            /m:1 /p:Configuration=Release /p:Platform=x64 /verbosity:minimal
        if ($LASTEXITCODE -ne 0) {
            throw "Release x64 build failed."
        }
    }
    if (-not (Test-Path -LiteralPath $executablePath -PathType Leaf)) {
        throw "Release executable not found: $executablePath"
    }

    $selectedScenes = @()
    foreach ($sceneId in $SceneIds) {
        $scene = $sourceManifest.scenes |
            Where-Object { [string]$_.id -eq $sceneId } |
            Select-Object -First 1
        if (-not $scene) {
            throw "Scene '$sceneId' is missing from classic-scenes.manifest.json."
        }
        $modelAbsolute = Join-Path $assetRoot ([string]$scene.modelPath)
        if (-not (Test-Path -LiteralPath $modelAbsolute -PathType Leaf)) {
            throw "Scene asset is missing: $modelAbsolute. Prepare classic scenes first."
        }
        $selectedScenes += $scene
    }

    $runRecords = [System.Collections.Generic.List[object]]::new()
    $captureFrame = $WarmupFrames + $MeasuredFrames
    foreach ($repeat in 1..$Repeats) {
        foreach ($scene in $selectedScenes) {
            foreach ($samples in (Get-RunOrder -Repeat $repeat)) {
                $sceneId = [string]$scene.id
                $configuration = Get-ConfigurationName -Samples $samples
                $rawDirectory = Join-Path $rawRoot "$sceneId\$configuration"
                $sceneCaptureDirectory =
                    Join-Path $captureRoot "$sceneId\$configuration"
                New-Item -ItemType Directory -Path $rawDirectory -Force |
                    Out-Null
                if ($repeat -eq 1) {
                    New-Item -ItemType Directory -Path $sceneCaptureDirectory -Force |
                        Out-Null
                }

                $resultPath = Join-Path $rawDirectory "run-$repeat.json"
                $logPath = Join-Path $logRoot (
                    "$sceneId-$configuration-run-$repeat.log")
                $finalCapturePath = Join-Path $sceneCaptureDirectory "run-1-final.ppm"
                $aoCapturePath = Join-Path $sceneCaptureDirectory "run-1-ao.ppm"
                $captureFinal = $repeat -eq 1

                if ($Resume -and
                    (Test-ValidResult `
                        -Path $resultPath `
                        -Samples $samples `
                        -ExpectedFrames $MeasuredFrames) -and
                    (-not $captureFinal -or
                        (Test-Path -LiteralPath $finalCapturePath -PathType Leaf)) -and
                    (-not $captureFinal -or $samples -eq 0 -or
                        (Test-Path -LiteralPath $aoCapturePath -PathType Leaf))) {
                    Write-Host (
                        "Reuse {0} {1} run {2}" -f
                        $sceneId,
                        $configuration,
                        $repeat) -ForegroundColor DarkGreen
                }
                else {
                    $modelPath = "classic-scenes/" +
                        ([string]$scene.modelPath).Replace("\", "/")
                    $arguments = @(
                        "--classic-scene-test", $modelPath,
                        "--classic-scene-name", $sceneId,
                        "--classic-scene-result",
                            (Get-RelativeProjectPath -Path $resultPath),
                        "--classic-scene-camera",
                            (Format-Invariant $scene.camera[0]),
                            (Format-Invariant $scene.camera[1]),
                            (Format-Invariant $scene.camera[2]),
                        "--classic-scene-target",
                            (Format-Invariant $scene.target[0]),
                            (Format-Invariant $scene.target[1]),
                            (Format-Invariant $scene.target[2]),
                        "--classic-scene-up",
                            (Format-Invariant $scene.up[0]),
                            (Format-Invariant $scene.up[1]),
                            (Format-Invariant $scene.up[2]),
                        "--classic-scene-radius",
                            (Format-Invariant $scene.normalizedRadius),
                        "--classic-scene-world-scale", "1",
                        "--classic-scene-fov",
                            (Format-Invariant $scene.fov),
                        "--classic-scene-render-path", "pbr-deferred",
                        "--classic-scene-width", "1920",
                        "--classic-scene-height", "1080",
                        "--classic-scene-ssao-samples", [string]$samples,
                        "--classic-scene-warmup-frames", [string]$WarmupFrames,
                        "--classic-scene-capture-frame", [string]$captureFrame
                    )
                    if ($captureFinal) {
                        $arguments += @(
                            "--classic-scene-capture",
                            (Get-RelativeProjectPath -Path $finalCapturePath)
                        )
                        if ($samples -gt 0) {
                            $arguments += @(
                                "--classic-scene-ssao-capture",
                                (Get-RelativeProjectPath -Path $aoCapturePath)
                            )
                        }
                    }
                    else {
                        $arguments += "--classic-scene-no-capture"
                    }

                    Write-Host (
                        "Run {0} {1} process {2}/{3}: {4}+{5} frames" -f
                        $sceneId,
                        $configuration,
                        $repeat,
                        $Repeats,
                        $WarmupFrames,
                        $MeasuredFrames) -ForegroundColor Cyan
                    Push-Location $projectDirectory
                    try {
                        & $executablePath @arguments *> $logPath
                        $exitCode = $LASTEXITCODE
                    }
                    finally {
                        Pop-Location
                    }
                    if ($exitCode -ne 0) {
                        throw "Run failed with exit code $exitCode. See $logPath"
                    }
                    if (-not (Test-ValidResult `
                        -Path $resultPath `
                        -Samples $samples `
                        -ExpectedFrames $MeasuredFrames)) {
                        throw "Run produced an invalid result: $resultPath"
                    }
                }

                $runRecords.Add([ordered]@{
                    scene = $sceneId
                    configuration = $configuration
                    samples = $samples
                    process = $repeat
                    result = Get-RelativeProjectPath -Path $resultPath
                    log = Get-RelativeProjectPath -Path $logPath
                    finalCapture = if ($captureFinal) {
                        Get-RelativeProjectPath -Path $finalCapturePath
                    }
                    else {
                        $null
                    }
                    aoCapture = if ($captureFinal -and $samples -gt 0) {
                        Get-RelativeProjectPath -Path $aoCapturePath
                    }
                    else {
                        $null
                    }
                })
            }
        }
    }

    $cpu = Get-CimInstance Win32_Processor | Select-Object -First 1
    $os = Get-CimInstance Win32_OperatingSystem
    $gpu = Get-CimInstance Win32_VideoController |
        Where-Object { $_.Name -notmatch "Virtual" } |
        Select-Object -First 1
    $gitCommit = (& git -C $repositoryDirectory rev-parse HEAD).Trim()
    $gitBranch = (& git -C $repositoryDirectory branch --show-current).Trim()
    $worktreeDirty =
        @(& git -C $repositoryDirectory status --porcelain).Count -gt 0
    $executableHash =
        (Get-FileHash -Algorithm SHA256 -LiteralPath $executablePath).Hash

    $runManifest = [ordered]@{
        schemaVersion = 1
        generatedAtUtc = [DateTime]::UtcNow.ToString("yyyy-MM-ddTHH:mm:ssZ")
        batchId = $BatchId
        preset = $presetName
        protocol = [ordered]@{
            warmupFrames = $WarmupFrames
            measuredFrames = $MeasuredFrames
            independentProcesses = $Repeats
            resolution = @(1920, 1080)
            configurations = @($SampleCounts)
            renderPath = "pbr-deferred"
            requestedSwapInterval = 0
            inputFrozen = $true
            bloom = $false
            shadows = $false
            gammaCorrection = $true
            autoReloadShaders = $false
            autoReloadMaterials = $false
            ssaoRadius = 0.35
            ssaoBias = 0.025
            ssaoOutput = "full-resolution GL_R16F"
            percentileMethod = "nearest-rank"
            processOrder = "forward, reverse, rotated"
        }
        source = [ordered]@{
            gitCommit = $gitCommit
            gitBranch = $gitBranch
            worktreeDirty = $worktreeDirty
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
        scenes = @(
            $selectedScenes | ForEach-Object {
                [ordered]@{
                    id = [string]$_.id
                    displayName = [string]$_.displayName
                    modelPath = [string]$_.modelPath
                    camera = @($_.camera)
                    target = @($_.target)
                    up = @($_.up)
                    normalizedRadius = [double]$_.normalizedRadius
                    worldScale = 1.0
                    fov = [double]$_.fov
                }
            })
        runs = $runRecords
    }
    $runManifest |
        ConvertTo-Json -Depth 10 |
        Set-Content -LiteralPath $runManifestPath -Encoding utf8
}

if (-not (Test-Path -LiteralPath $runManifestPath -PathType Leaf)) {
    throw "Run manifest not found: $runManifestPath"
}
if (-not (Test-Path -LiteralPath $reportScript -PathType Leaf)) {
    throw "Report generator not found: $reportScript"
}

$reportArguments = @(
    $reportScript,
    "--input", $experimentRoot,
    "--decision", $Decision
)
if ($DecisionReason) {
    $reportArguments += @("--decision-reason", $DecisionReason)
}
& $python @reportArguments
if ($LASTEXITCODE -ne 0) {
    throw "SSAO report generation failed."
}

Write-Host "SSAO baseline complete: $experimentRoot" -ForegroundColor Green
Write-Host "Report: $(Join-Path $experimentRoot 'SSAO_BASELINE_REPORT_CN.md')" `
    -ForegroundColor Green
