param(
    [Parameter(Mandatory)]
    [string]$RunDirectory
)

Set-StrictMode -Version Latest
$ErrorActionPreference = "Stop"

$projectDirectory = (Resolve-Path (Join-Path $PSScriptRoot "..")).Path
$repositoryDirectory = (Resolve-Path (Join-Path $projectDirectory "..")).Path
$executable = Join-Path $repositoryDirectory "x64\Release\OpenGL_Learn.exe"
$root = Join-Path ([System.IO.Path]::GetFullPath($RunDirectory)) "grid-final-smoke"
$raw = Join-Path $root "raw"
$images = Join-Path $root "images"
$logs = Join-Path $root "logs"
if (Test-Path -LiteralPath $root) {
    throw "Refusing to overwrite existing grid smoke evidence: $root"
}
foreach ($directory in @($raw, $images, $logs)) {
    New-Item -ItemType Directory -Force -Path $directory | Out-Null
}

$cases = @(
    [ordered]@{ name="legacy-tile16"; mode="tile16"; slices=$null; expected=1 },
    [ordered]@{ name="explicit-s01"; mode="tile16"; slices=1; expected=1 },
    [ordered]@{ name="legacy-cluster16"; mode="cluster16"; slices=$null; expected=16 },
    [ordered]@{ name="explicit-s16"; mode="cluster16"; slices=16; expected=16 },
    [ordered]@{ name="explicit-s08"; mode="cluster16"; slices=8; expected=8 }
)
$records = @()
foreach ($case in $cases) {
    $json = Join-Path $raw "$($case.name).json"
    $image = Join-Path $images "$($case.name).ppm"
    $log = Join-Path $logs "$($case.name).log"
    $arguments = @(
        "--gbuffer-position", "explicit",
        "--point-light-render-mode", [string]$case.mode,
        "--point-light-grid-update", "cached",
        "--point-light-offscreen-culling", "off",
        "--point-light-stress",
        "--point-light-count", "64",
        "--point-light-coverage", "representative",
        "--point-light-seed", "0x21D3F3A5",
        "--point-light-target-radius", "6",
        "--point-light-width", "640",
        "--point-light-height", "360",
        "--point-light-warmup-frames", "2",
        "--point-light-sample-frames", "2",
        "--point-light-result", $json,
        "--point-light-capture", $image
    )
    if ($null -ne $case.slices) {
        $arguments += @("--point-light-grid-slices", [string]$case.slices)
    }
    Push-Location $projectDirectory
    try {
        & $executable @arguments *> $log
        $exitCode = $LASTEXITCODE
    }
    finally {
        Pop-Location
    }
    if ($exitCode -ne 0) { throw "$($case.name) failed with exit code $exitCode; log=$log" }
    $data = Get-Content -LiteralPath $json -Raw -Encoding UTF8 | ConvertFrom-Json
    $logText = Get-Content -LiteralPath $log -Raw
    $explicitExpected = $null -ne $case.slices
    if (-not [bool]$data.success -or
        [string]$data.pointLightStress.renderMode -ne [string]$case.mode -or
        [int]$data.pointLightStress.gridRuntime.sliceCount -ne [int]$case.expected -or
        [bool]$data.pointLightStress.gridSliceCountExplicit -ne $explicitExpected -or
        -not [bool]$data.pointLightStress.gridRuntime.valid -or
        [bool]$data.pointLightStress.gridRuntime.overflow -or
        [string]$data.pointLightStress.gridRuntime.error -ne "" -or
        [double]$data.profiler.summary.pointLightScreenDraws.median -ne 1 -or
        [double]$data.profiler.summary.pointLightStencilDraws.median -ne 0 -or
        $logText -match "GL_INVALID|GL error|shader compilation failed|shader linking failed|failed to load shader") {
        throw "$($case.name) semantic validation failed"
    }
    $records += [ordered]@{
        name = $case.name
        mode = [string]$case.mode
        explicitSlices = $case.slices
        runtimeSlices = [int]$data.pointLightStress.gridRuntime.sliceCount
        csrSignature = [string]$data.pointLightStress.gridRuntime.csrSignature
        imageSha256 = (Get-FileHash -LiteralPath $image -Algorithm SHA256).Hash
        exitCode = $exitCode
        success = $true
    }
}

function Get-Record([string]$Name) {
    return $records | Where-Object { $_.name -eq $Name } | Select-Object -First 1
}
$tileLegacy = Get-Record "legacy-tile16"
$tileExplicit = Get-Record "explicit-s01"
$clusterLegacy = Get-Record "legacy-cluster16"
$clusterExplicit = Get-Record "explicit-s16"
$endpointChecks = [ordered]@{
    tileImageExact = $tileLegacy.imageSha256 -eq $tileExplicit.imageSha256
    tileCsrExact = $tileLegacy.csrSignature -eq $tileExplicit.csrSignature
    clusterImageExact = $clusterLegacy.imageSha256 -eq $clusterExplicit.imageSha256
    clusterCsrExact = $clusterLegacy.csrSignature -eq $clusterExplicit.csrSignature
}
if ($endpointChecks.Values -contains $false) { throw "Post-build endpoint compatibility failed" }

$remaining = @(Get-Process -Name "OpenGL_Learn" -ErrorAction SilentlyContinue).Count
if ($remaining -ne 0) { throw "Renderer processes remain after grid smoke: $remaining" }
$manifest = [ordered]@{
    schemaVersion = 1
    generatedUtc = [DateTime]::UtcNow.ToString("o")
    executable = $executable
    executableSha256 = (Get-FileHash -LiteralPath $executable -Algorithm SHA256).Hash
    records = $records
    endpointCompatibility = $endpointChecks
    remainingRendererProcesses = $remaining
    success = $true
}
$manifest | ConvertTo-Json -Depth 8 | Set-Content -LiteralPath (Join-Path $root "manifest.json") -Encoding UTF8
Write-Host "Grid slice final smoke evidence: $root"
