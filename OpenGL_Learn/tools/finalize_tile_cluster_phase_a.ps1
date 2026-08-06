param(
    [Parameter(Mandatory)]
    [string]$RunDirectory,
    [string]$PythonExecutable = "C:\Users\Link\.cache\codex-runtimes\codex-primary-runtime\dependencies\python\python.exe"
)

Set-StrictMode -Version Latest
$ErrorActionPreference = "Stop"

$projectDirectory = Split-Path -Parent $PSScriptRoot
$repositoryDirectory = Split-Path -Parent $projectDirectory
$solution = Join-Path $repositoryDirectory "OpenGL_Learn.sln"
$executable = Join-Path $repositoryDirectory "x64\Release\OpenGL_Learn.exe"
$msbuild = "C:\Program Files\Microsoft Visual Studio\2022\Community\MSBuild\Current\Bin\MSBuild.exe"
$RunDirectory = [System.IO.Path]::GetFullPath($RunDirectory)
$verificationDirectory = Join-Path $RunDirectory "verification"
New-Item -ItemType Directory -Force -Path $verificationDirectory | Out-Null

$buildLog = Join-Path $verificationDirectory "release-build.log"
& $msbuild $solution /t:Build /p:Configuration=Release /p:Platform=x64 /m /v:minimal *> $buildLog
$buildExitCode = $LASTEXITCODE
if ($buildExitCode -ne 0) {
    throw "Release build failed: $buildExitCode; log=$buildLog"
}

$manifest = Get-Content -LiteralPath (Join-Path $RunDirectory "capture-manifest.json") -Raw -Encoding UTF8 | ConvertFrom-Json
$executableHash = (Get-FileHash -LiteralPath $executable -Algorithm SHA256).Hash
if ($executableHash -ne [string]$manifest.executableSha256) {
    throw "Final executable differs from captured executable"
}

$smokeJson = Join-Path $verificationDirectory "smoke.json"
$smokePpm = Join-Path $verificationDirectory "smoke.ppm"
$smokeLog = Join-Path $verificationDirectory "smoke.log"
$arguments = @(
    "--gbuffer-position", "explicit",
    "--point-light-stencil-clear-mode", "coalesced-n-plus-one",
    "--point-light-offscreen-culling", "off",
    "--point-light-stress",
    "--point-light-count", "16",
    "--point-light-coverage", "small-local",
    "--point-light-seed", "0x21D3F3A5",
    "--point-light-width", "640",
    "--point-light-height", "360",
    "--point-light-warmup-frames", "2",
    "--point-light-sample-frames", "1",
    "--point-light-result", $smokeJson,
    "--point-light-capture", $smokePpm,
    "--point-light-stencil-lifecycle-check"
)
Push-Location $projectDirectory
try {
    & $executable @arguments *> $smokeLog
    $smokeExitCode = $LASTEXITCODE
}
finally {
    Pop-Location
}
if ($smokeExitCode -ne 0) {
    throw "Default smoke failed: $smokeExitCode; log=$smokeLog"
}
$smoke = Get-Content -LiteralPath $smokeJson -Raw -Encoding UTF8 | ConvertFrom-Json
$smokeText = Get-Content -LiteralPath $smokeLog -Raw
$glMatches = [regex]::Matches($smokeText, "GL_INVALID|GL error|shader compilation failed|shader linking failed")
if (-not [bool]$smoke.success -or
    [string]$smoke.pointLightStress.renderMode -ne "analytic-screen" -or
    [bool]$smoke.pointLightStress.renderModeExplicit -or
    -not [bool]$smoke.pointLightStress.stencilLifecycleValidation.clean -or
    $glMatches.Count -ne 0) {
    throw "Default smoke semantic validation failed"
}

Start-Sleep -Milliseconds 300
$remaining = @(Get-Process -Name "OpenGL_Learn" -ErrorAction SilentlyContinue).Count
if ($remaining -ne 0) {
    throw "Renderer process remains after smoke: $remaining"
}

$verification = [ordered]@{
    schemaVersion = 1
    generatedUtc = [DateTime]::UtcNow.ToString("o")
    releaseBuild = [ordered]@{
        success = $true
        exitCode = $buildExitCode
        configuration = "Release"
        architecture = "x64"
        solution = $solution
        log = "verification/release-build.log"
    }
    executable = [ordered]@{
        path = $executable
        sha256 = $executableHash
        capturedSha256 = [string]$manifest.executableSha256
        matchesCapturedExecutable = $true
    }
    smoke = [ordered]@{
        success = $true
        exitCode = $smokeExitCode
        resultSuccess = [bool]$smoke.success
        buildConfiguration = [string]$smoke.buildConfiguration
        architecture = [string]$smoke.architecture
        resolution = @([int]$smoke.resolution[0], [int]$smoke.resolution[1])
        pointLightCount = [int]$smoke.pointLightStress.generatedLightCount
        coverage = [string]$smoke.pointLightStress.coverage
        seed = "0x21D3F3A5"
        warmupFrames = 2
        sampleFrames = 1
        renderModeArgumentPassed = $false
        renderMode = [string]$smoke.pointLightStress.renderMode
        renderModeExplicit = [bool]$smoke.pointLightStress.renderModeExplicit
        stencilLifecycleClean = [bool]$smoke.pointLightStress.stencilLifecycleValidation.clean
        glErrorCount = $glMatches.Count
        result = "verification/smoke.json"
        capture = "verification/smoke.ppm"
        log = "verification/smoke.log"
    }
    processCleanup = [ordered]@{
        remainingRendererProcesses = $remaining
        passed = ($remaining -eq 0)
    }
    runtimeCandidateImplemented = $false
    defaultChanged = $false
}
$verification | ConvertTo-Json -Depth 8 | Set-Content -LiteralPath (Join-Path $verificationDirectory "verification.json") -Encoding UTF8

& $PythonExecutable (Join-Path $PSScriptRoot "analyze_tile_cluster_phase_a.py") `
    --run-dir $RunDirectory --report-only
if ($LASTEXITCODE -ne 0) {
    throw "Report/artifact finalization failed: $LASTEXITCODE"
}
Write-Host "Tile/Cluster final verification complete: $verificationDirectory"
