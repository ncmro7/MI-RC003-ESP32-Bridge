#Requires -Version 5.1
[CmdletBinding()]
param(
    [switch]$NoBuild,
    [ValidateSet('n16r8', 'n8r2', 'n4r2')]
    [string[]]$Profile = @('n16r8', 'n8r2', 'n4r2'),
    [string]$FlashMode = 'dio',
    [string]$FlashFreq = '80m'
)

$ErrorActionPreference = 'Stop'
. "$PSScriptRoot\common.ps1"

try {
    $profiles = @($Profile | ForEach-Object { $_.ToLower() })
    if ($profiles.Count -eq 0) { throw '请至少指定一个硬件配置。' }
    foreach ($p in $profiles) { $null = Get-ProfileInfo $p }

    Write-Title '一键生成烧录固件'
    $version = Get-FirmwareVersion
    Write-Info ('版本: ' + $version)
    Write-Info ('硬件: ' + (($profiles | ForEach-Object { (Get-ProfileInfo $_).Label }) -join ' / '))

    if (-not $NoBuild) {
        if (-not (Get-Command idf.py -ErrorAction SilentlyContinue)) { $null = Initialize-IdfEnv }
        if (-not (Get-Command idf.py -ErrorAction SilentlyContinue)) {
            throw '未找到 ESP-IDF 环境。请先安装 ESP-IDF，或在其终端中运行本脚本。'
        }
    }

    $tool = Get-Esptool
    if (-not $tool) { throw '未找到 esptool，请安装 ESP-IDF 或执行: pip install esptool' }

    New-Item -ItemType Directory -Force -Path $Script:FirmwareOutDir | Out-Null
    $baseDefaults = Join-Path $Script:RepoRoot 'sdkconfig.defaults'
    $mergedBins = [ordered]@{}

    foreach ($prof in $profiles) {
        $info = Get-ProfileInfo $prof
        $profileBuildDir = Get-ProfileBuildDir $prof
        $sdkconfig = Get-ProfileSdkconfig $prof
        $sdkconfigDefaults = $baseDefaults + ';' + (Get-ProfileDefaultsFile $prof)

        if (-not $NoBuild) {
            Write-Info ('编译固件 (' + $info.Label + ', idf.py build)')
            Push-Location $Script:RepoRoot
            try {
                $prevEap = $ErrorActionPreference
                $ErrorActionPreference = 'Continue'
                try {
                    & idf.py -B $profileBuildDir -D "SDKCONFIG=$sdkconfig" -D "SDKCONFIG_DEFAULTS=$sdkconfigDefaults" build
                    $buildCode = $LASTEXITCODE
                } finally {
                    $ErrorActionPreference = $prevEap
                }
                if ($buildCode -ne 0) { throw ('编译失败 (' + $info.Label + '，退出码 ' + $buildCode + ')') }
            } finally {
                Pop-Location
            }
        } else {
            Write-Info ('跳过编译 (' + $info.Label + ')，使用现有 build 产物')
        }

        if (-not (Test-Path (Join-Path $profileBuildDir 'flasher_args.json')) -and
            $prof -eq $Script:DefaultProfile -and
            (Test-Path (Join-Path $Script:BuildDir 'flasher_args.json'))) {
            $profileBuildDir = $Script:BuildDir
        }

        $plan = Get-FlashPlan -BuildDir $profileBuildDir
        $merged = Join-Path $Script:FirmwareOutDir ('merged-flash-' + $prof + '.bin')
        New-MergedBin -Esptool $tool -Plan $plan -OutFile $merged `
            -FlashMode $FlashMode -FlashFreq $FlashFreq -FlashSize $info.Flash
        Write-Ok ($info.Label + ' 固件: ' + $merged)
        $mergedBins[$prof] = $merged
    }

    $legacySource = if ($mergedBins.Contains($Script:DefaultProfile)) { $mergedBins[$Script:DefaultProfile] } else { @($mergedBins.Values)[0] }
    Copy-Item $legacySource (Join-Path $Script:BuildDir 'merged-flash.bin') -Force

    New-WebFlashFiles -Version $version -MergedBins $mergedBins
    Write-Ok ('网页烧录固件目录: ' + (Join-Path $Script:WebFlashDir 'firmware'))
    Write-Ok ('网页 manifest / boards: ' + $Script:WebFlashDir)

    Write-Title '完成'
    Write-Host ('  Windows 烧录工具固件: build\firmware\merged-flash-<profile>.bin（默认: build\merged-flash.bin）') -ForegroundColor DarkGray
    Write-Host ('  网页烧录固件:         webusb-config\flash\firmware\merged-flash-<profile>.bin') -ForegroundColor DarkGray
    Write-Host ''
    Write-Host '下一步: 运行 package-release.bat 打包 Windows 免安装烧录工具。' -ForegroundColor Cyan
} catch {
    Write-Fail $_.Exception.Message
    exit 1
}
