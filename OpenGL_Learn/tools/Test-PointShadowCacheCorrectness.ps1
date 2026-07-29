[CmdletBinding()]
param(
    [switch]$SkipBuild,
    [string]$MsBuildPath,
    [string]$ExecutablePath,
    [string]$PythonPath,
    [switch]$SkipReport,
    [switch]$AllowDirtySource,
    [string]$BatchId = "point-shadow-cache-correctness-final",
    [ValidateRange(64, 16384)]
    [int]$Width = 1920,
    [ValidateRange(64, 16384)]
    [int]$Height = 1080,
    [ValidateRange(1, 300)]
    [int]$InternalWarmupFrames = 12,
    [ValidateRange(4, 300)]
    [int]$MeasuredFrames = 12,
    [ValidateSet("sponza", "san-miguel")]
    [string[]]$SceneIds = @("sponza", "san-miguel"),
    [string]$ReportOutputDirectory,
    [string]$ReportPath
)

$ErrorActionPreference = "Stop"
$ProgressPreference = "SilentlyContinue"

if ($BatchId -notmatch "^[A-Za-z0-9][A-Za-z0-9._-]*$") {
    throw "BatchId may contain only letters, numbers, dot, underscore, and dash."
}

$runner = Join-Path $PSScriptRoot "Test-ShadowOptimizations.ps1"
$projectDirectory = Split-Path -Parent $PSScriptRoot
$repositoryDirectory = Split-Path -Parent $projectDirectory
$resultRoot = Join-Path (
    $projectDirectory
) "benchmark-results\shadow-optimizations"
$batchRoot = Join-Path $resultRoot $BatchId
$solutionPath = Join-Path $repositoryDirectory "OpenGL_Learn.sln"
$defaultExecutablePath =
    Join-Path $repositoryDirectory "x64\Release\OpenGL_Learn.exe"

function Resolve-MsBuild {
    $candidates = @(
        $MsBuildPath,
        (
            "C:\Program Files\Microsoft Visual Studio\2022\Community\" +
            "MSBuild\Current\Bin\MSBuild.exe"
        ),
        (
            "C:\Program Files\Microsoft Visual Studio\2022\Professional\" +
            "MSBuild\Current\Bin\MSBuild.exe"
        ),
        (
            "C:\Program Files\Microsoft Visual Studio\2022\Enterprise\" +
            "MSBuild\Current\Bin\MSBuild.exe"
        ),
        (
            "C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\" +
            "MSBuild\Current\Bin\MSBuild.exe"
        )
    )
    foreach ($candidate in $candidates) {
        if ($candidate -and
            (Test-Path -LiteralPath $candidate -PathType Leaf)) {
            return (Resolve-Path -LiteralPath $candidate).Path
        }
    }
    throw "MSBuild 2022 was not found."
}

function Resolve-ReportPython {
    $userProfileDirectory = [Environment]::GetFolderPath("UserProfile")
    $candidates = @(
        $PythonPath,
        (Join-Path $userProfileDirectory (
            "Anaconda3\envs\pytorch_cuda\python.exe"
        ))
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
        & $candidate -c "import matplotlib; import PIL" *> $null
        if ($LASTEXITCODE -eq 0) {
            return $candidate
        }
    }
    throw "Python with matplotlib and Pillow was not found."
}

if (-not $SkipBuild) {
    $msbuild = Resolve-MsBuild
    & $msbuild $solutionPath `
        /m /p:Configuration=Release /p:Platform=x64 /verbosity:minimal
    if ($LASTEXITCODE -ne 0) {
        throw "Release x64 build failed."
    }
}

$executable = if ($ExecutablePath) {
    (Resolve-Path -LiteralPath $ExecutablePath).Path
}
else {
    (Resolve-Path -LiteralPath $defaultExecutablePath).Path
}
if (-not (Test-Path -LiteralPath $executable -PathType Leaf)) {
    throw "Renderer executable not found: $executable"
}

$oracleEnvironment = @{
    OPENGL_LEARN_POINT_SHADOW_RENDER_PATH = "six-face"
    OPENGL_LEARN_POINT_SHADOW_FACE_CULLING = "1"
    OPENGL_LEARN_SHADOW_CACHE = "revision"
    OPENGL_LEARN_SHADOW_PER_LIGHT_CACHE = "1"
    OPENGL_LEARN_SHADOW_SPATIAL_CASTER_CACHE = "0"
    OPENGL_LEARN_POINT_SHADOW_PER_FACE_CACHE = "0"
    OPENGL_LEARN_POINT_SHADOW_FORCE_ALL_REQUIRED = "0"
    OPENGL_LEARN_SHADOW_SPOT_CASTER_DEPTH_FIT = "0"
}
$candidateEnvironment = $oracleEnvironment.Clone()
$candidateEnvironment.OPENGL_LEARN_SHADOW_SPATIAL_CASTER_CACHE = "1"
$candidateEnvironment.OPENGL_LEARN_POINT_SHADOW_PER_FACE_CACHE = "1"

$requestedCases = @(
    [ordered]@{
        id = "deferred-face-required"
        displayName = "Deferred Face becomes Required"
        workload = "deferred-face-required"
        scenes = @("sponza")
        frames = 4
    },
    [ordered]@{
        id = "point-move"
        displayName = "Point Light move"
        workload = "move-point"
        scenes = $SceneIds
        frames = $MeasuredFrames
    },
    [ordered]@{
        id = "local-caster"
        displayName = "Local Caster move"
        workload = "move-local-caster"
        scenes = $SceneIds
        frames = $MeasuredFrames
    },
    [ordered]@{
        id = "fbo-resize"
        displayName = "Point Shadow FBO Resize"
        workload = "resize-point-shadow"
        scenes = $SceneIds
        frames = $MeasuredFrames
    },
    [ordered]@{
        id = "fbo-replace"
        displayName = "Point Shadow FBO Replace"
        workload = "replace-point-shadow-target"
        scenes = $SceneIds
        frames = $MeasuredFrames
    },
    [ordered]@{
        id = "shader-reload"
        displayName = "Point Shadow Shader Reload"
        workload = "reload-shadow-point"
        scenes = $SceneIds
        frames = $MeasuredFrames
    }
)

New-Item -ItemType Directory -Path $batchRoot -Force | Out-Null
$caseResults = @()
foreach ($case in $requestedCases) {
    $experimentId = "$BatchId-$($case.id)"
    $arguments = @{
        SkipBuild = $true
        SkipExternalWarmup = $true
        ExecutablePath = $executable
        ExperimentId = $experimentId
        VariantALabel = "B-six-face-oracle"
        VariantBLabel = "C-per-face"
        VariantAEnvironment = $oracleEnvironment
        VariantBEnvironment = $candidateEnvironment
        Width = $Width
        Height = $Height
        MeasuredFrames = [int]$case.frames
        InternalWarmupFrames = $InternalWarmupFrames
        FormalRunsPerVariant = 1
        MaximumPixelChannelDelta = 0
        MaximumChangedPixels = 0
        Workload = [string]$case.workload
        Lights = "point"
        Mode = "hard"
        Sampling = "stable"
        RenderPath = "pbr-forward"
        SceneIds = @($case.scenes)
        TimelineFps = 60
        TimelineCycleFrames = [int]$case.frames
    }
    Write-Host "Correctness: $($case.displayName)"
    & $runner @arguments

    $experimentRoot = Join-Path $resultRoot $experimentId
    $summary =
        Get-Content -LiteralPath (
            Join-Path $experimentRoot "summary.json"
        ) -Raw |
            ConvertFrom-Json
    foreach ($scene in @($summary.scenes)) {
        if (@(
                $scene.correctness.captureComparisons |
                    Where-Object { -not [bool]$_.exact }
            ).Count -gt 0) {
            throw "$($case.displayName) $($scene.displayName) screenshot differs."
        }
        if (@(
                $scene.correctness.pointShadowCubeComparisons |
                    Where-Object { -not [bool]$_.exact }
            ).Count -gt 0) {
            throw "$($case.displayName) $($scene.displayName) cubemap hash differs."
        }
        foreach ($resource in @(
            "texture", "meshCpu", "meshGpu", "renderTarget"
        )) {
            if (-not [bool]$scene.correctness.rendererOwnedResources.$resource.exact) {
                throw "$($case.displayName) $($scene.displayName) $resource differs."
            }
        }
    }

    if ($case.id -eq "deferred-face-required") {
        $candidatePath = Join-Path (
            Join-Path $experimentRoot "formal\sponza"
        ) "B1.json"
        $candidate =
            Get-Content -LiteralPath $candidatePath -Raw |
                ConvertFrom-Json
        $samples = @($candidate.motionTimeline.samples)
        if ($samples.Count -lt 2 -or
            [int64]$samples[0].shadow.pointShadowDeferredFaceCount -le 0) {
            throw "Deferred Face test did not create a deferred stale face."
        }
        $initialDeferredMask =
            0x3f -band (-bnot [int]$samples[0].shadow.pointShadowRequiredFaceMask)
        $materialized = $false
        foreach ($sample in $samples | Select-Object -Skip 1) {
            $requiredAndUpdated =
                [int]$sample.shadow.pointShadowRequiredFaceMask -band
                [int]$sample.shadow.pointShadowUpdateFaceMask -band
                $initialDeferredMask
            if ($requiredAndUpdated -ne 0) {
                $materialized = $true
                break
            }
        }
        if (-not $materialized) {
            throw "Deferred stale face was not rebuilt when it became required."
        }
    }

    $caseResults += [ordered]@{
        id = $case.id
        displayName = $case.displayName
        workload = $case.workload
        experimentId = $experimentId
        scenes = @($case.scenes)
        measuredFrames = [int]$case.frames
        summary = (Join-Path $experimentRoot "summary.json")
    }
}

function Invoke-TopologyAbaSmoke {
    $classicManifest =
        Get-Content -LiteralPath (
            Join-Path $projectDirectory "classic-scenes.manifest.json"
        ) -Raw |
            ConvertFrom-Json
    $scene = @(
        $classicManifest.scenes |
            Where-Object { $_.id -eq "sponza" }
    )[0]
    $abaRoot = Join-Path $batchRoot "topology-aba"
    New-Item -ItemType Directory -Path $abaRoot -Force | Out-Null
    $projectPrefix =
        [System.IO.Path]::GetFullPath($projectDirectory).TrimEnd("\", "/") +
        "\"
    $absoluteAbaRoot = [System.IO.Path]::GetFullPath($abaRoot)
    if (-not $absoluteAbaRoot.StartsWith(
            $projectPrefix,
            [System.StringComparison]::OrdinalIgnoreCase
        )) {
        throw "ABA result directory is outside the project directory."
    }
    $relativeRoot =
        $absoluteAbaRoot.Substring($projectPrefix.Length).Replace("\", "/")
    $capturePath = "$relativeRoot/topology-aba.ppm"
    $resultPath = "$relativeRoot/topology-aba.json"
    $logPath = Join-Path $abaRoot "topology-aba.log"
    $captureFrame = $InternalWarmupFrames + 4
    $arguments = @(
        "--classic-scene-test",
            ("classic-scenes/" + ([string]$scene.modelPath).Replace("\", "/")),
        "--classic-scene-name", "sponza-topology-aba",
        "--classic-scene-capture", $capturePath,
        "--classic-scene-result", $resultPath,
        "--classic-scene-camera",
            [string]$scene.camera[0],
            [string]$scene.camera[1],
            [string]$scene.camera[2],
        "--classic-scene-target",
            [string]$scene.target[0],
            [string]$scene.target[1],
            [string]$scene.target[2],
        "--classic-scene-up",
            [string]$scene.up[0],
            [string]$scene.up[1],
            [string]$scene.up[2],
        "--classic-scene-directional-light", "-0.45", "-1.0", "-0.25",
        "--classic-scene-radius", [string]$scene.normalizedRadius,
        "--classic-scene-world-scale", "1.0",
        "--classic-scene-fov", [string]$scene.fov,
        "--classic-scene-width", [string]$Width,
        "--classic-scene-height", [string]$Height,
        "--classic-scene-warmup-frames", [string]$InternalWarmupFrames,
        "--classic-scene-capture-frame", [string]$captureFrame,
        "--classic-scene-timeline-fps", "60",
        "--classic-scene-timeline-cycle-frames", "4",
        "--classic-scene-shadow-mode", "hard",
        "--classic-scene-shadow-sampling", "stable",
        "--classic-scene-shadow-lights", "point",
        "--classic-scene-shadow-workload", "replace-model-aba",
        "--classic-scene-shadow-variant", "C-topology-aba",
        "--classic-scene-shadow-resolution", "0",
        "--classic-scene-render-path", "pbr-forward"
    )

    $savedEnvironment = @{}
    foreach ($name in $candidateEnvironment.Keys) {
        $savedEnvironment[$name] =
            [Environment]::GetEnvironmentVariable(
                $name,
                [EnvironmentVariableTarget]::Process
            )
        [Environment]::SetEnvironmentVariable(
            $name,
            [string]$candidateEnvironment[$name],
            [EnvironmentVariableTarget]::Process
        )
    }
    Push-Location $projectDirectory
    try {
        & $executable @arguments *> $logPath
        $exitCode = $LASTEXITCODE
    }
    finally {
        Pop-Location
        foreach ($name in $candidateEnvironment.Keys) {
            [Environment]::SetEnvironmentVariable(
                $name,
                $savedEnvironment[$name],
                [EnvironmentVariableTarget]::Process
            )
        }
    }
    if ($exitCode -ne 0) {
        throw "Topology ABA smoke exited with $exitCode. See $logPath"
    }
    $absoluteResult = Join-Path $projectDirectory $resultPath
    $result =
        Get-Content -LiteralPath $absoluteResult -Raw |
            ConvertFrom-Json
    if (-not [bool]$result.success -or
        [int64]$result.shadow.measuredSceneTopologyInvalidationCount -ne 1 -or
        [int64]$result.shadow.sceneTopologyRevision -
            [int64]$result.shadow.measurementStartSceneTopologyRevision -ne 1 -or
        [int64]$result.shadow.sceneTopologyModelCount -ne
            [int64]$result.shadow.measurementStartSceneTopologyModelCount -or
        [int64]$result.shadow.measuredPointLightUpdateCount -lt 1) {
        throw "Topology ABA smoke invariants failed."
    }
    return [ordered]@{
        id = "topology-aba"
        displayName = "SceneTopologyRevision same-slot replacement smoke"
        scene = "sponza"
        measuredFrames = 4
        result = $absoluteResult
        capture = (Join-Path $projectDirectory $capturePath)
        log = $logPath
        revisionDelta = 1
        invalidationCount = 1
        modelCountBefore =
            [int64]$result.shadow.measurementStartSceneTopologyModelCount
        modelCountAfter =
            [int64]$result.shadow.sceneTopologyModelCount
        pointUpdates =
            [int64]$result.shadow.measuredPointLightUpdateCount
    }
}

$abaSmoke = Invoke-TopologyAbaSmoke
$gitHead = (& git -C $repositoryDirectory rev-parse HEAD).Trim()
$gitDirty = @(& git -C $repositoryDirectory status --porcelain).Count -gt 0
$executableHash = (
    Get-FileHash -LiteralPath $executable -Algorithm SHA256
).Hash.ToLowerInvariant()
if (-not $AllowDirtySource -and $gitDirty) {
    throw (
        "Correctness audit requires gitDirty=false. " +
        "Run it from the clean frozen worktree."
    )
}

$manifest = [ordered]@{
    schemaVersion = 1
    batchId = $BatchId
    createdUtc = [DateTime]::UtcNow.ToString("o")
    resolution = @($Width, $Height)
    internalWarmupFrames = $InternalWarmupFrames
    mode = "hard"
    pixelThreshold = [ordered]@{
        maximumChannelDelta = 0
        maximumChangedPixels = 0
    }
    variants = [ordered]@{
        B = "six-face-oracle"
        C = "per-light-point-per-face"
    }
    cases = $caseResults
    topologyAbaSmoke = $abaSmoke
    provenance = [ordered]@{
        gitHead = $gitHead
        gitDirty = $gitDirty
        executablePath = $executable
        executableSha256 = $executableHash
        configuration = "Release x64"
        requestedSwapInterval = 0
    }
}
$manifestPath = Join-Path $batchRoot "manifest.json"
$manifest |
    ConvertTo-Json -Depth 12 |
    Set-Content -LiteralPath $manifestPath -Encoding UTF8
Write-Host "Correctness manifest: $manifestPath"

if (-not $SkipReport) {
    $python = Resolve-ReportPython
    if (-not $ReportOutputDirectory) {
        $ReportOutputDirectory = Join-Path $projectDirectory (
            "docs\benchmark-images\shadow-optimizations\$BatchId"
        )
    }
    if (-not $ReportPath) {
        $ReportPath = Join-Path $projectDirectory (
            "POINT_SHADOW_CACHE_CORRECTNESS_AUDIT_CN.md"
        )
    }
    $generator = Join-Path (
        $PSScriptRoot
    ) "generate_point_shadow_cache_correctness_report.py"
    & $python $generator `
        --manifest $manifestPath `
        --output-dir $ReportOutputDirectory `
        --report $ReportPath
    if ($LASTEXITCODE -ne 0) {
        throw "Correctness report generation failed."
    }
}
