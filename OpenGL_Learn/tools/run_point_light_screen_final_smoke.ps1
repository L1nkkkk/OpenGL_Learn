param(
    [Parameter(Mandatory)]
    [string]$RunDirectory
)

Set-StrictMode -Version Latest
$ErrorActionPreference = "Stop"

$projectDirectory = (Resolve-Path (Join-Path $PSScriptRoot "..")).Path
$repositoryDirectory = (Resolve-Path (Join-Path $projectDirectory "..")).Path
$executable = Join-Path $repositoryDirectory "x64\Release\OpenGL_Learn.exe"
$root = Join-Path ([System.IO.Path]::GetFullPath($RunDirectory)) "final-smoke"
$raw = Join-Path $root "raw"
$images = Join-Path $root "images"
$logs = Join-Path $root "logs"
if (Test-Path -LiteralPath $root) {
    throw "Refusing to overwrite existing final smoke evidence: $root"
}
foreach ($directory in @($raw, $images, $logs)) {
    New-Item -ItemType Directory -Force -Path $directory | Out-Null
}
if (-not (Test-Path -LiteralPath $executable -PathType Leaf)) {
    throw "Release executable is missing: $executable"
}

$records = @()
function Invoke-Smoke {
    param(
        [Parameter(Mandatory)] [string]$Name,
        [Parameter(Mandatory)] [string[]]$Arguments
    )
    $log = Join-Path $logs "$Name.log"
    $started = [DateTime]::UtcNow
    Push-Location $projectDirectory
    try {
        & $executable @Arguments *> $log
        $exitCode = $LASTEXITCODE
    }
    finally {
        Pop-Location
    }
    $script:records += [ordered]@{
        name = $Name
        startedUtc = $started.ToString("o")
        endedUtc = [DateTime]::UtcNow.ToString("o")
        exitCode = $exitCode
        log = $log
    }
    if ($exitCode -ne 0) { throw "$Name failed with exit code $exitCode; log=$log" }
    $text = Get-Content -LiteralPath $log -Raw
    if ($text -match "GL_INVALID|shader compilation failed|shader linking failed|failed to load shader") {
        throw "$Name logged a GL/shader failure; log=$log"
    }
}

function Invoke-PointSmoke {
    param(
        [Parameter(Mandatory)] [string]$Name,
        [Parameter(Mandatory)] [string]$Coverage,
        [Parameter(Mandatory)] [string[]]$ModeArguments,
        [Parameter(Mandatory)] [string]$ExpectedRenderMode,
        [Parameter(Mandatory)] [bool]$ExpectedRenderModeExplicit,
        [Parameter(Mandatory)] [double]$ExpectedPointClears,
        [Parameter(Mandatory)] [double]$ExpectedScreenDraws,
        [Parameter(Mandatory)] [double]$ExpectedStencilDraws
    )
    $json = Join-Path $raw "$Name.json"
    $capture = Join-Path $images "$Name.ppm"
    $arguments = @(
        "--gbuffer-position", "explicit",
        "--point-light-offscreen-culling", "off",
        "--point-light-stress",
        "--point-light-count", "16",
        "--point-light-coverage", $Coverage,
        "--point-light-seed", "0x21D3F3A5",
        "--point-light-width", "1920",
        "--point-light-height", "1080",
        "--point-light-warmup-frames", "2",
        "--point-light-sample-frames", "2",
        "--point-light-result", $json,
        "--point-light-capture", $capture,
        "--point-light-bounds-telemetry",
        "--point-light-stencil-lifecycle-check"
    ) + $ModeArguments
    Invoke-Smoke -Name $Name -Arguments $arguments
    $data = Get-Content -LiteralPath $json -Raw | ConvertFrom-Json
    $summary = $data.profiler.summary
    if (-not [bool]$data.success -or
        [string]$data.pointLightStress.renderMode -ne $ExpectedRenderMode -or
        [bool]$data.pointLightStress.renderModeExplicit -ne $ExpectedRenderModeExplicit -or
        [double]$summary.pointLightStencilClears.median -ne $ExpectedPointClears -or
        [double]$summary.pointLightScreenDraws.median -ne $ExpectedScreenDraws -or
        [double]$summary.pointLightStencilDraws.median -ne $ExpectedStencilDraws -or
        -not [bool]$data.pointLightStress.stencilLifecycleValidation.clean) {
        throw "$Name JSON validation failed: $json"
    }
}

Invoke-PointSmoke -Name "default-deferred-representative" -Coverage "representative" `
    -ModeArguments @("--point-light-stencil-clear-mode", "coalesced-n-plus-one") `
    -ExpectedRenderMode "analytic-screen" -ExpectedRenderModeExplicit $false `
    -ExpectedPointClears 0 -ExpectedScreenDraws 16 -ExpectedStencilDraws 0
Invoke-PointSmoke -Name "explicit-coalesced-control" -Coverage "representative" `
    -ModeArguments @(
        "--point-light-stencil-clear-mode", "coalesced-n-plus-one",
        "--point-light-render-mode", "coalesced-volume"
    ) `
    -ExpectedRenderMode "coalesced-volume" -ExpectedRenderModeExplicit $true `
    -ExpectedPointClears 17 -ExpectedScreenDraws 0 -ExpectedStencilDraws 16
Invoke-PointSmoke -Name "explicit-legacy2n-control" -Coverage "representative" `
    -ModeArguments @(
        "--point-light-stencil-clear-mode", "legacy-2n",
        "--point-light-render-mode", "coalesced-volume"
    ) `
    -ExpectedRenderMode "coalesced-volume" -ExpectedRenderModeExplicit $true `
    -ExpectedPointClears 32 -ExpectedScreenDraws 0 -ExpectedStencilDraws 16
Invoke-PointSmoke -Name "analytic-screen-high-overlap" -Coverage "high-overlap" `
    -ModeArguments @(
        "--point-light-stencil-clear-mode", "coalesced-n-plus-one",
        "--point-light-render-mode", "analytic-screen"
    ) `
    -ExpectedRenderMode "analytic-screen" -ExpectedRenderModeExplicit $true `
    -ExpectedPointClears 0 -ExpectedScreenDraws 16 -ExpectedStencilDraws 0
Invoke-PointSmoke -Name "analytic-screen-edge-cases" -Coverage "edge-cases" `
    -ModeArguments @(
        "--point-light-stencil-clear-mode", "coalesced-n-plus-one",
        "--point-light-render-mode", "analytic-screen"
    ) `
    -ExpectedRenderMode "analytic-screen" -ExpectedRenderModeExplicit $true `
    -ExpectedPointClears 0 -ExpectedScreenDraws 16 -ExpectedStencilDraws 0

Invoke-Smoke -Name "resource-forward-deferred-resize-effects" -Arguments @("--resource-smoke-test")
$resourceLog = Get-Content -LiteralPath (Join-Path $logs "resource-forward-deferred-resize-effects.log") -Raw
if ($resourceLog -notmatch "deferred-ssao-bloom-resized" -or
    $resourceLog -notmatch "all-effects-restored-size" -or
    $resourceLog -notmatch "released textureBytes=0 meshCpuBytes=0 meshGpuBytes=0 renderTargetBytes=0") {
    throw "Resource smoke did not complete all expected transitions"
}

foreach ($renderPath in @("pbr-forward", "pbr-deferred")) {
    $name = "classic-sponza-$renderPath"
    $json = Join-Path $raw "$name.json"
    $capture = Join-Path $images "$name.ppm"
    Invoke-Smoke -Name $name -Arguments @(
        "--gbuffer-position", "explicit",
        "--point-light-stencil-clear-mode", "coalesced-n-plus-one",
        "--classic-scene-test", "classic-scenes/sponza/sponza.obj",
        "--classic-scene-render-path", $renderPath,
        "--classic-scene-width", "1280",
        "--classic-scene-height", "720",
        "--classic-scene-warmup-frames", "2",
        "--classic-scene-capture-frame", "4",
        "--classic-scene-capture", $capture,
        "--classic-scene-result", $json
    )
    $data = Get-Content -LiteralPath $json -Raw | ConvertFrom-Json
    if (-not [bool]$data.success -or [string]$data.renderPath -ne $renderPath) {
        throw "$name result validation failed: $json"
    }
}

$manifest = [ordered]@{
    schemaVersion = 1
    generatedUtc = [DateTime]::UtcNow.ToString("o")
    executable = $executable
    executableSha256 = (Get-FileHash -LiteralPath $executable -Algorithm SHA256).Hash
    records = $records
    success = $true
}
$manifest | ConvertTo-Json -Depth 6 | Set-Content -LiteralPath (Join-Path $root "manifest.json") -Encoding utf8
Write-Host "Final smoke evidence: $root"
