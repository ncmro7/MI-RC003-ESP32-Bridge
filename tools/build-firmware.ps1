#Requires -Version 5.1
[CmdletBinding()]
param(
    [switch]$NoBuild,
    [ValidateSet('n16r8', 'n8r2', 'n4r2')]
    [string]$Profile = 'n16r8',
    [string]$FlashMode = 'dio',
    [string]$FlashFreq = '80m',
    [string]$FlashSize
)

$ErrorActionPreference = 'Stop'
. "$PSScriptRoot\common.ps1"

try {
    $profileDefaults = Join-Path $Script:RepoRoot ('sdkconfig.defaults.' + $Profile)
    if (-not (Test-Path $profileDefaults)) { throw ('未找到硬件配置: ' + $profileDefaults) }
    if (-not $FlashSize) {
        $FlashSize = @{ n16r8 = '16MB'; n8r2 = '8MB'; n4r2 = '4MB' }[$Profile]
    }
    $baseDefaults = Join-Path $Script:RepoRoot 'sdkconfig.defaults'
    $sdkconfigDefaults = $baseDefaults + ';' + $profileDefaults
    $sdkconfig = Join-Path $Script:RepoRoot ('.sdkconfig.' + $Profile)
    Write-Title '一键生成烧录固件'

    $version = Get-FirmwareVersion
    Write-Info ('版本: ' + $version + '，硬件: ' + $Profile.ToUpper())

    if (-not $NoBuild) {
        if (-not (Get-Command idf.py -ErrorAction SilentlyContinue)) { $null = Initialize-IdfEnv }
        if (-not (Get-Command idf.py -ErrorAction SilentlyContinue)) {
            throw '未找到 ESP-IDF 环境。请先安装 ESP-IDF，或在其终端中运行本脚本。'
        }
        Write-Info ('编译固件 (idf.py build, ' + $Profile.ToUpper() + ')')
        Push-Location $Script:RepoRoot
        try {
            & idf.py -D "SDKCONFIG=$sdkconfig" -D "SDKCONFIG_DEFAULTS=$sdkconfigDefaults" build
            if ($LASTEXITCODE -ne 0) { throw ('编译失败 (退出码 ' + $LASTEXITCODE + ')') }
        } finally {
            Pop-Location
        }
    } else {
        Write-Info '跳过编译，使用现有 build 产物'
    }

    $plan = Get-FlashPlan
    $tool = Get-Esptool
    if (-not $tool) { throw '未找到 esptool，请安装 ESP-IDF 或执行: pip install esptool' }

    $merged = Join-Path $Script:BuildDir 'merged-flash.bin'
    New-MergedBin -Esptool $tool -Plan $plan -OutFile $merged `
        -FlashMode $FlashMode -FlashFreq $FlashFreq -FlashSize $FlashSize
    Write-Ok ('Windows 烧录固件: ' + $merged)

    New-WebFlashFiles -Version $version -MergedBin $merged
    Write-Ok ('网页烧录固件: ' + (Join-Path $Script:WebFlashDir 'firmware\merged-flash.bin'))
    Write-Ok ('网页 manifest: ' + (Join-Path $Script:WebFlashDir 'manifest.json'))

    Write-Title '完成'
    Write-Host '  Windows 烧录工具固件: build\merged-flash.bin' -ForegroundColor DarkGray
    Write-Host '  网页烧录固件:         webusb-config\flash\firmware\merged-flash.bin' -ForegroundColor DarkGray
    Write-Host ''
    Write-Host '下一步: 运行 package-release.bat 打包 Windows 免安装烧录工具。' -ForegroundColor Cyan
} catch {
    Write-Fail $_.Exception.Message
    exit 1
}
