[CmdletBinding()]
param(
    [ValidateSet('Smoke', 'Formal')]
    [string]$Preset = 'Formal',
    [string]$BatchId = '',
    [switch]$SkipBuild
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest

function Write-Utf8Json {
    param(
        [Parameter(Mandatory = $true)]$Value,
        [Parameter(Mandatory = $true)][string]$Path,
        [int]$Depth = 16
    )
    $json = $Value | ConvertTo-Json -Depth $Depth
    $utf8 = New-Object System.Text.UTF8Encoding($false)
    [System.IO.File]::WriteAllText($Path, $json + [Environment]::NewLine, $utf8)
}

function Resolve-MSBuild {
    $candidates = @(
        'C:\Program Files\Microsoft Visual Studio\2022\Community\MSBuild\Current\Bin\MSBuild.exe',
        'C:\Program Files\Microsoft Visual Studio\2022\Professional\MSBuild\Current\Bin\MSBuild.exe',
        'C:\Program Files\Microsoft Visual Studio\2022\Enterprise\MSBuild\Current\Bin\MSBuild.exe',
        'C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\MSBuild\Current\Bin\MSBuild.exe'
    )
    foreach ($candidate in $candidates) {
        if (Test-Path -LiteralPath $candidate) {
            return $candidate
        }
    }
    throw 'MSBuild.exe was not found in a supported Visual Studio 2022 location.'
}

function Get-CameraSignature {
    param([Parameter(Mandatory = $true)]$Scene)
    $text = '{0}|{1}|{2}|{3}|{4}|{5}|{6}|{7}|{8}|{9}' -f `
        $Scene.camera[0], $Scene.camera[1], $Scene.camera[2], `
        $Scene.target[0], $Scene.target[1], $Scene.target[2], `
        $Scene.up[0], $Scene.up[1], $Scene.up[2], $Scene.fov
    $bytes = [System.Text.Encoding]::UTF8.GetBytes($text)
    $sha = [System.Security.Cryptography.SHA256]::Create()
    try {
        return ([System.BitConverter]::ToString($sha.ComputeHash($bytes))).Replace('-', '')
    }
    finally {
        $sha.Dispose()
    }
}

function ConvertTo-NativeArgument {
    param([AllowEmptyString()][Parameter(Mandatory = $true)][string]$Value)
    if ($Value.Length -gt 0 -and $Value -notmatch '[\s"]') {
        return $Value
    }
    $builder = New-Object System.Text.StringBuilder
    [void]$builder.Append('"')
    $backslashes = 0
    foreach ($character in $Value.ToCharArray()) {
        if ($character -eq [char]'\') {
            ++$backslashes
            continue
        }
        if ($character -eq [char]'"') {
            if ($backslashes -gt 0) {
                [void]$builder.Append([char]'\', 2 * $backslashes)
            }
            [void]$builder.Append([char]'\')
            [void]$builder.Append([char]'"')
            $backslashes = 0
            continue
        }
        if ($backslashes -gt 0) {
            [void]$builder.Append([char]'\', $backslashes)
            $backslashes = 0
        }
        [void]$builder.Append($character)
    }
    if ($backslashes -gt 0) {
        [void]$builder.Append([char]'\', 2 * $backslashes)
    }
    [void]$builder.Append('"')
    return $builder.ToString()
}

function Invoke-CapturedProcess {
    param(
        [Parameter(Mandatory = $true)][string]$FilePath,
        [Parameter(Mandatory = $true)][string[]]$ArgumentList,
        [Parameter(Mandatory = $true)][string]$LogBase
    )
    $stdoutPath = $LogBase + '.stdout.log'
    $stderrPath = $LogBase + '.stderr.log'
    $parent = Split-Path -Parent $stdoutPath
    [void](New-Item -ItemType Directory -Path $parent -Force)
    $started = Get-Date
    $startInfo = New-Object System.Diagnostics.ProcessStartInfo
    $startInfo.FileName = $FilePath
    $startInfo.Arguments = (($ArgumentList | ForEach-Object {
        ConvertTo-NativeArgument -Value $_
    }) -join ' ')
    $startInfo.WorkingDirectory = $projectRoot
    $startInfo.UseShellExecute = $false
    $startInfo.CreateNoWindow = $true
    $startInfo.RedirectStandardOutput = $true
    $startInfo.RedirectStandardError = $true
    $process = New-Object System.Diagnostics.Process
    $process.StartInfo = $startInfo
    if (-not $process.Start()) {
        throw "Failed to start process: $FilePath"
    }
    $stdoutTask = $process.StandardOutput.ReadToEndAsync()
    $stderrTask = $process.StandardError.ReadToEndAsync()
    [void]$process.WaitForExit()
    $stdout = $stdoutTask.GetAwaiter().GetResult()
    $stderr = $stderrTask.GetAwaiter().GetResult()
    $utf8 = New-Object System.Text.UTF8Encoding($false)
    [System.IO.File]::WriteAllText($stdoutPath, $stdout, $utf8)
    [System.IO.File]::WriteAllText($stderrPath, $stderr, $utf8)
    $exitCode = $process.ExitCode
    $process.Dispose()
    [pscustomobject]@{
        exitCode = $exitCode
        startedUtc = $started.ToUniversalTime().ToString('o')
        completedUtc = (Get-Date).ToUniversalTime().ToString('o')
        stdoutPath = $stdoutPath
        stderrPath = $stderrPath
    }
}

function Assert-ExecutableUnchanged {
    $current = (Get-FileHash -LiteralPath $exePath -Algorithm SHA256).Hash
    if ($current -ne $script:exeSha256) {
        throw "Executable SHA-256 changed during the batch: $current"
    }
    return $current
}

function Save-RunManifest {
    $manifest = [ordered]@{
        schemaVersion = 1
        batchId = $BatchId
        preset = $Preset
        executable = [ordered]@{
            path = $exePath
            sha256 = $script:exeSha256
        }
        sourceCheckpointSha256 = $script:sourceCheckpointSha256
        runs = @($script:runs)
    }
    Write-Utf8Json -Value $manifest -Path $runManifestPath -Depth 12
}

function Invoke-RendererRun {
    param(
        [Parameter(Mandatory = $true)][string]$Kind,
        [Parameter(Mandatory = $true)][string]$SceneName,
        [Parameter(Mandatory = $true)][string]$Condition,
        [Parameter(Mandatory = $true)][string]$Variant,
        [Parameter(Mandatory = $true)][string]$Mode,
        [Parameter(Mandatory = $true)][string[]]$Arguments,
        [Parameter(Mandatory = $true)][string]$LogBase,
        [string]$ResultPath = '',
        [int]$Pair = 0,
        [int]$Order = 0
    )
    $beforeHash = Assert-ExecutableUnchanged
    $sourceBefore = Assert-SourceCheckpointUnchanged
    Write-Host ("[{0}] {1}/{2} {3} pair={4} order={5}" -f `
        $Kind, $SceneName, $Condition, $Variant, $Pair, $Order)
    $process = Invoke-CapturedProcess `
        -FilePath $exePath `
        -ArgumentList $Arguments `
        -LogBase $LogBase
    $afterHash = Assert-ExecutableUnchanged
    $sourceAfter = Assert-SourceCheckpointUnchanged
    $entry = [ordered]@{
        kind = $Kind
        scene = $SceneName
        condition = $Condition
        variant = $Variant
        mode = $Mode
        pair = $Pair
        order = $Order
        resultPath = $ResultPath
        exeSha256Before = $beforeHash
        exeSha256After = $afterHash
        sourceCheckpointSha256Before = $sourceBefore
        sourceCheckpointSha256After = $sourceAfter
        exitCode = $process.exitCode
        startedUtc = $process.startedUtc
        completedUtc = $process.completedUtc
        stdoutPath = $process.stdoutPath
        stderrPath = $process.stderrPath
    }
    [void]$script:runs.Add([pscustomobject]$entry)
    Save-RunManifest
    if ($process.exitCode -ne 0) {
        throw "Renderer run failed with exit code $($process.exitCode). See $($process.stderrPath)"
    }
    if ($ResultPath -and -not (Test-Path -LiteralPath $ResultPath)) {
        throw "Renderer did not create the expected result: $ResultPath"
    }
}

function Get-ClassicArguments {
    param(
        [Parameter(Mandatory = $true)]$Scene,
        [Parameter(Mandatory = $true)][string]$Mode,
        [Parameter(Mandatory = $true)][string]$Condition,
        [Parameter(Mandatory = $true)][int]$Width,
        [Parameter(Mandatory = $true)][int]$Height,
        [Parameter(Mandatory = $true)][int]$Warmup,
        [Parameter(Mandatory = $true)][int]$Measured,
        [Parameter(Mandatory = $true)][string]$ResultPath,
        [string]$SceneLabel = ''
    )
    if (-not $SceneLabel) {
        $SceneLabel = $Scene.name
    }
    $captureFrame = $Warmup + $Measured
    $arguments = [System.Collections.Generic.List[string]]::new()
    foreach ($value in @(
        '--gbuffer-position', $Mode,
        '--classic-scene-test', $Scene.model,
        '--classic-scene-name', $SceneLabel,
        '--classic-scene-render-path', 'pbr-deferred',
        '--classic-scene-camera', [string]$Scene.camera[0], [string]$Scene.camera[1], [string]$Scene.camera[2],
        '--classic-scene-target', [string]$Scene.target[0], [string]$Scene.target[1], [string]$Scene.target[2],
        '--classic-scene-up', [string]$Scene.up[0], [string]$Scene.up[1], [string]$Scene.up[2],
        '--classic-scene-radius', [string]$Scene.radius,
        '--classic-scene-fov', [string]$Scene.fov,
        '--classic-scene-width', [string]$Width,
        '--classic-scene-height', [string]$Height,
        '--classic-scene-warmup-frames', [string]$Warmup,
        '--classic-scene-capture-frame', [string]$captureFrame,
        '--classic-scene-result', $ResultPath
    )) {
        [void]$arguments.Add($value)
    }
    if ($Condition -eq 'ssao-on') {
        foreach ($value in @(
            '--classic-scene-ssao-mode', 'legacy-full',
            '--classic-scene-ssao-samples', '64'
        )) {
            [void]$arguments.Add($value)
        }
    }
    # Prevent PowerShell from unrolling the mutable List into a fixed Object[].
    return ,$arguments
}

$projectRoot = Split-Path -Parent $PSScriptRoot
$repoRoot = Split-Path -Parent $projectRoot
$solutionPath = Join-Path $repoRoot 'OpenGL_Learn.sln'
$exePath = Join-Path $repoRoot 'x64\Release\OpenGL_Learn.exe'
if (-not $BatchId) {
    $BatchId = 'gbuffer-position-{0}-{1}' -f `
        $Preset.ToLowerInvariant(), (Get-Date -Format 'yyyyMMdd-HHmmss')
}
if ($BatchId -notmatch '^[A-Za-z0-9._-]+$') {
    throw 'BatchId may contain only letters, numbers, dot, underscore, and hyphen.'
}
$resultRoot = Join-Path $projectRoot `
    ('benchmark-results\gbuffer-position-reconstruction\' + $BatchId)
if (Test-Path -LiteralPath $resultRoot) {
    throw "Refusing to mix with an existing result directory: $resultRoot"
}
[void](New-Item -ItemType Directory -Path $resultRoot)
$runManifestPath = Join-Path $resultRoot 'run-manifest.json'
$script:runs = [System.Collections.ArrayList]::new()

if (-not $SkipBuild) {
    $msbuild = Resolve-MSBuild
    $buildLog = Join-Path $resultRoot 'build'
    $build = Invoke-CapturedProcess `
        -FilePath $msbuild `
        -ArgumentList @(
            $solutionPath,
            '/m:1',
            '/t:Build',
            '/p:Configuration=Release',
            '/p:Platform=x64',
            '/nologo',
            '/v:minimal'
        ) `
        -LogBase $buildLog
    if ($build.exitCode -ne 0) {
        throw "Release x64 build failed. See $($build.stderrPath)"
    }
}
if (-not (Test-Path -LiteralPath $exePath)) {
    throw "Release executable is missing: $exePath"
}
$script:exeSha256 = (Get-FileHash -LiteralPath $exePath -Algorithm SHA256).Hash
$exeInfo = Get-Item -LiteralPath $exePath

$scenes = @(
    [pscustomobject]@{
        name = 'sponza'
        model = 'classic-scenes/sponza/sponza.obj'
        camera = @(-6.0, -1.5, 0.0)
        target = @(6.0, -0.8, 0.0)
        up = @(0.0, 1.0, 0.0)
        radius = 15.0
        fov = 55.0
    },
    [pscustomobject]@{
        name = 'san-miguel'
        model = 'classic-scenes/san-miguel/san-miguel-low-poly.obj'
        camera = @(1.5, -1.2, 3.5)
        target = @(1.5, -0.7, -1.0)
        up = @(0.0, 1.0, 0.0)
        radius = 15.0
        fov = 60.0
    }
)
foreach ($scene in $scenes) {
    if (-not (Test-Path -LiteralPath (Join-Path $projectRoot $scene.model))) {
        throw "Scene is missing: $($scene.model)"
    }
    Add-Member -InputObject $scene -NotePropertyName cameraSignature `
        -NotePropertyValue (Get-CameraSignature -Scene $scene)
}

if ($Preset -eq 'Formal') {
    $width = 1920
    $height = 1080
    $warmupFrames = 300
    $measuredFrames = 2000
    $qualityWarmupFrames = 60
    $performanceSequence = @(
        @{ variant = 'A'; mode = 'explicit'; pair = 1 },
        @{ variant = 'B'; mode = 'reconstruct'; pair = 1 },
        @{ variant = 'B'; mode = 'reconstruct'; pair = 2 },
        @{ variant = 'A'; mode = 'explicit'; pair = 2 },
        @{ variant = 'A'; mode = 'explicit'; pair = 3 },
        @{ variant = 'B'; mode = 'reconstruct'; pair = 3 }
    )
}
else {
    $width = 640
    $height = 360
    $warmupFrames = 20
    $measuredFrames = 40
    $qualityWarmupFrames = 20
    $performanceSequence = @(
        @{ variant = 'A'; mode = 'explicit'; pair = 1 },
        @{ variant = 'B'; mode = 'reconstruct'; pair = 1 }
    )
}

$head = (& git -C $repoRoot rev-parse HEAD).Trim()
$sourceFiles = @(
    'OpenGL_Learn/Global.h',
    'OpenGL_Learn/FramebufferManager.cpp',
    'OpenGL_Learn/ShaderManager.h',
    'OpenGL_Learn/ShaderManager.cpp',
    'OpenGL_Learn/DeferRenderPass.h',
    'OpenGL_Learn/DeferRenderPass.cpp',
    'OpenGL_Learn/SSAORenderPass.cpp',
    'OpenGL_Learn/Material.h',
    'OpenGL_Learn/Model.cpp',
    'OpenGL_Learn/Scene.cpp',
    'OpenGL_Learn/Profiler.h',
    'OpenGL_Learn/test.cpp',
    'OpenGL_Learn/OpenGL_Learn.vcxproj',
    'OpenGL_Learn/shaders/deferProcessReconstructFragment.glsl',
    'OpenGL_Learn/shaders/positionReconstruction.glsl',
    'OpenGL_Learn/shaders/deferFragment.glsl',
    'OpenGL_Learn/shaders/deferDirLightVolumeFragment.glsl',
    'OpenGL_Learn/shaders/lightVolumeFragment.glsl',
    'OpenGL_Learn/shaders/lightVolumeFullscreenFragment.glsl',
    'OpenGL_Learn/shaders/ssaoFragment.glsl',
    'OpenGL_Learn/shaders/ssaoUpsampleFragment.glsl',
    'OpenGL_Learn/tools/run_gbuffer_position_reconstruction.ps1',
    'OpenGL_Learn/tools/analyze_gbuffer_position_reconstruction.py'
)
$checkpointFiles = @()
$script:sourceHashByPath = @{}
foreach ($relative in $sourceFiles) {
    $absolute = Join-Path $repoRoot $relative
    if (Test-Path -LiteralPath $absolute) {
        $fileHash = (Get-FileHash -LiteralPath $absolute -Algorithm SHA256).Hash
        $script:sourceHashByPath[$relative] = $fileHash
        $checkpointFiles += [ordered]@{
            path = $relative
            sha256 = $fileHash
            bytes = (Get-Item -LiteralPath $absolute).Length
        }
    }
}
$checkpoint = [ordered]@{
    gitHead = $head
    dirty = $true
    note = 'Selected-file hashes identify the tested dirty-worktree source without modifying or committing user changes.'
    files = $checkpointFiles
}
$checkpointPath = Join-Path $resultRoot 'source-checkpoint.json'
Write-Utf8Json -Value $checkpoint -Path $checkpointPath -Depth 8
$script:sourceCheckpointSha256 = `
    (Get-FileHash -LiteralPath $checkpointPath -Algorithm SHA256).Hash

function Assert-SourceCheckpointUnchanged {
    foreach ($relative in $sourceFiles) {
        $absolute = Join-Path $repoRoot $relative
        if (-not (Test-Path -LiteralPath $absolute)) {
            throw "Checkpoint source disappeared during the batch: $relative"
        }
        $current = (Get-FileHash -LiteralPath $absolute -Algorithm SHA256).Hash
        if ($current -ne $script:sourceHashByPath[$relative]) {
            throw "Checkpoint source changed during the batch: $relative"
        }
    }
    return $script:sourceCheckpointSha256
}

$metadata = [ordered]@{
    schemaVersion = 1
    experiment = 'explicit-gPosition-vs-depth-position-reconstruction'
    preregisteredBeforeData = $true
    createdUtc = (Get-Date).ToUniversalTime().ToString('o')
    preset = $Preset
    batchId = $BatchId
    invariant = [ordered]@{
        executablePath = $exePath
        executableSha256 = $script:exeSha256
        executableBytes = $exeInfo.Length
        sourceCheckpointSha256 = $script:sourceCheckpointSha256
        gitHead = $head
        build = 'Release x64'
        resolution = @($width, $height)
        swapInterval = 0
        renderPath = 'pbr-deferred'
        shadows = 'off'
        bloom = $false
        autoReloadShaders = $false
        autoReloadMaterials = $false
        ssaoKernelSizeWhenOn = 64
        ssaoModeWhenOn = 'legacy-full'
        ssaoRadius = 0.35
        ssaoBias = 0.025
    }
    protocol = [ordered]@{
        warmupFramesPerProcess = $warmupFrames
        measuredFramesPerProcess = $measuredFrames
        independentProcessesPerVariantSceneCondition = `
            $(if ($Preset -eq 'Formal') { 3 } else { 1 })
        balancedOrder = @($performanceSequence | ForEach-Object { $_.variant })
        conditions = @('ssao-off', 'ssao-on')
        conditionPooling = 'forbidden; every scene x SSAO condition is evaluated independently'
        performanceReadback = 'disabled'
        qualityCapture = 'separate-processes-after-performance-interval'
        requiredGpuZones = @(
            'GPU Frame',
            'G-Buffer Geometry',
            'Depth/Stencil Copy',
            'SSAO Generate',
            'SSAO Upsample when half-bilateral validation is active',
            'Deferred Lighting',
            'Deferred Pass'
        )
        requiredCpuZones = @(
            'CPU Frame',
            'G-Buffer Geometry',
            'Depth/Stencil Copy',
            'SSAO Generate',
            'SSAO Upsample when half-bilateral validation is active',
            'Deferred Lighting',
            'Deferred Pass'
        )
    }
    variants = [ordered]@{
        A = [ordered]@{
            cli = '--gbuffer-position explicit'
            colorMrtCount = 5
            attachments = @('position RGBA16F', 'normal RGB16F', 'albedo RGB8', 'material RGBA16F', 'emissive RGB16F')
            depthStencil = 'D24S8 renderbuffer'
            logicalBytesPerPixel = 35
        }
        B = [ordered]@{
            cli = '--gbuffer-position reconstruct'
            colorMrtCount = 4
            attachments = @('normal RGB16F', 'albedo RGB8', 'material RGBA16F', 'emissive RGB16F')
            depthStencil = 'sampleable D24S8 texture (shared depth and stencil storage)'
            logicalBytesPerPixel = 27
            reconstruction = [ordered]@{
                ndcZ = 'OpenGL [-1,1] from window depth [0,1]'
                background = 'clear depth 1.0 is invalid'
                ssaoSpace = 'view; four depth texelFetch plus up to four inverseProjection mat4xvec4/divides and bilinear mix to preserve control GL_LINEAR guide semantics'
                halfBilateralGuide = 'view depth; four depth texelFetch/reconstructions per guide sample before bilinear interpolation'
                lightingSpace = 'world; inverseProjection plus inverseView mat4xvec4 and divide'
                inverses = 'computed on CPU once per pass, never inverse() in fragment shader'
            }
        }
    }
    deterministicMemoryExpectation = [ordered]@{
        bytesSavedPerPixel = 8
        bytesSavedAtResolution = [int64]$width * [int64]$height * 8L
        mibSavedAtResolution = ([double]$width * [double]$height * 8.0 / 1MB)
    }
    cameras = @($scenes | ForEach-Object {
        [ordered]@{
            scene = $_.name
            model = $_.model
            position = $_.camera
            target = $_.target
            up = $_.up
            radius = $_.radius
            fov = $_.fov
            signature = $_.cameraSignature
        }
    })
    qualityThresholds = [ordered]@{
        ldrNormalizedChannelMaeMax = 0.0005
        ldrNormalizedChannelP95Max = (1.0 / 255.0)
        ldrNormalizedChannelP99Max = (2.0 / 255.0)
        ldrNormalizedChannelAbsoluteMax = (8.0 / 255.0)
        ldrGlobalSsimMin = 0.999
        aoMaeMax = 0.002
        aoP95Max = 0.005
        aoP99Max = 0.010
        aoAbsoluteMax = 0.050
        worldPositionMaeMax = 0.010
        worldPositionP95Max = 0.030
        worldPositionAbsoluteMax = 0.250
        viewPositionMaeMax = 0.010
        viewPositionP95Max = 0.030
        viewPositionAbsoluteMax = 0.250
        depthBucketStrategy = 'foreground view-depth tertiles from the control capture; thresholds are fixed before data'
        depthBuckets = @(
            @{ name = 'near'; quantileMinimum = 0.0; quantileMaximum = (1.0 / 3.0); p95Max = 0.010 },
            @{ name = 'mid'; quantileMinimum = (1.0 / 3.0); quantileMaximum = (2.0 / 3.0); p95Max = 0.030 },
            @{ name = 'far'; quantileMinimum = (2.0 / 3.0); quantileMaximum = 1.0; p95Max = 0.100 }
        )
        backgroundMaskMismatchPixelsMax = 0
        unchangedAttachmentHashesRequired = $true
    }
    performanceDecision = [ordered]@{
        primaryMetric = 'paired independent-process GPU Frame median and P95, evaluated per scene x SSAO condition without pooling'
        perSceneConditionMedianDeltaMillisecondsMaxForGo = -0.050
        perSceneConditionMedianDeltaPercentMaxForGo = -1.0
        perSceneConditionP95RegressionMillisecondsMax = 0.100
        perSceneConditionP95RegressionPercentMax = 2.0
        geometryMedianDeltaMustBeNegativeInEverySceneCondition = $true
        conflictOrQualityOrLifecycleFailure = 'No-Go'
        memoryOnlyWhenTimeFlat = 'memory trade-off / time No-Go'
    }
}
$metadataPath = Join-Path $resultRoot 'experiment-metadata.json'
Write-Utf8Json -Value $metadata -Path $metadataPath -Depth 16
Save-RunManifest

# Lifecycle and half-resolution guide coverage are separate from the formal
# full-resolution SSAO timing matrix.
foreach ($modeEntry in @(
    @{ variant = 'A'; mode = 'explicit' },
    @{ variant = 'B'; mode = 'reconstruct' }
)) {
    $variant = $modeEntry.variant
    $mode = $modeEntry.mode
    $logBase = Join-Path $resultRoot ("validation\resource-$variant")
    Invoke-RendererRun `
        -Kind 'lifecycle' `
        -SceneName 'saved-default-scene' `
        -Condition 'forward-deferred-resize-release' `
        -Variant $variant `
        -Mode $mode `
        -Arguments @('--gbuffer-position', $mode, '--resource-smoke-test') `
        -LogBase $logBase

    $scene = $scenes[0]
    $resultPath = Join-Path $resultRoot ("validation\half-bilateral-$variant.json")
    $args = Get-ClassicArguments `
        -Scene $scene -Mode $mode -Condition 'ssao-off' `
        -Width 640 -Height 360 -Warmup 20 -Measured 40 `
        -ResultPath $resultPath `
        -SceneLabel ("half-bilateral-$variant")
    [void]$args.Add('--classic-scene-ssao-mode')
    [void]$args.Add('half-bilateral')
    [void]$args.Add('--classic-scene-ssao-samples')
    [void]$args.Add('64')
    [void]$args.Add('--classic-scene-no-capture')
    Invoke-RendererRun `
        -Kind 'half-bilateral-validation' `
        -SceneName $scene.name `
        -Condition 'ssao-half-bilateral' `
        -Variant $variant `
        -Mode $mode `
        -Arguments $args.ToArray() `
        -LogBase (Join-Path $resultRoot ("validation\half-bilateral-$variant")) `
        -ResultPath $resultPath
}

$conditions = @('ssao-off', 'ssao-on')
foreach ($scene in $scenes) {
    foreach ($condition in $conditions) {
        for ($orderIndex = 0; $orderIndex -lt $performanceSequence.Count; ++$orderIndex) {
            $entry = $performanceSequence[$orderIndex]
            $variant = [string]$entry.variant
            $mode = [string]$entry.mode
            $pair = [int]$entry.pair
            $order = $orderIndex + 1
            $directory = Join-Path $resultRoot `
                ("raw\{0}\{1}" -f $scene.name, $condition)
            [void](New-Item -ItemType Directory -Path $directory -Force)
            $stem = 'order-{0}-pair-{1}-{2}' -f $order, $pair, $variant
            $resultPath = Join-Path $directory ($stem + '.json')
            $args = Get-ClassicArguments `
                -Scene $scene -Mode $mode -Condition $condition `
                -Width $width -Height $height `
                -Warmup $warmupFrames -Measured $measuredFrames `
                -ResultPath $resultPath `
                -SceneLabel ("{0}-{1}-{2}-p{3}" -f `
                    $scene.name, $condition, $variant, $pair)
            [void]$args.Add('--classic-scene-no-capture')
            Invoke-RendererRun `
                -Kind 'performance' `
                -SceneName $scene.name `
                -Condition $condition `
                -Variant $variant `
                -Mode $mode `
                -Pair $pair `
                -Order $order `
                -Arguments $args.ToArray() `
                -LogBase (Join-Path $directory $stem) `
                -ResultPath $resultPath
        }
    }
}

foreach ($scene in $scenes) {
    foreach ($condition in $conditions) {
        foreach ($modeEntry in @(
            @{ variant = 'A'; mode = 'explicit' },
            @{ variant = 'B'; mode = 'reconstruct' }
        )) {
            $variant = $modeEntry.variant
            $mode = $modeEntry.mode
            $directory = Join-Path $resultRoot `
                ("quality\{0}\{1}\{2}" -f $scene.name, $condition, $variant)
            [void](New-Item -ItemType Directory -Path $directory -Force)
            $resultPath = Join-Path $directory 'result.json'
            $args = Get-ClassicArguments `
                -Scene $scene -Mode $mode -Condition $condition `
                -Width $width -Height $height `
                -Warmup $qualityWarmupFrames -Measured 1 `
                -ResultPath $resultPath `
                -SceneLabel ("quality-{0}-{1}-{2}" -f `
                    $scene.name, $condition, $variant)
            foreach ($value in @(
                '--classic-scene-capture', (Join-Path $directory 'final.ppm'),
                '--classic-scene-gbuffer-depth-capture', (Join-Path $directory 'depth.pfm'),
                '--classic-scene-gbuffer-normal-capture', (Join-Path $directory 'normal.pfm'),
                '--classic-scene-gbuffer-albedo-capture', (Join-Path $directory 'albedo.pfm'),
                '--classic-scene-gbuffer-material-capture', (Join-Path $directory 'material.pfm'),
                '--classic-scene-gbuffer-material-alpha-capture', (Join-Path $directory 'material-alpha.pfm'),
                '--classic-scene-gbuffer-emissive-capture', (Join-Path $directory 'emissive.pfm')
            )) {
                [void]$args.Add($value)
            }
            if ($variant -eq 'A') {
                [void]$args.Add('--classic-scene-gbuffer-position-capture')
                [void]$args.Add((Join-Path $directory 'position-world-explicit.pfm'))
            }
            if ($condition -eq 'ssao-on') {
                [void]$args.Add('--classic-scene-ssao-float-capture')
                [void]$args.Add((Join-Path $directory 'ao.pfm'))
            }
            Invoke-RendererRun `
                -Kind 'quality' `
                -SceneName $scene.name `
                -Condition $condition `
                -Variant $variant `
                -Mode $mode `
                -Arguments $args.ToArray() `
                -LogBase (Join-Path $directory 'capture') `
                -ResultPath $resultPath
        }
    }
}

$analyzerPath = Join-Path $PSScriptRoot `
    'analyze_gbuffer_position_reconstruction.py'
if (-not (Test-Path -LiteralPath $analyzerPath)) {
    throw "Analyzer is missing: $analyzerPath"
}
$analysisLog = Join-Path $resultRoot 'analysis'
$analysis = Invoke-CapturedProcess `
    -FilePath 'py' `
    -ArgumentList @('-3', $analyzerPath, '--root', $resultRoot) `
    -LogBase $analysisLog
if ($analysis.exitCode -ne 0) {
    throw "Analysis failed. See $($analysis.stderrPath)"
}
Assert-ExecutableUnchanged | Out-Null
Assert-SourceCheckpointUnchanged | Out-Null
Save-RunManifest
Write-Host "Completed: $resultRoot"
