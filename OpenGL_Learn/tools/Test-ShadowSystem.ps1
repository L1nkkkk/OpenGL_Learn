[CmdletBinding()]
param(
    [switch]$SkipBuild,
    [switch]$ReportOnly,
    [switch]$NoOpen,
    [switch]$SamplingOptimization,
    [string]$PythonPath,
    [string]$MsBuildPath,
    [ValidateRange(60, 5000)]
    [int]$MeasuredFrames = 300,
    [ValidateRange(15, 1000)]
    [int]$ExternalWarmupFrames = 60,
    [ValidateSet("sponza", "san-miguel")]
    [string[]]$SceneIds = @("sponza", "san-miguel"),
    [ValidateSet(
        "off-to-hard",
        "hard-to-pcf",
        "pcf-to-pcss",
        "pcf-legacy-to-stable",
        "pcss-legacy-to-stable"
    )]
    [string[]]$ComparisonIds = @()
)

$ErrorActionPreference = "Stop"
$ProgressPreference = "SilentlyContinue"

$toolsDirectory = $PSScriptRoot
$projectDirectory = Split-Path -Parent $toolsDirectory
$repositoryDirectory = Split-Path -Parent $projectDirectory
$manifestPath = Join-Path $projectDirectory "classic-scenes.manifest.json"
$imageToolPath = Join-Path $toolsDirectory "classic_scene_images.py"
$compareToolPath = Join-Path $toolsDirectory "shadow_image_compare.py"
$resultRelativeRoot = if ($SamplingOptimization) {
    "benchmark-results/shadow-sampling"
}
else {
    "benchmark-results/shadows"
}
$resultRoot = Join-Path $projectDirectory $resultRelativeRoot
$formalRoot = Join-Path $resultRoot "formal"
$finalRoot = Join-Path $resultRoot "final"
$reportImageDirectory = if ($SamplingOptimization) {
    "docs\benchmark-images\shadow-sampling"
}
else {
    "docs\benchmark-images\shadow-system"
}
$reportImageRoot = Join-Path $projectDirectory $reportImageDirectory
$executablePath =
    Join-Path $repositoryDirectory "x64\Release\OpenGL_Learn.exe"

$manifest = Get-Content -LiteralPath $manifestPath -Raw | ConvertFrom-Json
$scenes = @(
    $manifest.scenes |
        Where-Object { $_.id -in $SceneIds }
)
$inspectionCrops = @{
    "sponza" = @(430, 610, 320, 260)
    "san-miguel" = @(0, 620, 360, 270)
}
$comparisons = if ($SamplingOptimization) {
    @(
        [pscustomobject]@{
            id = "pcf-legacy-to-stable"
            beforeMode = "pcf"
            afterMode = "pcf"
            beforeSampling = "legacy"
            afterSampling = "stable"
            beforeLabel = "Legacy randomized PCF"
            afterLabel = "Stable Vogel PCF"
        },
        [pscustomobject]@{
            id = "pcss-legacy-to-stable"
            beforeMode = "pcss"
            afterMode = "pcss"
            beforeSampling = "legacy"
            afterSampling = "stable"
            beforeLabel = "Legacy randomized PCSS"
            afterLabel = "Stable Vogel PCSS"
        }
    )
}
else {
    @(
        [pscustomobject]@{
            id = "off-to-hard"
            beforeMode = "off"
            afterMode = "hard"
            beforeSampling = "stable"
            afterSampling = "stable"
            beforeLabel = "Shadows disabled"
            afterLabel = "Hard shadows"
        },
        [pscustomobject]@{
            id = "hard-to-pcf"
            beforeMode = "hard"
            afterMode = "pcf"
            beforeSampling = "stable"
            afterSampling = "stable"
            beforeLabel = "Hard shadows"
            afterLabel = "Stable Vogel PCF"
        },
        [pscustomobject]@{
            id = "pcf-to-pcss"
            beforeMode = "pcf"
            afterMode = "pcss"
            beforeSampling = "stable"
            afterSampling = "stable"
            beforeLabel = "Stable Vogel PCF"
            afterLabel = "Stable Vogel PCSS"
        }
    )
}
if ($ComparisonIds.Count -gt 0) {
    $comparisons = @(
        $comparisons |
            Where-Object { $_.id -in $ComparisonIds }
    )
}
if ($scenes.Count -eq 0 -or $comparisons.Count -eq 0) {
    throw "The selected scene/comparison filter produced no experiments."
}

function Resolve-Python {
    $candidates = @(
        $PythonPath,
        (Join-Path $HOME (
            ".cache\codex-runtimes\codex-primary-runtime\" +
            "dependencies\python\python.exe"
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
        & $candidate -c "from PIL import Image" 2>$null
        if ($LASTEXITCODE -eq 0) {
            return (Resolve-Path -LiteralPath $candidate).Path
        }
    }
    throw "Python with Pillow was not found. Pass -PythonPath <python.exe>."
}

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
    throw "MSBuild 2022 was not found. Pass -MsBuildPath <MSBuild.exe>."
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

function Invoke-ShadowRun {
    param(
        $Scene,
        [string]$Mode,
        [string]$SamplingPattern,
        [string]$Label,
        [string]$OutputDirectory,
        [int]$CaptureFrame = 315
    )

    New-Item -ItemType Directory -Path $OutputDirectory -Force |
        Out-Null
    $resultRootPath = (Resolve-Path -LiteralPath $resultRoot).Path
    $outputPath = (Resolve-Path -LiteralPath $OutputDirectory).Path
    $relativeOutputPath = $outputPath.Substring(
        $resultRootPath.Length + 1
    ).Replace("\", "/")
    $modelPath =
        "classic-scenes/" + ([string]$Scene.modelPath).Replace("\", "/")
    $captureRelative = (
        "$resultRelativeRoot/$relativeOutputPath/$Label.ppm"
    ).Replace("\", "/")
    $resultRelative =
        [System.IO.Path]::ChangeExtension($captureRelative, ".json")
    $logPath = Join-Path $OutputDirectory "$Label.log"
    $arguments = @(
        "--classic-scene-test", $modelPath,
        "--classic-scene-name", "$($Scene.id)-$Label",
        "--classic-scene-capture", $captureRelative,
        "--classic-scene-result", $resultRelative,
        "--classic-scene-camera",
            [string]$Scene.camera[0],
            [string]$Scene.camera[1],
            [string]$Scene.camera[2],
        "--classic-scene-target",
            [string]$Scene.target[0],
            [string]$Scene.target[1],
            [string]$Scene.target[2],
        "--classic-scene-up",
            [string]$Scene.up[0],
            [string]$Scene.up[1],
            [string]$Scene.up[2],
        "--classic-scene-radius", [string]$Scene.normalizedRadius,
        "--classic-scene-fov", [string]$Scene.fov,
        "--classic-scene-capture-frame", [string]$CaptureFrame,
        "--classic-scene-shadow-mode", $Mode,
        "--classic-scene-shadow-sampling", $SamplingPattern,
        "--classic-scene-shadow-lights", "directional",
        "--classic-scene-render-path", "pbr-forward"
    )

    Push-Location $projectDirectory
    try {
        & $executablePath @arguments *> $logPath
        $exitCode = $LASTEXITCODE
    }
    finally {
        Pop-Location
    }
    if ($exitCode -ne 0) {
        throw (
            "$($Scene.displayName) $Label failed with exit code " +
            "$exitCode. See $logPath"
        )
    }

    $resultPath = Join-Path $projectDirectory $resultRelative
    $result =
        Get-Content -LiteralPath $resultPath -Raw | ConvertFrom-Json
    if (-not $result.success) {
        throw "$($Scene.displayName) $Label produced an unsuccessful result."
    }
    if ([string]$result.shadow.mode -ne $Mode) {
        throw "$($Scene.displayName) $Label reported the wrong shadow mode."
    }
    if ([string]$result.shadow.sampling -ne $SamplingPattern) {
        throw (
            "$($Scene.displayName) $Label reported the wrong " +
            "shadow sampling pattern."
        )
    }
    if ([int64]$result.triangleCount -ne
        [int64]$Scene.expectedTriangles) {
        throw (
            "$($Scene.displayName) triangle count changed: " +
            "$($result.triangleCount)"
        )
    }
    if ([int64]$result.memoryBytes.meshCpu -ne 0) {
        throw "$($Scene.displayName) retained CPU mesh staging memory."
    }
    $expectedSamples = $CaptureFrame - 15
    if ([int64]$result.frameTimeMilliseconds.sampleCount -ne
        $expectedSamples) {
        throw (
            "$($Scene.displayName) produced " +
            "$($result.frameTimeMilliseconds.sampleCount) timed frames; " +
            "expected $expectedSamples."
        )
    }
    if ($Mode -eq "off") {
        if ([int64]$result.shadow.updateCount -ne 0) {
            throw (
                "$($Scene.displayName) updated a shadow map while " +
                "shadows were disabled."
            )
        }
    }
    else {
        if ([int64]$result.shadow.updateCount -ne 1 -or
            [int64]$result.shadow.updatedLightCount -ne 1 -or
            [int64]$result.shadow.cacheHitCount -lt ($CaptureFrame - 2)) {
            throw (
                "$($Scene.displayName) shadow cache invariant failed " +
                "for $Label."
            )
        }
    }

    return $result
}

function Get-Average {
    param([object[]]$Values)
    return [double](($Values | Measure-Object -Average).Average)
}

function Get-Minimum {
    param([object[]]$Values)
    return [double](($Values | Measure-Object -Minimum).Minimum)
}

function Get-Maximum {
    param([object[]]$Values)
    return [double](($Values | Measure-Object -Maximum).Maximum)
}

function Format-Delta {
    param(
        [double]$Before,
        [double]$After,
        [string]$Unit
    )

    $delta = $After - $Before
    $percent = 0.0
    if ([math]::Abs($Before) -gt 1e-9) {
        $percent = 100.0 * $delta / $Before
    }
    return (
        "{0:+0.000;-0.000;0.000} {1} " +
        "({2:+0.00;-0.00;0.00}%)"
    ) -f $delta, $Unit, $percent
}

function Get-ShadowUpdateAverage {
    param([object[]]$Results, [string]$Property)

    $values = @($Results | ForEach-Object {
        if ([int64]$_.shadow.updateCount -gt 0) {
            [double]$_.shadow.$Property
        }
        else {
            0.0
        }
    })
    return Get-Average $values
}

function Write-Summary {
    param([string]$Python)

    New-Item -ItemType Directory -Path $finalRoot -Force | Out-Null
    New-Item -ItemType Directory -Path $reportImageRoot -Force |
        Out-Null

    $summaryScenes = @()
    $reportTitle = if ($SamplingOptimization) {
        "# Stable shadow sampling controlled A/B report"
    }
    else {
        "# Shadow filtering controlled A/B report"
    }
    $samplingDescription = if ($SamplingOptimization) {
        (
            "Both variants use 16 filter samples. A uses the legacy " +
            "per-fragment randomized ring kernel and 16 PCSS blocker " +
            "samples; B uses the stable golden-angle Vogel kernel and " +
            "eight evenly distributed PCSS blocker samples."
        )
    }
    else {
        (
            "Filtering uses 16 samples with the stable golden-angle Vogel " +
            "kernel. Frame time is GPU-synchronized wall time."
        )
    }
    $markdown = @(
        $reportTitle,
        "",
        (
            "Release x64, 1440 x 900. Every measured run is a fresh " +
            "process with 15 internal warm-up frames and " +
            "$MeasuredFrames measured frames."
        ),
        (
            "Each adjacent comparison receives one external unmeasured " +
            "warm-up per variant; measured order is A/B/B/A/A/B."
        ),
        $samplingDescription,
        ""
    )

    foreach ($scene in $scenes) {
        $sceneComparisons = @()
        $markdown += @(
            "## $($scene.displayName)",
            ""
        )

        foreach ($comparison in $comparisons) {
            $comparisonDirectory = Join-Path (
                Join-Path $formalRoot $scene.id
            ) $comparison.id
            $beforeFiles = @(
                Get-ChildItem -LiteralPath $comparisonDirectory `
                    -Filter "A*.json" |
                    Sort-Object Name
            )
            $afterFiles = @(
                Get-ChildItem -LiteralPath $comparisonDirectory `
                    -Filter "B*.json" |
                    Sort-Object Name
            )
            if ($beforeFiles.Count -ne 3 -or $afterFiles.Count -ne 3) {
                throw (
                    "Expected three A and three B results for " +
                    "$($scene.id)/$($comparison.id)."
                )
            }

            $beforeResults = @($beforeFiles | ForEach-Object {
                Get-Content -LiteralPath $_.FullName -Raw |
                    ConvertFrom-Json
            })
            $afterResults = @($afterFiles | ForEach-Object {
                Get-Content -LiteralPath $_.FullName -Raw |
                    ConvertFrom-Json
            })
            foreach ($result in @($beforeResults) + @($afterResults)) {
                if ([int]$result.frameTimeMilliseconds.sampleCount -ne
                    $MeasuredFrames) {
                    throw (
                        "$($scene.id)/$($comparison.id) contains " +
                        "$($result.frameTimeMilliseconds.sampleCount)-frame " +
                        "results; rerun with -MeasuredFrames " +
                        "$MeasuredFrames or pass the matching value to " +
                        "-ReportOnly."
                    )
                }
            }

            $beforePpm = [System.IO.Path]::ChangeExtension(
                $beforeFiles[-1].FullName,
                ".ppm"
            )
            $afterPpm = [System.IO.Path]::ChangeExtension(
                $afterFiles[-1].FullName,
                ".ppm"
            )
            $beforePng = Join-Path $finalRoot (
                "$($scene.id)-$($comparison.id)-before.png"
            )
            $afterPng = Join-Path $finalRoot (
                "$($scene.id)-$($comparison.id)-after.png"
            )
            & $Python $imageToolPath "ppm-to-png" `
                $beforePpm $beforePng
            if ($LASTEXITCODE -ne 0) {
                throw "A image conversion failed for $($scene.id)."
            }
            & $Python $imageToolPath "ppm-to-png" `
                $afterPpm $afterPng
            if ($LASTEXITCODE -ne 0) {
                throw "B image conversion failed for $($scene.id)."
            }

            $imagePrefix =
                "$($scene.id)-$($comparison.id)"
            $visualMetricsPath =
                Join-Path $finalRoot "$imagePrefix-visual.json"
            $differencePath =
                Join-Path $finalRoot "$imagePrefix-difference.png"
            $sideBySidePath =
                Join-Path $finalRoot "$imagePrefix-comparison.png"
            $cropComparisonPath =
                Join-Path $finalRoot "$imagePrefix-crop-comparison.png"
            $compareArguments = @(
                $compareToolPath,
                $beforePng,
                $afterPng,
                "--metrics",
                $visualMetricsPath,
                "--difference",
                $differencePath,
                "--side-by-side",
                $sideBySidePath,
                "--before-label",
                $comparison.beforeLabel,
                "--after-label",
                $comparison.afterLabel
            )
            if ($SamplingOptimization) {
                $crop = $inspectionCrops[$scene.id]
                $compareArguments += @(
                    "--crop",
                    [string]$crop[0],
                    [string]$crop[1],
                    [string]$crop[2],
                    [string]$crop[3],
                    "--crop-comparison",
                    $cropComparisonPath,
                    "--crop-scale",
                    "3"
                )
            }
            & $Python @compareArguments | Out-Null
            if ($LASTEXITCODE -ne 0) {
                throw "Image comparison failed for $imagePrefix."
            }

            $reportImages = @(
                $differencePath,
                $sideBySidePath
            )
            if ($SamplingOptimization) {
                $reportImages += $cropComparisonPath
            }
            foreach ($imagePath in $reportImages) {
                Copy-Item -LiteralPath $imagePath `
                    -Destination (
                        Join-Path $reportImageRoot (
                            Split-Path -Leaf $imagePath
                        )
                    ) `
                    -Force
            }
            $visual =
                Get-Content -LiteralPath $visualMetricsPath -Raw |
                    ConvertFrom-Json

            $beforeMeanValues = @($beforeResults | ForEach-Object {
                [double]$_.frameTimeMilliseconds.mean
            })
            $afterMeanValues = @($afterResults | ForEach-Object {
                [double]$_.frameTimeMilliseconds.mean
            })
            $beforeMedianValues = @($beforeResults | ForEach-Object {
                [double]$_.frameTimeMilliseconds.median
            })
            $afterMedianValues = @($afterResults | ForEach-Object {
                [double]$_.frameTimeMilliseconds.median
            })
            $beforeP95Values = @($beforeResults | ForEach-Object {
                [double]$_.frameTimeMilliseconds.p95
            })
            $afterP95Values = @($afterResults | ForEach-Object {
                [double]$_.frameTimeMilliseconds.p95
            })
            $beforeLoadValues = @($beforeResults | ForEach-Object {
                [double]$_.loadMilliseconds
            })
            $afterLoadValues = @($afterResults | ForEach-Object {
                [double]$_.loadMilliseconds
            })
            $beforeRenderTargetValues = @(
                $beforeResults | ForEach-Object {
                    [double]$_.memoryBytes.renderTarget
                }
            )
            $afterRenderTargetValues = @(
                $afterResults | ForEach-Object {
                    [double]$_.memoryBytes.renderTarget
                }
            )

            $beforeMean = Get-Average $beforeMeanValues
            $afterMean = Get-Average $afterMeanValues
            $beforeMedian = Get-Average $beforeMedianValues
            $afterMedian = Get-Average $afterMedianValues
            $beforeP95 = Get-Average $beforeP95Values
            $afterP95 = Get-Average $afterP95Values
            $beforeLoad = Get-Average $beforeLoadValues
            $afterLoad = Get-Average $afterLoadValues
            $beforeRenderTarget =
                Get-Average $beforeRenderTargetValues
            $afterRenderTarget =
                Get-Average $afterRenderTargetValues
            $beforeUpdateGpu = Get-ShadowUpdateAverage `
                -Results $beforeResults `
                -Property "updateGpuMilliseconds"
            $afterUpdateGpu = Get-ShadowUpdateAverage `
                -Results $afterResults `
                -Property "updateGpuMilliseconds"
            $beforeUpdateCpu = Get-ShadowUpdateAverage `
                -Results $beforeResults `
                -Property "lastUpdateCpuMilliseconds"
            $afterUpdateCpu = Get-ShadowUpdateAverage `
                -Results $afterResults `
                -Property "lastUpdateCpuMilliseconds"

            $comparisonSummary = [ordered]@{
                id = [string]$comparison.id
                beforeMode = [string]$comparison.beforeMode
                afterMode = [string]$comparison.afterMode
                beforeSampling = [string]$comparison.beforeSampling
                afterSampling = [string]$comparison.afterSampling
                beforeLabel = [string]$comparison.beforeLabel
                afterLabel = [string]$comparison.afterLabel
                samples = [ordered]@{
                    beforeMeanFrameMilliseconds = $beforeMeanValues
                    afterMeanFrameMilliseconds = $afterMeanValues
                    beforeMedianFrameMilliseconds = $beforeMedianValues
                    afterMedianFrameMilliseconds = $afterMedianValues
                    beforeP95FrameMilliseconds = $beforeP95Values
                    afterP95FrameMilliseconds = $afterP95Values
                    beforeLoadMilliseconds = $beforeLoadValues
                    afterLoadMilliseconds = $afterLoadValues
                }
                average = [ordered]@{
                    beforeMeanFrameMilliseconds = $beforeMean
                    afterMeanFrameMilliseconds = $afterMean
                    meanFrameDeltaMilliseconds =
                        $afterMean - $beforeMean
                    meanFrameDeltaPercent =
                        100.0 * ($afterMean - $beforeMean) /
                        $beforeMean
                    beforeMedianFrameMilliseconds = $beforeMedian
                    afterMedianFrameMilliseconds = $afterMedian
                    medianFrameDeltaMilliseconds =
                        $afterMedian - $beforeMedian
                    medianFrameDeltaPercent =
                        100.0 * ($afterMedian - $beforeMedian) /
                        $beforeMedian
                    beforeP95FrameMilliseconds = $beforeP95
                    afterP95FrameMilliseconds = $afterP95
                    p95FrameDeltaMilliseconds =
                        $afterP95 - $beforeP95
                    p95FrameDeltaPercent =
                        100.0 * ($afterP95 - $beforeP95) /
                        $beforeP95
                    beforeFps = 1000.0 / $beforeMean
                    afterFps = 1000.0 / $afterMean
                    beforeLoadMilliseconds = $beforeLoad
                    afterLoadMilliseconds = $afterLoad
                    loadDeltaMilliseconds = $afterLoad - $beforeLoad
                    beforeRenderTargetBytes = $beforeRenderTarget
                    afterRenderTargetBytes = $afterRenderTarget
                    renderTargetDeltaBytes =
                        $afterRenderTarget - $beforeRenderTarget
                    beforeShadowUpdateGpuMilliseconds =
                        $beforeUpdateGpu
                    afterShadowUpdateGpuMilliseconds =
                        $afterUpdateGpu
                    beforeShadowUpdateCpuMilliseconds =
                        $beforeUpdateCpu
                    afterShadowUpdateCpuMilliseconds =
                        $afterUpdateCpu
                }
                range = [ordered]@{
                    beforeMeanFrameMilliseconds = @(
                        (Get-Minimum $beforeMeanValues),
                        (Get-Maximum $beforeMeanValues)
                    )
                    afterMeanFrameMilliseconds = @(
                        (Get-Minimum $afterMeanValues),
                        (Get-Maximum $afterMeanValues)
                    )
                }
                visual = $visual
                images = [ordered]@{
                    before = $beforePng
                    after = $afterPng
                    comparison = $sideBySidePath
                    difference = $differencePath
                    cropComparison = if ($SamplingOptimization) {
                        $cropComparisonPath
                    }
                    else {
                        $null
                    }
                }
            }
            $sceneComparisons +=
                [pscustomobject]$comparisonSummary

            $markdown += @(
                (
                    "### $($comparison.beforeLabel) -> " +
                    "$($comparison.afterLabel)"
                ),
                "",
                (
                    "| Metric | A: $($comparison.beforeLabel) | " +
                    "B: $($comparison.afterLabel) | Delta |"
                ),
                "|---|---:|---:|---:|",
                (
                    "| Mean frame | {0:N3} ms | {1:N3} ms | {2} |" -f
                    $beforeMean,
                    $afterMean,
                    (Format-Delta $beforeMean $afterMean "ms")
                ),
                (
                    "| Median frame | {0:N3} ms | {1:N3} ms | {2} |" -f
                    $beforeMedian,
                    $afterMedian,
                    (Format-Delta $beforeMedian $afterMedian "ms")
                ),
                (
                    "| P95 frame | {0:N3} ms | {1:N3} ms | {2} |" -f
                    $beforeP95,
                    $afterP95,
                    (Format-Delta $beforeP95 $afterP95 "ms")
                ),
                (
                    (
                        "| FPS | {0:N1} | {1:N1} | " +
                        "{2:+0.0;-0.0;0.0} |"
                    ) -f
                    (1000.0 / $beforeMean),
                    (1000.0 / $afterMean),
                    (
                        (1000.0 / $afterMean) -
                        (1000.0 / $beforeMean)
                    )
                ),
                (
                    "| Load | {0:N1} ms | {1:N1} ms | {2} |" -f
                    $beforeLoad,
                    $afterLoad,
                    (Format-Delta $beforeLoad $afterLoad "ms")
                ),
                (
                    (
                        "| Render targets | {0:N2} MiB | {1:N2} MiB | " +
                        "{2:+0.00;-0.00;0.00} MiB |"
                    ) -f
                    ($beforeRenderTarget / 1MB),
                    ($afterRenderTarget / 1MB),
                    (
                        ($afterRenderTarget - $beforeRenderTarget) /
                        1MB
                    )
                ),
                "",
                (
                    (
                        "Frame ranges: A {0:N3}-{1:N3} ms; " +
                        "B {2:N3}-{3:N3} ms."
                    ) -f
                    (Get-Minimum $beforeMeanValues),
                    (Get-Maximum $beforeMeanValues),
                    (Get-Minimum $afterMeanValues),
                    (Get-Maximum $afterMeanValues)
                ),
                (
                    (
                        "Visual change: {0:P2} pixels changed; " +
                        "luminance MAE {1:P2}."
                    ) -f
                    [double]$visual.changedPixelRatio,
                    [double]$visual.luminanceMeanAbsoluteDifference
                ),
                "",
                "![A/B comparison]($imagePrefix-comparison.png)",
                "",
                "![Enhanced difference]($imagePrefix-difference.png)"
            )
            if ($SamplingOptimization) {
                $roughness = $visual.cropHighFrequencyResidual
                $markdown += @(
                    "",
                    (
                        (
                            "Inspection-crop high-frequency luminance " +
                            "residual: {0:P3} -> {1:P3} ({2:+0.00%;-0.00%;0.00%})."
                        ) -f
                        [double]$roughness.before,
                        [double]$roughness.after,
                        [double]$roughness.relativeDelta
                    ),
                    "",
                    "![Magnified edge crop]($imagePrefix-crop-comparison.png)"
                )
            }
            $markdown += ""
        }

        $summaryScenes += [pscustomobject][ordered]@{
            id = [string]$scene.id
            displayName = [string]$scene.displayName
            comparisons = $sceneComparisons
        }
    }

    $summaryObject = [ordered]@{
        schemaVersion = 4
        experiment = if ($SamplingOptimization) {
            "shadow-sampling-optimization"
        }
        else {
            "shadow-filtering-modes"
        }
        configuration = "Release x64"
        resolution = @(1440, 900)
        internalWarmupFrames = 15
        measuredFrames = $MeasuredFrames
        externalWarmupFrames = $ExternalWarmupFrames
        order = @("A", "B", "B", "A", "A", "B")
        shadowSamples = 16
        shadowRings = 8
        stablePcssBlockerSamples = 8
        comparisons = @($comparisons | ForEach-Object {
            [ordered]@{
                id = [string]$_.id
                beforeMode = [string]$_.beforeMode
                afterMode = [string]$_.afterMode
                beforeSampling = [string]$_.beforeSampling
                afterSampling = [string]$_.afterSampling
            }
        })
        scenes = $summaryScenes
    }
    $summaryJsonPath = Join-Path $finalRoot "summary.json"
    $summaryMarkdownPath = Join-Path $finalRoot "summary.md"
    $summaryObject | ConvertTo-Json -Depth 15 |
        Set-Content -LiteralPath $summaryJsonPath -Encoding UTF8
    $markdown -join [Environment]::NewLine |
        Set-Content -LiteralPath $summaryMarkdownPath -Encoding UTF8
    Write-Host "Shadow report: $summaryMarkdownPath"
    return $summaryMarkdownPath
}

$python = Resolve-Python
if (-not $ReportOnly) {
    if (-not $SkipBuild) {
        Build-Renderer -MsBuild (Resolve-MsBuild)
    }
    if (-not (Test-Path -LiteralPath $executablePath -PathType Leaf)) {
        throw "Renderer executable not found: $executablePath"
    }

    New-Item -ItemType Directory -Path $formalRoot -Force |
        Out-Null
    foreach ($scene in $scenes) {
        foreach ($comparison in $comparisons) {
            $warmupDirectory = Join-Path (
                Join-Path (
                    Join-Path $resultRoot "warmup"
                ) $scene.id
            ) $comparison.id
            Write-Host (
                "Warm-up A ($($comparison.beforeMode)): " +
                "$($scene.displayName), $($comparison.id)"
            )
            Invoke-ShadowRun `
                -Scene $scene `
                -Mode $comparison.beforeMode `
                -SamplingPattern $comparison.beforeSampling `
                -Label "A-warmup" `
                -OutputDirectory $warmupDirectory `
                -CaptureFrame (15 + $ExternalWarmupFrames) |
                Out-Null
            Write-Host (
                "Warm-up B ($($comparison.afterMode)): " +
                "$($scene.displayName), $($comparison.id)"
            )
            Invoke-ShadowRun `
                -Scene $scene `
                -Mode $comparison.afterMode `
                -SamplingPattern $comparison.afterSampling `
                -Label "B-warmup" `
                -OutputDirectory $warmupDirectory `
                -CaptureFrame (15 + $ExternalWarmupFrames) |
                Out-Null

            $comparisonDirectory = Join-Path (
                Join-Path $formalRoot $scene.id
            ) $comparison.id
            $counts = @{ A = 0; B = 0 }
            foreach ($variant in @("A", "B", "B", "A", "A", "B")) {
                $counts[$variant]++
                if ($variant -eq "A") {
                    $mode = $comparison.beforeMode
                    $samplingPattern = $comparison.beforeSampling
                }
                else {
                    $mode = $comparison.afterMode
                    $samplingPattern = $comparison.afterSampling
                }
                $label = "$variant$($counts[$variant])"
                Write-Host (
                    "Measured $label ($mode/$samplingPattern): " +
                    "$($scene.displayName), $($comparison.id)"
                )
                $result = Invoke-ShadowRun `
                    -Scene $scene `
                    -Mode $mode `
                    -SamplingPattern $samplingPattern `
                    -Label $label `
                    -OutputDirectory $comparisonDirectory `
                    -CaptureFrame (15 + $MeasuredFrames)
                Write-Host (
                    (
                        "PASS {0} {1} {2}: frame={3:N3} ms, " +
                        "load={4:N1} ms"
                    ) -f
                    $scene.id,
                    $comparison.id,
                    $label,
                    [double]$result.averageFrameMilliseconds,
                    [double]$result.loadMilliseconds
                )
            }
        }
    }
}

$reportPath = Write-Summary -Python $python
if (-not $NoOpen) {
    Invoke-Item -LiteralPath $reportPath
}
