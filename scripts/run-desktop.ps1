[CmdletBinding()]
param(
    [string]$Executable = '',
    [string]$QtRoot = 'D:\Qt\6.7.3\msvc2019_64',
    [string]$OpenSslBin = 'D:\Qt\Tools\OpenSSLv3\Win_x64\bin',
    [switch]$SoftwareRendering
)

$ErrorActionPreference = 'Stop'
if ([string]::IsNullOrWhiteSpace($Executable)) {
    $Executable = Join-Path $PSScriptRoot '..\build-clang\vibePlayerQT.exe'
}
$appPath = (Get-Item -LiteralPath $Executable -ErrorAction Stop).FullName
if ([IO.Path]::GetExtension($appPath) -ne '.exe') {
    throw 'Executable must point to the desktop .exe entry point.'
}

# Change only this launch environment; leave user/system settings untouched.
$environmentNames = @('PATH', 'QT_PLUGIN_PATH', 'QT_QPA_PLATFORM_PLUGIN_PATH', 'QT_QPA_PLATFORM', 'QSG_RHI_BACKEND', 'QSG_RHI_PREFER_SOFTWARE_RENDERER')
$savedEnvironment = @{}
foreach ($name in $environmentNames) {
    $savedEnvironment[$name] = [Environment]::GetEnvironmentVariable($name, 'Process')
}

try {
    $runtimePaths = @()
    if (Test-Path -LiteralPath (Join-Path $QtRoot 'bin') -PathType Container) {
        $runtimePaths += (Join-Path $QtRoot 'bin')
    }
    if (Test-Path -LiteralPath $OpenSslBin -PathType Container) {
        $runtimePaths += $OpenSslBin
    }
    $env:PATH = (@($runtimePaths) + @($env:PATH)) -join ';'
    if (Test-Path -LiteralPath (Join-Path $QtRoot 'plugins') -PathType Container) {
        $env:QT_PLUGIN_PATH = Join-Path $QtRoot 'plugins'
        $env:QT_QPA_PLATFORM_PLUGIN_PATH = Join-Path $QtRoot 'plugins\platforms'
    }
    # Do not inherit an offscreen/minimal platform from a preceding test session.
    $env:QT_QPA_PLATFORM = 'windows'
    if ($SoftwareRendering) {
        # Qt's D3D11 WARP backend retains RHI shader effects. This setting applies
        # to the Qt interface; libmpv owns its separate embedded video renderer.
        $env:QSG_RHI_BACKEND = 'd3d11'
        $env:QSG_RHI_PREFER_SOFTWARE_RENDERER = '1'
    }

    $desktopProcess = Start-Process -FilePath $appPath -WorkingDirectory (Split-Path -Parent $appPath) -WindowStyle Normal -PassThru
    Write-Host "Desktop application launched (PID $($desktopProcess.Id)): $appPath"
}
finally {
    foreach ($name in $environmentNames) {
        [Environment]::SetEnvironmentVariable($name, $savedEnvironment[$name], 'Process')
    }
}
