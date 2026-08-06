param(
    [Parameter(Mandatory)]
    [string]$RunDirectory,

    [ValidateRange(2, 1000000)]
    [int]$WarmupFrames = 30,

    [ValidateRange(1, 1000000)]
    [int]$SampleFrames = 5,

    [switch]$SkipBuild
)

Set-StrictMode -Version Latest
$ErrorActionPreference = "Stop"

$projectDirectory = (Resolve-Path (Join-Path $PSScriptRoot "..")).Path
$repositoryDirectory = (Resolve-Path (Join-Path $projectDirectory "..")).Path
$resolvedRunDirectory = (Resolve-Path -LiteralPath $RunDirectory).Path
$evidenceDirectory = Join-Path $resolvedRunDirectory "renderdoc"
$captureDirectory = Join-Path $evidenceDirectory "captures"
$diagnosticDirectory = Join-Path $evidenceDirectory "diagnostics"
$imageDirectory = Join-Path $evidenceDirectory "images"
$logDirectory = Join-Path $evidenceDirectory "logs"
$replayDirectory = Join-Path $evidenceDirectory "replay"
$executable = Join-Path $repositoryDirectory "x64\Release\OpenGL_Learn.exe"
$renderDocDirectory = Join-Path $projectDirectory "benchmark-results\ssao-renderdoc-evidence\renderdoc-1.45.0\portable\RenderDoc"
$renderDocCmd = Join-Path $renderDocDirectory "renderdoccmd.exe"
$qRenderDoc = Join-Path $renderDocDirectory "qrenderdoc.exe"
$analysisScript = Join-Path $PSScriptRoot "analyze_point_light_stencil_clear_renderdoc.py"

if (Test-Path -LiteralPath $evidenceDirectory) {
    throw "Refusing to overwrite existing RenderDoc evidence: $evidenceDirectory"
}
foreach ($path in @($captureDirectory, $diagnosticDirectory, $imageDirectory, $logDirectory, $replayDirectory)) {
    New-Item -ItemType Directory -Force -Path $path | Out-Null
}
foreach ($path in @($renderDocCmd, $qRenderDoc, $analysisScript)) {
    if (-not (Test-Path -LiteralPath $path -PathType Leaf)) { throw "Required file is missing: $path" }
}

function ConvertTo-ProcessArgument {
    param([Parameter(Mandatory)] [string]$Value)
    return '"' + $Value.Replace('"', '\"') + '"'
}

function Invoke-HiddenProcess {
    param(
        [Parameter(Mandatory)] [string]$FilePath,
        [Parameter(Mandatory)] [string[]]$Arguments,
        [Parameter(Mandatory)] [string]$StdoutPath,
        [Parameter(Mandatory)] [string]$StderrPath
    )
    $quoted = @($Arguments | ForEach-Object { ConvertTo-ProcessArgument $_ })
    $process = Start-Process -FilePath $FilePath -ArgumentList $quoted `
        -RedirectStandardOutput $StdoutPath -RedirectStandardError $StderrPath `
        -WorkingDirectory $projectDirectory -WindowStyle Hidden -Wait -PassThru
    if ($process.ExitCode -ne 0) {
        throw "Process failed with exit code $($process.ExitCode): $FilePath; stderr=$StderrPath"
    }
}

if (-not $SkipBuild) {
    $vswhere = Join-Path ${env:ProgramFiles(x86)} "Microsoft Visual Studio\Installer\vswhere.exe"
    $msbuild = & $vswhere -latest -products * -requires Microsoft.Component.MSBuild -find "MSBuild\**\Bin\MSBuild.exe" | Select-Object -First 1
    if (-not $msbuild) { throw "MSBuild was not found" }
    & $msbuild (Join-Path $repositoryDirectory "OpenGL_Learn.sln") /m /p:Configuration=Release /p:Platform=x64 /v:minimal /nologo
    if ($LASTEXITCODE -ne 0) { throw "Release x64 build failed" }
}

$captureTemplate = Join-Path $captureDirectory "representative-0512-coalesced"
$capturePath = $captureTemplate + "_capture.rdc"
$diagnosticPath = Join-Path $diagnosticDirectory "representative-0512-coalesced.json"
$applicationImage = Join-Path $imageDirectory "representative-0512-coalesced-app.ppm"
$replayJson = Join-Path $replayDirectory "representative-0512-coalesced-replay.json"
$metadata = [ordered]@{
    schemaVersion = 1
    role = "final coalesced RenderDoc capture executable"
    path = $executable
    bytes = (Get-Item -LiteralPath $executable).Length
    sha256 = (Get-FileHash -LiteralPath $executable -Algorithm SHA256).Hash
    capturedAtUtc = [DateTime]::UtcNow.ToString("o")
    mode = "coalesced-n-plus-one"
    lightCount = 512
}
$metadata | ConvertTo-Json -Depth 4 | Set-Content -LiteralPath (Join-Path $evidenceDirectory "capture-executable.json") -Encoding utf8

$captureArguments = @(
    "capture",
    "--working-dir", $projectDirectory,
    "--capture-file", $captureTemplate,
    "--opt-disallow-vsync",
    "--wait-for-exit",
    $executable,
    "--gbuffer-position", "explicit",
    "--point-light-stencil-clear-mode", "coalesced-n-plus-one",
    "--point-light-stress",
    "--point-light-count", "512",
    "--point-light-coverage", "representative",
    "--point-light-seed", "0x21D3F3A5",
    "--point-light-warmup-frames", [string]$WarmupFrames,
    "--point-light-sample-frames", [string]$SampleFrames,
    "--point-light-result", $diagnosticPath,
    "--point-light-capture", $applicationImage,
    "--point-light-renderdoc-markers",
    "--classic-scene-renderdoc-capture-frame", [string]$WarmupFrames,
    "--classic-scene-renderdoc-capture-template", $captureTemplate
)
Invoke-HiddenProcess -FilePath $renderDocCmd -Arguments $captureArguments `
    -StdoutPath (Join-Path $logDirectory "capture.stdout.log") `
    -StderrPath (Join-Path $logDirectory "capture.stderr.log")
if (-not (Test-Path -LiteralPath $capturePath -PathType Leaf)) { throw "RDC was not produced: $capturePath" }

Invoke-HiddenProcess -FilePath $renderDocCmd `
    -Arguments @("thumb", "--out", (Join-Path $imageDirectory "representative-0512-coalesced-thumbnail.png"), $capturePath) `
    -StdoutPath (Join-Path $logDirectory "thumbnail.stdout.log") `
    -StderrPath (Join-Path $logDirectory "thumbnail.stderr.log")
Invoke-HiddenProcess -FilePath $renderDocCmd `
    -Arguments @("replay", "--width", "640", "--height", "360", "--loops", "1", $capturePath) `
    -StdoutPath (Join-Path $logDirectory "renderdoccmd-replay.stdout.log") `
    -StderrPath (Join-Path $logDirectory "renderdoccmd-replay.stderr.log")

$env:POINT_LIGHT_CLEAR_RDC = $capturePath
$env:POINT_LIGHT_CLEAR_RDC_OUTPUT = $replayJson
$env:POINT_LIGHT_CLEAR_DIAGNOSTIC = $diagnosticPath
$env:POINT_LIGHT_CLEAR_COUNT = "512"
try {
    Invoke-HiddenProcess -FilePath $qRenderDoc -Arguments @("--python", $analysisScript) `
        -StdoutPath (Join-Path $logDirectory "qrenderdoc-analysis.stdout.log") `
        -StderrPath (Join-Path $logDirectory "qrenderdoc-analysis.stderr.log")
}
finally {
    Remove-Item Env:POINT_LIGHT_CLEAR_RDC -ErrorAction SilentlyContinue
    Remove-Item Env:POINT_LIGHT_CLEAR_RDC_OUTPUT -ErrorAction SilentlyContinue
    Remove-Item Env:POINT_LIGHT_CLEAR_DIAGNOSTIC -ErrorAction SilentlyContinue
    Remove-Item Env:POINT_LIGHT_CLEAR_COUNT -ErrorAction SilentlyContinue
}

$replay = Get-Content -Raw -LiteralPath $replayJson | ConvertFrom-Json
if (-not [bool]$replay.success -or
    [int]$replay.eventTree.counts.lightMarkers -ne 512 -or
    [int]$replay.eventTree.counts.stencilVolumeDraw -ne 512 -or
    [int]$replay.eventTree.counts.lightingVolumeDraw -ne 512 -or
    [int]$replay.eventTree.counts.pointLightStencilClears -ne 513) {
    throw "RenderDoc event-tree validation failed: $replayJson"
}

Write-Host "RenderDoc evidence: $evidenceDirectory"
Write-Host "RDC: $capturePath"
Write-Host "Replay JSON: $replayJson"
