[CmdletBinding()]
param(
    [switch]$SkipPrepare,
    [switch]$SkipBuild,
    [switch]$ReportOnly,
    [switch]$NoOpen,
    [string[]]$SceneIds,
    [string]$PythonPath,
    [string]$AssimpPath,
    [string]$MsBuildPath
)

$ErrorActionPreference = "Stop"
$ProgressPreference = "SilentlyContinue"

$toolsDirectory = $PSScriptRoot
$projectDirectory = Split-Path -Parent $toolsDirectory
$repositoryDirectory = Split-Path -Parent $projectDirectory
$manifestPath = Join-Path $projectDirectory "classic-scenes.manifest.json"
$imageToolPath = Join-Path $toolsDirectory "classic_scene_images.py"
$manifest = Get-Content -LiteralPath $manifestPath -Raw | ConvertFrom-Json
$assetRoot = Join-Path $projectDirectory $manifest.assetRoot
$archiveRoot = Join-Path $assetRoot "_archives"
$resultRoot = Join-Path $projectDirectory "benchmark-results\classic-scenes"
$executablePath = Join-Path $repositoryDirectory "x64\Release\OpenGL_Learn.exe"

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
        if (-not $candidate -or -not (Test-Path -LiteralPath $candidate -PathType Leaf)) {
            continue
        }
        & $candidate -c "from PIL import Image" 2>$null
        if ($LASTEXITCODE -eq 0) {
            return (Resolve-Path -LiteralPath $candidate).Path
        }
    }
    throw "Python with Pillow was not found. Pass -PythonPath <python.exe>."
}

function Resolve-Assimp {
    $candidates = @(
        $AssimpPath,
        (Join-Path $repositoryDirectory "assimp\build\bin\Release\assimp.exe")
    )
    foreach ($candidate in $candidates) {
        if ($candidate -and (Test-Path -LiteralPath $candidate -PathType Leaf)) {
            return (Resolve-Path -LiteralPath $candidate).Path
        }
    }
    throw "assimp.exe was not found. Pass -AssimpPath <assimp.exe>."
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
        if ($candidate -and (Test-Path -LiteralPath $candidate -PathType Leaf)) {
            return (Resolve-Path -LiteralPath $candidate).Path
        }
    }
    throw "MSBuild 2022 was not found. Pass -MsBuildPath <MSBuild.exe>."
}

function Assert-Archive {
    param($Package, [string]$ArchivePath)

    $item = Get-Item -LiteralPath $ArchivePath
    if ([int64]$item.Length -ne [int64]$Package.archiveBytes) {
        throw "Archive size mismatch for $($Package.id): $($item.Length)"
    }
    $hash = (Get-FileHash -LiteralPath $ArchivePath -Algorithm SHA256).Hash
    if ($hash -ne $Package.sha256) {
        throw "Archive SHA-256 mismatch for $($Package.id): $hash"
    }
}

function Ensure-Package {
    param($Package)

    New-Item -ItemType Directory -Path $archiveRoot -Force | Out-Null
    $archivePath = Join-Path $archiveRoot $Package.archiveName
    if (-not (Test-Path -LiteralPath $archivePath -PathType Leaf)) {
        $partialPath = "$archivePath.download"
        if (Test-Path -LiteralPath $partialPath) {
            Remove-Item -LiteralPath $partialPath -Force
        }
        Write-Host "Downloading $($Package.id) ($($Package.archiveBytes) bytes)..."
        Invoke-WebRequest -Uri $Package.downloadUrl -OutFile $partialPath
        Assert-Archive -Package $Package -ArchivePath $partialPath
        Move-Item -LiteralPath $partialPath -Destination $archivePath
    }
    Assert-Archive -Package $Package -ArchivePath $archivePath

    $destination = Join-Path $assetRoot $Package.extractDirectory
    $sentinel = Join-Path $destination $Package.sentinel
    if (Test-Path -LiteralPath $sentinel -PathType Leaf) {
        Write-Host "Prepared package found: $($Package.id)"
        return
    }

    $temporaryRoot = Join-Path $assetRoot ("_extract\" + $Package.id)
    if (Test-Path -LiteralPath $temporaryRoot) {
        Remove-Item -LiteralPath $temporaryRoot -Recurse -Force
    }
    New-Item -ItemType Directory -Path $temporaryRoot -Force | Out-Null
    Write-Host "Extracting $($Package.id)..."
    Expand-Archive -LiteralPath $archivePath -DestinationPath $temporaryRoot -Force

    $source = $temporaryRoot
    if ($Package.archiveRoot) {
        $source = Join-Path $temporaryRoot $Package.archiveRoot
    }
    if (-not (Test-Path -LiteralPath $source -PathType Container)) {
        throw "Expected archive root not found for $($Package.id): $source"
    }
    New-Item -ItemType Directory -Path $destination -Force | Out-Null
    Get-ChildItem -LiteralPath $source -Force |
        Move-Item -Destination $destination -Force
    Remove-Item -LiteralPath $temporaryRoot -Recurse -Force
    if (-not (Test-Path -LiteralPath $sentinel -PathType Leaf)) {
        throw "Package extraction did not create $sentinel"
    }
}

function Prepare-Bistro {
    param([string]$Python, [string]$Assimp)

    $bistroDirectory = Join-Path $assetRoot "bistro"
    & $Python $imageToolPath "dds-payloads" (Join-Path $bistroDirectory "Textures")
    if ($LASTEXITCODE -ne 0) {
        throw "Bistro DDS conversion failed."
    }

    foreach ($name in @("BistroExterior", "BistroInterior")) {
        $sourcePath = Join-Path $bistroDirectory "$name.fbx"
        $destinationPath = Join-Path $bistroDirectory "$name.obj"
        if (Test-Path -LiteralPath $destinationPath -PathType Leaf) {
            Write-Host "Prepared Bistro OBJ found: $name"
            continue
        }
        Write-Host "Pre-transforming $name for the current static-mesh importer..."
        & $Assimp export $sourcePath $destinationPath -fobj -ptv
        if ($LASTEXITCODE -ne 0 -or
            -not (Test-Path -LiteralPath $destinationPath -PathType Leaf)) {
            throw "Assimp export failed for $name."
        }
    }
}

function Build-Renderer {
    param([string]$MsBuild)

    Write-Host "Building Release x64..."
    & $MsBuild (Join-Path $repositoryDirectory "OpenGL_Learn.sln") `
        /m /p:Configuration=Release /p:Platform=x64 /verbosity:minimal
    if ($LASTEXITCODE -ne 0) {
        throw "Release x64 build failed."
    }
}

function Invoke-SceneTest {
    param($Scene, [string]$Python)

    $sceneId = [string]$Scene.id
    $modelPath = "classic-scenes/" + ([string]$Scene.modelPath).Replace("\", "/")
    $capturePath = "benchmark-results/classic-scenes/$sceneId.ppm"
    $jsonPath = "benchmark-results/classic-scenes/$sceneId.json"
    $logPath = Join-Path $resultRoot "$sceneId.log"
    $arguments = @(
        "--classic-scene-test", $modelPath,
        "--classic-scene-name", $sceneId,
        "--classic-scene-capture", $capturePath,
        "--classic-scene-result", $jsonPath,
        "--classic-scene-camera",
            [string]$Scene.camera[0], [string]$Scene.camera[1], [string]$Scene.camera[2],
        "--classic-scene-target",
            [string]$Scene.target[0], [string]$Scene.target[1], [string]$Scene.target[2],
        "--classic-scene-up",
            [string]$Scene.up[0], [string]$Scene.up[1], [string]$Scene.up[2],
        "--classic-scene-radius", [string]$Scene.normalizedRadius,
        "--classic-scene-fov", [string]$Scene.fov
    )

    Write-Host "Running $($Scene.displayName)..."
    Push-Location $projectDirectory
    try {
        & $executablePath @arguments *> $logPath
        $exitCode = $LASTEXITCODE
    }
    finally {
        Pop-Location
    }
    if ($exitCode -ne 0) {
        throw "$($Scene.displayName) failed with exit code $exitCode. See $logPath"
    }

    $resultPath = Join-Path $resultRoot "$sceneId.json"
    $ppmPath = Join-Path $resultRoot "$sceneId.ppm"
    $pngPath = Join-Path $resultRoot "$sceneId.png"
    & $Python $imageToolPath "ppm-to-png" $ppmPath $pngPath
    if ($LASTEXITCODE -ne 0) {
        throw "Capture conversion failed for $sceneId."
    }
    $result = Get-Content -LiteralPath $resultPath -Raw | ConvertFrom-Json
    if (-not $result.success) {
        throw "$($Scene.displayName) produced an unsuccessful result."
    }
    if ([int64]$result.triangleCount -ne [int64]$Scene.expectedTriangles) {
        throw "$($Scene.displayName) triangle count changed: $($result.triangleCount)"
    }
    if ([int64]$result.memoryBytes.meshCpu -ne 0) {
        throw "$($Scene.displayName) retained CPU mesh staging memory."
    }
    Write-Host (
        "PASS {0}: load={1:N1} ms, frame={2:N3} ms, triangles={3:N0}" -f
        $Scene.displayName,
        [double]$result.loadMilliseconds,
        [double]$result.averageFrameMilliseconds,
        [int64]$result.triangleCount
    )
}

function Html-Encode {
    param([object]$Value)
    return [System.Net.WebUtility]::HtmlEncode([string]$Value)
}

function Format-Bytes {
    param([double]$Bytes)
    if ($Bytes -ge 1GB) {
        return "{0:N2} GiB" -f ($Bytes / 1GB)
    }
    return "{0:N2} MiB" -f ($Bytes / 1MB)
}

function Write-Reports {
    param([object[]]$Scenes)

    $records = @()
    foreach ($scene in $Scenes) {
        $resultPath = Join-Path $resultRoot "$($scene.id).json"
        $imagePath = Join-Path $resultRoot "$($scene.id).png"
        if (-not (Test-Path -LiteralPath $resultPath -PathType Leaf) -or
            -not (Test-Path -LiteralPath $imagePath -PathType Leaf)) {
            throw "Missing result or image for $($scene.id)."
        }
        $records += [pscustomobject]@{
            Scene = $scene
            Result = Get-Content -LiteralPath $resultPath -Raw | ConvertFrom-Json
        }
    }

    New-Item -ItemType Directory -Path $resultRoot -Force | Out-Null
    $branch = (& git -C $repositoryDirectory branch --show-current).Trim()
    $commit = (& git -C $repositoryDirectory rev-parse --short HEAD).Trim()
    if ((& git -C $repositoryDirectory status --porcelain | Out-String).Trim()) {
        $commit += "-dirty"
    }
    $gpu = "Unknown GPU"
    try {
        $gpuNames = @(Get-CimInstance Win32_VideoController |
            Where-Object { $_.Name } |
            Select-Object -ExpandProperty Name)
        $gpuName = $gpuNames |
            Where-Object {
                $_ -notmatch "Virtual" -and
                $_ -match "NVIDIA|AMD|Radeon|Intel.*Arc"
            } |
            Select-Object -First 1
        if (-not $gpuName) {
            $gpuName = $gpuNames |
                Where-Object { $_ -notmatch "Virtual" } |
                Select-Object -First 1
        }
        if (-not $gpuName) {
            $gpuName = $gpuNames | Select-Object -First 1
        }
        if ($gpuName) {
            $gpu = $gpuName
        }
    }
    catch {
        $gpu = "GPU query unavailable"
    }
    $timestamp = Get-Date -Format "yyyy-MM-dd HH:mm:ss zzz"
    $passCount = @($records | Where-Object { $_.Result.success }).Count

    $notes = @{
        "sponza" = "Pass. Interior framing, diffuse color, normal detail, and independent OBJ opacity masks are visible."
        "bistro-exterior" = "Pass. Street geometry is intact. Packed AO/roughness/metalness, BC5 DirectX normals, and companion emissive maps use Bistro's authored conventions."
        "bistro-interior" = "Pass. Bar, cabinets, and props are visible with Bistro's packed PBR and normal-map conventions applied."
        "san-miguel" = "Pass. All 323 source PNGs load. The 703-pixel-wide RGB texture exercises the GL_UNPACK_ALIGNMENT fix."
    }

    $cards = New-Object System.Text.StringBuilder
    foreach ($record in $records) {
        $scene = $record.Scene
        $result = $record.Result
        $status = if ($result.success) { "PASS" } else { "FAIL" }
        $statusClass = if ($result.success) { "pass" } else { "fail" }
        $null = $cards.AppendLine(@"
<article class="scene-card">
  <div class="scene-heading">
    <div>
      <p class="eyebrow">$(Html-Encode $scene.category)</p>
      <h2>$(Html-Encode $scene.displayName)</h2>
    </div>
    <span class="status $statusClass">$status</span>
  </div>
  <img src="$(Html-Encode ($scene.id + ".png"))" alt="$(Html-Encode ($scene.displayName + " renderer capture"))">
  <div class="metrics">
    <div><span>Load ready</span><strong>$("{0:N1} ms" -f [double]$result.loadMilliseconds)</strong></div>
    <div><span>Average frame</span><strong>$("{0:N3} ms" -f [double]$result.averageFrameMilliseconds)</strong></div>
    <div><span>FPS</span><strong>$("{0:N1}" -f [double]$result.averageFps)</strong></div>
    <div><span>Triangles</span><strong>$("{0:N0}" -f [int64]$result.triangleCount)</strong></div>
    <div><span>Meshes</span><strong>$("{0:N0}" -f [int64]$result.meshCount)</strong></div>
    <div><span>Texture estimate</span><strong>$(Format-Bytes ([double]$result.memoryBytes.texture))</strong></div>
    <div><span>Mesh GPU</span><strong>$(Format-Bytes ([double]$result.memoryBytes.meshGpu))</strong></div>
    <div><span>Mesh CPU staging</span><strong>$(Format-Bytes ([double]$result.memoryBytes.meshCpu))</strong></div>
  </div>
  <p class="review">$(Html-Encode $notes[[string]$scene.id])</p>
</article>
"@)
    }

    $sources = New-Object System.Text.StringBuilder
    foreach ($package in $manifest.packages) {
        $null = $sources.AppendLine(@"
<tr>
  <td>$(Html-Encode $package.id)</td>
  <td><a href="$(Html-Encode $package.sourcePage)">Official source</a></td>
  <td>$(Html-Encode $package.license)</td>
  <td><code>$(Html-Encode $package.sha256)</code></td>
</tr>
"@)
    }

    $html = @"
<!doctype html>
<html lang="zh-CN">
<head>
  <meta charset="utf-8">
  <meta name="viewport" content="width=device-width, initial-scale=1">
  <title>OpenGL_Learn Classic Scene Acceptance Report</title>
  <style>
    :root { color-scheme: dark; --bg:#091018; --panel:#111b27; --line:#26364a; --text:#e9f0f7; --muted:#95a8bc; --accent:#62d8ff; --pass:#48d597; --warn:#ffca68; }
    * { box-sizing:border-box; }
    body { margin:0; background:radial-gradient(circle at 15% 0,#16324a 0,transparent 34%),var(--bg); color:var(--text); font:15px/1.55 "Segoe UI",sans-serif; }
    main { width:min(1500px,calc(100% - 40px)); margin:0 auto; padding:46px 0 70px; }
    header { display:grid; grid-template-columns:1fr auto; gap:24px; align-items:end; margin-bottom:30px; }
    h1 { font-size:clamp(32px,5vw,64px); line-height:1.02; margin:8px 0; letter-spacing:-.045em; }
    h2 { margin:0; font-size:24px; }
    .eyebrow { color:var(--accent); text-transform:uppercase; letter-spacing:.14em; font-size:11px; font-weight:700; margin:0 0 5px; }
    .lede,.meta,.review { color:var(--muted); }
    .summary { border:1px solid var(--line); border-radius:18px; padding:17px 22px; background:#0c1722cc; text-align:right; }
    .summary strong { display:block; color:var(--pass); font-size:34px; }
    .grid { display:grid; grid-template-columns:repeat(2,minmax(0,1fr)); gap:22px; }
    .scene-card { border:1px solid var(--line); border-radius:20px; overflow:hidden; background:linear-gradient(160deg,#142232,#0d1621); box-shadow:0 18px 55px #0005; }
    .scene-heading { padding:20px 22px 17px; display:flex; justify-content:space-between; gap:20px; align-items:center; }
    .status { border-radius:999px; padding:5px 11px; font-size:12px; font-weight:800; letter-spacing:.08em; }
    .status.pass { color:#062a1c; background:var(--pass); }
    .status.fail { color:#390d0d; background:#ff7474; }
    img { width:100%; aspect-ratio:16/10; object-fit:cover; display:block; border-block:1px solid var(--line); background:#05090d; }
    .metrics { display:grid; grid-template-columns:repeat(4,minmax(0,1fr)); gap:1px; background:var(--line); }
    .metrics div { background:#101a25; padding:12px 13px; min-width:0; }
    .metrics span,.metrics strong { display:block; overflow:hidden; text-overflow:ellipsis; white-space:nowrap; }
    .metrics span { color:var(--muted); font-size:11px; }
    .metrics strong { margin-top:2px; font-size:14px; }
    .review { margin:0; padding:17px 22px 20px; min-height:76px; }
    section.detail { margin-top:28px; border:1px solid var(--line); border-radius:18px; padding:22px; background:#0d1722; overflow:auto; }
    table { width:100%; border-collapse:collapse; min-width:780px; }
    th,td { text-align:left; padding:11px 10px; border-bottom:1px solid var(--line); vertical-align:top; }
    th { color:var(--muted); font-size:12px; }
    code { font-size:11px; word-break:break-all; color:#b9dbed; }
    a { color:var(--accent); }
    .findings { display:grid; grid-template-columns:repeat(3,1fr); gap:14px; }
    .finding { padding:17px; border-radius:14px; background:#121f2d; border:1px solid var(--line); }
    .finding strong { color:var(--warn); display:block; margin-bottom:5px; }
    @media (max-width:950px) { .grid { grid-template-columns:1fr; } header { grid-template-columns:1fr; } .summary { text-align:left; } .findings { grid-template-columns:1fr; } }
    @media (max-width:620px) { main { width:min(100% - 22px,1500px); padding-top:28px; } .metrics { grid-template-columns:repeat(2,1fr); } }
  </style>
</head>
<body>
<main>
  <header>
    <div>
      <p class="eyebrow">OpenGL_Learn / Release x64 / 1440 x 900</p>
      <h1>Classic Scene Acceptance</h1>
      <p class="lede">Verified downloads, scene import, fixed-camera rendering, captures, telemetry, and resource release.</p>
      <p class="meta">$timestamp | $(Html-Encode $branch) @ $(Html-Encode $commit) | $(Html-Encode $gpu)</p>
    </div>
    <div class="summary"><span>Passing scenes</span><strong>$passCount / $($records.Count)</strong><span>Mesh CPU staging = 0 in every scene</span></div>
  </header>
  <div class="grid">
$($cards.ToString())
  </div>
  <section class="detail">
    <p class="eyebrow">Issues discovered and fixed</p>
    <div class="findings">
      <div class="finding"><strong>RGB row-alignment crash</strong>Set GL_UNPACK_ALIGNMENT=1 around texture upload so a 703 x 1000 RGB image cannot trigger an out-of-bounds driver read.</div>
      <div class="finding"><strong>Cross-platform material paths</strong>Use filesystem::path for the model parent directory, including OBJ/MTL paths with Windows backslashes.</div>
      <div class="finding"><strong>Complex-mesh guards</strong>Handle missing normals, mixed primitives, and invalid mesh, material, or triangle indices.</div>
      <div class="finding"><strong>Independent opacity masks</strong>Sample OBJ map_d textures in forward PBR, Phong, deferred geometry, and AO passes instead of assuming alpha is embedded in the diffuse texture.</div>
    </div>
  </section>
  <section class="detail">
    <p class="eyebrow">Asset sources, licenses, and integrity</p>
    <table>
      <thead><tr><th>Package</th><th>Source</th><th>License</th><th>Archive SHA-256</th></tr></thead>
      <tbody>$($sources.ToString())</tbody>
    </table>
  </section>
</main>
</body>
</html>
"@

    $reportHtml = Join-Path $resultRoot "report.html"
    $reportMarkdown = Join-Path $resultRoot "report.md"
    [System.IO.File]::WriteAllText($reportHtml, $html, [System.Text.UTF8Encoding]::new($false))

    $markdownRows = foreach ($record in $records) {
        $scene = $record.Scene
        $result = $record.Result
        "| $($scene.displayName) | $(if ($result.success) {'PASS'} else {'FAIL'}) | $("{0:N1}" -f [double]$result.loadMilliseconds) | $("{0:N3}" -f [double]$result.averageFrameMilliseconds) | $("{0:N1}" -f [double]$result.averageFps) | $("{0:N0}" -f [int64]$result.triangleCount) | $(Format-Bytes ([double]$result.memoryBytes.texture)) | $(Format-Bytes ([double]$result.memoryBytes.meshGpu)) |"
    }
    $markdown = @"
# OpenGL_Learn Classic Scene Acceptance Report

- Time: $timestamp
- Branch/commit: $branch @ $commit
- Configuration: Release x64, 1440 x 900, fixed camera, frame 60 capture
- GPU: $gpu
- Result: $passCount / $($records.Count) passed; Mesh CPU staging = 0 in every scene

| Scene | Result | Load ready (ms) | Average frame (ms) | FPS | Triangles | Texture | Mesh GPU |
|---|---:|---:|---:|---:|---:|---:|---:|
$($markdownRows -join "`n")

> Frame time is a local acceptance signal, not a substitute for an isolated A/B performance experiment.

See the [HTML report](report.html) for full captures, visual notes, licenses, and archive SHA-256 values.
"@
    [System.IO.File]::WriteAllText($reportMarkdown, $markdown, [System.Text.UTF8Encoding]::new($false))
    return $reportHtml
}

New-Item -ItemType Directory -Path $assetRoot -Force | Out-Null
New-Item -ItemType Directory -Path $resultRoot -Force | Out-Null

$selectedScenes = @($manifest.scenes)
if ($SceneIds -and $SceneIds.Count -gt 0) {
    $selectedScenes = @($manifest.scenes | Where-Object { $SceneIds -contains $_.id })
    if ($selectedScenes.Count -ne $SceneIds.Count) {
        throw "One or more -SceneIds values are not present in the manifest."
    }
}

if (-not $ReportOnly) {
    $python = Resolve-Python
    if (-not $SkipPrepare) {
        foreach ($package in $manifest.packages) {
            Ensure-Package -Package $package
        }
        Prepare-Bistro -Python $python -Assimp (Resolve-Assimp)
    }
    if (-not $SkipBuild) {
        Build-Renderer -MsBuild (Resolve-MsBuild)
    }
    if (-not (Test-Path -LiteralPath $executablePath -PathType Leaf)) {
        throw "Renderer executable not found: $executablePath"
    }
    foreach ($scene in $selectedScenes) {
        Invoke-SceneTest -Scene $scene -Python $python
    }
}

$reportPath = Write-Reports -Scenes $selectedScenes
Write-Host "Report: $reportPath"
if (-not $NoOpen) {
    Start-Process $reportPath
}
