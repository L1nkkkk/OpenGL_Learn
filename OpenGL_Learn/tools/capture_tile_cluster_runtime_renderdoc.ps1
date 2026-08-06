param(
    [Parameter(Mandatory)]
    [string]$RunDirectory,
    [ValidateRange(2, 1000000)]
    [int]$WarmupFrames = 30,
    [ValidateRange(1, 1000000)]
    [int]$SampleFrames = 5
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
$analysisScript = Join-Path $PSScriptRoot "analyze_tile_cluster_runtime_renderdoc.py"
$preCaptureManifest = Get-Content -LiteralPath (Join-Path $resolvedRunDirectory "pre-capture-manifest.json") -Raw -Encoding UTF8 | ConvertFrom-Json

if ((Get-FileHash -LiteralPath $executable -Algorithm SHA256).Hash -ne [string]$preCaptureManifest.executableSha256) {
    throw "Current executable does not match the frozen formal binary."
}
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
    $startInfo = New-Object System.Diagnostics.ProcessStartInfo
    $startInfo.FileName = $FilePath
    $startInfo.Arguments = $quoted -join " "
    $startInfo.WorkingDirectory = $projectDirectory
    $startInfo.UseShellExecute = $false
    $startInfo.CreateNoWindow = $true
    $startInfo.RedirectStandardOutput = $true
    $startInfo.RedirectStandardError = $true
    $process = [System.Diagnostics.Process]::Start($startInfo)
    $stdout = $process.StandardOutput.ReadToEndAsync()
    $stderr = $process.StandardError.ReadToEndAsync()
    $process.WaitForExit()
    [System.IO.File]::WriteAllText($StdoutPath, $stdout.Result)
    [System.IO.File]::WriteAllText($StderrPath, $stderr.Result)
    if ($process.ExitCode -ne 0) {
        throw "Process failed with exit code $($process.ExitCode): $FilePath; stderr=$StderrPath"
    }
}

$captureTemplate = Join-Path $captureDirectory "cached-n0512-r030-cluster16"
$capturePath = $captureTemplate + "_capture.rdc"
$diagnosticPath = Join-Path $diagnosticDirectory "cached-n0512-r030-cluster16.json"
$applicationImage = Join-Path $imageDirectory "cached-n0512-r030-cluster16-app.ppm"
$replayJson = Join-Path $replayDirectory "cached-n0512-r030-cluster16-replay.json"

$captureArguments = @(
    "capture",
    "--working-dir", $projectDirectory,
    "--capture-file", $captureTemplate,
    "--opt-disallow-vsync",
    "--wait-for-exit",
    $executable,
    "--gbuffer-position", "explicit",
    "--point-light-render-mode", "cluster16",
    "--point-light-grid-update", "cached",
    "--point-light-offscreen-culling", "off",
    "--point-light-stencil-clear-mode", "coalesced-n-plus-one",
    "--point-light-stress",
    "--point-light-count", "512",
    "--point-light-coverage", "representative",
    "--point-light-seed", "0x21D3F3A5",
    "--point-light-target-radius", "3",
    "--point-light-width", "1920",
    "--point-light-height", "1080",
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
    -Arguments @("thumb", "--out", (Join-Path $imageDirectory "cached-n0512-r030-cluster16-thumbnail.png"), $capturePath) `
    -StdoutPath (Join-Path $logDirectory "thumbnail.stdout.log") `
    -StderrPath (Join-Path $logDirectory "thumbnail.stderr.log")
Invoke-HiddenProcess -FilePath $renderDocCmd `
    -Arguments @("replay", "--width", "640", "--height", "360", "--loops", "1", $capturePath) `
    -StdoutPath (Join-Path $logDirectory "renderdoccmd-replay.stdout.log") `
    -StderrPath (Join-Path $logDirectory "renderdoccmd-replay.stderr.log")

$env:TILE_CLUSTER_RUNTIME_RDC = $capturePath
$env:TILE_CLUSTER_RUNTIME_RDC_OUTPUT = $replayJson
$env:TILE_CLUSTER_RUNTIME_DIAGNOSTIC = $diagnosticPath
try {
    Invoke-HiddenProcess -FilePath $qRenderDoc -Arguments @("--python", $analysisScript) `
        -StdoutPath (Join-Path $logDirectory "qrenderdoc-analysis.stdout.log") `
        -StderrPath (Join-Path $logDirectory "qrenderdoc-analysis.stderr.log")
}
finally {
    Remove-Item Env:TILE_CLUSTER_RUNTIME_RDC -ErrorAction SilentlyContinue
    Remove-Item Env:TILE_CLUSTER_RUNTIME_RDC_OUTPUT -ErrorAction SilentlyContinue
    Remove-Item Env:TILE_CLUSTER_RUNTIME_DIAGNOSTIC -ErrorAction SilentlyContinue
}

$replay = Get-Content -LiteralPath $replayJson -Raw -Encoding UTF8 | ConvertFrom-Json
if (-not [bool]$replay.success) {
    throw "RenderDoc replay validation failed: $replayJson"
}
$files = Get-ChildItem -LiteralPath $evidenceDirectory -File -Recurse | ForEach-Object {
    [ordered]@{
        path = $_.FullName.Substring($resolvedRunDirectory.Length + 1)
        bytes = $_.Length
        sha256 = (Get-FileHash -LiteralPath $_.FullName -Algorithm SHA256).Hash
    }
}
$manifest = [ordered]@{
    schemaVersion = 1
    valid = $true
    createdUtc = [DateTime]::UtcNow.ToString("o")
    protocolSha256 = [string]$preCaptureManifest.protocolSha256
    executableSha256 = [string]$preCaptureManifest.executableSha256
    renderDocVersion = "1.45.0"
    case = "cached N=512 R=3 Cluster16"
    rdc = $capturePath.Substring($resolvedRunDirectory.Length + 1)
    replay = $replayJson.Substring($resolvedRunDirectory.Length + 1)
    files = $files
}
$manifest | ConvertTo-Json -Depth 8 | Set-Content -LiteralPath (Join-Path $evidenceDirectory "renderdoc-manifest.json") -Encoding UTF8

Write-Host "RenderDoc Cluster16 evidence: $evidenceDirectory"
