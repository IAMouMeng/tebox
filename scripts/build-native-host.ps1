# Local native Windows build entry point. Requires an existing MSYS2 UCRT64
# toolchain; installs no packages and never invokes WSL. See the shell helper
# for dependency requirements, environment overrides and staged source guards.
[CmdletBinding()]
param(
    [string]$MsysRoot = 'C:\msys64',
    [string]$HostRoot = $(if ($env:NATIVE_HOST_ROOT) { $env:NATIVE_HOST_ROOT } else { Join-Path $env:LOCALAPPDATA 'TeboxNative\host' }),
    [string]$SourceRef = $(if ($env:SOURCE_REF) { $env:SOURCE_REF } else { 'HEAD' }),
    [ValidateRange(0, 256)][int]$Jobs = 0,
    [switch]$StageOnly,
    [switch]$BuildOnly
)
$ErrorActionPreference = 'Stop'
$bash = Join-Path $MsysRoot 'usr\bin\bash.exe'
if (-not (Test-Path -LiteralPath $bash -PathType Leaf)) {
    throw "MSYS2 bash not found: $bash"
}
$helper = Join-Path $PSScriptRoot 'build-native-host.sh'
$overrides = @{
    MSYSTEM = 'UCRT64'
    MSYS2_PATH_TYPE = 'minimal'
    TEBOX_NATIVE_HELPER = $helper.Replace('\', '/')
    SOURCE_ROOT = (Split-Path $PSScriptRoot -Parent).Replace('\', '/')
    SOURCE_REF = $SourceRef
    NATIVE_HOST_ROOT = $HostRoot.Replace('\', '/')
}
if ($Jobs -gt 0) { $overrides.JOBS = [string]$Jobs }
if ($StageOnly) { $overrides.STAGE_ONLY = '1' }
if ($BuildOnly) { $overrides.BUILD_ONLY = '1' }
$original = @{}
$code = 1
try {
    foreach ($name in $overrides.Keys) {
        $original[$name] = [Environment]::GetEnvironmentVariable($name, 'Process')
        [Environment]::SetEnvironmentVariable($name, $overrides[$name], 'Process')
    }
    & $bash --login -c 'exec bash "$TEBOX_NATIVE_HELPER"'
    $code = $LASTEXITCODE
} finally {
    foreach ($name in $original.Keys) {
        [Environment]::SetEnvironmentVariable($name, $original[$name], 'Process')
    }
}
exit $code
