param(
    [string]$BatchId = ("legacy-breakdown-" + (Get-Date -Format "yyyyMMdd-HHmmss")),
    [int]$WarmupFrames = 30,
    [int]$SampleFrames = 5,
    [int]$ReplayCount = 3,
    [switch]$SkipBuild
)

$ErrorActionPreference = "Stop"
Set-StrictMode -Version Latest

if ($WarmupFrames -lt 2 -or $SampleFrames -lt 1 -or $ReplayCount -ne 3) {
    throw "WarmupFrames >= 2, SampleFrames >= 1, and exactly 3 replays are required."
}

$projectDirectory = (Resolve-Path (Join-Path $PSScriptRoot "..")).Path
$repositoryDirectory = (Resolve-Path (Join-Path $projectDirectory "..")).Path
$executable = Join-Path $repositoryDirectory "x64\Release\OpenGL_Learn.exe"
$baselineDirectory = Join-Path $projectDirectory "benchmark-results\point-light-heavy\legacy-baseline-20260801"
$renderDocDirectory = Join-Path $projectDirectory "benchmark-results\ssao-renderdoc-evidence\renderdoc-1.45.0\portable\RenderDoc"
$renderDocCmd = Join-Path $renderDocDirectory "renderdoccmd.exe"
$qRenderDoc = Join-Path $renderDocDirectory "qrenderdoc.exe"
$analysisScript = Join-Path $PSScriptRoot "analyze_point_light_renderdoc_replay.py"
$aggregateScript = Join-Path $PSScriptRoot "aggregate_point_light_renderdoc.py"
$batchDirectory = Join-Path $projectDirectory "benchmark-results\point-light-renderdoc\$BatchId"

foreach ($path in @($renderDocCmd, $qRenderDoc, $analysisScript, $aggregateScript)) {
    if (-not (Test-Path -LiteralPath $path -PathType Leaf)) {
        throw "Required file is missing: $path"
    }
}
if (Test-Path -LiteralPath $batchDirectory) {
    throw "Refusing to overwrite an existing evidence directory: $batchDirectory"
}
foreach ($name in @("captures", "diagnostics", "images", "logs", "replays")) {
    New-Item -ItemType Directory -Force -Path (Join-Path $batchDirectory $name) | Out-Null
}

function ConvertTo-ProcessArgument {
    param([Parameter(Mandatory)] [string]$Value)
    return '"' + $Value.Replace('"', '\"') + '"'
}

function Start-EvidenceProcess {
    param(
        [Parameter(Mandatory)] [string]$FilePath,
        [Parameter(Mandatory)] [string[]]$Arguments,
        [Parameter(Mandatory)] [string]$StdoutPath,
        [Parameter(Mandatory)] [string]$StderrPath
    )
    $quotedArguments = @($Arguments | ForEach-Object { ConvertTo-ProcessArgument $_ })
    $process = Start-Process `
        -FilePath $FilePath `
        -ArgumentList $quotedArguments `
        -RedirectStandardOutput $StdoutPath `
        -RedirectStandardError $StderrPath `
        -WorkingDirectory $projectDirectory `
        -WindowStyle Hidden `
        -Wait `
        -PassThru
    if ($process.ExitCode -ne 0) {
        throw "Process failed with exit code $($process.ExitCode): $FilePath; stderr=$StderrPath"
    }
}

if (-not $SkipBuild) {
    $vsWhere = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"
    if (-not (Test-Path -LiteralPath $vsWhere -PathType Leaf)) {
        throw "vswhere.exe was not found"
    }
    $msbuild = & $vsWhere -latest -products * -requires Microsoft.Component.MSBuild -find "MSBuild\**\Bin\MSBuild.exe" |
        Select-Object -First 1
    if (-not $msbuild) {
        throw "MSBuild was not found"
    }
    & $msbuild (Join-Path $repositoryDirectory "OpenGL_Learn.sln") /m /p:Configuration=Release /p:Platform=x64 /nologo
    if ($LASTEXITCODE -ne 0) {
        throw "Release build failed"
    }
}
if (-not (Test-Path -LiteralPath $executable -PathType Leaf)) {
    throw "Release executable is missing: $executable"
}

$configurations = @(
    [ordered]@{
        Coverage = "representative"; LightCount = 16; Stem = "representative-0016";
        Baseline = "formal-representative-0016-run01.json"
    },
    [ordered]@{
        Coverage = "high-overlap"; LightCount = 16; Stem = "high-overlap-0016";
        Baseline = "smoke-high-overlap-0016-run01.json"
    },
    [ordered]@{
        Coverage = "representative"; LightCount = 256; Stem = "representative-0256";
        Baseline = "formal-representative-0256-run01.json"
    },
    [ordered]@{
        Coverage = "representative"; LightCount = 512; Stem = "representative-0512";
        Baseline = "formal-representative-0512-run01.json"
    }
)

$captureExecutableItem = Get-Item -LiteralPath $executable
$captureExecutableMetadata = [ordered]@{
    schemaVersion = 1
    role = "captureExecutable"
    path = $captureExecutableItem.FullName
    bytes = $captureExecutableItem.Length
    sha256 = (Get-FileHash -Algorithm SHA256 -LiteralPath $executable).Hash
    capturedBeforeInstrumentationCleanup = $false
    source = "run_point_light_renderdoc_breakdown.ps1 recorded immediately before capture"
    covers = @($configurations | ForEach-Object { ([string]$_.Stem) + "_capture.rdc" })
}
$captureExecutableMetadata |
    ConvertTo-Json -Depth 4 |
    Set-Content -LiteralPath (Join-Path $batchDirectory "capture-executable.json") -Encoding utf8

foreach ($configuration in $configurations) {
    $stem = [string]$configuration.Stem
    $captureTemplate = Join-Path $batchDirectory "captures\$stem"
    $capturePath = $captureTemplate + "_capture.rdc"
    $diagnosticPath = Join-Path $batchDirectory "diagnostics\$stem-capture.json"
    $applicationCapturePath = Join-Path $batchDirectory "images\$stem-app.ppm"
    $captureArguments = @(
        "capture",
        "--working-dir", $projectDirectory,
        "--capture-file", $captureTemplate,
        "--opt-disallow-vsync",
        "--wait-for-exit",
        $executable,
        "--gbuffer-position", "explicit",
        "--point-light-stress",
        "--point-light-count", [string]$configuration.LightCount,
        "--point-light-coverage", [string]$configuration.Coverage,
        "--point-light-seed", "0x21D3F3A5",
        "--point-light-warmup-frames", [string]$WarmupFrames,
        "--point-light-sample-frames", [string]$SampleFrames,
        "--point-light-result", $diagnosticPath,
        "--point-light-capture", $applicationCapturePath,
        "--point-light-renderdoc-markers",
        "--classic-scene-renderdoc-capture-frame", [string]$WarmupFrames,
        "--classic-scene-renderdoc-capture-template", $captureTemplate
    )
    Start-EvidenceProcess `
        -FilePath $renderDocCmd `
        -Arguments $captureArguments `
        -StdoutPath (Join-Path $batchDirectory "logs\capture-$stem.stdout.log") `
        -StderrPath (Join-Path $batchDirectory "logs\capture-$stem.stderr.log")
    if (-not (Test-Path -LiteralPath $capturePath -PathType Leaf)) {
        throw "Capture was not produced: $capturePath"
    }

    Start-EvidenceProcess `
        -FilePath $renderDocCmd `
        -Arguments @("thumb", "--out", (Join-Path $batchDirectory "images\$stem-thumbnail.png"), $capturePath) `
        -StdoutPath (Join-Path $batchDirectory "logs\thumbnail-$stem.stdout.log") `
        -StderrPath (Join-Path $batchDirectory "logs\thumbnail-$stem.stderr.log")
    Start-EvidenceProcess `
        -FilePath $renderDocCmd `
        -Arguments @("replay", "--width", "640", "--height", "360", "--loops", "1", $capturePath) `
        -StdoutPath (Join-Path $batchDirectory "logs\renderdoccmd-replay-$stem.stdout.log") `
        -StderrPath (Join-Path $batchDirectory "logs\renderdoccmd-replay-$stem.stderr.log")

    foreach ($replayIndex in 1..$ReplayCount) {
        $env:POINT_LIGHT_RENDERDOC_CAPTURE = $capturePath
        $env:POINT_LIGHT_RENDERDOC_OUTPUT = Join-Path $batchDirectory ("replays\$stem-replay{0:D2}.json" -f $replayIndex)
        $env:POINT_LIGHT_RENDERDOC_DIAGNOSTIC = $diagnosticPath
        $env:POINT_LIGHT_RENDERDOC_BASELINE = Join-Path $baselineDirectory ([string]$configuration.Baseline)
        $env:POINT_LIGHT_RENDERDOC_COVERAGE = [string]$configuration.Coverage
        $env:POINT_LIGHT_RENDERDOC_LIGHT_COUNT = [string]$configuration.LightCount
        $env:POINT_LIGHT_RENDERDOC_REPLAY_INDEX = [string]$replayIndex
        $env:POINT_LIGHT_RENDERDOC_TEXTURE = if ($replayIndex -eq 1) {
            Join-Path $batchDirectory "images\$stem-lighting-target.png"
        }
        else {
            ""
        }
        Start-EvidenceProcess `
            -FilePath $qRenderDoc `
            -Arguments @("--python", $analysisScript) `
            -StdoutPath (Join-Path $batchDirectory ("logs\replay-$stem-{0:D2}.stdout.log" -f $replayIndex)) `
            -StderrPath (Join-Path $batchDirectory ("logs\replay-$stem-{0:D2}.stderr.log" -f $replayIndex))
    }
}

# Default-off smoke is intentionally outside RenderDoc and omits the marker flag.
$smokeJson = Join-Path $batchDirectory "diagnostics\default-off-representative-0016-smoke.json"
$smokePpm = Join-Path $batchDirectory "images\default-off-representative-0016-smoke.ppm"
Start-EvidenceProcess `
    -FilePath $executable `
    -Arguments @(
        "--gbuffer-position", "explicit",
        "--point-light-stress",
        "--point-light-count", "16",
        "--point-light-coverage", "representative",
        "--point-light-seed", "0x21D3F3A5",
        "--point-light-warmup-frames", "5",
        "--point-light-sample-frames", "10",
        "--point-light-result", $smokeJson,
        "--point-light-capture", $smokePpm
    ) `
    -StdoutPath (Join-Path $batchDirectory "logs\default-off-smoke.stdout.log") `
    -StderrPath (Join-Path $batchDirectory "logs\default-off-smoke.stderr.log")

$pythonLauncher = (Get-Command py.exe -ErrorAction SilentlyContinue).Source
if ($pythonLauncher) {
    & $pythonLauncher -3 $aggregateScript --batch-dir $batchDirectory
}
else {
    $pythonLauncher = (Get-Command python.exe -ErrorAction Stop).Source
    & $pythonLauncher $aggregateScript --batch-dir $batchDirectory
}
if ($LASTEXITCODE -ne 0) {
    throw "Aggregation failed"
}

Write-Host "RenderDoc point-light evidence complete: $batchDirectory"
