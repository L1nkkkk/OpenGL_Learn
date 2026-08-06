param(
    [Parameter(Mandatory)]
    [string]$RunDirectory
)

Set-StrictMode -Version Latest
$ErrorActionPreference = "Stop"

$projectDirectory = (Resolve-Path (Join-Path $PSScriptRoot "..")).Path
$repositoryDirectory = (Resolve-Path (Join-Path $projectDirectory "..")).Path
$run = (Resolve-Path -LiteralPath $RunDirectory).Path
$executable = Join-Path $repositoryDirectory "x64\Release\OpenGL_Learn.exe"

function Read-Json([string]$Path) {
    return Get-Content -LiteralPath $Path -Raw -Encoding UTF8 | ConvertFrom-Json
}
function Relative([string]$Path) {
    return [IO.Path]::GetFullPath($Path).Substring($run.Length + 1).Replace("\", "/")
}
function File-Record([string]$Path) {
    $item = Get-Item -LiteralPath $Path
    return [ordered]@{
        path = Relative $item.FullName
        bytes = $item.Length
        sha256 = (Get-FileHash -LiteralPath $item.FullName -Algorithm SHA256).Hash
    }
}
function Assert-True([bool]$Condition, [string]$Message) {
    if (-not $Condition) { throw $Message }
}

$pre = Read-Json (Join-Path $run "pre-capture-manifest.json")
$capture = Read-Json (Join-Path $run "capture-manifest.json")
$correctness = Read-Json (Join-Path $run "correctness-manifest.json")
$aggregate = Read-Json (Join-Path $run "aggregate.json")
$independent = Read-Json (Join-Path $run "verification\independent-verification.json")
$performance = Read-Json (Join-Path $run "verification\performance-recalculation.json")
$replayPath = Join-Path $run "renderdoc\replay\cached-n0512-r060-s08-replay.json"
$replay = Read-Json $replayPath
$finalSmoke = Read-Json (Join-Path $run "final-smoke\manifest.json")
$gridSmoke = Read-Json (Join-Path $run "grid-final-smoke\manifest.json")
$invalidViewport = Read-Json (Join-Path $run "invalid-viewport-smoke\manifest.json")
$liveResize = Read-Json (Join-Path $run "live-resize-smoke\manifest.json")
$executableHash = (Get-FileHash -LiteralPath $executable -Algorithm SHA256).Hash

Assert-True ([bool]$capture.valid) "capture manifest is invalid"
Assert-True ([int]$capture.expectedRunCount -eq 615 -and [int]$capture.completedRunCount -eq 615) "performance capture count is incomplete"
Assert-True ([int]$capture.correctnessRunCount -eq 38) "correctness capture count is incomplete"
Assert-True ([bool]$correctness.valid -and @($correctness.completedRuns).Count -eq 38) "correctness manifest is invalid"
Assert-True ([bool]$aggregate.valid -and [int]$aggregate.runCount -eq 615) "aggregate is invalid"
Assert-True ([bool]$aggregate.imageParity.allExact -and [int]$aggregate.imageParity.exactGroups -eq 123) "formal S image parity failed"
Assert-True ([bool]$independent.passed -and [int]$independent.csrCellsVerified -eq 125) "independent CSR verification failed"
Assert-True ([bool]$independent.fullImageMembership.allMissesZero) "full G-buffer membership validation found misses"
Assert-True ([bool]$performance.success -and [int]$performance.performanceProcessCount -eq 615) "independent performance recalculation failed"
Assert-True ([int]$performance.cachedGoCount -eq 9 -and [int]$performance.rebuildGoCount -eq 0) "unexpected Go counts"
Assert-True ([bool]$replay.success) "RenderDoc replay validation failed"
Assert-True (-not (@($replay.validation.PSObject.Properties | ForEach-Object { [bool]$_.Value }) -contains $false)) "RenderDoc validation contains a failed check"
Assert-True ([int]$replay.fragmentUniforms.gridSliceCount -eq 8) "captured gridSliceCount uniform is not 8"
Assert-True ([bool]$finalSmoke.success -and @($finalSmoke.records).Count -eq 8) "general final smoke failed"
Assert-True ([bool]$gridSmoke.success -and @($gridSmoke.records).Count -eq 5) "grid final smoke failed"
Assert-True (-not (@($gridSmoke.endpointCompatibility.PSObject.Properties | ForEach-Object { [bool]$_.Value }) -contains $false)) "post-build endpoint compatibility failed"
Assert-True ([int]$gridSmoke.remainingRendererProcesses -eq 0) "renderer process remains after smoke"
Assert-True ([bool]$invalidViewport.success -and [int64]$invalidViewport.invalidPixelCount -gt 0) "invalid/sky viewport smoke failed"
Assert-True ([bool]$invalidViewport.s1S8OutputExact -and [bool]$invalidViewport.s1S8GBufferExact) "invalid/sky S1/S8 parity failed"
Assert-True ([bool]$liveResize.success -and [int64]$liveResize.buildCount -ge 3 -and [int64]$liveResize.cacheHitCount -gt 0) "live resize smoke failed"
Assert-True ([int]$liveResize.finalResolution[0] -ne [int]$liveResize.requestedInitialResolution[0] -or [int]$liveResize.finalResolution[1] -ne [int]$liveResize.requestedInitialResolution[1]) "live resize did not change the framebuffer"
Assert-True ([int]$liveResize.logicalCells -eq [int]$liveResize.expectedLogicalCells) "live resize grid dimensions are stale"
Assert-True ([string]$liveResize.executableSha256 -eq $executableHash) "live resize used a different executable"
Assert-True ($executableHash -eq [string]$pre.executableSha256 -and $executableHash -eq [string]$capture.executableSha256) "final executable differs from formal binary"
Assert-True ([string]$aggregate.protocolSha256 -eq [string]$pre.protocolSha256) "protocol hash drift"
Assert-True ([bool]$independent.defaultRenderModeRemainsAnalyticScreen) "default render mode changed"

$renderDocDirectory = Join-Path $run "renderdoc"
$renderDocFiles = Get-ChildItem -LiteralPath $renderDocDirectory -File -Recurse |
    Where-Object { $_.Name -ne "renderdoc-manifest.json" } |
    ForEach-Object { File-Record $_.FullName }
$renderDocManifest = [ordered]@{
    schemaVersion = 1
    valid = $true
    createdUtc = [DateTime]::UtcNow.ToString("o")
    protocolSha256 = [string]$pre.protocolSha256
    executableSha256 = $executableHash
    renderDocVersion = "1.45.0"
    case = "cached N=512 R=6 S=8"
    capture = "renderdoc/captures/cached-n0512-r060-s08_capture.rdc"
    replay = Relative $replayPath
    capturedSliceUniform = [int]$replay.fragmentUniforms.gridSliceCount
    pointLightDrawCount = [int]$replay.eventTree.counts.drawActionsInsidePointLightPhase
    gridTboBindingCount = [int]$replay.fragmentResources.bufferTextureBindingCount
    replayDebugMessageCount = @($replay.debugMessages).Count
    files = $renderDocFiles
}
$renderDocManifestPath = Join-Path $renderDocDirectory "renderdoc-manifest.json"
$renderDocManifest | ConvertTo-Json -Depth 10 | Set-Content -LiteralPath $renderDocManifestPath -Encoding UTF8

$cachedCells = @($aggregate.cells | Where-Object { $_.regime -eq "cached" })
$rebuildCells = @($aggregate.cells | Where-Object { $_.regime -eq "rebuild" })
foreach ($cell in @($cachedCells) + @($rebuildCells)) {
    $candidate = [int]$cell.statisticalCandidateSliceCount
    $finalRuntime = [int]$cell.finalRuntimeSliceCount
    Assert-True ([int]$cell.recommendedSliceCount -eq $finalRuntime) "recommendedSliceCount is not the final-runtime alias"
    if ([bool]$cell.depthSlicingGo) {
        Assert-True ($candidate -gt 1 -and $finalRuntime -eq $candidate) "Go cell does not select its statistical candidate"
    }
    else {
        Assert-True ($finalRuntime -eq 1) "Tie/No-Go cell did not fall back to S=1"
    }
}
$representative = $cachedCells | Where-Object { [int]$_.lightCount -eq 512 -and [double]$_.radius -eq 6.0 } | Select-Object -First 1
$s1 = $representative.slices.'1'
$s8 = $representative.slices.'8'
$relativeRows = foreach ($cell in $cachedCells | Where-Object { [bool]$_.depthSlicingGo }) {
    $selected = [string]$cell.finalRuntimeSliceCount
    $base = [double]$cell.slices.'1'.metrics.wallFrame.medianOfProcessMedians
    $chosen = [double]$cell.slices.$selected.metrics.wallFrame.medianOfProcessMedians
    [pscustomobject]@{
        lightCount = [int]$cell.lightCount
        radius = [double]$cell.radius
        slices = [int]$cell.finalRuntimeSliceCount
        relativePercent = ($chosen - $base) / $base * 100.0
    }
}
$bestRelative = $relativeRows | Sort-Object relativePercent | Select-Object -First 1
$remainingProcesses = @(
    Get-CimInstance Win32_Process -ErrorAction SilentlyContinue |
        Where-Object {
            $_.Name -in @("qrenderdoc.exe", "renderdoccmd.exe") -or
            ($_.Name -eq "OpenGL_Learn.exe" -and
                [string]$_.CommandLine -match "--point-light|--classic-scene|--resource-smoke-test|--gbuffer-position")
        }
).Count
Assert-True ($remainingProcesses -eq 0) "experiment-related processes remain at finalization"

$finalValidation = [ordered]@{
    schemaVersion = 1
    success = $true
    generatedUtc = [DateTime]::UtcNow.ToString("o")
    protocolSha256 = [string]$pre.protocolSha256
    executableSha256 = $executableHash
    executableMatchesFormalBinary = $true
    formalMatrix = [ordered]@{
        performanceProcesses = [int]$capture.completedRunCount
        correctnessProcesses = [int]$capture.correctnessRunCount
        warmupFramesPerPerformanceProcess = 300
        sampleFramesPerPerformanceProcess = 600
        imageGroupsExact = [int]$aggregate.imageParity.exactGroups
        imageGroupsTotal = [int]$aggregate.imageParity.groupCount
    }
    decision = [ordered]@{
        cachedGoCells = [int]$performance.cachedGoCount
        cachedCellCount = $cachedCells.Count
        rebuildGoCells = [int]$performance.rebuildGoCount
        rebuildCellCount = $rebuildCells.Count
        representative = [ordered]@{
            lightCount = 512
            radius = 6.0
            selectedSlices = 8
            s1WallMs = [double]$s1.metrics.wallFrame.medianOfProcessMedians
            s8WallMs = [double]$s8.metrics.wallFrame.medianOfProcessMedians
            wallDeltaMs = [double]$s8.metrics.wallFrame.medianOfProcessMedians - [double]$s1.metrics.wallFrame.medianOfProcessMedians
            wallRelativePercent = (([double]$s8.metrics.wallFrame.medianOfProcessMedians - [double]$s1.metrics.wallFrame.medianOfProcessMedians) / [double]$s1.metrics.wallFrame.medianOfProcessMedians) * 100.0
            s1ResidentBytes = [int64]$s1.grid.residentBytes
            s8ResidentBytes = [int64]$s8.grid.residentBytes
        }
        bestRelative = $bestRelative
    }
    correctness = [ordered]@{
        csrCellsVerified = [int]$independent.csrCellsVerified
        truthInteractions = [int64]$independent.fullImageMembership.totalGroundTruthInteractions
        allMembershipMissesZero = [bool]$independent.fullImageMembership.allMissesZero
        endpointCompatibility = $independent.endpointCompatibility
        cacheSliceSwitchPassed = [bool]$independent.sliceSwitchCacheInvalidation.passed
        qualityCases = $independent.quality.cases
        invalidViewport = [ordered]@{
            requestedResolution = $invalidViewport.requestedResolution
            actualResolution = $invalidViewport.actualResolution
            invalidPixelCount = [int64]$invalidViewport.invalidPixelCount
            invalidPixelRatio = [double]$invalidViewport.invalidPixelRatio
            s1S8OutputExact = [bool]$invalidViewport.s1S8OutputExact
            s1S8GBufferExact = [bool]$invalidViewport.s1S8GBufferExact
        }
        liveResize = [ordered]@{
            requestedInitialResolution = $liveResize.requestedInitialResolution
            resizeSequence = $liveResize.resizeSequence
            finalResolution = $liveResize.finalResolution
            finalGridDimensions = $liveResize.finalGridDimensions
            buildCount = [int64]$liveResize.buildCount
            cacheHitCount = [int64]$liveResize.cacheHitCount
            logicalCells = [int]$liveResize.logicalCells
        }
    }
    renderDoc = [ordered]@{
        success = [bool]$replay.success
        capturedSliceUniform = [int]$replay.fragmentUniforms.gridSliceCount
        pointLightDrawCount = [int]$replay.eventTree.counts.drawActionsInsidePointLightPhase
        tboBindingCount = [int]$replay.fragmentResources.bufferTextureBindingCount
        debugMessageCount = @($replay.debugMessages).Count
        manifest = "renderdoc/renderdoc-manifest.json"
    }
    finalSmoke = [ordered]@{
        generalCases = @($finalSmoke.records).Count
        gridCases = @($gridSmoke.records).Count
        invalidViewportCases = @($invalidViewport.records).Count
        liveResizeCases = 1
        postBuildEndpointCompatibility = $gridSmoke.endpointCompatibility
        defaultRenderMode = "analytic-screen"
        relatedProcessesRemaining = $remainingProcesses
    }
    scope = [ordered]@{
        defaultChanged = $false
        defaultRenderMode = "analytic-screen"
        adaptivePolicyImplemented = $false
        latexModified = $false
    }
}
$finalValidationPath = Join-Path $run "FINAL_VALIDATION.json"
$finalValidation | ConvertTo-Json -Depth 12 | Set-Content -LiteralPath $finalValidationPath -Encoding UTF8

$topLevel = @(
    "PHASE0_FROZEN_PROTOCOL_CN.md",
    "pre-capture-manifest.json",
    "capture-manifest.json",
    "correctness-manifest.json",
    "aggregate.json",
    "summary.csv",
    "REPORT_CN.md",
    "BOUNDARY_SUMMARY_CN.md",
    "FINAL_VALIDATION.json"
) | ForEach-Object { Join-Path $run $_ }
$artifactFiles = @($topLevel)
foreach ($directory in @("charts", "verification", "renderdoc", "final-smoke", "grid-final-smoke", "invalid-viewport-smoke", "live-resize-smoke")) {
    $artifactFiles += @(Get-ChildItem -LiteralPath (Join-Path $run $directory) -File -Recurse | Select-Object -ExpandProperty FullName)
}
$artifactFiles = @($artifactFiles | Sort-Object -Unique)
$artifactManifest = [ordered]@{
    schemaVersion = 1
    valid = $true
    createdUtc = [DateTime]::UtcNow.ToString("o")
    protocolSha256 = [string]$pre.protocolSha256
    executableSha256 = $executableHash
    formalRawEvidence = [ordered]@{
        captureManifest = "capture-manifest.json"
        captureManifestSha256 = (Get-FileHash -LiteralPath (Join-Path $run "capture-manifest.json") -Algorithm SHA256).Hash
        performanceProcesses = 615
        correctnessProcesses = 38
        rawAndCaptureFilesAreEnumeratedByCaptureManifest = $true
    }
    artifactCount = $artifactFiles.Count
    artifacts = @($artifactFiles | ForEach-Object { File-Record $_ })
}
$artifactManifest | ConvertTo-Json -Depth 10 | Set-Content -LiteralPath (Join-Path $run "artifact-manifest.json") -Encoding UTF8

Write-Host "Grid slice experiment finalization PASS: $run"
