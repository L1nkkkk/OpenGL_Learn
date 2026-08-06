param(
    [Parameter(Mandatory)]
    [string]$RunDirectory,
    [string]$PythonExecutable = ""
)

Set-StrictMode -Version Latest
$ErrorActionPreference = "Stop"

$projectDirectory = (Resolve-Path (Join-Path $PSScriptRoot "..")).Path
$repositoryDirectory = (Resolve-Path (Join-Path $projectDirectory "..")).Path
$run = (Resolve-Path -LiteralPath $RunDirectory).Path
$root = Join-Path $run "invalid-viewport-smoke"
$executable = Join-Path $repositoryDirectory "x64\Release\OpenGL_Learn.exe"
$pre = Get-Content -LiteralPath (Join-Path $run "pre-capture-manifest.json") -Raw -Encoding UTF8 | ConvertFrom-Json
if ((Get-FileHash -LiteralPath $executable -Algorithm SHA256).Hash -ne [string]$pre.executableSha256) {
    throw "Current executable differs from the formal binary."
}
if (Test-Path -LiteralPath $root) {
    throw "Refusing to overwrite invalid-viewport evidence: $root"
}
New-Item -ItemType Directory -Force -Path $root | Out-Null

foreach ($slices in @(1, 8)) {
    $stem = "wide-s$($slices.ToString('00'))"
    $arguments = @(
        "--gbuffer-position", "explicit",
        "--point-light-render-mode", "cluster16",
        "--point-light-grid-slices", [string]$slices,
        "--point-light-grid-update", "cached",
        "--point-light-offscreen-culling", "off",
        "--point-light-stress",
        "--point-light-count", "16",
        "--point-light-coverage", "representative",
        "--point-light-seed", "0x21D3F3A5",
        "--point-light-target-radius", "6",
        "--point-light-width", "16384",
        "--point-light-height", "64",
        "--point-light-warmup-frames", "2",
        "--point-light-sample-frames", "1",
        "--point-light-result", (Join-Path $root "$stem.json"),
        "--point-light-capture", (Join-Path $root "$stem.ppm"),
        "--classic-scene-gbuffer-position-capture", (Join-Path $root "$stem-position.pfm"),
        "--classic-scene-ssao-depth-capture", (Join-Path $root "$stem-validity.pfm")
    )
    Push-Location $projectDirectory
    try {
        & $executable @arguments *> (Join-Path $root "$stem.log")
        $exitCode = $LASTEXITCODE
    }
    finally {
        Pop-Location
    }
    if ($exitCode -ne 0) { throw "$stem failed with exit code $exitCode" }
}

if ([string]::IsNullOrWhiteSpace($PythonExecutable)) {
    foreach ($candidate in @(
        "C:\Users\Link\AppData\Local\Python\bin\python.exe",
        "C:\Users\Link\.cache\codex-runtimes\codex-primary-runtime\dependencies\python\python.exe",
        "python")) {
        try {
            & $candidate --version *> $null
            if ($LASTEXITCODE -eq 0) { $PythonExecutable = $candidate; break }
        }
        catch {}
    }
}
if ([string]::IsNullOrWhiteSpace($PythonExecutable)) { throw "Python 3 with NumPy was not found" }
& $PythonExecutable (Join-Path $PSScriptRoot "verify_grid_slice_invalid_viewport.py") --root $root
if ($LASTEXITCODE -ne 0) { throw "Invalid-viewport verification failed: $LASTEXITCODE" }
Write-Host "Grid invalid-viewport smoke PASS: $root"
