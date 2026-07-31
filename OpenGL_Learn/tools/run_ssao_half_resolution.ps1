[CmdletBinding()]
param(
    [ValidateSet("Smoke", "Formal")]
    [string]$Preset = "Smoke",

    [string]$BatchId = (
        "ssao-half-resolution-{0}" -f (Get-Date -Format "yyyyMMdd-HHmmss")),

    [ValidateSet("sponza", "san-miguel")]
    [string[]]$SceneIds = @("sponza", "san-miguel"),

    [ValidateRange(0, 10000000)]
    [int]$WarmupFrames = 0,

    [ValidateRange(0, 10000000)]
    [int]$MeasuredFrames = 0,

    [ValidateRange(0, 100)]
    [int]$Repeats = 0,

    [switch]$SkipBuild,
    [switch]$Resume,
    [switch]$ReportOnly,

    [ValidateSet("pending", "go", "no-go")]
    [string]$Decision = "pending",

    [string]$DecisionReason = "",
    [string]$PythonPath,
    [string]$MsBuildPath
)

$ErrorActionPreference = "Stop"
$ProgressPreference = "SilentlyContinue"

$toolsDirectory = $PSScriptRoot
$projectDirectory = Split-Path -Parent $toolsDirectory
$repositoryDirectory = Split-Path -Parent $projectDirectory
$sceneManifestPath = Join-Path $projectDirectory "classic-scenes.manifest.json"
$sceneManifest =
    Get-Content -LiteralPath $sceneManifestPath -Raw | ConvertFrom-Json
$assetRoot = Join-Path $projectDirectory $sceneManifest.assetRoot
$executablePath =
    Join-Path $repositoryDirectory "x64\Release\OpenGL_Learn.exe"
$presetName = $Preset.ToLowerInvariant()
$experimentRoot = Join-Path $projectDirectory (
    "benchmark-results\ssao-half-resolution\$BatchId\$presetName")
$rawRoot = Join-Path $experimentRoot "raw"
$captureRoot = Join-Path $experimentRoot "captures"
$logRoot = Join-Path $experimentRoot "logs"
$checkpointRoot = Join-Path $experimentRoot "source-checkpoint"
$runManifestPath = Join-Path $experimentRoot "run-manifest.json"
$reportScript =
    Join-Path $toolsDirectory "generate_ssao_half_resolution_report.py"

# Keep this table authoritative. CLI construction, validation, ordering, and the
# report manifest all derive from it so mode-name/schema changes stay localized.
$configurations = @(
    [pscustomobject][ordered]@{
        name = "legacy-full64"
        mode = "legacy-full"
        samples = 64
        halfResolution = $false
        bilateral = $false
    },
    [pscustomobject][ordered]@{
        name = "half-raw64"
        mode = "half-raw"
        samples = 64
        halfResolution = $true
        bilateral = $false
    },
    [pscustomobject][ordered]@{
        name = "half-bilateral64"
        mode = "half-bilateral"
        samples = 64
        halfResolution = $true
        bilateral = $true
    },
    [pscustomobject][ordered]@{
        name = "legacy-full32"
        mode = "legacy-full"
        samples = 32
        halfResolution = $false
        bilateral = $false
    },
    [pscustomobject][ordered]@{
        name = "half-bilateral32"
        mode = "half-bilateral"
        samples = 32
        halfResolution = $true
        bilateral = $true
    }
)

if ($WarmupFrames -eq 0) {
    $WarmupFrames = if ($Preset -eq "Formal") { 300 } else { 30 }
}
if ($MeasuredFrames -eq 0) {
    $MeasuredFrames = if ($Preset -eq "Formal") { 2000 } else { 120 }
}
if ($Repeats -eq 0) {
    $Repeats = if ($Preset -eq "Formal") { 3 } else { 1 }
}
if ($Preset -eq "Formal" -and
    ($WarmupFrames -ne 300 -or
        $MeasuredFrames -ne 2000 -or
        $Repeats -ne 3)) {
    throw "Formal runs require exactly 300 warmup, 2000 measured, and 3 processes."
}
if ($Decision -ne "pending" -and $Preset -ne "Formal") {
    throw "Go/No-Go may only be attached to a Formal run."
}
if ($Decision -ne "pending" -and
    [string]::IsNullOrWhiteSpace($DecisionReason)) {
    throw "Go/No-Go requires a non-empty, data-backed DecisionReason."
}

function Resolve-Python {
    $candidates = @(
        $PythonPath,
        (Join-Path $HOME (
            ".cache\codex-runtimes\codex-primary-runtime\" +
            "dependencies\python\python.exe"))
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
        & $candidate -c "import numpy; from PIL import Image, ImageDraw, ImageFont" `
            2>$null
        if ($LASTEXITCODE -eq 0) {
            return (Resolve-Path -LiteralPath $candidate).Path
        }
    }
    throw "Python with NumPy and Pillow was not found. Pass -PythonPath."
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
        if ($candidate -and
            (Test-Path -LiteralPath $candidate -PathType Leaf)) {
            return (Resolve-Path -LiteralPath $candidate).Path
        }
    }
    throw "MSBuild 2022 was not found. Pass -MsBuildPath."
}

function Format-Invariant {
    param([double]$Value)
    return $Value.ToString(
        "R",
        [System.Globalization.CultureInfo]::InvariantCulture)
}

function Get-RelativeProjectPath {
    param([string]$Path)
    $projectPrefix =
        [System.IO.Path]::GetFullPath($projectDirectory).TrimEnd("\") + "\"
    $fullPath = [System.IO.Path]::GetFullPath($Path)
    if (-not $fullPath.StartsWith(
            $projectPrefix,
            [System.StringComparison]::OrdinalIgnoreCase)) {
        throw "Path is outside the project directory: $fullPath"
    }
    return $fullPath.Substring($projectPrefix.Length).Replace("\", "/")
}

function Get-ProjectAbsolutePath {
    param([string]$RelativePath)
    if ([string]::IsNullOrWhiteSpace($RelativePath) -or
        [System.IO.Path]::IsPathRooted($RelativePath)) {
        throw "Expected a non-empty project-relative path: $RelativePath"
    }
    $projectPrefix =
        [System.IO.Path]::GetFullPath($projectDirectory).TrimEnd("\") + "\"
    $fullPath = [System.IO.Path]::GetFullPath(
        (Join-Path $projectDirectory $RelativePath.Replace("/", "\")))
    if (-not $fullPath.StartsWith(
            $projectPrefix,
            [System.StringComparison]::OrdinalIgnoreCase)) {
        throw "Manifest path is outside the project directory: $RelativePath"
    }
    return $fullPath
}

function Get-FileSha256 {
    param([string]$Path)
    return (
        Get-FileHash -Algorithm SHA256 -LiteralPath $Path
    ).Hash.ToLowerInvariant()
}

function Get-TextSha256 {
    param([string]$Text)
    $sha256 = [System.Security.Cryptography.SHA256]::Create()
    try {
        $bytes = [System.Text.UTF8Encoding]::new($false).GetBytes($Text)
        $hash = $sha256.ComputeHash($bytes)
        return ([System.BitConverter]::ToString($hash) -replace "-", "").
            ToLowerInvariant()
    }
    finally {
        $sha256.Dispose()
    }
}

function Get-NestedValue {
    param(
        [object]$Object,
        [string]$Path
    )
    $current = $Object
    foreach ($part in $Path.Split(".")) {
        if ($null -eq $current) {
            return $null
        }
        $property = $current.PSObject.Properties[$part]
        if ($null -eq $property) {
            return $null
        }
        $current = $property.Value
    }
    return $current
}

function Get-FirstNestedValue {
    param(
        [object]$Object,
        [string[]]$Paths
    )
    foreach ($path in $Paths) {
        $value = Get-NestedValue -Object $Object -Path $path
        if ($null -ne $value) {
            return $value
        }
    }
    return $null
}

function Get-ZoneCount {
    param(
        [object]$Result,
        [ValidateSet("cpuZones", "gpuZones")]
        [string]$Kind,
        [string]$Name
    )
    $zone = Get-NestedValue `
        -Object $Result `
        -Path ("profiler.summary.{0}.{1}" -f $Kind, $Name)
    if ($null -eq $zone) {
        return 0
    }
    return [int]$zone.count
}

function Get-ZoneSamples {
    param(
        [object]$Result,
        [ValidateSet("cpuZones", "gpuZones")]
        [string]$Kind,
        [string]$Name
    )
    $zones = Get-NestedValue `
        -Object $Result `
        -Path ("profiler.samples.{0}" -f $Kind)
    if ($null -eq $zones) {
        return @()
    }
    $property = $zones.PSObject.Properties[$Name]
    if ($null -eq $property -or $null -eq $property.Value) {
        return @()
    }
    return @($property.Value)
}

function Test-NestedTiming {
    param(
        [double[]]$Total,
        [double[]]$Generate,
        [double[]]$Upsample
    )
    if ($Total.Count -ne $Generate.Count) {
        return $false
    }
    for ($index = 0; $index -lt $Total.Count; ++$index) {
        if ($Total[$index] -le 0.0 -or
            $Generate[$index] -le 0.0 -or
            $Generate[$index] -gt $Total[$index] + 0.005) {
            return $false
        }
        if ($Upsample.Count -gt 0) {
            if ($Upsample.Count -ne $Total.Count -or
                $Upsample[$index] -le 0.0 -or
                $Upsample[$index] -gt $Total[$index] + 0.005 -or
                $Generate[$index] + $Upsample[$index] -gt
                    $Total[$index] + 0.010) {
                return $false
            }
        }
    }
    return $true
}

function Test-ValidResult {
    param(
        [string]$Path,
        [object]$Configuration,
        [int]$ExpectedFrames
    )
    if (-not (Test-Path -LiteralPath $Path -PathType Leaf)) {
        return $false
    }
    try {
        $result = Get-Content -LiteralPath $Path -Raw | ConvertFrom-Json
        if (-not [bool]$result.success -or
            [int]$result.schemaVersion -lt 21 -or
            [string]$result.buildConfiguration -ne "Release" -or
            [string]$result.architecture -ne "x64" -or
            [int]$result.resolution[0] -ne 1920 -or
            [int]$result.resolution[1] -ne 1080 -or
            [string]$result.renderPath -ne "pbr-deferred" -or
            [string]$result.frameMeasurement -ne "cpu-submission-wall" -or
            [string]$result.ssao.mode -ne [string]$Configuration.mode -or
            [int]$result.ssao.requestedSamples -ne
                [int]$Configuration.samples -or
            [int]$result.ssao.kernelSize -ne [int]$Configuration.samples -or
            -not [bool]$result.ssao.enabled -or
            -not [bool]$result.profiler.gpuTimingSupported) {
            return $false
        }

        $expectedWidth = if ([bool]$Configuration.halfResolution) {
            960
        }
        else {
            1920
        }
        $expectedHeight = if ([bool]$Configuration.halfResolution) {
            540
        }
        else {
            1080
        }
        $expectedOutputWidth = if (
            [string]$Configuration.mode -eq "half-raw") {
            960
        }
        else {
            1920
        }
        $expectedOutputHeight = if (
            [string]$Configuration.mode -eq "half-raw") {
            540
        }
        else {
            1080
        }
        $generateWidth = Get-FirstNestedValue $result @(
            "ssao.generate.width",
            "ssao.generate.outputWidth")
        $generateHeight = Get-FirstNestedValue $result @(
            "ssao.generate.height",
            "ssao.generate.outputHeight")
        $generateFormat = Get-FirstNestedValue $result @(
            "ssao.generate.internalFormatName",
            "ssao.generate.outputInternalFormatName",
            "ssao.generate.format")
        $outputWidth = Get-FirstNestedValue $result @(
            "ssao.output.width",
            "ssao.output.outputWidth")
        $outputHeight = Get-FirstNestedValue $result @(
            "ssao.output.height",
            "ssao.output.outputHeight")
        $outputFormat = Get-FirstNestedValue $result @(
            "ssao.output.internalFormatName",
            "ssao.output.outputInternalFormatName",
            "ssao.output.format")
        $upsampleEnabled = Get-FirstNestedValue $result @(
            "ssao.upsample.enabled",
            "ssao.upsampleEnabled")
        if ([int]$generateWidth -ne $expectedWidth -or
            [int]$generateHeight -ne $expectedHeight -or
            [string]$generateFormat -ne "GL_R16F" -or
            [int]$outputWidth -ne $expectedOutputWidth -or
            [int]$outputHeight -ne $expectedOutputHeight -or
            [string]$outputFormat -ne "GL_R16F" -or
            [bool]$upsampleEnabled -ne [bool]$Configuration.bilateral) {
            return $false
        }
        if ([bool]$Configuration.bilateral) {
            $algorithm = [string](Get-FirstNestedValue $result @(
                "ssao.upsample.algorithm",
                "ssao.upsample.name"))
            if ($algorithm -notmatch "(?i)bilateral" -or
                $algorithm -notmatch "(?i)depth" -or
                $algorithm -notmatch "(?i)normal") {
                return $false
            }
        }

        $requiredBase = @(
            "profiler.samples.wallFrame",
            "profiler.samples.cpuFrame",
            "profiler.samples.gpuFrame",
            "profiler.samples.drawCalls")
        foreach ($pathName in $requiredBase) {
            if (@(Get-NestedValue $result $pathName).Count -ne $ExpectedFrames) {
                return $false
            }
        }
        foreach ($kind in @("cpuZones", "gpuZones")) {
            foreach ($zoneName in @(
                    "Deferred Pass",
                    "SSAO Pass",
                    "SSAO Generate")) {
                if ((Get-ZoneCount $result $kind $zoneName) -ne
                        $ExpectedFrames -or
                    @(Get-ZoneSamples $result $kind $zoneName).Count -ne
                        $ExpectedFrames) {
                    return $false
                }
            }
            $expectedUpsample = if ([bool]$Configuration.bilateral) {
                $ExpectedFrames
            }
            else {
                0
            }
            if ((Get-ZoneCount $result $kind "SSAO Upsample") -ne
                    $expectedUpsample -or
                @(Get-ZoneSamples $result $kind "SSAO Upsample").Count -ne
                    $expectedUpsample) {
                return $false
            }
        }

        $gpuTotal = [double[]]@(Get-ZoneSamples $result "gpuZones" "SSAO Pass")
        $gpuGenerate =
            [double[]]@(Get-ZoneSamples $result "gpuZones" "SSAO Generate")
        $gpuUpsample =
            [double[]]@(Get-ZoneSamples $result "gpuZones" "SSAO Upsample")
        $cpuTotal = [double[]]@(Get-ZoneSamples $result "cpuZones" "SSAO Pass")
        $cpuGenerate =
            [double[]]@(Get-ZoneSamples $result "cpuZones" "SSAO Generate")
        $cpuUpsample =
            [double[]]@(Get-ZoneSamples $result "cpuZones" "SSAO Upsample")
        if (-not (Test-NestedTiming $gpuTotal $gpuGenerate $gpuUpsample) -or
            -not (Test-NestedTiming $cpuTotal $cpuGenerate $cpuUpsample)) {
            return $false
        }
        return $true
    }
    catch {
        return $false
    }
}

function Get-ProvenanceCaptureRecords {
    param([object[]]$CapturePaths)
    return @(
        foreach ($capturePath in @($CapturePaths)) {
            [ordered]@{
                path = Get-RelativeProjectPath ([string]$capturePath)
                sha256 = Get-FileSha256 ([string]$capturePath)
            }
        })
}

function Test-RunProvenance {
    param(
        [string]$Path,
        [string]$ExpectedScene,
        [string]$ExpectedConfiguration,
        [int]$ExpectedProcess,
        [string]$ExpectedExecutableSha256,
        [string]$ExpectedSourceFingerprint,
        [string]$ResultPath,
        [object[]]$CapturePaths
    )
    if (-not (Test-Path -LiteralPath $Path -PathType Leaf)) {
        return $false
    }
    try {
        $provenance =
            Get-Content -LiteralPath $Path -Raw | ConvertFrom-Json
        if ([int]$provenance.schemaVersion -ne 1 -or
            [string]$provenance.scene -ne $ExpectedScene -or
            [string]$provenance.configuration -ne
                $ExpectedConfiguration -or
            [int]$provenance.process -ne $ExpectedProcess -or
            [string]$provenance.executableSha256 -ne
                $ExpectedExecutableSha256 -or
            [string]$provenance.sourceCheckpointFingerprint -ne
                $ExpectedSourceFingerprint) {
            return $false
        }

        $expectedResultPath = Get-RelativeProjectPath $ResultPath
        if ([string]$provenance.result.path -ne $expectedResultPath -or
            [string]$provenance.result.sha256 -ne
                (Get-FileSha256 $ResultPath)) {
            return $false
        }

        $expectedCaptures = @($CapturePaths)
        $actualCaptures = if ($null -eq $provenance.captures -or
            ($provenance.captures -is [pscustomobject] -and
                @($provenance.captures.PSObject.Properties).Count -eq 0)) {
            @()
        }
        else {
            @($provenance.captures)
        }
        if ($actualCaptures.Count -ne $expectedCaptures.Count) {
            return $false
        }
        for ($index = 0; $index -lt $expectedCaptures.Count; $index++) {
            $capturePath = [string]$expectedCaptures[$index]
            if ([string]$actualCaptures[$index].path -ne
                    (Get-RelativeProjectPath $capturePath) -or
                [string]$actualCaptures[$index].sha256 -ne
                    (Get-FileSha256 $capturePath)) {
                return $false
            }
        }
        return $true
    }
    catch {
        return $false
    }
}

function Write-RunProvenance {
    param(
        [string]$Path,
        [string]$Scene,
        [string]$Configuration,
        [int]$Process,
        [string]$ExecutableSha256,
        [string]$SourceFingerprint,
        [string]$ResultPath,
        [object[]]$CapturePaths
    )
    $captureRecords = @(
        Get-ProvenanceCaptureRecords -CapturePaths $CapturePaths)
    $provenance = [ordered]@{
        schemaVersion = 1
        generatedAtUtc =
            [DateTime]::UtcNow.ToString("yyyy-MM-ddTHH:mm:ssZ")
        scene = $Scene
        configuration = $Configuration
        process = $Process
        executableSha256 = $ExecutableSha256
        sourceCheckpointFingerprint = $SourceFingerprint
        result = [ordered]@{
            path = Get-RelativeProjectPath $ResultPath
            sha256 = Get-FileSha256 $ResultPath
        }
        captures = @($captureRecords)
    }
    $temporaryPath = "{0}.tmp-{1}-{2}" -f
        $Path,
        $PID,
        [Guid]::NewGuid().ToString("N")
    try {
        $json = $provenance | ConvertTo-Json -Depth 8
        [System.IO.File]::WriteAllText(
            $temporaryPath,
            $json,
            [System.Text.UTF8Encoding]::new($false))
        if (Test-Path -LiteralPath $Path -PathType Leaf) {
            [System.IO.File]::Replace($temporaryPath, $Path, $null)
        }
        else {
            [System.IO.File]::Move($temporaryPath, $Path)
        }
    }
    finally {
        if (Test-Path -LiteralPath $temporaryPath -PathType Leaf) {
            Remove-Item -LiteralPath $temporaryPath -Force
        }
    }
}

function Get-CheckpointSnapshotIdentity {
    param([string]$CheckpointDirectory)
    $checkpointPath = Join-Path $CheckpointDirectory "checkpoint.json"
    if (-not (Test-Path -LiteralPath $checkpointPath -PathType Leaf)) {
        throw "Checkpoint metadata is missing: $checkpointPath"
    }
    $checkpoint =
        Get-Content -LiteralPath $checkpointPath -Raw | ConvertFrom-Json
    if ([string]$checkpoint.fingerprintMethod -notlike "canonical-v1:*" -or
        [string]::IsNullOrWhiteSpace(
            [string]$checkpoint.fingerprintSha256) -or
        [string]::IsNullOrWhiteSpace([string]$checkpoint.gitCommit)) {
        throw "Checkpoint metadata has no supported canonical fingerprint."
    }

    $snapshotRoot = Join-Path $CheckpointDirectory "files"
    $snapshotPrefix =
        [System.IO.Path]::GetFullPath($snapshotRoot).TrimEnd("\") + "\"
    $fingerprintLines = @("gitCommit=$($checkpoint.gitCommit)")
    foreach ($file in @($checkpoint.files | Sort-Object {
                [string]$_.path
            })) {
        $snapshotPath = [System.IO.Path]::GetFullPath(
            (Join-Path $snapshotRoot ([string]$file.path).Replace("/", "\")))
        if (-not $snapshotPath.StartsWith(
                $snapshotPrefix,
                [System.StringComparison]::OrdinalIgnoreCase) -or
            -not (Test-Path -LiteralPath $snapshotPath -PathType Leaf)) {
            throw "Checkpoint file is missing or outside its snapshot: $($file.path)"
        }
        $snapshotSha256 = Get-FileSha256 $snapshotPath
        if ($snapshotSha256 -ne [string]$file.sha256) {
            throw "Checkpoint snapshot hash mismatch: $($file.path)"
        }
        $fingerprintLines +=
            "file={0}:{1}" -f ([string]$file.path), $snapshotSha256
    }
    $fingerprintSha256 =
        Get-TextSha256 ($fingerprintLines -join "`n")
    if ($fingerprintSha256 -ne [string]$checkpoint.fingerprintSha256) {
        throw "Checkpoint canonical fingerprint does not match its snapshot."
    }
    return [pscustomobject]@{
        gitCommit = [string]$checkpoint.gitCommit
        fingerprintSha256 = $fingerprintSha256
    }
}

function Assert-ManifestProvenance {
    param([object]$Manifest)
    if ([int]$Manifest.provenanceSchemaVersion -ne 1) {
        $hasNewProvenance =
            -not [string]::IsNullOrWhiteSpace(
                [string]$Manifest.source.checkpointFingerprintSha256) -or
            @($Manifest.runs | Where-Object {
                    -not [string]::IsNullOrWhiteSpace(
                        [string]$_.provenance)
                }).Count -gt 0
        if ($hasNewProvenance) {
            throw "Manifest contains provenance fields without a supported schema marker."
        }
        Write-Warning (
            "Run manifest predates per-run provenance; report-only " +
            "compatibility mode cannot verify executable/source identity.")
        return
    }

    $sourceFingerprint =
        [string]$Manifest.source.checkpointFingerprintSha256
    $executableSha256 = [string]$Manifest.source.releaseExecutableSha256
    if ([string]::IsNullOrWhiteSpace($sourceFingerprint) -or
        [string]::IsNullOrWhiteSpace($executableSha256)) {
        throw "Provenance-enabled manifest is missing source/executable identity."
    }
    $checkpointIdentity = Get-CheckpointSnapshotIdentity (
        Get-ProjectAbsolutePath ([string]$Manifest.source.checkpoint))
    if ($checkpointIdentity.fingerprintSha256 -ne $sourceFingerprint -or
        $checkpointIdentity.gitCommit -ne [string]$Manifest.source.gitCommit) {
        throw "Manifest source identity disagrees with its checkpoint snapshot."
    }

    foreach ($run in @($Manifest.runs)) {
        if ([string]$run.sourceCheckpointFingerprint -ne
                $sourceFingerprint -or
            [string]$run.executableSha256 -ne $executableSha256) {
            throw "Run identity disagrees with the manifest: $($run.result)"
        }
        $capturePaths = @()
        if ($null -ne $run.captures) {
            foreach ($property in $run.captures.PSObject.Properties) {
                if (-not [string]::IsNullOrWhiteSpace(
                        [string]$property.Value)) {
                    $capturePaths +=
                        Get-ProjectAbsolutePath ([string]$property.Value)
                }
            }
        }
        $valid = Test-RunProvenance `
            -Path (Get-ProjectAbsolutePath ([string]$run.provenance)) `
            -ExpectedScene ([string]$run.scene) `
            -ExpectedConfiguration ([string]$run.configuration) `
            -ExpectedProcess ([int]$run.process) `
            -ExpectedExecutableSha256 $executableSha256 `
            -ExpectedSourceFingerprint $sourceFingerprint `
            -ResultPath (Get-ProjectAbsolutePath ([string]$run.result)) `
            -CapturePaths $capturePaths
        if (-not $valid) {
            throw "Per-run provenance validation failed: $($run.result)"
        }
    }
}

function Get-RunOrder {
    param([int]$Repeat)
    $base = @($configurations | ForEach-Object { $_ })
    if (($Repeat % 3) -eq 2) {
        [array]::Reverse($base)
        return $base
    }
    if (($Repeat % 3) -eq 0) {
        $offset = 2
        return @(
            $base[$offset..($base.Count - 1)] +
            $base[0..($offset - 1)])
    }
    return $base
}

# All capture CLI assumptions live here so the runner can be aligned with the
# executable without touching experiment/validation logic.
function Get-CaptureArguments {
    param(
        [object]$Configuration,
        [string]$FinalPath,
        [string]$AoPpmPath,
        [string]$AoPfmPath,
        [string]$RawHalfPfmPath,
        [string]$DepthPfmPath,
        [string]$NormalPfmPath
    )
    $arguments = @(
        "--classic-scene-capture",
        (Get-RelativeProjectPath $FinalPath),
        "--classic-scene-ssao-capture",
        (Get-RelativeProjectPath $AoPpmPath),
        "--classic-scene-ssao-float-capture",
        (Get-RelativeProjectPath $AoPfmPath)
    )
    if ([bool]$Configuration.bilateral) {
        $arguments += @(
            "--classic-scene-ssao-raw-float-capture",
            (Get-RelativeProjectPath $RawHalfPfmPath))
    }
    if ([string]$Configuration.name -eq "legacy-full64") {
        $arguments += @(
            "--classic-scene-ssao-depth-capture",
            (Get-RelativeProjectPath $DepthPfmPath),
            "--classic-scene-ssao-normal-capture",
            (Get-RelativeProjectPath $NormalPfmPath))
    }
    return $arguments
}

function Save-SourceCheckpoint {
    New-Item -ItemType Directory -Path $checkpointRoot -Force | Out-Null
    $relativeFiles = @(
        "OpenGL_Learn/SSAORenderPass.h",
        "OpenGL_Learn/SSAORenderPass.cpp",
        "OpenGL_Learn/DeferRenderPass.h",
        "OpenGL_Learn/DeferRenderPass.cpp",
        "OpenGL_Learn/Global.h",
        "OpenGL_Learn/Profiler.h",
        "OpenGL_Learn/Profiler.cpp",
        "OpenGL_Learn/ShaderManager.h",
        "OpenGL_Learn/ShaderManager.cpp",
        "OpenGL_Learn/test.cpp",
        "OpenGL_Learn/OpenGL_Learn.vcxproj",
        "OpenGL_Learn/shaders/ssaoVertex.glsl",
        "OpenGL_Learn/shaders/ssaoFragment.glsl",
        "OpenGL_Learn/shaders/ssaoUpsampleVertex.glsl",
        "OpenGL_Learn/shaders/ssaoUpsampleFragment.glsl",
        "OpenGL_Learn/docs/SSAO_RENDERDOC_CAPTURE.md",
        "OpenGL_Learn/tools/generate_ssao_baseline_report.py",
        "OpenGL_Learn/tools/run_ssao_half_resolution.ps1",
        "OpenGL_Learn/tools/generate_ssao_half_resolution_report.py")
    $existingRelativeFiles = @(
        $relativeFiles | Where-Object {
            Test-Path -LiteralPath (
                Join-Path $repositoryDirectory $_) -PathType Leaf
        })

    $patchLines = @(
        & git -C $repositoryDirectory diff --binary HEAD -- `
            @existingRelativeFiles)
    [System.IO.File]::WriteAllLines(
        (Join-Path $checkpointRoot "working-tree.patch"),
        [string[]]$patchLines,
        [System.Text.UTF8Encoding]::new($false))
    $statusLines = @(& git -C $repositoryDirectory status --porcelain=v2)
    [System.IO.File]::WriteAllLines(
        (Join-Path $checkpointRoot "git-status-porcelain-v2.txt"),
        [string[]]$statusLines,
        [System.Text.UTF8Encoding]::new($false))

    $snapshotRoot = Join-Path $checkpointRoot "files"
    $fileRecords = @()
    foreach ($relativePath in $existingRelativeFiles) {
        $sourcePath = Join-Path $repositoryDirectory $relativePath
        $destinationPath = Join-Path $snapshotRoot $relativePath
        $destinationDirectory = Split-Path -Parent $destinationPath
        New-Item -ItemType Directory -Path $destinationDirectory -Force |
            Out-Null
        $sourceSha256 = Get-FileSha256 $sourcePath
        Copy-Item -LiteralPath $sourcePath -Destination $destinationPath -Force
        $snapshotSha256 = Get-FileSha256 $destinationPath
        if ($sourceSha256 -ne $snapshotSha256) {
            throw "Source changed while checkpointing: $relativePath"
        }
        $fileRecords += [ordered]@{
            path = $relativePath.Replace("\", "/")
            sha256 = $snapshotSha256
        }
    }
    $gitCommit = (& git -C $repositoryDirectory rev-parse HEAD).Trim()
    $fingerprintLines = @("gitCommit=$gitCommit")
    $fingerprintLines += @(
        $fileRecords |
            Sort-Object { [string]$_.path } |
            ForEach-Object {
                "file={0}:{1}" -f $_.path, $_.sha256
            })
    $sourceFingerprint = Get-TextSha256 ($fingerprintLines -join "`n")
    $checkpoint = [ordered]@{
        generatedAtUtc =
            [DateTime]::UtcNow.ToString("yyyy-MM-ddTHH:mm:ssZ")
        gitCommit = $gitCommit
        gitBranch =
            (& git -C $repositoryDirectory branch --show-current).Trim()
        worktreeDirty =
            @(& git -C $repositoryDirectory status --porcelain).Count -gt 0
        fingerprintMethod =
            "canonical-v1: gitCommit + sorted selected-file path/sha256"
        fingerprintSha256 = $sourceFingerprint
        patch = "working-tree.patch"
        status = "git-status-porcelain-v2.txt"
        files = $fileRecords
    }
    $checkpoint |
        ConvertTo-Json -Depth 8 |
        Set-Content -LiteralPath (
            Join-Path $checkpointRoot "checkpoint.json") -Encoding utf8
    return $checkpoint
}

$python = Resolve-Python

if (-not $ReportOnly) {
    if ((Test-Path -LiteralPath $experimentRoot) -and -not $Resume) {
        throw "Output already exists: $experimentRoot. Use -Resume or a new BatchId."
    }
    New-Item -ItemType Directory -Path $rawRoot -Force | Out-Null
    New-Item -ItemType Directory -Path $captureRoot -Force | Out-Null
    New-Item -ItemType Directory -Path $logRoot -Force | Out-Null

    $sourceCheckpoint = Save-SourceCheckpoint
    if (-not $SkipBuild) {
        $msbuild = Resolve-MsBuild
        Write-Host "Building Release x64..." -ForegroundColor Cyan
        & $msbuild (Join-Path $repositoryDirectory "OpenGL_Learn.sln") `
            /m:1 /p:Configuration=Release /p:Platform=x64 /verbosity:minimal
        if ($LASTEXITCODE -ne 0) {
            throw "Release x64 build failed."
        }
    }
    if (-not (Test-Path -LiteralPath $executablePath -PathType Leaf)) {
        throw "Release executable not found: $executablePath"
    }
    $executableHashBefore = Get-FileSha256 $executablePath

    $selectedScenes = @()
    foreach ($sceneId in $SceneIds) {
        $scene = $sceneManifest.scenes |
            Where-Object { [string]$_.id -eq $sceneId } |
            Select-Object -First 1
        if (-not $scene) {
            throw "Scene '$sceneId' is missing from classic-scenes.manifest.json."
        }
        $modelAbsolute = Join-Path $assetRoot ([string]$scene.modelPath)
        if (-not (Test-Path -LiteralPath $modelAbsolute -PathType Leaf)) {
            throw "Scene asset is missing: $modelAbsolute"
        }
        $selectedScenes += $scene
    }

    $runRecords = [System.Collections.Generic.List[object]]::new()
    $captureFrame = $WarmupFrames + $MeasuredFrames
    foreach ($repeat in 1..$Repeats) {
        foreach ($scene in $selectedScenes) {
            foreach ($configuration in (Get-RunOrder -Repeat $repeat)) {
                $sceneId = [string]$scene.id
                $configurationName = [string]$configuration.name
                $rawDirectory =
                    Join-Path $rawRoot "$sceneId\$configurationName"
                $sceneCaptureDirectory =
                    Join-Path $captureRoot "$sceneId\$configurationName"
                New-Item -ItemType Directory -Path $rawDirectory -Force |
                    Out-Null
                if ($repeat -eq 1) {
                    New-Item `
                        -ItemType Directory `
                        -Path $sceneCaptureDirectory `
                        -Force | Out-Null
                }

                $resultPath = Join-Path $rawDirectory "run-$repeat.json"
                $provenancePath =
                    Join-Path $rawDirectory "run-$repeat.provenance.json"
                $logPath = Join-Path $logRoot (
                    "$sceneId-$configurationName-run-$repeat.log")
                $finalCapturePath =
                    Join-Path $sceneCaptureDirectory "run-1-final.ppm"
                $aoPpmPath =
                    Join-Path $sceneCaptureDirectory "run-1-ao.ppm"
                $aoPfmPath =
                    Join-Path $sceneCaptureDirectory "run-1-ao.pfm"
                $rawHalfPfmPath =
                    Join-Path $sceneCaptureDirectory "run-1-raw-half.pfm"
                $depthPfmPath =
                    Join-Path $sceneCaptureDirectory "run-1-depth.pfm"
                $normalPfmPath =
                    Join-Path $sceneCaptureDirectory "run-1-normal.pfm"
                $captureThisProcess = $repeat -eq 1

                $requiredCaptures = @()
                if ($captureThisProcess) {
                    $requiredCaptures += @(
                        $finalCapturePath,
                        $aoPpmPath,
                        $aoPfmPath)
                    if ([bool]$configuration.bilateral) {
                        $requiredCaptures += $rawHalfPfmPath
                    }
                    if ($configurationName -eq "legacy-full64") {
                        $requiredCaptures += @($depthPfmPath, $normalPfmPath)
                    }
                }
                $capturesExist = $true
                foreach ($capturePath in $requiredCaptures) {
                    if (-not (Test-Path -LiteralPath $capturePath -PathType Leaf)) {
                        $capturesExist = $false
                    }
                }

                if ($Resume -and
                    (Test-ValidResult `
                        -Path $resultPath `
                        -Configuration $configuration `
                        -ExpectedFrames $MeasuredFrames) -and
                    $capturesExist -and
                    (Test-RunProvenance `
                        -Path $provenancePath `
                        -ExpectedScene $sceneId `
                        -ExpectedConfiguration $configurationName `
                        -ExpectedProcess $repeat `
                        -ExpectedExecutableSha256 $executableHashBefore `
                        -ExpectedSourceFingerprint `
                            $sourceCheckpoint.fingerprintSha256 `
                        -ResultPath $resultPath `
                        -CapturePaths $requiredCaptures)) {
                    Write-Host (
                        "Reuse {0} {1} process {2}" -f
                        $sceneId,
                        $configurationName,
                        $repeat) -ForegroundColor DarkGreen
                }
                else {
                    $modelPath = "classic-scenes/" +
                        ([string]$scene.modelPath).Replace("\", "/")
                    $arguments = @(
                        "--classic-scene-test", $modelPath,
                        "--classic-scene-name", $sceneId,
                        "--classic-scene-result",
                            (Get-RelativeProjectPath $resultPath),
                        "--classic-scene-camera",
                            (Format-Invariant $scene.camera[0]),
                            (Format-Invariant $scene.camera[1]),
                            (Format-Invariant $scene.camera[2]),
                        "--classic-scene-target",
                            (Format-Invariant $scene.target[0]),
                            (Format-Invariant $scene.target[1]),
                            (Format-Invariant $scene.target[2]),
                        "--classic-scene-up",
                            (Format-Invariant $scene.up[0]),
                            (Format-Invariant $scene.up[1]),
                            (Format-Invariant $scene.up[2]),
                        "--classic-scene-radius",
                            (Format-Invariant $scene.normalizedRadius),
                        "--classic-scene-world-scale", "1",
                        "--classic-scene-fov",
                            (Format-Invariant $scene.fov),
                        "--classic-scene-render-path", "pbr-deferred",
                        "--classic-scene-width", "1920",
                        "--classic-scene-height", "1080",
                        "--classic-scene-ssao-mode",
                            [string]$configuration.mode,
                        "--classic-scene-ssao-samples",
                            [string]$configuration.samples,
                        "--classic-scene-warmup-frames",
                            [string]$WarmupFrames,
                        "--classic-scene-capture-frame",
                            [string]$captureFrame)
                    if ($captureThisProcess) {
                        $arguments += Get-CaptureArguments `
                            -Configuration $configuration `
                            -FinalPath $finalCapturePath `
                            -AoPpmPath $aoPpmPath `
                            -AoPfmPath $aoPfmPath `
                            -RawHalfPfmPath $rawHalfPfmPath `
                            -DepthPfmPath $depthPfmPath `
                            -NormalPfmPath $normalPfmPath
                    }
                    else {
                        $arguments += "--classic-scene-no-capture"
                    }

                    Write-Host (
                        "Run {0} {1} process {2}/{3}: {4}+{5} frames" -f
                        $sceneId,
                        $configurationName,
                        $repeat,
                        $Repeats,
                        $WarmupFrames,
                        $MeasuredFrames) -ForegroundColor Cyan
                    $runExecutableShaBefore = Get-FileSha256 $executablePath
                    if ($runExecutableShaBefore -ne $executableHashBefore) {
                        throw "Release executable changed before launching a run."
                    }
                    Push-Location $projectDirectory
                    try {
                        & $executablePath @arguments *> $logPath
                        $exitCode = $LASTEXITCODE
                    }
                    finally {
                        Pop-Location
                    }
                    $runExecutableShaAfter = Get-FileSha256 $executablePath
                    if ($runExecutableShaAfter -ne $runExecutableShaBefore) {
                        throw "Release executable changed while a run was active."
                    }
                    if ($exitCode -ne 0) {
                        throw "Run failed with exit code $exitCode. See $logPath"
                    }
                    if (-not (Test-ValidResult `
                            -Path $resultPath `
                            -Configuration $configuration `
                            -ExpectedFrames $MeasuredFrames)) {
                        throw "Run produced an invalid result: $resultPath"
                    }
                    foreach ($capturePath in $requiredCaptures) {
                        if (-not (Test-Path `
                                -LiteralPath $capturePath `
                                -PathType Leaf)) {
                            throw "Required capture is missing: $capturePath"
                        }
                    }
                    Write-RunProvenance `
                        -Path $provenancePath `
                        -Scene $sceneId `
                        -Configuration $configurationName `
                        -Process $repeat `
                        -ExecutableSha256 $executableHashBefore `
                        -SourceFingerprint `
                            $sourceCheckpoint.fingerprintSha256 `
                        -ResultPath $resultPath `
                        -CapturePaths $requiredCaptures
                }

                $runRecords.Add([ordered]@{
                    scene = $sceneId
                    configuration = $configurationName
                    mode = [string]$configuration.mode
                    samples = [int]$configuration.samples
                    process = $repeat
                    result = Get-RelativeProjectPath $resultPath
                    provenance = Get-RelativeProjectPath $provenancePath
                    sourceCheckpointFingerprint =
                        $sourceCheckpoint.fingerprintSha256
                    executableSha256 = $executableHashBefore
                    log = Get-RelativeProjectPath $logPath
                    captures = if ($captureThisProcess) {
                        [ordered]@{
                            finalPpm =
                                Get-RelativeProjectPath $finalCapturePath
                            aoPpm = Get-RelativeProjectPath $aoPpmPath
                            aoPfm = Get-RelativeProjectPath $aoPfmPath
                            rawHalfPfm = if (
                                [bool]$configuration.bilateral) {
                                Get-RelativeProjectPath $rawHalfPfmPath
                            }
                            else {
                                $null
                            }
                            depthPfm = if (
                                $configurationName -eq "legacy-full64") {
                                Get-RelativeProjectPath $depthPfmPath
                            }
                            else {
                                $null
                            }
                            normalPfm = if (
                                $configurationName -eq "legacy-full64") {
                                Get-RelativeProjectPath $normalPfmPath
                            }
                            else {
                                $null
                            }
                        }
                    }
                    else {
                        $null
                    }
                })
            }
        }
    }

    $executableHashAfter = Get-FileSha256 $executablePath
    if ($executableHashBefore -ne $executableHashAfter) {
        throw "Release executable changed during the experiment."
    }
    $cpu = Get-CimInstance Win32_Processor | Select-Object -First 1
    $os = Get-CimInstance Win32_OperatingSystem
    $gpu = Get-CimInstance Win32_VideoController |
        Where-Object { $_.Name -notmatch "Virtual" } |
        Select-Object -First 1

    $runManifest = [ordered]@{
        schemaVersion = 1
        provenanceSchemaVersion = 1
        generatedAtUtc =
            [DateTime]::UtcNow.ToString("yyyy-MM-ddTHH:mm:ssZ")
        batchId = $BatchId
        preset = $presetName
        protocol = [ordered]@{
            warmupFrames = $WarmupFrames
            measuredFrames = $MeasuredFrames
            independentProcesses = $Repeats
            resolution = @(1920, 1080)
            configurations = @(
                $configurations | ForEach-Object {
                    [ordered]@{
                        name = $_.name
                        mode = $_.mode
                        samples = $_.samples
                        halfResolution = $_.halfResolution
                        bilateral = $_.bilateral
                    }
                })
            renderPath = "pbr-deferred"
            requestedSwapInterval = 0
            inputFrozen = $true
            bloom = $false
            shadows = $false
            gammaCorrection = $true
            autoReloadShaders = $false
            autoReloadMaterials = $false
            ssaoRadius = 0.35
            ssaoBias = 0.025
            formats = [ordered]@{
                generate = "GL_R16F"
                output = "GL_R16F"
            }
            halfDimensions = @(960, 540)
            percentileMethod = "nearest-rank"
            processOrder = "forward, reverse, rotated-by-2"
            quality = [ordered]@{
                reference = "legacy-full64"
                halfRawReconstruction =
                    "GL_LINEAR pixel-center mapping with CLAMP_TO_EDGE"
                edgeRelativeDepthThreshold = 0.02
                edgeNormalAngleDegrees = 25.0
                edgeDilationPixels = 3
                ssimWindow = "11x11 uniform"
            }
            kernelMethodCaveat = (
                "The deterministic 64-vector kernel uses radial scale i/64; " +
                "32-sample modes select its prefix. Comparisons between 32 and " +
                "64 therefore change both sample count and radial distribution. " +
                "Only same-sample resolution comparisons isolate resolution.")
        }
        source = [ordered]@{
            gitCommit = $sourceCheckpoint.gitCommit
            gitBranch = $sourceCheckpoint.gitBranch
            worktreeDirty = $sourceCheckpoint.worktreeDirty
            checkpoint = Get-RelativeProjectPath $checkpointRoot
            checkpointFingerprintSha256 =
                $sourceCheckpoint.fingerprintSha256
            releaseExecutableSha256 = $executableHashBefore
            executableUnchangedDuringRuns = $true
        }
        system = [ordered]@{
            os = $os.Caption
            osVersion = $os.Version
            cpu = $cpu.Name
            logicalProcessors = $cpu.NumberOfLogicalProcessors
            gpu = $gpu.Name
            gpuDriverVersion = $gpu.DriverVersion
        }
        scenes = @(
            $selectedScenes | ForEach-Object {
                [ordered]@{
                    id = [string]$_.id
                    displayName = [string]$_.displayName
                    modelPath = [string]$_.modelPath
                    camera = @($_.camera)
                    target = @($_.target)
                    up = @($_.up)
                    normalizedRadius = [double]$_.normalizedRadius
                    worldScale = 1.0
                    fov = [double]$_.fov
                }
            })
        runs = $runRecords
    }
    $runManifest |
        ConvertTo-Json -Depth 12 |
        Set-Content -LiteralPath $runManifestPath -Encoding utf8
}

if (-not (Test-Path -LiteralPath $runManifestPath -PathType Leaf)) {
    throw "Run manifest not found: $runManifestPath"
}
if (-not (Test-Path -LiteralPath $reportScript -PathType Leaf)) {
    throw "Report generator not found: $reportScript"
}
$reportManifest =
    Get-Content -LiteralPath $runManifestPath -Raw | ConvertFrom-Json
Assert-ManifestProvenance -Manifest $reportManifest

$reportArguments = @(
    $reportScript,
    "--input", $experimentRoot,
    "--decision", $Decision)
if (-not [string]::IsNullOrWhiteSpace($DecisionReason)) {
    $reportArguments += @("--decision-reason", $DecisionReason)
}
& $python @reportArguments
if ($LASTEXITCODE -ne 0) {
    throw "SSAO half-resolution report generation failed."
}

Write-Host "SSAO half-resolution experiment complete: $experimentRoot" `
    -ForegroundColor Green
Write-Host (
    "Report: {0}" -f (
        Join-Path $experimentRoot "SSAO_HALF_RESOLUTION_REPORT_CN.md")) `
    -ForegroundColor Green
