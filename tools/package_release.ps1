<#
.SYNOPSIS
    把已有的 Windows Release 构建打成可分发的 zip。

.DESCRIPTION
    从 build/Release 收集客户端和服务端，连同 README、docs、VERSION.txt
    一起打包到 dist/，并生成 SHA-256 校验文件。
    版本号默认读取 CMakeLists.txt 里的 project(VERSION ...)。

.PARAMETER Version
    发布版本号，不带前导 v。默认使用 CMakeLists.txt 的 project 版本。

.PARAMETER BuildDir
    CMake 构建目录，默认 build。

.PARAMETER OutputDir
    输出目录，默认 dist。

.EXAMPLE
    ./tools/package_release.ps1 -Version 0.1.0
#>
[CmdletBinding()]
param(
    [string]$Version = '',
    [string]$BuildDir = 'build',
    [string]$OutputDir = 'dist'
)

$ErrorActionPreference = 'Stop'

$root = Split-Path -Parent $PSScriptRoot
$buildPath = Join-Path $root $BuildDir
$outputPath = Join-Path $root $OutputDir

function Read-Match {
    param(
        [Parameter(Mandatory = $true)][string]$Path,
        [Parameter(Mandatory = $true)][string]$Pattern,
        [Parameter(Mandatory = $true)][string]$Description
    )

    if (-not (Test-Path -LiteralPath $Path)) {
        throw "Missing $Description source: $Path"
    }
    $text = Get-Content -LiteralPath $Path -Raw
    if ($text -notmatch $Pattern) {
        throw "Cannot read $Description from $Path"
    }
    return $Matches[1]
}

$Version = $Version.Trim() -replace '^[vV]', ''
if ([string]::IsNullOrWhiteSpace($Version)) {
    $Version = Read-Match `
        -Path (Join-Path $root 'CMakeLists.txt') `
        -Pattern 'project\s*\(\s*stellar_lord_taskbar\s+VERSION\s+([0-9]+\.[0-9]+\.[0-9]+)' `
        -Description 'project version'
}
if ($Version -notmatch '^[0-9]+\.[0-9]+\.[0-9]+([-.][0-9A-Za-z.-]+)?$') {
    throw "Invalid release version: '$Version'"
}

$client = Join-Path $buildPath 'Release\stellar_lord_taskbar.exe'
$server = Join-Path $buildPath 'Release\stellar_lord_server.exe'
foreach ($exe in @($client, $server)) {
    if (-not (Test-Path -LiteralPath $exe)) {
        throw "Missing build output: $exe. Run cmake --build $BuildDir --config Release first."
    }
}

$protocolVersion = Read-Match `
    -Path (Join-Path $root 'server\server_core.h') `
    -Pattern 'kProtocolVersion\s*=\s*(\d+)' `
    -Description 'protocol version'
$saveVersion = Read-Match `
    -Path (Join-Path $root 'src\game_model.cpp') `
    -Pattern 'text \+= "version=(\d+)\\n"' `
    -Description 'save version'

$commit = 'unknown'
try {
    $commitText = & git -C $root rev-parse --short=12 HEAD 2>$null
    if ($LASTEXITCODE -eq 0 -and $commitText) {
        $commit = $commitText.Trim()
    }
} catch {
    # 不在 git 工作区时保留 unknown。
}

$packageName = "StellarLord-$Version-windows-x64"
$stageRoot = Join-Path $outputPath 'staging'
$stage = Join-Path $stageRoot $packageName
$zip = Join-Path $outputPath "$packageName.zip"
$checksum = "$zip.sha256"

New-Item -ItemType Directory -Path $outputPath -Force | Out-Null
if (Test-Path -LiteralPath $stageRoot) {
    Remove-Item -LiteralPath $stageRoot -Recurse -Force
}
New-Item -ItemType Directory -Path $stage -Force | Out-Null

Copy-Item -LiteralPath $client -Destination $stage
Copy-Item -LiteralPath $server -Destination $stage
Copy-Item -LiteralPath (Join-Path $root 'README.md') -Destination $stage
Copy-Item -LiteralPath (Join-Path $root 'docs') -Destination $stage -Recurse

$builtAt = (Get-Date).ToUniversalTime().ToString('yyyy-MM-ddTHH:mm:ssZ')
$versionText = @"
Stellar Lord / 星际领主
version=$Version
protocol_version=$protocolVersion
save_version=$saveVersion
commit=$commit
built_at=$builtAt
"@
$utf8NoBom = New-Object System.Text.UTF8Encoding($false)
[System.IO.File]::WriteAllText(
    (Join-Path $stage 'VERSION.txt'), $versionText + "`n", $utf8NoBom)

if (Test-Path -LiteralPath $zip) {
    Remove-Item -LiteralPath $zip -Force
}
Compress-Archive -Path $stage -DestinationPath $zip -CompressionLevel Optimal

$hash = (Get-FileHash -LiteralPath $zip -Algorithm SHA256).Hash.ToLowerInvariant()
$hashLine = "$hash  $packageName.zip"
[System.IO.File]::WriteAllText(
    $checksum, $hashLine + "`n", (New-Object System.Text.ASCIIEncoding))

Remove-Item -LiteralPath $stageRoot -Recurse -Force

[pscustomobject]@{
    Version = $Version
    ProtocolVersion = $protocolVersion
    SaveVersion = $saveVersion
    Commit = $commit
    Zip = $zip
    Sha256 = $hash
}
