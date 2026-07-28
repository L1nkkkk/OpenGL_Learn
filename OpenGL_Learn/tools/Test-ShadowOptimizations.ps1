[CmdletBinding()]
param(
    [switch]$SkipBuild,
    [string]$MsBuildPath,
    [string]$ExecutablePath,
    [string]$BeforeExecutablePath,
    [string]$AfterExecutablePath,
    [string]$ExperimentId = "shadow-optimization",
    [string]$VariantALabel = "control",
    [string]$VariantBLabel = "candidate",
    [string[]]$VariantAArguments = @(),
    [string[]]$VariantBArguments = @(),
    [hashtable]$VariantAEnvironment = @{},
    [hashtable]$VariantBEnvironment = @{},
    [ValidateRange(64, 16384)]
    [int]$Width = 1440,
    [ValidateRange(64, 16384)]
    [int]$Height = 900,
    [ValidateRange(4, 5000)]
    [int]$MeasuredFrames = 1000,
    [ValidateRange(4, 1000)]
    [int]$ExternalWarmupFrames = 60,
    [ValidateRange(1, 300)]
    [int]$InternalWarmupFrames = 15,
    [ValidateRange(1, 3)]
    [int]$FormalRunsPerVariant = 3,
    [switch]$SkipExternalWarmup,
    [ValidateRange(0, 255)]
    [int]$MaximumPixelChannelDelta = 255,
    [ValidateRange(0, 1000000)]
    [int]$MaximumChangedPixels = 32,
    [ValidateSet(
        "static-hit",
        "force-update",
        "move-directional",
        "move-point",
        "move-spot",
        "move-caster",
        "change-caster-material",
        "reload-shadow-2d",
        "reload-shadow-point",
        "resize-point-shadow",
        "replace-point-shadow-target",
        "toggle-caster",
        "timeline-point",
        "timeline-point-camera",
        "timeline-caster",
        "timeline-camera",
        "timeline-mixed"
    )]
    [string]$Workload = "static-hit",
    [ValidateSet("directional", "point", "spot", "all")]
    [string]$Lights = "directional",
    [ValidateSet("hard", "pcf", "pcss")]
    [string]$Mode = "hard",
    [ValidateSet("legacy", "stable")]
    [string]$Sampling = "stable",
    [ValidateSet(
        "pbr-forward",
        "phong-forward",
        "pbr-deferred",
        "phong-deferred",
        "phong-deferred-volume"
    )]
    [string]$RenderPath = "pbr-forward",
    [ValidateSet("sponza", "san-miguel")]
    [string[]]$SceneIds = @("sponza", "san-miguel"),
    [ValidateScript({
        $_ -eq 0 -or ($_ -ge 128 -and $_ -le 4096)
    })]
    [int]$ShadowResolution = 0,
    [ValidateRange(0.05, 2.0)]
    [double]$WorldScale = 1.0,
    [ValidateCount(3, 3)]
    [double[]]$DirectionalLight = @(-0.45, -1.0, -0.25),
    [ValidateScript({
        $_ -eq 0.0 -or $_ -ge 0.001
    })]
    [double]$SpotNearPlane = 0.0,
    [ValidateScript({
        $_ -eq 0.0 -or $_ -ge 0.002
    })]
    [double]$SpotFarPlane = 0.0,
    [double[]]$SpotLight = @(),
    [double[]]$SpotDirection = @(),
    [ValidateRange(1, 1000)]
    [int]$TimelineFps = 60,
    [ValidateRange(4, 36000)]
    [int]$TimelineCycleFrames = 600
)

$ErrorActionPreference = "Stop"
$ProgressPreference = "SilentlyContinue"

if (-not ("OpenGLLearnPpmCaptureComparer" -as [type])) {
    Add-Type -TypeDefinition @'
using System;
using System.IO;

public sealed class OpenGLLearnPpmComparison
{
    public int Width { get; set; }
    public int Height { get; set; }
    public long ChangedPixelCount { get; set; }
    public int MaximumChannelDelta { get; set; }
    public double NormalizedMeanAbsoluteDifference { get; set; }
    public bool Exact { get; set; }
}

public static class OpenGLLearnPpmCaptureComparer
{
    private static string ReadToken(byte[] data, ref int offset)
    {
        while (offset < data.Length)
        {
            byte current = data[offset];
            if (current == '#')
            {
                while (offset < data.Length && data[offset] != '\n')
                {
                    ++offset;
                }
                continue;
            }
            if (!Char.IsWhiteSpace((char)current))
            {
                break;
            }
            ++offset;
        }
        int start = offset;
        while (offset < data.Length &&
               !Char.IsWhiteSpace((char)data[offset]))
        {
            ++offset;
        }
        if (offset == start)
        {
            throw new InvalidDataException("Unexpected end of PPM header.");
        }
        return System.Text.Encoding.ASCII.GetString(
            data,
            start,
            offset - start);
    }

    private static int ReadHeader(
        byte[] data,
        out int width,
        out int height)
    {
        int offset = 0;
        if (ReadToken(data, ref offset) != "P6")
        {
            throw new InvalidDataException("Only binary P6 PPM is supported.");
        }
        width = Int32.Parse(ReadToken(data, ref offset));
        height = Int32.Parse(ReadToken(data, ref offset));
        int maximum = Int32.Parse(ReadToken(data, ref offset));
        if (width <= 0 || height <= 0 || maximum != 255)
        {
            throw new InvalidDataException("Invalid PPM dimensions or range.");
        }
        if (offset >= data.Length ||
            !Char.IsWhiteSpace((char)data[offset]))
        {
            throw new InvalidDataException("PPM header delimiter is missing.");
        }
        if (data[offset] == '\r' &&
            offset + 1 < data.Length &&
            data[offset + 1] == '\n')
        {
            offset += 2;
        }
        else
        {
            ++offset;
        }
        return offset;
    }

    public static OpenGLLearnPpmComparison Compare(
        string beforePath,
        string afterPath)
    {
        byte[] before = File.ReadAllBytes(beforePath);
        byte[] after = File.ReadAllBytes(afterPath);
        int beforeWidth;
        int beforeHeight;
        int afterWidth;
        int afterHeight;
        int beforeOffset = ReadHeader(
            before,
            out beforeWidth,
            out beforeHeight);
        int afterOffset = ReadHeader(
            after,
            out afterWidth,
            out afterHeight);
        if (beforeWidth != afterWidth || beforeHeight != afterHeight)
        {
            throw new InvalidDataException("PPM dimensions differ.");
        }
        long channelCount =
            checked((long)beforeWidth * beforeHeight * 3L);
        if (before.Length - beforeOffset != channelCount ||
            after.Length - afterOffset != channelCount)
        {
            throw new InvalidDataException("PPM payload length is invalid.");
        }

        long changedPixels = 0;
        long absoluteDifference = 0;
        int maximumDelta = 0;
        for (long pixel = 0; pixel < channelCount / 3L; ++pixel)
        {
            bool changed = false;
            long pixelOffset = pixel * 3L;
            for (int channel = 0; channel < 3; ++channel)
            {
                int delta = Math.Abs(
                    before[beforeOffset + pixelOffset + channel] -
                    after[afterOffset + pixelOffset + channel]);
                if (delta != 0)
                {
                    changed = true;
                }
                if (delta > maximumDelta)
                {
                    maximumDelta = delta;
                }
                absoluteDifference += delta;
            }
            if (changed)
            {
                ++changedPixels;
            }
        }

        return new OpenGLLearnPpmComparison
        {
            Width = beforeWidth,
            Height = beforeHeight,
            ChangedPixelCount = changedPixels,
            MaximumChannelDelta = maximumDelta,
            NormalizedMeanAbsoluteDifference =
                (double)absoluteDifference / (channelCount * 255.0),
            Exact = changedPixels == 0
        };
    }
}
'@
}

if ($ExperimentId -notmatch "^[A-Za-z0-9][A-Za-z0-9._-]*$") {
    throw "ExperimentId may contain only letters, numbers, dot, underscore, and dash."
}
if ($Workload -eq "move-directional" -and
    $Lights -notin @("directional", "all")) {
    throw "move-directional requires -Lights directional or all."
}
if ($Workload -in @(
        "move-point",
        "timeline-point",
        "timeline-point-camera"
    ) -and
    $Lights -notin @("point", "all")) {
    throw "$Workload requires -Lights point or all."
}
if ($Workload -eq "timeline-mixed" -and $Lights -ne "all") {
    throw "timeline-mixed requires -Lights all."
}
if ($Workload -in @(
        "reload-shadow-point",
        "resize-point-shadow",
        "replace-point-shadow-target"
    ) -and
    $Lights -notin @("point", "all")) {
    throw "$Workload requires -Lights point or all."
}
if ($Workload -eq "move-spot" -and $Lights -notin @("spot", "all")) {
    throw "move-spot requires -Lights spot or all."
}
foreach ($value in @(
    $DirectionalLight +
    $SpotLight +
    $SpotDirection +
    @($SpotNearPlane, $SpotFarPlane)
)) {
    if ([double]::IsNaN([double]$value) -or
        [double]::IsInfinity([double]$value)) {
        throw "Light vectors and Spot shadow planes must be finite."
    }
}
if ([Math]::Sqrt(
        $DirectionalLight[0] * $DirectionalLight[0] +
        $DirectionalLight[1] * $DirectionalLight[1] +
        $DirectionalLight[2] * $DirectionalLight[2]
    ) -lt 0.001) {
    throw "DirectionalLight must be non-zero."
}
if (($SpotNearPlane -eq 0.0) -xor ($SpotFarPlane -eq 0.0)) {
    throw "SpotNearPlane and SpotFarPlane must be provided together."
}
if ($SpotNearPlane -gt 0.0 -and
    $SpotFarPlane -le $SpotNearPlane + 0.001) {
    throw "SpotFarPlane must be greater than SpotNearPlane + 0.001."
}
if ($SpotNearPlane -gt 0.0 -and
    ($SpotNearPlane * $WorldScale -lt 0.001 -or
        ($SpotFarPlane - $SpotNearPlane) * $WorldScale -le 0.001)) {
    throw (
        "WorldScale would move the requested Spot shadow planes below " +
        "the renderer's safe projection limits."
    )
}
if ($SpotLight.Count -notin @(0, 3) -or
    $SpotDirection.Count -notin @(0, 3)) {
    throw "SpotLight and SpotDirection must each contain exactly 3 values."
}
if (($SpotLight.Count -eq 0) -xor ($SpotDirection.Count -eq 0)) {
    throw "SpotLight and SpotDirection must be provided together."
}
if (($SpotNearPlane -gt 0.0 -or $SpotLight.Count -eq 3) -and
    $Lights -notin @("spot", "all")) {
    throw (
        "SpotLight/SpotDirection and Spot shadow planes require " +
        "-Lights spot or all."
    )
}
if ($SpotDirection.Count -eq 3 -and
    [Math]::Sqrt(
        $SpotDirection[0] * $SpotDirection[0] +
        $SpotDirection[1] * $SpotDirection[1] +
        $SpotDirection[2] * $SpotDirection[2]
    ) -lt 0.001) {
    throw "SpotDirection must be non-zero."
}
$reservedVariantArguments = @(
    "--classic-scene-camera",
    "--classic-scene-target",
    "--classic-scene-up",
    "--classic-scene-radius",
    "--classic-scene-fov",
    "--classic-scene-width",
    "--classic-scene-height",
    "--classic-scene-world-scale",
    "--classic-scene-shadow-resolution",
    "--classic-scene-directional-light",
    "--classic-scene-point-light",
    "--classic-scene-spot-light",
    "--classic-scene-spot-direction",
    "--classic-scene-spot-near-plane",
    "--classic-scene-spot-far-plane",
    "--classic-scene-timeline-fps",
    "--classic-scene-timeline-cycle-frames"
)
foreach ($argument in @($VariantAArguments) + @($VariantBArguments)) {
    if ($argument -in $reservedVariantArguments) {
        throw (
            "$argument is controlled by a common script parameter and " +
            "may not appear in VariantAArguments/VariantBArguments."
        )
    }
}
$controlledShadowEnvironmentNames = @(
    "OPENGL_LEARN_SHADOW_CACHE",
    "OPENGL_LEARN_SHADOW_PER_LIGHT_CACHE",
    "OPENGL_LEARN_SHADOW_CASTER_CULLING",
    "OPENGL_LEARN_DIRECTIONAL_SHADOW_FIT",
    "OPENGL_LEARN_DIRECTIONAL_SHADOW_RESOLUTION",
    "OPENGL_LEARN_SHADOW_EXACT_EARLY_OUT",
    "OPENGL_LEARN_SHADOW_PREPARED_POINT_INPUTS",
    "OPENGL_LEARN_SHADOW_ADAPTIVE_POINT_SAMPLES",
    "OPENGL_LEARN_SHADOW_ADAPTIVE_PCSS_FILTER",
    "OPENGL_LEARN_SHADOW_STAGED_BLOCKER",
    "OPENGL_LEARN_SHADOW_HARDWARE_COMPARE",
    "OPENGL_LEARN_SHADOW_HARDWARE_LINEAR",
    "OPENGL_LEARN_SHADOW_HARDWARE_REDUCED_PCF",
    "OPENGL_LEARN_SHADOW_TEXEL_BIAS",
    "OPENGL_LEARN_SHADOW_SPOT_RADIAL_BIAS_DIRECTION",
    "OPENGL_LEARN_SHADOW_SPOT_PCSS_LINEAR_DEPTH",
    "OPENGL_LEARN_SHADOW_SPOT_PCSS_REDUCED_FILTER",
    "OPENGL_LEARN_SHADOW_SPOT_CASTER_DEPTH_FIT",
    "OPENGL_LEARN_SHADOW_BIAS_2D_MIN_TEXELS",
    "OPENGL_LEARN_SHADOW_BIAS_2D_SLOPE_TEXELS",
    "OPENGL_LEARN_SHADOW_BIAS_CUBE_MIN_TEXELS",
    "OPENGL_LEARN_SHADOW_BIAS_CUBE_SLOPE_TEXELS",
    "OPENGL_LEARN_POINT_SHADOW_RENDER_PATH",
    "OPENGL_LEARN_POINT_SHADOW_FACE_CULLING",
    "OPENGL_LEARN_SHADOW_ADAPTIVE_MIN_SAMPLES"
)

$toolsDirectory = $PSScriptRoot
$projectDirectory = Split-Path -Parent $toolsDirectory
$repositoryDirectory = Split-Path -Parent $projectDirectory
$manifestPath = Join-Path $projectDirectory "classic-scenes.manifest.json"
$solutionPath = Join-Path $repositoryDirectory "OpenGL_Learn.sln"
$defaultExecutablePath =
    Join-Path $repositoryDirectory "x64\Release\OpenGL_Learn.exe"
$resultRelativeRoot =
    "benchmark-results/shadow-optimizations/$ExperimentId"
$resultRoot = Join-Path $projectDirectory $resultRelativeRoot
$formalRoot = Join-Path $resultRoot "formal"
$warmupRoot = Join-Path $resultRoot "warmup"

function Get-RelativePathWithin {
    param(
        [string]$BasePath,
        [string]$ChildPath
    )

    $base = [System.IO.Path]::GetFullPath($BasePath).TrimEnd("\", "/") + "\"
    $child = [System.IO.Path]::GetFullPath($ChildPath)
    if (-not $child.StartsWith(
        $base,
        [System.StringComparison]::OrdinalIgnoreCase
    )) {
        throw "$child is not inside $base"
    }
    return $child.Substring($base.Length)
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
    $msbuild = Resolve-MsBuild
    Write-Host "Building Release x64..."
    & $msbuild $solutionPath `
        /m /p:Configuration=Release /p:Platform=x64 /verbosity:minimal
    if ($LASTEXITCODE -ne 0) {
        throw "Release x64 build failed."
    }
}

function Resolve-Executable {
    param([string]$RequestedPath)

    $path = if ($RequestedPath) {
        $RequestedPath
    }
    elseif ($ExecutablePath) {
        $ExecutablePath
    }
    else {
        $defaultExecutablePath
    }
    if (-not (Test-Path -LiteralPath $path -PathType Leaf)) {
        throw "Renderer executable not found: $path"
    }
    return (Resolve-Path -LiteralPath $path).Path
}

function Get-SourceFingerprint {
    $extensions = @(
        ".cpp", ".h", ".glsl", ".vs", ".fs", ".gs",
        ".vcxproj", ".props", ".ps1", ".py"
    )
    $excludedSegments = @(
        "\benchmark-results\",
        "\classic-scenes\",
        "\docs\",
        "\models\"
    )
    $validationAssetPath = [System.IO.Path]::GetFullPath(
        (Join-Path `
            $projectDirectory `
            "models\shadow_bias_probe\shadow_bias_probe.obj")
    )
    $files = @(
        Get-ChildItem -LiteralPath $projectDirectory -Recurse -File |
            Where-Object {
                if ($_.FullName.Equals(
                    $validationAssetPath,
                    [System.StringComparison]::OrdinalIgnoreCase
                )) {
                    return $true
                }
                $extension = $_.Extension.ToLowerInvariant()
                if ($extension -notin $extensions) {
                    return $false
                }
                foreach ($segment in $excludedSegments) {
                    if ($_.FullName.IndexOf(
                        $segment,
                        [System.StringComparison]::OrdinalIgnoreCase) -ge 0) {
                        return $false
                    }
                }
                return $true
            } |
            Sort-Object FullName
    )
    $manifest = [System.Text.StringBuilder]::new()
    foreach ($file in $files) {
        $relativePath = (Get-RelativePathWithin `
            -BasePath $repositoryDirectory `
            -ChildPath $file.FullName
        ).Replace("\", "/")
        $hash = (Get-FileHash -LiteralPath $file.FullName -Algorithm SHA256).Hash
        [void]$manifest.Append($relativePath)
        [void]$manifest.Append("`t")
        [void]$manifest.Append($hash)
        [void]$manifest.Append("`n")
    }
    $sha256 = [System.Security.Cryptography.SHA256]::Create()
    try {
        $bytes = [System.Text.Encoding]::UTF8.GetBytes($manifest.ToString())
        return ([BitConverter]::ToString(
            $sha256.ComputeHash($bytes)
        )).Replace("-", "").ToLowerInvariant()
    }
    finally {
        $sha256.Dispose()
    }
}

function Get-GitMetadata {
    $head = (& git -C $repositoryDirectory rev-parse HEAD 2>$null)
    if ($LASTEXITCODE -ne 0) {
        $head = ""
    }
    $status = @(& git -C $repositoryDirectory status --porcelain 2>$null)
    return [ordered]@{
        head = [string]$head
        dirty = $status.Count -gt 0
    }
}

function Get-MachineMetadata {
    $cpuNames = @()
    $gpuEntries = @()
    try {
        $cpuNames = @(
            Get-CimInstance Win32_Processor |
                ForEach-Object { [string]$_.Name }
        )
        $gpuEntries = @(
            Get-CimInstance Win32_VideoController |
                ForEach-Object {
                    [ordered]@{
                        name = [string]$_.Name
                        driverVersion = [string]$_.DriverVersion
                    }
                }
        )
    }
    catch {
        Write-Warning "CIM hardware metadata was unavailable: $($_.Exception.Message)"
    }
    return [ordered]@{
        os = [System.Environment]::OSVersion.VersionString
        cpu = $cpuNames
        gpu = $gpuEntries
    }
}

function Get-Average {
    param([object[]]$Values)

    $numeric = @(
        $Values |
            Where-Object { $null -ne $_ } |
            ForEach-Object { [double]$_ }
    )
    if ($numeric.Count -eq 0) {
        return $null
    }
    return [double](($numeric | Measure-Object -Average).Average)
}

function New-MetricComparison {
    param(
        [object[]]$AValues,
        [object[]]$BValues
    )

    $a = Get-Average $AValues
    $b = Get-Average $BValues
    $delta = $null
    $deltaPercent = $null
    if ($null -ne $a -and $null -ne $b) {
        $delta = [double]$b - [double]$a
        if ([Math]::Abs([double]$a) -gt 1e-12) {
            $deltaPercent = 100.0 * $delta / [double]$a
        }
    }
    return [ordered]@{
        A = $a
        B = $b
        delta = $delta
        deltaPercent = $deltaPercent
    }
}

function New-PerProcessStatisticComparison {
    param(
        [object[]]$ADistributions,
        [object[]]$BDistributions,
        [ValidateSet("median", "p95", "p99")]
        [string]$Statistic
    )

    $aValues = @(
        foreach ($distribution in @($ADistributions)) {
            if ($null -eq $distribution) {
                continue
            }
            $property = $distribution.PSObject.Properties[$Statistic]
            if ($property -and $null -ne $property.Value) {
                [double]$property.Value
            }
        }
    )
    $bValues = @(
        foreach ($distribution in @($BDistributions)) {
            if ($null -eq $distribution) {
                continue
            }
            $property = $distribution.PSObject.Properties[$Statistic]
            if ($property -and $null -ne $property.Value) {
                [double]$property.Value
            }
        }
    )
    $comparison = New-MetricComparison `
        -AValues $aValues `
        -BValues $bValues
    return [ordered]@{
        AValues = @($aValues)
        BValues = @($bValues)
        A = $comparison.A
        B = $comparison.B
        delta = $comparison.delta
        deltaPercent = $comparison.deltaPercent
    }
}

function New-PerProcessDistributionComparison {
    param(
        [object[]]$ADistributions,
        [object[]]$BDistributions
    )

    return [ordered]@{
        median = New-PerProcessStatisticComparison `
            -ADistributions $ADistributions `
            -BDistributions $BDistributions `
            -Statistic "median"
        p95 = New-PerProcessStatisticComparison `
            -ADistributions $ADistributions `
            -BDistributions $BDistributions `
            -Statistic "p95"
        p99 = New-PerProcessStatisticComparison `
            -ADistributions $ADistributions `
            -BDistributions $BDistributions `
            -Statistic "p99"
    }
}

function Get-FrameDistribution {
    param(
        $Result,
        [ValidateSet("wallFrame", "cpuFrame", "gpuFrame")]
        [string]$Name
    )

    if (-not $Result -or
        -not $Result.profiler -or
        -not $Result.profiler.summary) {
        return $null
    }
    $property = $Result.profiler.summary.PSObject.Properties[$Name]
    if (-not $property) {
        return $null
    }
    return $property.Value
}

function Get-ZoneDistribution {
    param(
        $Result,
        [ValidateSet("cpu", "gpu")]
        [string]$Type,
        [string]$Name
    )

    if (-not $Result -or
        -not $Result.profiler -or
        -not $Result.profiler.summary) {
        return $null
    }
    $zones = if ($Type -eq "cpu") {
        $Result.profiler.summary.cpuZones
    }
    else {
        $Result.profiler.summary.gpuZones
    }
    if (-not $zones) {
        return $null
    }
    $property = $zones.PSObject.Properties[$Name]
    if (-not $property) {
        return $null
    }
    return $property.Value
}

function Get-ZoneMean {
    param(
        $Result,
        [ValidateSet("cpu", "gpu")]
        [string]$Type,
        [string]$Name
    )

    $zones = if ($Type -eq "cpu") {
        $Result.profiler.summary.cpuZones
    }
    else {
        $Result.profiler.summary.gpuZones
    }
    if (-not $zones) {
        return $null
    }
    $property = $zones.PSObject.Properties[$Name]
    if (-not $property -or $null -eq $property.Value.mean) {
        return $null
    }
    return [double]$property.Value.mean
}

function Get-ZoneAmortizedMean {
    param(
        $Result,
        [ValidateSet("cpu", "gpu")]
        [string]$Type,
        [string]$Name
    )

    $zones = if ($Type -eq "cpu") {
        $Result.profiler.summary.cpuZones
    }
    else {
        $Result.profiler.summary.gpuZones
    }
    if (-not $zones) {
        return 0.0
    }
    $property = $zones.PSObject.Properties[$Name]
    if (-not $property -or
        $null -eq $property.Value.mean -or
        $null -eq $property.Value.count) {
        return 0.0
    }
    $measuredFrames = [int]$Result.frameTimeMilliseconds.sampleCount
    if ($measuredFrames -le 0) {
        return 0.0
    }
    return (
        [double]$property.Value.mean *
        [double]$property.Value.count /
        [double]$measuredFrames
    )
}

function Get-OptionalShadowMetric {
    param(
        $Result,
        [string]$Name
    )

    if (-not $Result -or -not $Result.shadow) {
        return $null
    }
    $property = $Result.shadow.PSObject.Properties[$Name]
    if (-not $property -or $null -eq $property.Value) {
        return $null
    }
    return [double]$property.Value
}

function Get-OptionalShadowMetricPerFrame {
    param(
        $Result,
        [string]$Name
    )

    $value = Get-OptionalShadowMetric $Result $Name
    if ($null -eq $value) {
        return $null
    }
    $measuredFrames = [int]$Result.frameTimeMilliseconds.sampleCount
    if ($measuredFrames -le 0) {
        return $null
    }
    return [double]$value / [double]$measuredFrames
}

function Get-DirectionalFitCpuMillisecondsPerFrame {
    param($Result)

    $fitCount = Get-OptionalShadowMetric `
        $Result "measuredDirectionalFitCount"
    if ($null -eq $fitCount) {
        return $null
    }
    if ([double]$fitCount -le 0.0) {
        return 0.0
    }

    $zones = $Result.profiler.summary.cpuZones
    if (-not $zones) {
        return $null
    }
    $property = $zones.PSObject.Properties["Directional Shadow Fit"]
    if (-not $property -or
        $null -eq $property.Value.mean -or
        $null -eq $property.Value.count) {
        return $null
    }
    $measuredFrames = [int]$Result.frameTimeMilliseconds.sampleCount
    if ($measuredFrames -le 0) {
        return $null
    }
    return (
        [double]$property.Value.mean *
        [double]$property.Value.count /
        [double]$measuredFrames
    )
}

function ConvertTo-ExpectedBoolean {
    param(
        [string]$Name,
        [object]$Value
    )

    $normalized = ([string]$Value).Trim().ToLowerInvariant()
    if ($normalized -in @("1", "true", "on", "yes")) {
        return $true
    }
    if ($normalized -in @("0", "false", "off", "no")) {
        return $false
    }
    throw "$Name has an invalid Boolean value: $Value"
}

function Assert-RunResult {
    param(
        $Result,
        $Scene,
        [int]$ExpectedSamples,
        [string]$Label,
        [string]$ExpectedVariant,
        [hashtable]$ExpectedEnvironment
    )

    if (-not $Result.success) {
        throw "$($Scene.displayName) $Label reported failure."
    }
    if ([int]$Result.schemaVersion -lt 4) {
        throw "$($Scene.displayName) $Label did not emit profiler schema 4."
    }
    $requiresSpotSchema13 =
        $SpotNearPlane -gt 0.0 -or
        $SpotLight.Count -eq 3 -or
        $ExpectedEnvironment.ContainsKey(
            "OPENGL_LEARN_SHADOW_SPOT_PCSS_LINEAR_DEPTH"
        )
    $requiresSpotSchema14 =
        $ExpectedEnvironment.ContainsKey(
            "OPENGL_LEARN_SHADOW_SPOT_PCSS_REDUCED_FILTER"
        )
    $requiresSpotSchema15 =
        $ExpectedEnvironment.ContainsKey(
            "OPENGL_LEARN_SHADOW_SPOT_CASTER_DEPTH_FIT"
        )
    $requiresPreparedSchema16 =
        $ExpectedEnvironment.ContainsKey(
            "OPENGL_LEARN_SHADOW_PREPARED_POINT_INPUTS"
        )
    $requiresPerLightSchema17 =
        $ExpectedEnvironment.ContainsKey(
            "OPENGL_LEARN_SHADOW_PER_LIGHT_CACHE"
        )
    if ($requiresPerLightSchema17 -and [int]$Result.schemaVersion -lt 17) {
        throw (
            "$($Scene.displayName) $Label requires profiler schema 17 " +
            "for shadow-cache readiness and fallback telemetry."
        )
    }
    if ($requiresPreparedSchema16 -and [int]$Result.schemaVersion -lt 16) {
        throw (
            "$($Scene.displayName) $Label requires profiler schema 16 " +
            "for the prepared point-input experiment."
        )
    }
    if ($requiresSpotSchema15 -and [int]$Result.schemaVersion -lt 15) {
        throw (
            "$($Scene.displayName) $Label requires profiler schema 15 " +
            "for the Spot caster-depth-fit experiment."
        )
    }
    if ($requiresSpotSchema14 -and [int]$Result.schemaVersion -lt 14) {
        throw (
            "$($Scene.displayName) $Label requires profiler schema 14 " +
            "for the Spot PCSS reduced-filter experiment."
        )
    }
    if ($requiresSpotSchema13 -and [int]$Result.schemaVersion -lt 13) {
        throw (
            "$($Scene.displayName) $Label requires profiler schema 13 " +
            "for the Spot PCSS experiment."
        )
    }
    if ([int64]$Result.triangleCount -ne
        [int64]$Scene.expectedTriangles) {
        throw "$($Scene.displayName) $Label triangle count changed."
    }
    if (-not $Result.resolution -or
        $Result.resolution.Count -ne 2 -or
        [int]$Result.resolution[0] -ne $Width -or
        [int]$Result.resolution[1] -ne $Height) {
        throw (
            "$($Scene.displayName) $Label viewport resolution mismatch: " +
            "expected ${Width}x${Height}."
        )
    }
    if ([string]$Result.renderPath -ne $RenderPath -or
        [string]$Result.shadow.mode -ne $Mode -or
        [string]$Result.shadow.sampling -ne $Sampling -or
        [string]$Result.shadow.lights -ne $Lights -or
        [string]$Result.shadow.workload -ne $Workload -or
        [string]$Result.shadow.variant -ne $ExpectedVariant) {
        throw "$($Scene.displayName) $Label experiment configuration mismatch."
    }
    if ([Math]::Abs(
            [double]$Result.shadow.worldScale - $WorldScale
        ) -gt 1e-6 -or
        [int]$Result.shadow.requestedResolution -ne $ShadowResolution) {
        throw "$($Scene.displayName) $Label scale/resolution mismatch."
    }
    for ($component = 0; $component -lt 3; $component++) {
        if ([Math]::Abs(
                [double]$Result.shadow.directionalLight[$component] -
                [double]$DirectionalLight[$component]
            ) -gt 1e-6) {
            throw "$($Scene.displayName) $Label directional light mismatch."
        }
        if ([Math]::Abs(
                [double]$Result.camera.position[$component] -
                [double]$Scene.camera[$component] * $WorldScale
            ) -gt 1e-5 -or
            [Math]::Abs(
                [double]$Result.camera.target[$component] -
                [double]$Scene.target[$component] * $WorldScale
            ) -gt 1e-5 -or
            [Math]::Abs(
                [double]$Result.camera.up[$component] -
                [double]$Scene.up[$component]
            ) -gt 1e-6) {
            throw "$($Scene.displayName) $Label camera configuration mismatch."
        }
    }
    if ([Math]::Abs(
            [double]$Result.camera.fovDegrees -
            [double]$Scene.fov
        ) -gt 1e-6) {
        throw "$($Scene.displayName) $Label camera FOV mismatch."
    }
    if ($SpotLight.Count -eq 3) {
        $directionLength = [Math]::Sqrt(
            $SpotDirection[0] * $SpotDirection[0] +
            $SpotDirection[1] * $SpotDirection[1] +
            $SpotDirection[2] * $SpotDirection[2]
        )
        for ($component = 0; $component -lt 3; $component++) {
            $expectedPosition =
                [double]$SpotLight[$component] * $WorldScale
            $expectedDirection =
                [double]$SpotDirection[$component] / $directionLength
            if ([Math]::Abs(
                    [double]$Result.shadow.spotLightPosition[$component] -
                    $expectedPosition
                ) -gt 1e-5 -or
                [Math]::Abs(
                    [double]$Result.shadow.spotLightDirection[$component] -
                    $expectedDirection
                ) -gt 1e-5) {
                throw (
                    "$($Scene.displayName) $Label common Spot light " +
                    "configuration mismatch."
                )
            }
        }
    }

    $booleanEnvironmentProperties = [ordered]@{
        "OPENGL_LEARN_SHADOW_PER_LIGHT_CACHE" =
            "perLightCacheEnabled"
        "OPENGL_LEARN_SHADOW_EXACT_EARLY_OUT" =
            "exactEarlyOutEnabled"
        "OPENGL_LEARN_SHADOW_PREPARED_POINT_INPUTS" =
            "preparedPointInputsEnabled"
        "OPENGL_LEARN_SHADOW_ADAPTIVE_POINT_SAMPLES" =
            "adaptivePointSamplesEnabled"
        "OPENGL_LEARN_SHADOW_ADAPTIVE_PCSS_FILTER" =
            "adaptivePcssFilterEnabled"
        "OPENGL_LEARN_SHADOW_STAGED_BLOCKER" =
            "stagedPcssBlockerEnabled"
        "OPENGL_LEARN_SHADOW_HARDWARE_COMPARE" =
            "hardwareDepthCompareEnabled"
        "OPENGL_LEARN_SHADOW_HARDWARE_LINEAR" =
            "hardwareLinearPcfEnabled"
        "OPENGL_LEARN_SHADOW_HARDWARE_REDUCED_PCF" =
            "hardwareReducedPcfEnabled"
        "OPENGL_LEARN_SHADOW_TEXEL_BIAS" =
            "texelScaledBiasEnabled"
        "OPENGL_LEARN_SHADOW_SPOT_RADIAL_BIAS_DIRECTION" =
            "spotRadialBiasDirectionEnabled"
        "OPENGL_LEARN_SHADOW_SPOT_PCSS_LINEAR_DEPTH" =
            "spotPcssLinearDepthEnabled"
        "OPENGL_LEARN_SHADOW_SPOT_PCSS_REDUCED_FILTER" =
            "spotPcssReducedFilterEnabled"
        "OPENGL_LEARN_SHADOW_SPOT_CASTER_DEPTH_FIT" =
            "spotCasterDepthFitEnabled"
        "OPENGL_LEARN_SHADOW_CASTER_CULLING" =
            "casterCullingEnabled"
        "OPENGL_LEARN_POINT_SHADOW_FACE_CULLING" =
            "pointShadowFaceCullingEnabled"
    }
    foreach ($environmentName in $booleanEnvironmentProperties.Keys) {
        if (-not $ExpectedEnvironment.ContainsKey($environmentName)) {
            continue
        }
        $propertyName = $booleanEnvironmentProperties[$environmentName]
        $property = $Result.shadow.PSObject.Properties[$propertyName]
        if (-not $property) {
            throw "$($Scene.displayName) $Label did not record $propertyName."
        }
        $expectedValue = ConvertTo-ExpectedBoolean `
            -Name $environmentName `
            -Value $ExpectedEnvironment[$environmentName]
        if ([bool]$property.Value -ne $expectedValue) {
            throw (
                "$($Scene.displayName) $Label effective $environmentName " +
                "did not match the requested value."
            )
        }
    }
    if ($ExpectedEnvironment.ContainsKey(
            "OPENGL_LEARN_POINT_SHADOW_RENDER_PATH"
        )) {
        $requestedPointPolicy = (
            [string]$ExpectedEnvironment[
                "OPENGL_LEARN_POINT_SHADOW_RENDER_PATH"
            ]
        ).Trim().ToLowerInvariant()
        if ($requestedPointPolicy -in @("geometry", "0")) {
            $requestedPointPolicy = "layered"
        }
        elseif ($requestedPointPolicy -in @("faces", "1")) {
            $requestedPointPolicy = "six-face"
        }
        if ($requestedPointPolicy -notin @(
                "layered",
                "six-face",
                "adaptive"
            )) {
            throw (
                "OPENGL_LEARN_POINT_SHADOW_RENDER_PATH has an invalid " +
                "value: $requestedPointPolicy"
            )
        }
        if (-not $Result.shadow.PSObject.Properties[
                "pointShadowRenderPolicy"
            ] -or
            ([string]$Result.shadow.pointShadowRenderPolicy).
                ToLowerInvariant() -ne $requestedPointPolicy) {
            throw (
                "$($Scene.displayName) $Label effective point-shadow " +
                "render policy did not match the requested value."
            )
        }
    }
    $optimizationFlagBits = [ordered]@{
        "OPENGL_LEARN_SHADOW_EXACT_EARLY_OUT" = 1
        "OPENGL_LEARN_SHADOW_PREPARED_POINT_INPUTS" = 4096
        "OPENGL_LEARN_SHADOW_ADAPTIVE_POINT_SAMPLES" = 2
        "OPENGL_LEARN_SHADOW_ADAPTIVE_PCSS_FILTER" = 4
        "OPENGL_LEARN_SHADOW_STAGED_BLOCKER" = 8
        "OPENGL_LEARN_SHADOW_HARDWARE_COMPARE" = 16
        "OPENGL_LEARN_SHADOW_HARDWARE_LINEAR" = 32
        "OPENGL_LEARN_SHADOW_HARDWARE_REDUCED_PCF" = 64
        "OPENGL_LEARN_SHADOW_TEXEL_BIAS" = 128
        "OPENGL_LEARN_SHADOW_SPOT_RADIAL_BIAS_DIRECTION" = 256
        "OPENGL_LEARN_SHADOW_SPOT_PCSS_LINEAR_DEPTH" = 512
        "OPENGL_LEARN_SHADOW_SPOT_PCSS_REDUCED_FILTER" = 1024
        "OPENGL_LEARN_SHADOW_SPOT_CASTER_DEPTH_FIT" = 2048
    }
    $optimizationFlagsProperty =
        $Result.shadow.PSObject.Properties["optimizationFlags"]
    if (-not $optimizationFlagsProperty) {
        throw (
            "$($Scene.displayName) $Label did not record " +
            "optimizationFlags."
        )
    }
    $actualOptimizationFlags = [int]$optimizationFlagsProperty.Value
    foreach ($environmentName in $optimizationFlagBits.Keys) {
        if (-not $ExpectedEnvironment.ContainsKey($environmentName)) {
            continue
        }
        $expectedEnabled = ConvertTo-ExpectedBoolean `
            -Name $environmentName `
            -Value $ExpectedEnvironment[$environmentName]
        $bit = [int]$optimizationFlagBits[$environmentName]
        $actualEnabled = ($actualOptimizationFlags -band $bit) -ne 0
        if ($actualEnabled -ne $expectedEnabled) {
            throw (
                "$($Scene.displayName) $Label flag bit for " +
                "$environmentName did not match the requested value."
            )
        }
    }

    if ($Lights -in @("spot", "all")) {
        $nearProperty =
            $Result.shadow.PSObject.Properties["spotShadowNearPlane"]
        $farProperty =
            $Result.shadow.PSObject.Properties["spotShadowFarPlane"]
        if (-not $nearProperty -or -not $farProperty) {
            throw (
                "$($Scene.displayName) $Label did not record the effective " +
                "Spot shadow planes."
            )
        }
        $actualNear = [double]$nearProperty.Value
        $actualFar = [double]$farProperty.Value
        if ($actualNear -lt 0.001 -or
            $actualFar -le $actualNear + 0.001) {
            throw (
                "$($Scene.displayName) $Label recorded invalid effective " +
                "Spot shadow planes."
            )
        }
        if ($SpotNearPlane -gt 0.0) {
            $expectedNear = $SpotNearPlane * $WorldScale
            $expectedFar = $SpotFarPlane * $WorldScale
            if ([Math]::Abs($actualNear - $expectedNear) -gt 1e-5 -or
                [Math]::Abs($actualFar - $expectedFar) -gt 1e-5) {
                throw (
                    "$($Scene.displayName) $Label effective Spot shadow " +
                    "planes did not match the common requested values."
                )
            }
        }
        if ($SpotNearPlane -eq 0.0 -and
            $ExpectedEnvironment.ContainsKey(
                "OPENGL_LEARN_SHADOW_SPOT_CASTER_DEPTH_FIT"
            )) {
            $expectedFitValue =
                $ExpectedEnvironment["OPENGL_LEARN_SHADOW_SPOT_CASTER_DEPTH_FIT"]
            $expectedFit = ConvertTo-ExpectedBoolean `
                -Name "OPENGL_LEARN_SHADOW_SPOT_CASTER_DEPTH_FIT" `
                -Value $expectedFitValue
            foreach ($propertyName in @(
                "lastSpotFitProjectionAware",
                "lastSpotFitCandidateCount",
                "lastSpotFitAcceptedCount",
                "lastSpotFitRejectedCount",
                "lastSpotFitLegacyNear",
                "lastSpotFitLegacyFar",
                "lastSpotFitRawNear",
                "lastSpotFitRawFar",
                "lastSpotFitNear",
                "lastSpotFitFar",
                "lastSpotFitDepthSpanReduction",
                "lastSpotFitDepthUtilization",
                "lastSpotFitProjectionDepthScale",
                "lastSpotFitPrecisionGain",
                "lastSpotFitMinimumProjectedCoverageMargin",
                "lastSpotFitLightIndex",
                "lastSpotFitRawNearClipped"
            )) {
                if (-not $Result.shadow.PSObject.Properties[$propertyName]) {
                    throw (
                        "$($Scene.displayName) $Label did not record " +
                        "$propertyName."
                    )
                }
            }
            $projectionAwareApplied =
                [bool]$Result.shadow.lastSpotFitProjectionAware
            if (-not $expectedFit -and $projectionAwareApplied) {
                throw (
                    "$($Scene.displayName) $Label Spot caster-depth-fit " +
                    "ran while its effective switch was disabled."
                )
            }
            if ($expectedFit -and -not $projectionAwareApplied -and
                [int64]$Result.shadow.spotFitFallbackCount -le 0) {
                throw (
                    "$($Scene.displayName) $Label enabled Spot " +
                    "caster-depth-fit without applying it or recording " +
                    "a conservative fallback."
                )
            }
            if ($projectionAwareApplied -and
                ([int64]$Result.shadow.lastSpotFitCandidateCount -le 0 -or
                    [int64]$Result.shadow.lastSpotFitAcceptedCount -le 0 -or
                    [int64]$Result.shadow.lastSpotFitAcceptedCount +
                        [int64]$Result.shadow.lastSpotFitRejectedCount -ne
                    [int64]$Result.shadow.lastSpotFitCandidateCount)) {
                throw (
                    "$($Scene.displayName) $Label recorded inconsistent " +
                    "Spot caster-depth-fit classification counts."
                )
            }
            if ($projectionAwareApplied -and
                ([double]$Result.shadow.lastSpotFitDepthSpanReduction -le 0.0 -or
                    [double]$Result.shadow.lastSpotFitDepthUtilization -le 0.0 -or
                    [double]$Result.shadow.lastSpotFitDepthUtilization -gt
                        1.0001 -or
                    [double]$Result.shadow.lastSpotFitPrecisionGain -le 0.0 -or
                    [double]$Result.shadow.lastSpotFitMinimumProjectedCoverageMargin -lt
                        -0.0001)) {
                throw (
                    "$($Scene.displayName) $Label recorded invalid Spot " +
                    "depth-fit range or coverage telemetry."
                )
            }
            if ($projectionAwareApplied -and
                [int64]$Result.shadow.lastSpotFitLightIndex -ne 0) {
                throw (
                    "$($Scene.displayName) $Label attributed its only Spot " +
                    "fit to an unexpected light index."
                )
            }
        }
    }

    $floatEnvironmentProperties = [ordered]@{
        "OPENGL_LEARN_SHADOW_BIAS_2D_MIN_TEXELS" =
            "bias2DMinTexels"
        "OPENGL_LEARN_SHADOW_BIAS_2D_SLOPE_TEXELS" =
            "bias2DSlopeTexels"
        "OPENGL_LEARN_SHADOW_BIAS_CUBE_MIN_TEXELS" =
            "biasCubeMinTexels"
        "OPENGL_LEARN_SHADOW_BIAS_CUBE_SLOPE_TEXELS" =
            "biasCubeSlopeTexels"
    }
    foreach ($environmentName in $floatEnvironmentProperties.Keys) {
        if (-not $ExpectedEnvironment.ContainsKey($environmentName)) {
            continue
        }
        $propertyName = $floatEnvironmentProperties[$environmentName]
        $property = $Result.shadow.PSObject.Properties[$propertyName]
        if (-not $property) {
            throw "$($Scene.displayName) $Label did not record $propertyName."
        }
        $expectedValue = [double]::Parse(
            [string]$ExpectedEnvironment[$environmentName],
            [Globalization.CultureInfo]::InvariantCulture
        )
        if ([Math]::Abs([double]$property.Value - $expectedValue) -gt 1e-5) {
            throw (
                "$($Scene.displayName) $Label effective $environmentName " +
                "did not match the requested value."
            )
        }
    }
    if ([int]$Result.frameTimeMilliseconds.sampleCount -ne
        $ExpectedSamples) {
        throw "$($Scene.displayName) $Label wall sample count mismatch."
    }
    if ([int]$Result.profiler.summary.cpuFrame.count -ne
        $ExpectedSamples) {
        throw "$($Scene.displayName) $Label CPU frame sample count mismatch."
    }
    if ([bool]$Result.profiler.gpuTimingSupported -and
        [int]$Result.profiler.summary.gpuFrame.count -ne
        $ExpectedSamples) {
        throw "$($Scene.displayName) $Label GPU frame sample count mismatch."
    }
    $shadowMaps = $Result.profiler.summary.cpuZones.PSObject.Properties[
        "Shadow Maps"
    ]
    if (-not $shadowMaps -or
        [int]$shadowMaps.Value.count -ne $ExpectedSamples) {
        throw "$($Scene.displayName) $Label Shadow Maps sample count mismatch."
    }
    $cacheDisabled = $false
    $cacheDisabledProperty =
        $Result.shadow.PSObject.Properties["cacheDisabled"]
    if ($cacheDisabledProperty) {
        $cacheDisabled = [bool]$cacheDisabledProperty.Value
    }
    if ($ExpectedEnvironment.ContainsKey("OPENGL_LEARN_SHADOW_CACHE")) {
        $requestedCacheStrategy =
            ([string]$ExpectedEnvironment["OPENGL_LEARN_SHADOW_CACHE"]).ToLowerInvariant()
        $expectedCacheDisabled = $requestedCacheStrategy -in @(
            "none",
            "disabled",
            "off"
        )
        if (-not $cacheDisabledProperty -or
            $cacheDisabled -ne $expectedCacheDisabled) {
            throw (
                "$($Scene.displayName) $Label effective shadow-cache " +
                "strategy did not match the requested value."
            )
        }
    }
    $timelineWorkloads = @(
        "timeline-point",
        "timeline-point-camera",
        "timeline-caster",
        "timeline-camera",
        "timeline-mixed"
    )
    $timelineProperty =
        $Result.PSObject.Properties["motionTimeline"]
    if (-not $timelineProperty) {
        throw "$($Scene.displayName) $Label did not record motionTimeline."
    }
    $isTimelineWorkload = $Workload -in $timelineWorkloads
    $timeline = $timelineProperty.Value
    if ([bool]$timeline.enabled -ne $isTimelineWorkload) {
        throw "$($Scene.displayName) $Label timeline enabled state mismatch."
    }
    $timelineSamples = @($timeline.samples)
    if ($isTimelineWorkload) {
        $expectedProfile = $Workload.Substring("timeline-".Length)
        $expectedTracks = switch ($Workload) {
            "timeline-point" { @("point") }
            "timeline-point-camera" { @("point", "camera") }
            "timeline-caster" { @("caster") }
            "timeline-camera" { @("camera") }
            "timeline-mixed" { @("point", "caster", "camera") }
        }
        if ([int]$timeline.schemaVersion -ne 1 -or
            [string]$timeline.profile -ne $expectedProfile -or
            [int]$timeline.fixedFramesPerSecond -ne $TimelineFps -or
            [int]$timeline.cycleFrames -ne $TimelineCycleFrames -or
            $timelineSamples.Count -ne $ExpectedSamples -or
            (@($timeline.tracks) -join ",") -ne
                ($expectedTracks -join ",")) {
            throw "$($Scene.displayName) $Label timeline metadata mismatch."
        }
        for ($sampleIndex = 0;
            $sampleIndex -lt $timelineSamples.Count;
            ++$sampleIndex) {
            $sample = $timelineSamples[$sampleIndex]
            $expectedCycleFrame =
                $sampleIndex % $TimelineCycleFrames
            $expectedFixedTime =
                [double]$sampleIndex / [double]$TimelineFps
            $expectedPhase =
                [double]$expectedCycleFrame /
                [double]$TimelineCycleFrames
            if ([int]$sample.measurementFrame -ne $sampleIndex -or
                [int]$sample.timelineFrame -ne $sampleIndex -or
                [int]$sample.cycleFrame -ne $expectedCycleFrame -or
                [Math]::Abs(
                    [double]$sample.fixedTimeSeconds -
                    $expectedFixedTime
                ) -gt 1e-5 -or
                [Math]::Abs(
                    [double]$sample.normalizedPhase -
                    $expectedPhase
                ) -gt 1e-5) {
                throw (
                    "$($Scene.displayName) $Label timeline sample " +
                    "$sampleIndex is not frame-deterministic."
                )
            }
            foreach ($vectorName in @(
                "pointPosition",
                "casterPosition",
                "cameraPosition",
                "cameraTarget"
            )) {
                $vector = @($sample.$vectorName)
                if ($vector.Count -ne 3) {
                    throw (
                        "$($Scene.displayName) $Label timeline " +
                        "$vectorName is not a vec3."
                    )
                }
                foreach ($component in $vector) {
                    if ([double]::IsNaN([double]$component) -or
                        [double]::IsInfinity([double]$component)) {
                        throw (
                            "$($Scene.displayName) $Label timeline " +
                            "$vectorName contains a non-finite value."
                        )
                    }
                }
            }
            if ([Math]::Abs(
                    [double]$sample.wallMilliseconds -
                    [double]$Result.profiler.samples.wallFrame[$sampleIndex]
                ) -gt 1e-5) {
                throw (
                    "$($Scene.displayName) $Label timeline wall sample " +
                    "$sampleIndex is not aligned with profiler samples."
                )
            }
        }
        $timelineCounterMap = [ordered]@{
            "updateCount" = "measuredUpdateCount"
            "cacheHitCount" = "measuredCacheHitCount"
            "lightCacheHitCount" = "measuredLightCacheHitCount"
            "updatedLightCount" = "measuredUpdatedLightCount"
            "directionalLightUpdateCount" =
                "measuredDirectionalLightUpdateCount"
            "pointLightUpdateCount" = "measuredPointLightUpdateCount"
            "pointShadowSubmissionPassCount" =
                "measuredPointShadowSubmissionPassCount"
            "spotLightUpdateCount" = "measuredSpotLightUpdateCount"
            "casterBoundsRebuildCount" =
                "measuredCasterBoundsRebuildCount"
        }
        foreach ($frameField in $timelineCounterMap.Keys) {
            $aggregateField = $timelineCounterMap[$frameField]
            $sum = (
                $timelineSamples |
                    ForEach-Object {
                        [int64]$_.shadow.$frameField
                    } |
                    Measure-Object -Sum
            ).Sum
            if ([int64]$sum -ne
                [int64]$Result.shadow.$aggregateField) {
                throw (
                    "$($Scene.displayName) $Label timeline $frameField " +
                    "does not reconcile with $aggregateField."
                )
            }
        }
    }
    elseif ([string]$timeline.profile -ne "none" -or
        $timelineSamples.Count -ne 0) {
        throw "$($Scene.displayName) $Label emitted unexpected timeline samples."
    }
    $shadowStableWorkload =
        $Workload -in @("static-hit", "timeline-camera")
    if ($shadowStableWorkload) {
        if ($cacheDisabled) {
            if ([int64]$Result.shadow.measuredUpdateCount -ne
                    $ExpectedSamples -or
                [int64]$Result.shadow.measuredCacheHitCount -ne 0) {
                throw (
                    "$($Scene.displayName) $Label uncached stable-shadow " +
                    "invariant failed."
                )
            }
        }
        else {
            if ([int64]$Result.shadow.measuredUpdateCount -ne 0 -or
                [int64]$Result.shadow.measuredCacheHitCount -ne
                $ExpectedSamples) {
                throw (
                    "$($Scene.displayName) $Label stable-shadow cache " +
                    "invariant failed."
                )
            }
            $updatedLightsProperty =
                $Result.shadow.PSObject.Properties["measuredUpdatedLightCount"]
            if ($updatedLightsProperty -and
                [int64]$updatedLightsProperty.Value -ne 0) {
                throw (
                    "$($Scene.displayName) $Label stable-shadow light-update " +
                    "invariant failed."
                )
            }
        }
    }
    else {
        if ([int64]$Result.shadow.measuredUpdateCount -ne
            $ExpectedSamples) {
            throw "$($Scene.displayName) $Label update workload invariant failed."
        }
        if ([bool]$Result.profiler.gpuTimingSupported) {
            $updateZone =
                $Result.profiler.summary.gpuZones.PSObject.Properties[
                    "Shadow Map Update"
                ]
            if (-not $updateZone -or
                [int]$updateZone.Value.count -ne $ExpectedSamples) {
                throw (
                    "$($Scene.displayName) $Label Shadow Map Update " +
                    "sample count mismatch."
                )
            }
        }
        $updatedLightsProperty =
            $Result.shadow.PSObject.Properties["measuredUpdatedLightCount"]
        if ($updatedLightsProperty -and
            [int64]$updatedLightsProperty.Value -lt $ExpectedSamples) {
            throw "$($Scene.displayName) $Label rendered no shadow light in one or more measured frames."
        }
        $candidateProperty =
            $Result.shadow.PSObject.Properties["measuredCasterCandidateCount"]
        $culledProperty =
            $Result.shadow.PSObject.Properties["measuredCasterCulledCount"]
        $drawProperty =
            $Result.shadow.PSObject.Properties["measuredCasterDrawCount"]
        if ($candidateProperty -and $culledProperty -and $drawProperty -and
            [int64]$candidateProperty.Value -ne
                ([int64]$culledProperty.Value + [int64]$drawProperty.Value)) {
            throw "$($Scene.displayName) $Label caster accounting invariant failed."
        }
    }

    $directionalEnabled = $Lights -in @("directional", "all")
    $pointEnabled = $Lights -in @("point", "all")
    $spotEnabled = $Lights -in @("spot", "all")
    $perLightCacheEnabled =
        [bool]$Result.shadow.perLightCacheEnabled
    $expectedDirectionalUpdates = 0L
    $expectedPointUpdates = 0L
    $expectedSpotUpdates = 0L
    if (-not $shadowStableWorkload -or -not $perLightCacheEnabled) {
        $updateEveryEnabledLight =
            $Workload -in @(
                "force-update",
                "move-caster",
                "change-caster-material",
                "toggle-caster",
                "timeline-caster",
                "timeline-mixed"
            ) -or
            -not $perLightCacheEnabled
        $expectedDirectionalUpdates = if (
            $directionalEnabled -and
            ($updateEveryEnabledLight -or
                $Workload -in @(
                    "move-directional",
                    "reload-shadow-2d"
                ))
        ) {
            [int64]$ExpectedSamples
        }
        else {
            0L
        }
        $expectedPointUpdates = if (
            $pointEnabled -and
            ($updateEveryEnabledLight -or
                $Workload -in @(
                    "move-point",
                    "timeline-point",
                    "timeline-point-camera",
                    "timeline-mixed",
                    "reload-shadow-point",
                    "resize-point-shadow",
                    "replace-point-shadow-target"
                ))
        ) {
            [int64]$ExpectedSamples
        }
        else {
            0L
        }
        $expectedSpotUpdates = if (
            $spotEnabled -and
            ($updateEveryEnabledLight -or
                $Workload -in @(
                    "move-spot",
                    "reload-shadow-2d"
                ))
        ) {
            [int64]$ExpectedSamples
        }
        else {
            0L
        }
    }
    if ([int64]$Result.shadow.measuredDirectionalLightUpdateCount -ne
            $expectedDirectionalUpdates -or
        [int64]$Result.shadow.measuredPointLightUpdateCount -ne
            $expectedPointUpdates -or
        [int64]$Result.shadow.measuredSpotLightUpdateCount -ne
            $expectedSpotUpdates) {
        throw (
            "$($Scene.displayName) $Label per-light update invariant failed."
        )
    }
    $expectedUpdatedLights =
        $expectedDirectionalUpdates +
        $expectedPointUpdates +
        $expectedSpotUpdates
    if ([int64]$Result.shadow.measuredUpdatedLightCount -ne
        $expectedUpdatedLights) {
        throw (
            "$($Scene.displayName) $Label total light-update invariant failed."
        )
    }
    $enabledLightCount =
        [int]$directionalEnabled +
        [int]$pointEnabled +
        [int]$spotEnabled
    $expectedLightHits = 0L
    if ($perLightCacheEnabled) {
        if ($shadowStableWorkload) {
            $expectedLightHits =
                [int64]$ExpectedSamples * $enabledLightCount
        }
        else {
            $expectedLightHits =
                [int64]$ExpectedSamples * $enabledLightCount -
                $expectedUpdatedLights
        }
    }
    if ([int64]$Result.shadow.measuredLightCacheHitCount -ne
        $expectedLightHits) {
        throw (
            "$($Scene.displayName) $Label per-light cache-hit invariant failed."
        )
    }
    if ([int]$Result.schemaVersion -ge 17) {
        foreach ($propertyName in @(
            "emptyShadowClearCount",
            "measuredEmptyShadowClearCount",
            "shadowResourceFailureCount",
            "measuredShadowResourceFailureCount",
            "conservativeShadowFallbackCount",
            "measuredConservativeShadowFallbackCount"
        )) {
            if (-not $Result.shadow.PSObject.Properties[$propertyName]) {
                throw (
                    "$($Scene.displayName) $Label did not record " +
                    "$propertyName."
                )
            }
        }
        if ([int64]$Result.shadow.measuredShadowResourceFailureCount -ne 0) {
            throw (
                "$($Scene.displayName) $Label encountered a shadow " +
                "render-target or shader failure."
            )
        }
        if ([int64]$Result.shadow.measuredConservativeShadowFallbackCount -ne
            0) {
            throw (
                "$($Scene.displayName) $Label entered the conservative " +
                "shadow fallback path."
            )
        }
        $expectedEmptyClears = if ($Workload -eq "toggle-caster") {
            $firstMeasuredFrame = $InternalWarmupFrames + 1
            $lastMeasuredFrame =
                $InternalWarmupFrames + $ExpectedSamples
            $inactiveFrames =
                [Math]::Floor(($lastMeasuredFrame + 1) / 2) -
                [Math]::Floor($firstMeasuredFrame / 2)
            [int64]$inactiveFrames * $enabledLightCount
        }
        else {
            0L
        }
        if ([int64]$Result.shadow.measuredEmptyShadowClearCount -ne
            $expectedEmptyClears) {
            throw (
                "$($Scene.displayName) $Label empty-caster clear " +
                "accounting failed."
            )
        }
    }

    if ($pointEnabled) {
        if ([int]$Result.schemaVersion -ge 17) {
            $evidenceProperty =
                $Result.shadow.PSObject.Properties[
                    "pointShadowCubeEvidence"
                ]
            if (-not $evidenceProperty -or
                -not [bool]$evidenceProperty.Value.valid) {
                throw (
                    "$($Scene.displayName) $Label did not capture a " +
                    "sampleable point-shadow cubemap."
                )
            }
            $pointEvidence = $evidenceProperty.Value
            if (-not $pointEvidence.resolution -or
                $pointEvidence.resolution.Count -ne 2 -or
                [int]$pointEvidence.resolution[0] -le 0 -or
                [int]$pointEvidence.resolution[1] -le 0 -or
                [int64]$pointEvidence.sampleCountPerFace -ne
                    [int64]$pointEvidence.resolution[0] *
                    [int64]$pointEvidence.resolution[1] -or
                $pointEvidence.faces.Count -ne 6) {
                throw (
                    "$($Scene.displayName) $Label recorded invalid " +
                    "point-shadow cubemap dimensions."
                )
            }
            foreach ($face in $pointEvidence.faces) {
                if (-not [bool]$face.valid -or
                    [string]$face.bitwiseHash -notmatch
                        "^0x[0-9a-fA-F]{16}$" -or
                    [double]$face.minDepth -lt -0.000001 -or
                    [double]$face.maxDepth -gt 1.000001 -or
                    [double]$face.minDepth -gt
                        [double]$face.maxDepth -or
                    [double]$face.maxDepth -le 0.0) {
                    throw (
                        "$($Scene.displayName) $Label point-shadow face " +
                        "$($face.name) is invalid or unwritten."
                    )
                }
            }
        }
        foreach ($propertyName in @(
            "pointLightUpdateCount",
            "pointShadowLayeredUpdateCount",
            "pointShadowSixFaceUpdateCount",
            "pointShadowSubmissionPassCount",
            "pointShadowFaceCullingPassCount",
            "measuredPointLightUpdateCount",
            "measuredPointShadowLayeredUpdateCount",
            "measuredPointShadowSixFaceUpdateCount",
            "measuredPointShadowSubmissionPassCount",
            "measuredPointShadowFaceCullingPassCount"
        )) {
            if (-not $Result.shadow.PSObject.Properties[$propertyName]) {
                throw (
                    "$($Scene.displayName) $Label did not record " +
                    "$propertyName."
                )
            }
        }
        $pointUpdates = [int64]$Result.shadow.pointLightUpdateCount
        $layeredUpdates =
            [int64]$Result.shadow.pointShadowLayeredUpdateCount
        $sixFaceUpdates =
            [int64]$Result.shadow.pointShadowSixFaceUpdateCount
        $submissionPasses =
            [int64]$Result.shadow.pointShadowSubmissionPassCount
        $pointEmptyClears = if (
            [int]$Result.schemaVersion -ge 17 -and
            $enabledLightCount -gt 0
        ) {
            [int64]$Result.shadow.emptyShadowClearCount /
                $enabledLightCount
        }
        else {
            0L
        }
        if ($layeredUpdates + $sixFaceUpdates + $pointEmptyClears -ne
                $pointUpdates -or
            $submissionPasses -ne
                $layeredUpdates + 6L * $sixFaceUpdates) {
            throw (
                "$($Scene.displayName) $Label cumulative corrected-point " +
                "path accounting failed."
            )
        }
        $measuredLayeredUpdates =
            [int64]$Result.shadow.measuredPointShadowLayeredUpdateCount
        $measuredSixFaceUpdates =
            [int64]$Result.shadow.measuredPointShadowSixFaceUpdateCount
        $measuredSubmissionPasses =
            [int64]$Result.shadow.measuredPointShadowSubmissionPassCount
        $measuredPointEmptyClears = if (
            [int]$Result.schemaVersion -ge 17 -and
            $enabledLightCount -gt 0
        ) {
            [int64]$Result.shadow.measuredEmptyShadowClearCount /
                $enabledLightCount
        }
        else {
            0L
        }
        if ($measuredLayeredUpdates + $measuredSixFaceUpdates +
                $measuredPointEmptyClears -ne
                $expectedPointUpdates -or
            $measuredSubmissionPasses -ne
                $measuredLayeredUpdates + 6L * $measuredSixFaceUpdates) {
            throw (
                "$($Scene.displayName) $Label measured corrected-point " +
                "path accounting failed."
            )
        }
        $faceCullingEnabled =
            [bool]$Result.shadow.pointShadowFaceCullingEnabled
        $expectedFaceCullingPasses = if ($faceCullingEnabled) {
            6L * $sixFaceUpdates
        }
        else {
            0L
        }
        $expectedMeasuredFaceCullingPasses = if ($faceCullingEnabled) {
            6L * $measuredSixFaceUpdates
        }
        else {
            0L
        }
        if ([int64]$Result.shadow.pointShadowFaceCullingPassCount -ne
                $expectedFaceCullingPasses -or
            [int64]$Result.shadow.measuredPointShadowFaceCullingPassCount -ne
                $expectedMeasuredFaceCullingPasses) {
            throw (
                "$($Scene.displayName) $Label point-face culling-pass " +
                "accounting failed."
            )
        }
    }
}

function Invoke-ShadowOptimizationRun {
    param(
        $Scene,
        [string]$Executable,
        [string]$Variant,
        [string[]]$AdditionalArguments,
        [hashtable]$Environment,
        [string]$Label,
        [string]$OutputDirectory,
        [int]$SampleFrames
    )

    New-Item -ItemType Directory -Path $OutputDirectory -Force |
        Out-Null
    $relativeOutput = (Get-RelativePathWithin `
        -BasePath $projectDirectory `
        -ChildPath $OutputDirectory
    ).Replace("\", "/")
    $modelPath =
        "classic-scenes/" + ([string]$Scene.modelPath).Replace("\", "/")
    $capturePath = "$relativeOutput/$Label.ppm"
    $resultPath = "$relativeOutput/$Label.json"
    $logPath = Join-Path $OutputDirectory "$Label.log"
    $captureFrame = $InternalWarmupFrames + $SampleFrames
    $arguments = @(
        "--classic-scene-test", $modelPath,
        "--classic-scene-name", "$($Scene.id)-$Label",
        "--classic-scene-capture", $capturePath,
        "--classic-scene-result", $resultPath,
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
        "--classic-scene-directional-light",
            $DirectionalLight[0].ToString(
                [Globalization.CultureInfo]::InvariantCulture
            ),
            $DirectionalLight[1].ToString(
                [Globalization.CultureInfo]::InvariantCulture
            ),
            $DirectionalLight[2].ToString(
                [Globalization.CultureInfo]::InvariantCulture
            ),
        "--classic-scene-radius", [string]$Scene.normalizedRadius,
        "--classic-scene-world-scale",
            $WorldScale.ToString(
                [Globalization.CultureInfo]::InvariantCulture
            ),
        "--classic-scene-fov", [string]$Scene.fov,
        "--classic-scene-width", [string]$Width,
        "--classic-scene-height", [string]$Height,
        "--classic-scene-warmup-frames", [string]$InternalWarmupFrames,
        "--classic-scene-capture-frame", [string]$captureFrame,
        "--classic-scene-timeline-fps", [string]$TimelineFps,
        "--classic-scene-timeline-cycle-frames",
            [string]$TimelineCycleFrames,
        "--classic-scene-shadow-mode", $Mode,
        "--classic-scene-shadow-sampling", $Sampling,
        "--classic-scene-shadow-lights", $Lights,
        "--classic-scene-shadow-workload", $Workload,
        "--classic-scene-shadow-variant", $Variant,
        "--classic-scene-shadow-resolution", [string]$ShadowResolution,
        "--classic-scene-render-path", $RenderPath
    )
    if ($SpotNearPlane -gt 0.0) {
        $arguments += @(
            "--classic-scene-spot-near-plane",
            $SpotNearPlane.ToString(
                [Globalization.CultureInfo]::InvariantCulture
            ),
            "--classic-scene-spot-far-plane",
            $SpotFarPlane.ToString(
                [Globalization.CultureInfo]::InvariantCulture
            )
        )
    }
    if ($SpotLight.Count -eq 3) {
        $arguments += @(
            "--classic-scene-spot-light",
            $SpotLight[0].ToString(
                [Globalization.CultureInfo]::InvariantCulture
            ),
            $SpotLight[1].ToString(
                [Globalization.CultureInfo]::InvariantCulture
            ),
            $SpotLight[2].ToString(
                [Globalization.CultureInfo]::InvariantCulture
            ),
            "--classic-scene-spot-direction",
            $SpotDirection[0].ToString(
                [Globalization.CultureInfo]::InvariantCulture
            ),
            $SpotDirection[1].ToString(
                [Globalization.CultureInfo]::InvariantCulture
            ),
            $SpotDirection[2].ToString(
                [Globalization.CultureInfo]::InvariantCulture
            )
        )
    }
    if ($AdditionalArguments) {
        $arguments += $AdditionalArguments
    }

    $savedEnvironment = @{}
    foreach ($name in $controlledShadowEnvironmentNames) {
        $savedEnvironment[[string]$name] =
            [Environment]::GetEnvironmentVariable(
                [string]$name,
                [EnvironmentVariableTarget]::Process
            )
        $requestedValue = if ($Environment.ContainsKey($name)) {
            [string]$Environment[$name]
        }
        else {
            $null
        }
        [Environment]::SetEnvironmentVariable(
            [string]$name,
            $requestedValue,
            [EnvironmentVariableTarget]::Process
        )
    }
    Push-Location $projectDirectory
    try {
        & $Executable @arguments *> $logPath
        $exitCode = $LASTEXITCODE
    }
    finally {
        Pop-Location
        foreach ($name in $controlledShadowEnvironmentNames) {
            [Environment]::SetEnvironmentVariable(
                [string]$name,
                $savedEnvironment[[string]$name],
                [EnvironmentVariableTarget]::Process
            )
        }
    }
    if ($exitCode -ne 0) {
        throw (
            "$($Scene.displayName) $Label exited with $exitCode. " +
            "See $logPath"
        )
    }

    $absoluteResultPath = Join-Path $projectDirectory $resultPath
    $result =
        Get-Content -LiteralPath $absoluteResultPath -Raw |
            ConvertFrom-Json
    Assert-RunResult `
        -Result $result `
        -Scene $Scene `
        -ExpectedSamples $SampleFrames `
        -Label $Label `
        -ExpectedVariant $Variant `
        -ExpectedEnvironment $Environment
    return $result
}

if (-not $SkipBuild) {
    Build-Renderer
}
$beforeExecutable = Resolve-Executable $BeforeExecutablePath
$afterExecutable = Resolve-Executable $AfterExecutablePath
$manifest = Get-Content -LiteralPath $manifestPath -Raw | ConvertFrom-Json
$scenes = @(
    $manifest.scenes |
        Where-Object { $_.id -in $SceneIds }
)
if ($scenes.Count -ne $SceneIds.Count) {
    throw "One or more requested scenes were not found in the manifest."
}

New-Item -ItemType Directory -Path $resultRoot -Force | Out-Null
$gitMetadata = Get-GitMetadata
$formalOrder = switch ($FormalRunsPerVariant) {
    1 { @("A", "B") }
    2 { @("A", "B", "B", "A") }
    3 { @("A", "B", "B", "A", "A", "B") }
}
$formalLabels = [ordered]@{
    A = @(1..$FormalRunsPerVariant | ForEach-Object { "A$_" })
    B = @(1..$FormalRunsPerVariant | ForEach-Object { "B$_" })
}
$metadata = [ordered]@{
    schemaVersion = 1
    experimentId = $ExperimentId
    createdUtc = [DateTime]::UtcNow.ToString("o")
    source = [ordered]@{
        gitHead = $gitMetadata.head
        gitDirty = $gitMetadata.dirty
        sha256 = Get-SourceFingerprint
    }
    executables = [ordered]@{
        A = [ordered]@{
            path = $beforeExecutable
            sha256 = (
                Get-FileHash -LiteralPath $beforeExecutable -Algorithm SHA256
            ).Hash.ToLowerInvariant()
        }
        B = [ordered]@{
            path = $afterExecutable
            sha256 = (
                Get-FileHash -LiteralPath $afterExecutable -Algorithm SHA256
            ).Hash.ToLowerInvariant()
        }
    }
    machine = Get-MachineMetadata
    configuration = "Release x64"
    resolution = @($Width, $Height)
    order = $formalOrder
    internalWarmupFrames = $InternalWarmupFrames
    externalWarmupFrames = $ExternalWarmupFrames
    externalWarmupPerformed = -not $SkipExternalWarmup
    formalRunsPerVariant = $FormalRunsPerVariant
    pixelCorrectnessThreshold = [ordered]@{
        maximumChannelDelta = $MaximumPixelChannelDelta
        maximumChangedPixels = $MaximumChangedPixels
    }
    measuredFrames = $MeasuredFrames
    settings = [ordered]@{
        workload = $Workload
        lights = $Lights
        mode = $Mode
        sampling = $Sampling
        renderPath = $RenderPath
        shadowResolution = $ShadowResolution
        worldScale = $WorldScale
        directionalLight = @($DirectionalLight)
        spotNearPlane = $SpotNearPlane
        spotFarPlane = $SpotFarPlane
        spotLight = @($SpotLight)
        spotDirection = @($SpotDirection)
        timelineFps = $TimelineFps
        timelineCycleFrames = $TimelineCycleFrames
        sceneIds = $SceneIds
        variantALabel = $VariantALabel
        variantBLabel = $VariantBLabel
        variantAArguments = $VariantAArguments
        variantBArguments = $VariantBArguments
        variantAEnvironment = $VariantAEnvironment
        variantBEnvironment = $VariantBEnvironment
    }
}
$metadataPath = Join-Path $resultRoot "metadata.json"
$metadata |
    ConvertTo-Json -Depth 12 |
    Set-Content -LiteralPath $metadataPath -Encoding UTF8

$sceneSummaries = @()
foreach ($scene in $scenes) {
    $sceneWarmupRoot = Join-Path $warmupRoot $scene.id
    if (-not $SkipExternalWarmup) {
        Write-Host "Warm-up A: $($scene.displayName), $Workload"
        Invoke-ShadowOptimizationRun `
            -Scene $scene `
            -Executable $beforeExecutable `
            -Variant $VariantALabel `
            -AdditionalArguments $VariantAArguments `
            -Environment $VariantAEnvironment `
            -Label "A-warmup" `
            -OutputDirectory $sceneWarmupRoot `
            -SampleFrames $ExternalWarmupFrames |
            Out-Null
        Write-Host "Warm-up B: $($scene.displayName), $Workload"
        Invoke-ShadowOptimizationRun `
            -Scene $scene `
            -Executable $afterExecutable `
            -Variant $VariantBLabel `
            -AdditionalArguments $VariantBArguments `
            -Environment $VariantBEnvironment `
            -Label "B-warmup" `
            -OutputDirectory $sceneWarmupRoot `
            -SampleFrames $ExternalWarmupFrames |
            Out-Null
    }

    $sceneFormalRoot = Join-Path $formalRoot $scene.id
    $counts = @{ A = 0; B = 0 }
    $resultsA = @()
    $resultsB = @()
    foreach ($variant in $formalOrder) {
        $counts[$variant]++
        $label = "$variant$($counts[$variant])"
        if ($variant -eq "A") {
            $executable = $beforeExecutable
            $variantLabel = $VariantALabel
            $additionalArguments = $VariantAArguments
            $environment = $VariantAEnvironment
        }
        else {
            $executable = $afterExecutable
            $variantLabel = $VariantBLabel
            $additionalArguments = $VariantBArguments
            $environment = $VariantBEnvironment
        }
        Write-Host "Measured ${label}: $($scene.displayName), $Workload"
        $result = Invoke-ShadowOptimizationRun `
            -Scene $scene `
            -Executable $executable `
            -Variant $variantLabel `
            -AdditionalArguments $additionalArguments `
            -Environment $environment `
            -Label $label `
            -OutputDirectory $sceneFormalRoot `
            -SampleFrames $MeasuredFrames
        if ($variant -eq "A") {
            $resultsA += $result
        }
        else {
            $resultsB += $result
        }
        Write-Host (
            "PASS {0} {1}: wall={2:N3} ms, gpu={3:N3} ms" -f
            $scene.id,
            $label,
            [double]$result.profiler.summary.wallFrame.mean,
            [double]$result.profiler.summary.gpuFrame.mean
        )
    }

    $rendererResourceFields = [ordered]@{
        texture = "Texture"
        meshCpu = "Mesh CPU"
        meshGpu = "Mesh GPU"
        renderTarget = "Render Target"
    }
    $rendererResourceEvidence = [ordered]@{}
    $allFormalResults = @($resultsA) + @($resultsB)
    foreach ($field in $rendererResourceFields.Keys) {
        $values = @(
            $allFormalResults |
                ForEach-Object {
                    [int64]$_.memoryBytes.$field
                }
        )
        if (@($values | Select-Object -Unique).Count -ne 1) {
            throw (
                "$($scene.displayName) renderer-owned " +
                "$($rendererResourceFields[$field]) bytes differ across " +
                "the A/B formal processes."
            )
        }
        $rendererResourceEvidence[$field] = [ordered]@{
            label = $rendererResourceFields[$field]
            values = $values
            exact = $true
        }
    }

    $captureComparisons = @()
    $pointShadowCubeComparisons = @()
    for ($runIndex = 1;
        $runIndex -le $FormalRunsPerVariant;
        ++$runIndex) {
        $beforeCapture = Join-Path $sceneFormalRoot "A$runIndex.ppm"
        $afterCapture = Join-Path $sceneFormalRoot "B$runIndex.ppm"
        $comparison = [OpenGLLearnPpmCaptureComparer]::Compare(
            $beforeCapture,
            $afterCapture
        )
        $withinTolerance =
            $comparison.MaximumChannelDelta -le
                $MaximumPixelChannelDelta -and
            $comparison.ChangedPixelCount -le $MaximumChangedPixels
        if (-not $withinTolerance) {
            throw (
                "$($scene.displayName) A$runIndex/B$runIndex pixel " +
                "difference exceeded the correctness threshold: " +
                "max channel delta=$($comparison.MaximumChannelDelta), " +
                "changed pixels=$($comparison.ChangedPixelCount)."
            )
        }
        $captureComparisons += [pscustomobject][ordered]@{
            pair = $runIndex
            resolution = @($comparison.Width, $comparison.Height)
            exact = $comparison.Exact
            maximumChannelDelta = $comparison.MaximumChannelDelta
            changedPixelCount = $comparison.ChangedPixelCount
            normalizedMeanAbsoluteDifference =
                $comparison.NormalizedMeanAbsoluteDifference
            withinTolerance = $withinTolerance
        }

        if ($Lights -in @("point", "all")) {
            $beforeEvidence =
                $resultsA[$runIndex - 1].shadow.pointShadowCubeEvidence
            $afterEvidence =
                $resultsB[$runIndex - 1].shadow.pointShadowCubeEvidence
            $facesExact =
                [bool]$beforeEvidence.valid -and
                [bool]$afterEvidence.valid -and
                [int]$beforeEvidence.resolution[0] -eq
                    [int]$afterEvidence.resolution[0] -and
                [int]$beforeEvidence.resolution[1] -eq
                    [int]$afterEvidence.resolution[1] -and
                [int64]$beforeEvidence.sampleCountPerFace -eq
                    [int64]$afterEvidence.sampleCountPerFace
            $faceEvidence = @()
            for ($faceIndex = 0; $faceIndex -lt 6; ++$faceIndex) {
                $beforeFace = $beforeEvidence.faces[$faceIndex]
                $afterFace = $afterEvidence.faces[$faceIndex]
                $faceExact =
                    [string]$beforeFace.bitwiseHash -eq
                        [string]$afterFace.bitwiseHash -and
                    [int64]$beforeFace.nonFarSampleCount -eq
                        [int64]$afterFace.nonFarSampleCount -and
                    [double]$beforeFace.minDepth -eq
                        [double]$afterFace.minDepth -and
                    [double]$beforeFace.maxDepth -eq
                        [double]$afterFace.maxDepth
                $facesExact = $facesExact -and $faceExact
                $faceEvidence += [pscustomobject][ordered]@{
                    index = $faceIndex
                    name = [string]$beforeFace.name
                    hashA = [string]$beforeFace.bitwiseHash
                    hashB = [string]$afterFace.bitwiseHash
                    nonFarSamplesA =
                        [int64]$beforeFace.nonFarSampleCount
                    nonFarSamplesB =
                        [int64]$afterFace.nonFarSampleCount
                    exact = $faceExact
                }
            }
            if (-not $facesExact) {
                throw (
                    "$($scene.displayName) A$runIndex/B$runIndex " +
                    "point-shadow cubemap content differs."
                )
            }
            $pointShadowCubeComparisons +=
                [pscustomobject][ordered]@{
                    pair = $runIndex
                    resolution = @(
                        [int]$beforeEvidence.resolution[0],
                        [int]$beforeEvidence.resolution[1]
                    )
                    faces = $faceEvidence
                    exact = $facesExact
                }
        }
    }

    $sceneSummaries += [pscustomobject][ordered]@{
        id = [string]$scene.id
        displayName = [string]$scene.displayName
        correctness = [ordered]@{
            pixelThreshold = [ordered]@{
                maximumChannelDelta = $MaximumPixelChannelDelta
                maximumChangedPixels = $MaximumChangedPixels
            }
            captureComparisons = $captureComparisons
            pointShadowCubeComparisons = $pointShadowCubeComparisons
            rendererOwnedResources = $rendererResourceEvidence
        }
        perProcessStatistics = [ordered]@{
            wallFrameMilliseconds = New-PerProcessDistributionComparison `
                -ADistributions @(
                    $resultsA |
                        ForEach-Object {
                            Get-FrameDistribution $_ "wallFrame"
                        }
                ) `
                -BDistributions @(
                    $resultsB |
                        ForEach-Object {
                            Get-FrameDistribution $_ "wallFrame"
                        }
                )
            cpuFrameMilliseconds = New-PerProcessDistributionComparison `
                -ADistributions @(
                    $resultsA |
                        ForEach-Object {
                            Get-FrameDistribution $_ "cpuFrame"
                        }
                ) `
                -BDistributions @(
                    $resultsB |
                        ForEach-Object {
                            Get-FrameDistribution $_ "cpuFrame"
                        }
                )
            gpuFrameMilliseconds = New-PerProcessDistributionComparison `
                -ADistributions @(
                    $resultsA |
                        ForEach-Object {
                            Get-FrameDistribution $_ "gpuFrame"
                        }
                ) `
                -BDistributions @(
                    $resultsB |
                        ForEach-Object {
                            Get-FrameDistribution $_ "gpuFrame"
                        }
                )
            shadowMapsCpuMilliseconds =
                New-PerProcessDistributionComparison `
                    -ADistributions @(
                        $resultsA |
                            ForEach-Object {
                                Get-ZoneDistribution `
                                    $_ "cpu" "Shadow Maps"
                            }
                    ) `
                    -BDistributions @(
                        $resultsB |
                            ForEach-Object {
                                Get-ZoneDistribution `
                                    $_ "cpu" "Shadow Maps"
                            }
                    )
            shadowMapUpdateGpuMilliseconds =
                New-PerProcessDistributionComparison `
                    -ADistributions @(
                        $resultsA |
                            ForEach-Object {
                                Get-ZoneDistribution `
                                    $_ "gpu" "Shadow Map Update"
                            }
                    ) `
                    -BDistributions @(
                        $resultsB |
                            ForEach-Object {
                                Get-ZoneDistribution `
                                    $_ "gpu" "Shadow Map Update"
                            }
                    )
        }
        average = [ordered]@{
            wallFrameMilliseconds = New-MetricComparison `
                -AValues @(
                    $resultsA |
                        ForEach-Object {
                            $_.profiler.summary.wallFrame.mean
                        }
                ) `
                -BValues @(
                    $resultsB |
                        ForEach-Object {
                            $_.profiler.summary.wallFrame.mean
                        }
                )
            cpuFrameMilliseconds = New-MetricComparison `
                -AValues @(
                    $resultsA |
                        ForEach-Object {
                            $_.profiler.summary.cpuFrame.mean
                        }
                ) `
                -BValues @(
                    $resultsB |
                        ForEach-Object {
                            $_.profiler.summary.cpuFrame.mean
                        }
                )
            gpuFrameMilliseconds = New-MetricComparison `
                -AValues @(
                    $resultsA |
                        ForEach-Object {
                            $_.profiler.summary.gpuFrame.mean
                        }
                ) `
                -BValues @(
                    $resultsB |
                        ForEach-Object {
                            $_.profiler.summary.gpuFrame.mean
                        }
                )
            shadowMapsCpuMilliseconds = New-MetricComparison `
                -AValues @(
                    $resultsA |
                        ForEach-Object {
                            Get-ZoneMean $_ "cpu" "Shadow Maps"
                        }
                ) `
                -BValues @(
                    $resultsB |
                        ForEach-Object {
                            Get-ZoneMean $_ "cpu" "Shadow Maps"
                    }
                )
            directionalShadowFitCpuMillisecondsPerFrame = New-MetricComparison `
                -AValues @(
                    $resultsA |
                        ForEach-Object {
                            Get-DirectionalFitCpuMillisecondsPerFrame $_
                        }
                ) `
                -BValues @(
                    $resultsB |
                        ForEach-Object {
                            Get-DirectionalFitCpuMillisecondsPerFrame $_
                        }
                )
            cacheCheckCpuMilliseconds = New-MetricComparison `
                -AValues @(
                    $resultsA |
                        ForEach-Object {
                            $_.shadow.measuredAverageCacheCheckCpuMilliseconds
                        }
                ) `
                -BValues @(
                    $resultsB |
                        ForEach-Object {
                            $_.shadow.measuredAverageCacheCheckCpuMilliseconds
                        }
                )
            casterStateSyncCpuMilliseconds = New-MetricComparison `
                -AValues @(
                    $resultsA |
                        ForEach-Object {
                            $_.shadow.measuredAverageCasterStateSyncCpuMilliseconds
                        }
                ) `
                -BValues @(
                    $resultsB |
                        ForEach-Object {
                            $_.shadow.measuredAverageCasterStateSyncCpuMilliseconds
                        }
                )
            shadowMapUpdateGpuMilliseconds = New-MetricComparison `
                -AValues @(
                    $resultsA |
                        ForEach-Object {
                            Get-ZoneMean $_ "gpu" "Shadow Map Update"
                        }
                ) `
                -BValues @(
                    $resultsB |
                        ForEach-Object {
                            Get-ZoneMean $_ "gpu" "Shadow Map Update"
                        }
                )
            directionalShadowUpdateGpuMillisecondsPerFrame = New-MetricComparison `
                -AValues @(
                    $resultsA |
                        ForEach-Object {
                            Get-ZoneAmortizedMean `
                                $_ "gpu" "Directional Shadow Update"
                        }
                ) `
                -BValues @(
                    $resultsB |
                        ForEach-Object {
                            Get-ZoneAmortizedMean `
                                $_ "gpu" "Directional Shadow Update"
                        }
                )
            pointShadowUpdateGpuMillisecondsPerFrame = New-MetricComparison `
                -AValues @(
                    $resultsA |
                        ForEach-Object {
                            Get-ZoneAmortizedMean `
                                $_ "gpu" "Point Shadow Update"
                        }
                ) `
                -BValues @(
                    $resultsB |
                        ForEach-Object {
                            Get-ZoneAmortizedMean `
                                $_ "gpu" "Point Shadow Update"
                        }
                )
            spotShadowUpdateGpuMillisecondsPerFrame = New-MetricComparison `
                -AValues @(
                    $resultsA |
                        ForEach-Object {
                            Get-ZoneAmortizedMean `
                                $_ "gpu" "Spot Shadow Update"
                        }
                ) `
                -BValues @(
                    $resultsB |
                        ForEach-Object {
                            Get-ZoneAmortizedMean `
                                $_ "gpu" "Spot Shadow Update"
                        }
                )
            updatedLightsPerFrame = New-MetricComparison `
                -AValues @(
                    $resultsA |
                        ForEach-Object {
                            [double]$_.shadow.measuredUpdatedLightCount /
                                [double]$MeasuredFrames
                        }
                ) `
                -BValues @(
                    $resultsB |
                        ForEach-Object {
                            [double]$_.shadow.measuredUpdatedLightCount /
                                [double]$MeasuredFrames
                        }
                )
            directionalLightUpdatesPerFrame = New-MetricComparison `
                -AValues @(
                    $resultsA |
                        ForEach-Object {
                            [double]$_.shadow.measuredDirectionalLightUpdateCount /
                                [double]$MeasuredFrames
                        }
                ) `
                -BValues @(
                    $resultsB |
                        ForEach-Object {
                            [double]$_.shadow.measuredDirectionalLightUpdateCount /
                                [double]$MeasuredFrames
                    }
                )
            directionalFitsPerFrame = New-MetricComparison `
                -AValues @(
                    $resultsA |
                        ForEach-Object {
                            Get-OptionalShadowMetricPerFrame `
                                $_ "measuredDirectionalFitCount"
                        }
                ) `
                -BValues @(
                    $resultsB |
                        ForEach-Object {
                            Get-OptionalShadowMetricPerFrame `
                                $_ "measuredDirectionalFitCount"
                        }
                )
            directionalLightAabbFitsPerFrame = New-MetricComparison `
                -AValues @(
                    $resultsA |
                        ForEach-Object {
                            Get-OptionalShadowMetricPerFrame `
                                $_ "measuredDirectionalLightAabbFitCount"
                        }
                ) `
                -BValues @(
                    $resultsB |
                        ForEach-Object {
                            Get-OptionalShadowMetricPerFrame `
                                $_ "measuredDirectionalLightAabbFitCount"
                        }
                )
            directionalLightAabbFitEnabled = New-MetricComparison `
                -AValues @(
                    $resultsA |
                        ForEach-Object {
                            Get-OptionalShadowMetric `
                                $_ "directionalLightAabbFitEnabled"
                        }
                ) `
                -BValues @(
                    $resultsB |
                        ForEach-Object {
                            Get-OptionalShadowMetric `
                                $_ "directionalLightAabbFitEnabled"
                        }
                )
            directionalResolutionChangesPerFrame = New-MetricComparison `
                -AValues @(
                    $resultsA |
                        ForEach-Object {
                            Get-OptionalShadowMetricPerFrame `
                                $_ "measuredDirectionalResolutionChangeCount"
                        }
                ) `
                -BValues @(
                    $resultsB |
                        ForEach-Object {
                            Get-OptionalShadowMetricPerFrame `
                                $_ "measuredDirectionalResolutionChangeCount"
                        }
                )
            directionalDensityResolutionEnabled = New-MetricComparison `
                -AValues @(
                    $resultsA |
                        ForEach-Object {
                            Get-OptionalShadowMetric `
                                $_ "directionalDensityResolutionEnabled"
                        }
                ) `
                -BValues @(
                    $resultsB |
                        ForEach-Object {
                            Get-OptionalShadowMetric `
                                $_ "directionalDensityResolutionEnabled"
                        }
                )
            directionalFitReferenceTexelSize = New-MetricComparison `
                -AValues @(
                    $resultsA |
                        ForEach-Object {
                            Get-OptionalShadowMetric `
                                $_ "lastDirectionalFitReferenceTexelSize"
                        }
                ) `
                -BValues @(
                    $resultsB |
                        ForEach-Object {
                            Get-OptionalShadowMetric `
                                $_ "lastDirectionalFitReferenceTexelSize"
                        }
                )
            directionalEffectiveResolution = New-MetricComparison `
                -AValues @(
                    $resultsA |
                        ForEach-Object {
                            Get-OptionalShadowMetric `
                                $_ "lastDirectionalFitResolution"
                        }
                ) `
                -BValues @(
                    $resultsB |
                        ForEach-Object {
                            Get-OptionalShadowMetric `
                                $_ "lastDirectionalFitResolution"
                        }
                )
            directionalFitCpuMillisecondsPerFit = New-MetricComparison `
                -AValues @(
                    $resultsA |
                        ForEach-Object {
                            Get-OptionalShadowMetric `
                                $_ "measuredAverageDirectionalFitCpuMilliseconds"
                        }
                ) `
                -BValues @(
                    $resultsB |
                        ForEach-Object {
                            Get-OptionalShadowMetric `
                                $_ "measuredAverageDirectionalFitCpuMilliseconds"
                        }
                )
            directionalFitRawWidth = New-MetricComparison `
                -AValues @(
                    $resultsA |
                        ForEach-Object {
                            Get-OptionalShadowMetric `
                                $_ "lastDirectionalFitRawWidth"
                        }
                ) `
                -BValues @(
                    $resultsB |
                        ForEach-Object {
                            Get-OptionalShadowMetric `
                                $_ "lastDirectionalFitRawWidth"
                        }
                )
            directionalFitRawHeight = New-MetricComparison `
                -AValues @(
                    $resultsA |
                        ForEach-Object {
                            Get-OptionalShadowMetric `
                                $_ "lastDirectionalFitRawHeight"
                        }
                ) `
                -BValues @(
                    $resultsB |
                        ForEach-Object {
                            Get-OptionalShadowMetric `
                                $_ "lastDirectionalFitRawHeight"
                        }
                )
            directionalFitRawDepth = New-MetricComparison `
                -AValues @(
                    $resultsA |
                        ForEach-Object {
                            Get-OptionalShadowMetric `
                                $_ "lastDirectionalFitRawDepth"
                        }
                ) `
                -BValues @(
                    $resultsB |
                        ForEach-Object {
                            Get-OptionalShadowMetric `
                                $_ "lastDirectionalFitRawDepth"
                        }
                )
            directionalFitWidth = New-MetricComparison `
                -AValues @(
                    $resultsA |
                        ForEach-Object {
                            Get-OptionalShadowMetric `
                                $_ "lastDirectionalFitWidth"
                        }
                ) `
                -BValues @(
                    $resultsB |
                        ForEach-Object {
                            Get-OptionalShadowMetric `
                                $_ "lastDirectionalFitWidth"
                        }
                )
            directionalFitHeight = New-MetricComparison `
                -AValues @(
                    $resultsA |
                        ForEach-Object {
                            Get-OptionalShadowMetric `
                                $_ "lastDirectionalFitHeight"
                        }
                ) `
                -BValues @(
                    $resultsB |
                        ForEach-Object {
                            Get-OptionalShadowMetric `
                                $_ "lastDirectionalFitHeight"
                        }
                )
            directionalFitDepth = New-MetricComparison `
                -AValues @(
                    $resultsA |
                        ForEach-Object {
                            Get-OptionalShadowMetric `
                                $_ "lastDirectionalFitDepth"
                        }
                ) `
                -BValues @(
                    $resultsB |
                        ForEach-Object {
                            Get-OptionalShadowMetric `
                                $_ "lastDirectionalFitDepth"
                        }
                )
            directionalFitTexelSizeX = New-MetricComparison `
                -AValues @(
                    $resultsA |
                        ForEach-Object {
                            Get-OptionalShadowMetric `
                                $_ "lastDirectionalFitTexelSizeX"
                        }
                ) `
                -BValues @(
                    $resultsB |
                        ForEach-Object {
                            Get-OptionalShadowMetric `
                                $_ "lastDirectionalFitTexelSizeX"
                        }
                )
            directionalFitTexelSizeY = New-MetricComparison `
                -AValues @(
                    $resultsA |
                        ForEach-Object {
                            Get-OptionalShadowMetric `
                                $_ "lastDirectionalFitTexelSizeY"
                        }
                ) `
                -BValues @(
                    $resultsB |
                        ForEach-Object {
                            Get-OptionalShadowMetric `
                                $_ "lastDirectionalFitTexelSizeY"
                        }
                )
            directionalFitUtilization = New-MetricComparison `
                -AValues @(
                    $resultsA |
                        ForEach-Object {
                            Get-OptionalShadowMetric `
                                $_ "lastDirectionalFitUtilization"
                        }
                ) `
                -BValues @(
                    $resultsB |
                        ForEach-Object {
                            Get-OptionalShadowMetric `
                                $_ "lastDirectionalFitUtilization"
                        }
                )
            pointLightUpdatesPerFrame = New-MetricComparison `
                -AValues @(
                    $resultsA |
                        ForEach-Object {
                            [double]$_.shadow.measuredPointLightUpdateCount /
                                [double]$MeasuredFrames
                        }
                ) `
                -BValues @(
                    $resultsB |
                        ForEach-Object {
                            [double]$_.shadow.measuredPointLightUpdateCount /
                                [double]$MeasuredFrames
                        }
                )
            spotLightUpdatesPerFrame = New-MetricComparison `
                -AValues @(
                    $resultsA |
                        ForEach-Object {
                            [double]$_.shadow.measuredSpotLightUpdateCount /
                                [double]$MeasuredFrames
                        }
                ) `
                -BValues @(
                    $resultsB |
                        ForEach-Object {
                            [double]$_.shadow.measuredSpotLightUpdateCount /
                            [double]$MeasuredFrames
                        }
                )
            spotFitsPerFrame = New-MetricComparison `
                -AValues @(
                    $resultsA |
                        ForEach-Object {
                            Get-OptionalShadowMetricPerFrame `
                                $_ "measuredSpotFitCount"
                        }
                ) `
                -BValues @(
                    $resultsB |
                        ForEach-Object {
                            Get-OptionalShadowMetricPerFrame `
                                $_ "measuredSpotFitCount"
                        }
                )
            spotProjectionAwareFitsPerFrame = New-MetricComparison `
                -AValues @(
                    $resultsA |
                        ForEach-Object {
                            Get-OptionalShadowMetricPerFrame `
                                $_ "measuredSpotProjectionAwareFitCount"
                        }
                ) `
                -BValues @(
                    $resultsB |
                        ForEach-Object {
                            Get-OptionalShadowMetricPerFrame `
                                $_ "measuredSpotProjectionAwareFitCount"
                        }
                )
            spotFitFallbacksPerFrame = New-MetricComparison `
                -AValues @(
                    $resultsA |
                        ForEach-Object {
                            Get-OptionalShadowMetricPerFrame `
                                $_ "measuredSpotFitFallbackCount"
                        }
                ) `
                -BValues @(
                    $resultsB |
                        ForEach-Object {
                            Get-OptionalShadowMetricPerFrame `
                                $_ "measuredSpotFitFallbackCount"
                        }
                )
            spotFitCpuMillisecondsPerFit = New-MetricComparison `
                -AValues @(
                    $resultsA |
                        ForEach-Object {
                            Get-OptionalShadowMetric `
                                $_ "measuredAverageSpotFitCpuMilliseconds"
                        }
                ) `
                -BValues @(
                    $resultsB |
                        ForEach-Object {
                            Get-OptionalShadowMetric `
                                $_ "measuredAverageSpotFitCpuMilliseconds"
                        }
                )
            spotFitCpuMillisecondsPerFrame = New-MetricComparison `
                -AValues @(
                    $resultsA |
                        ForEach-Object {
                            [double]$_.shadow.measuredSpotFitCpuMilliseconds /
                                [double]$MeasuredFrames
                        }
                ) `
                -BValues @(
                    $resultsB |
                        ForEach-Object {
                            [double]$_.shadow.measuredSpotFitCpuMilliseconds /
                                [double]$MeasuredFrames
                        }
                )
            spotFitCandidateMeshes = New-MetricComparison `
                -AValues @(
                    $resultsA |
                        ForEach-Object {
                            Get-OptionalShadowMetric `
                                $_ "lastSpotFitCandidateCount"
                        }
                ) `
                -BValues @(
                    $resultsB |
                        ForEach-Object {
                            Get-OptionalShadowMetric `
                                $_ "lastSpotFitCandidateCount"
                        }
                )
            spotFitAcceptedMeshes = New-MetricComparison `
                -AValues @(
                    $resultsA |
                        ForEach-Object {
                            Get-OptionalShadowMetric `
                                $_ "lastSpotFitAcceptedCount"
                        }
                ) `
                -BValues @(
                    $resultsB |
                        ForEach-Object {
                            Get-OptionalShadowMetric `
                                $_ "lastSpotFitAcceptedCount"
                        }
                )
            spotFitRejectedMeshes = New-MetricComparison `
                -AValues @(
                    $resultsA |
                        ForEach-Object {
                            Get-OptionalShadowMetric `
                                $_ "lastSpotFitRejectedCount"
                        }
                ) `
                -BValues @(
                    $resultsB |
                        ForEach-Object {
                            Get-OptionalShadowMetric `
                                $_ "lastSpotFitRejectedCount"
                        }
                )
            spotFitLegacyNear = New-MetricComparison `
                -AValues @(
                    $resultsA |
                        ForEach-Object {
                            Get-OptionalShadowMetric `
                                $_ "lastSpotFitLegacyNear"
                        }
                ) `
                -BValues @(
                    $resultsB |
                        ForEach-Object {
                            Get-OptionalShadowMetric `
                                $_ "lastSpotFitLegacyNear"
                        }
                )
            spotFitLegacyFar = New-MetricComparison `
                -AValues @(
                    $resultsA |
                        ForEach-Object {
                            Get-OptionalShadowMetric `
                                $_ "lastSpotFitLegacyFar"
                        }
                ) `
                -BValues @(
                    $resultsB |
                        ForEach-Object {
                            Get-OptionalShadowMetric `
                                $_ "lastSpotFitLegacyFar"
                        }
                )
            spotFitNear = New-MetricComparison `
                -AValues @(
                    $resultsA |
                        ForEach-Object {
                            Get-OptionalShadowMetric $_ "lastSpotFitNear"
                        }
                ) `
                -BValues @(
                    $resultsB |
                        ForEach-Object {
                            Get-OptionalShadowMetric $_ "lastSpotFitNear"
                        }
                )
            spotFitFar = New-MetricComparison `
                -AValues @(
                    $resultsA |
                        ForEach-Object {
                            Get-OptionalShadowMetric $_ "lastSpotFitFar"
                        }
                ) `
                -BValues @(
                    $resultsB |
                        ForEach-Object {
                            Get-OptionalShadowMetric $_ "lastSpotFitFar"
                        }
                )
            spotFitDepthSpanReduction = New-MetricComparison `
                -AValues @(
                    $resultsA |
                        ForEach-Object {
                            Get-OptionalShadowMetric `
                                $_ "lastSpotFitDepthSpanReduction"
                        }
                ) `
                -BValues @(
                    $resultsB |
                        ForEach-Object {
                            Get-OptionalShadowMetric `
                                $_ "lastSpotFitDepthSpanReduction"
                        }
                )
            spotFitRawNear = New-MetricComparison `
                -AValues @(
                    $resultsA |
                        ForEach-Object {
                            Get-OptionalShadowMetric $_ "lastSpotFitRawNear"
                        }
                ) `
                -BValues @(
                    $resultsB |
                        ForEach-Object {
                            Get-OptionalShadowMetric $_ "lastSpotFitRawNear"
                        }
                )
            spotFitRawFar = New-MetricComparison `
                -AValues @(
                    $resultsA |
                        ForEach-Object {
                            Get-OptionalShadowMetric $_ "lastSpotFitRawFar"
                        }
                ) `
                -BValues @(
                    $resultsB |
                        ForEach-Object {
                            Get-OptionalShadowMetric $_ "lastSpotFitRawFar"
                        }
                )
            spotFitDepthUtilization = New-MetricComparison `
                -AValues @(
                    $resultsA |
                        ForEach-Object {
                            Get-OptionalShadowMetric `
                                $_ "lastSpotFitDepthUtilization"
                        }
                ) `
                -BValues @(
                    $resultsB |
                        ForEach-Object {
                            Get-OptionalShadowMetric `
                                $_ "lastSpotFitDepthUtilization"
                        }
                )
            spotFitProjectionDepthScale = New-MetricComparison `
                -AValues @(
                    $resultsA |
                        ForEach-Object {
                            Get-OptionalShadowMetric `
                                $_ "lastSpotFitProjectionDepthScale"
                        }
                ) `
                -BValues @(
                    $resultsB |
                        ForEach-Object {
                            Get-OptionalShadowMetric `
                                $_ "lastSpotFitProjectionDepthScale"
                        }
                )
            spotFitPrecisionGain = New-MetricComparison `
                -AValues @(
                    $resultsA |
                        ForEach-Object {
                            Get-OptionalShadowMetric `
                                $_ "lastSpotFitPrecisionGain"
                        }
                ) `
                -BValues @(
                    $resultsB |
                        ForEach-Object {
                            Get-OptionalShadowMetric `
                                $_ "lastSpotFitPrecisionGain"
                        }
                )
            spotFitMinimumProjectedCoverageMargin = New-MetricComparison `
                -AValues @(
                    $resultsA |
                        ForEach-Object {
                            Get-OptionalShadowMetric `
                                $_ "lastSpotFitMinimumProjectedCoverageMargin"
                        }
                ) `
                -BValues @(
                    $resultsB |
                        ForEach-Object {
                            Get-OptionalShadowMetric `
                                $_ "lastSpotFitMinimumProjectedCoverageMargin"
                        }
                )
            lightCacheHitsPerFrame = New-MetricComparison `
                -AValues @(
                    $resultsA |
                        ForEach-Object {
                            [double]$_.shadow.measuredLightCacheHitCount /
                                [double]$MeasuredFrames
                        }
                ) `
                -BValues @(
                    $resultsB |
                        ForEach-Object {
                            [double]$_.shadow.measuredLightCacheHitCount /
                                [double]$MeasuredFrames
                        }
                )
            casterCandidatesPerFrame = New-MetricComparison `
                -AValues @(
                    $resultsA |
                        ForEach-Object {
                            [double]$_.shadow.measuredCasterCandidateCount /
                                [double]$MeasuredFrames
                        }
                ) `
                -BValues @(
                    $resultsB |
                        ForEach-Object {
                            [double]$_.shadow.measuredCasterCandidateCount /
                                [double]$MeasuredFrames
                        }
                )
            casterCulledPerFrame = New-MetricComparison `
                -AValues @(
                    $resultsA |
                        ForEach-Object {
                            [double]$_.shadow.measuredCasterCulledCount /
                                [double]$MeasuredFrames
                        }
                ) `
                -BValues @(
                    $resultsB |
                        ForEach-Object {
                            [double]$_.shadow.measuredCasterCulledCount /
                                [double]$MeasuredFrames
                        }
                )
            casterCullingLightsPerFrame = New-MetricComparison `
                -AValues @(
                    $resultsA |
                        ForEach-Object {
                            [double]$_.shadow.measuredCasterCullingLightCount /
                                [double]$MeasuredFrames
                        }
                ) `
                -BValues @(
                    $resultsB |
                        ForEach-Object {
                            [double]$_.shadow.measuredCasterCullingLightCount /
                                [double]$MeasuredFrames
                        }
                )
            casterDrawsPerFrame = New-MetricComparison `
                -AValues @(
                    $resultsA |
                        ForEach-Object {
                            [double]$_.shadow.measuredCasterDrawCount /
                                [double]$MeasuredFrames
                        }
                ) `
                -BValues @(
                    $resultsB |
                        ForEach-Object {
                            [double]$_.shadow.measuredCasterDrawCount /
                                [double]$MeasuredFrames
                        }
                )
            casterTrianglesPerFrame = New-MetricComparison `
                -AValues @(
                    $resultsA |
                        ForEach-Object {
                            [double]$_.shadow.measuredCasterTriangleCount /
                                [double]$MeasuredFrames
                        }
                ) `
                -BValues @(
                    $resultsB |
                        ForEach-Object {
                            [double]$_.shadow.measuredCasterTriangleCount /
                                [double]$MeasuredFrames
                        }
                )
            buildShadowCasterListCpuMillisecondsPerFrame = New-MetricComparison `
                -AValues @(
                    $resultsA |
                        ForEach-Object {
                            Get-ZoneAmortizedMean `
                                $_ "cpu" "Build Shadow Caster List"
                        }
                ) `
                -BValues @(
                    $resultsB |
                        ForEach-Object {
                            Get-ZoneAmortizedMean `
                                $_ "cpu" "Build Shadow Caster List"
                        }
                )
            shadowCasterSubmissionCpuMillisecondsPerFrame = New-MetricComparison `
                -AValues @(
                    $resultsA |
                        ForEach-Object {
                            Get-ZoneAmortizedMean `
                                $_ "cpu" "Shadow Caster Submission"
                        }
                ) `
                -BValues @(
                    $resultsB |
                        ForEach-Object {
                            Get-ZoneAmortizedMean `
                                $_ "cpu" "Shadow Caster Submission"
                        }
                )
            processWorkingSetBytes = New-MetricComparison `
                -AValues @(
                    $resultsA |
                        ForEach-Object {
                            $_.memoryBytes.processWorkingSet
                        }
                ) `
                -BValues @(
                    $resultsB |
                        ForEach-Object {
                            $_.memoryBytes.processWorkingSet
                        }
                )
            processPrivateBytes = New-MetricComparison `
                -AValues @(
                    $resultsA |
                        ForEach-Object {
                            $_.memoryBytes.processPrivate
                        }
                ) `
                -BValues @(
                    $resultsB |
                        ForEach-Object {
                            $_.memoryBytes.processPrivate
                        }
                )
        }
    }
}

$summary = [ordered]@{
    schemaVersion = 4
    experimentId = $ExperimentId
    metadata = $metadataPath
    statisticsDefinition = [ordered]@{
        formalProcessesPerVariant = $FormalRunsPerVariant
        formalProcessLabels = $formalLabels
        processStatisticAggregation = "arithmetic-mean"
        processValuesRetained = $true
        percentiles = @("median", "p95", "p99")
        percentileMethod = "linear-interpolation-q-times-n-minus-one"
    }
    scenes = $sceneSummaries
}
$summaryPath = Join-Path $resultRoot "summary.json"
$summary |
    ConvertTo-Json -Depth 12 |
    Set-Content -LiteralPath $summaryPath -Encoding UTF8
Write-Host "Shadow optimization metadata: $metadataPath"
Write-Host "Shadow optimization summary: $summaryPath"
