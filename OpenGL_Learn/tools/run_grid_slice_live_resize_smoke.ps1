param(
    [Parameter(Mandatory)]
    [string]$RunDirectory,
    [switch]$FinalizeExisting
)

Set-StrictMode -Version Latest
$ErrorActionPreference = "Stop"

$projectDirectory = (Resolve-Path (Join-Path $PSScriptRoot "..")).Path
$repositoryDirectory = (Resolve-Path (Join-Path $projectDirectory "..")).Path
$run = (Resolve-Path -LiteralPath $RunDirectory).Path
$root = Join-Path $run "live-resize-smoke"
$executable = Join-Path $repositoryDirectory "x64\Release\OpenGL_Learn.exe"
$pre = Get-Content -LiteralPath (Join-Path $run "pre-capture-manifest.json") -Raw -Encoding UTF8 | ConvertFrom-Json
if ((Get-FileHash -LiteralPath $executable -Algorithm SHA256).Hash -ne [string]$pre.executableSha256) {
    throw "Current executable differs from the formal binary."
}
if (Test-Path -LiteralPath $root) {
    if (-not $FinalizeExisting) { throw "Refusing to overwrite live-resize evidence: $root" }
}
else {
    if ($FinalizeExisting) { throw "Live-resize evidence does not exist: $root" }
    New-Item -ItemType Directory -Force -Path $root | Out-Null
}

$processId = 0
$windowHandle = 0
$resultPath = Join-Path $root "result.json"
$capturePath = Join-Path $root "frame.ppm"
$stdoutPath = Join-Path $root "stdout.log"
$stderrPath = Join-Path $root "stderr.log"
if (-not $FinalizeExisting) {
Add-Type @"
using System;
using System.Collections.Generic;
using System.Runtime.InteropServices;
public static class GridResizeNative {
    public delegate bool EnumWindowsProc(IntPtr hwnd, IntPtr lParam);
    [DllImport("user32.dll")] public static extern bool EnumWindows(EnumWindowsProc callback, IntPtr lParam);
    [DllImport("user32.dll")] public static extern uint GetWindowThreadProcessId(IntPtr hwnd, out uint processId);
    [DllImport("user32.dll", SetLastError=true)] public static extern bool SetWindowPos(IntPtr hwnd, IntPtr insertAfter, int x, int y, int width, int height, uint flags);
    public static IntPtr FindWindow(uint targetProcessId) {
        IntPtr found = IntPtr.Zero;
        EnumWindows((hwnd, state) => {
            uint processId;
            GetWindowThreadProcessId(hwnd, out processId);
            if (processId == targetProcessId) { found = hwnd; return false; }
            return true;
        }, IntPtr.Zero);
        return found;
    }
}
"@

function Quote-Argument([string]$Value) {
    return '"' + $Value.Replace('"', '\"') + '"'
}

$arguments = @(
    "--gbuffer-position", "explicit",
    "--point-light-render-mode", "cluster16",
    "--point-light-grid-slices", "8",
    "--point-light-grid-update", "cached",
    "--point-light-offscreen-culling", "off",
    "--point-light-stress",
    "--point-light-count", "64",
    "--point-light-coverage", "representative",
    "--point-light-seed", "0x21D3F3A5",
    "--point-light-target-radius", "6",
    "--point-light-width", "640",
    "--point-light-height", "360",
    "--point-light-warmup-frames", "8000",
    "--point-light-sample-frames", "1",
    "--point-light-result", $resultPath,
    "--point-light-capture", $capturePath
)
$startInfo = New-Object Diagnostics.ProcessStartInfo
$startInfo.FileName = $executable
$startInfo.Arguments = (@($arguments | ForEach-Object { Quote-Argument $_ }) -join " ")
$startInfo.WorkingDirectory = $projectDirectory
$startInfo.UseShellExecute = $false
$startInfo.CreateNoWindow = $true
$startInfo.RedirectStandardOutput = $true
$startInfo.RedirectStandardError = $true
$process = [Diagnostics.Process]::Start($startInfo)
$processId = $process.Id
$stdoutTask = $process.StandardOutput.ReadToEndAsync()
$stderrTask = $process.StandardError.ReadToEndAsync()

$window = [IntPtr]::Zero
$deadline = [DateTime]::UtcNow.AddSeconds(10)
while ($window -eq [IntPtr]::Zero -and [DateTime]::UtcNow -lt $deadline -and -not $process.HasExited) {
    $window = [GridResizeNative]::FindWindow([uint32]$process.Id)
    if ($window -eq [IntPtr]::Zero) { Start-Sleep -Milliseconds 50 }
}
if ($window -eq [IntPtr]::Zero) {
    if (-not $process.HasExited) { $process.Kill(); $process.WaitForExit() }
    throw "Could not locate the hidden GLFW window for resize testing"
}
$windowHandle = $window.ToInt64()

Start-Sleep -Milliseconds 2000
if ($process.HasExited) { throw "Renderer exited before the first resize" }
$flags = [uint32](0x0002 -bor 0x0004 -bor 0x0010)
$firstResize = [GridResizeNative]::SetWindowPos($window, [IntPtr]::Zero, 0, 0, 800, 450, $flags)
if (-not $firstResize) { throw "First SetWindowPos failed" }
Start-Sleep -Milliseconds 1000
if ($process.HasExited) { throw "Renderer exited before the second resize" }
$secondResize = [GridResizeNative]::SetWindowPos($window, [IntPtr]::Zero, 0, 0, 1024, 576, $flags)
if (-not $secondResize) { throw "Second SetWindowPos failed" }

if (-not $process.WaitForExit(120000)) {
    $process.Kill()
    $process.WaitForExit()
    throw "Renderer timed out during live resize smoke"
}
[IO.File]::WriteAllText($stdoutPath, $stdoutTask.Result)
[IO.File]::WriteAllText($stderrPath, $stderrTask.Result)
if ($process.ExitCode -ne 0) { throw "Renderer failed with exit code $($process.ExitCode)" }
}

$result = Get-Content -LiteralPath $resultPath -Raw -Encoding UTF8 | ConvertFrom-Json
$grid = $result.pointLightStress.gridRuntime
$logs = (Get-Content -LiteralPath $stdoutPath -Raw) + (Get-Content -LiteralPath $stderrPath -Raw)
$finalWidth = [int]$result.resolution[0]
$finalHeight = [int]$result.resolution[1]
$expectedTilesX = [int][Math]::Ceiling($finalWidth / 16.0)
$expectedTilesY = [int][Math]::Ceiling($finalHeight / 16.0)
$expectedLogicalCells = $expectedTilesX * $expectedTilesY * 8
if (-not [bool]$result.success -or
    [int]$grid.sliceCount -ne 8 -or
    -not [bool]$grid.valid -or
    [bool]$grid.overflow -or
    [string]$grid.error -ne "" -or
    [int64]$grid.buildCount -lt 3 -or
    [int64]$grid.cacheHitCount -le 0 -or
    ($finalWidth -eq 640 -and $finalHeight -eq 360) -or
    [int]$grid.tilesX -ne $expectedTilesX -or
    [int]$grid.tilesY -ne $expectedTilesY -or
    [int]$grid.logicalCells -ne $expectedLogicalCells -or
    $logs -match "GL_INVALID|GL error|shader compilation failed|shader linking failed|failed to load shader") {
    throw "Live resize semantic validation failed"
}
$remaining = @(Get-Process -Name "OpenGL_Learn" -ErrorAction SilentlyContinue).Count
if ($remaining -ne 0) { throw "Renderer remains after live resize smoke" }
$manifest = [ordered]@{
    schemaVersion = 1
    success = $true
    executableSha256 = (Get-FileHash -LiteralPath $executable -Algorithm SHA256).Hash
    processId = $processId
    windowHandle = $windowHandle
    requestedInitialResolution = @(640, 360)
    resizeSequence = @(@(800, 450), @(1024, 576))
    finalResolution = @($finalWidth, $finalHeight)
    finalGridDimensions = @([int]$grid.tilesX, [int]$grid.tilesY, [int]$grid.sliceCount)
    expectedLogicalCells = $expectedLogicalCells
    logicalCells = [int]$grid.logicalCells
    sliceCount = [int]$grid.sliceCount
    buildCount = [int64]$grid.buildCount
    cacheHitCount = [int64]$grid.cacheHitCount
    csrSignature = [string]$grid.csrSignature
    remainingRendererProcesses = $remaining
    resultSha256 = (Get-FileHash -LiteralPath $resultPath -Algorithm SHA256).Hash
    captureSha256 = (Get-FileHash -LiteralPath $capturePath -Algorithm SHA256).Hash
}
$manifest | ConvertTo-Json -Depth 8 | Set-Content -LiteralPath (Join-Path $root "manifest.json") -Encoding UTF8
Write-Host "Grid live-resize smoke PASS: builds=$($grid.buildCount), hits=$($grid.cacheHitCount), final=$($result.resolution[0])x$($result.resolution[1])"
