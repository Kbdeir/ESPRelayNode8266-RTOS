[CmdletBinding()]
param()

$ErrorActionPreference = 'Stop'
$toolchainUrl = 'https://dl.espressif.com/dl/xtensa-lx106-elf-gcc8_4_0-esp-2020r3-win32.zip'
$platformioHome = if ($env:PLATFORMIO_CORE_DIR) {
    $env:PLATFORMIO_CORE_DIR
} else {
    Join-Path $env:USERPROFILE '.platformio'
}
$packagesDir = Join-Path $platformioHome 'packages'
$packageDir = Join-Path $packagesDir 'toolchain-xtensa'
$compiler = Join-Path $packageDir 'bin\xtensa-lx106-elf-gcc.exe'
$runtime = Join-Path $packageDir 'xtensa-lx106-elf\lib\libc_nano.a'
$manifestPath = Join-Path $packageDir 'package.json'

function Write-PackageMetadata([string]$root) {
    $manifest = [ordered]@{
        name = 'toolchain-xtensa'
        version = '8.4.0'
        description = 'Espressif GCC toolchain for ESP8266 RTOS SDK 3.4'
        system = @('windows_amd64', 'windows_x86', 'windows')
    }
    $manifest | ConvertTo-Json | Set-Content -LiteralPath (Join-Path $root 'package.json') -Encoding ASCII

    $metadata = [ordered]@{
        type = 'tool'
        name = 'toolchain-xtensa'
        version = '8.4.0'
        spec = [ordered]@{
            owner = 'platformio'
            id = $null
            name = 'toolchain-xtensa'
            requirements = '~8.4.0'
            uri = $toolchainUrl
        }
    }
    $metadata | ConvertTo-Json -Depth 3 -Compress | Set-Content -LiteralPath (Join-Path $root '.piopm') -Encoding ASCII
}

function Move-IncompleteCompetitors {
    $backupRoot = Join-Path $platformioHome 'package-backups'
    foreach ($candidate in Get-ChildItem -LiteralPath $packagesDir -Directory -Filter 'toolchain-xtensa@*') {
        $candidateManifest = Join-Path $candidate.FullName 'package.json'
        $candidateRuntime = Join-Path $candidate.FullName 'xtensa-lx106-elf\lib\libc_nano.a'
        if (!(Test-Path -LiteralPath $candidateManifest) -or (Test-Path -LiteralPath $candidateRuntime)) {
            continue
        }
        $details = Get-Content -LiteralPath $candidateManifest -Raw | ConvertFrom-Json
        if ($details.version -notlike '8.4.0*') {
            continue
        }
        New-Item -ItemType Directory -Path $backupRoot -Force | Out-Null
        $destination = Join-Path $backupRoot $candidate.Name
        if (Test-Path -LiteralPath $destination) {
            $destination += '-' + (Get-Date -Format 'yyyyMMddHHmmss')
        }
        Move-Item -LiteralPath $candidate.FullName -Destination $destination
        Write-Host "Incomplete competing package preserved at $destination"
    }
}

if ((Test-Path -LiteralPath $compiler) -and (Test-Path -LiteralPath $runtime) -and
    (Test-Path -LiteralPath $manifestPath)) {
    $installed = Get-Content -LiteralPath $manifestPath -Raw | ConvertFrom-Json
    if ($installed.version -like '8.4.0*') {
        Write-PackageMetadata $packageDir
        Move-IncompleteCompetitors
        Write-Host 'ESP8266 GCC 8.4.0 toolchain is already installed.'
        exit 0
    }
}

$stage = Join-Path ([IO.Path]::GetTempPath()) ('esp8266-toolchain-' + [guid]::NewGuid().ToString('N'))
$archive = Join-Path $stage 'toolchain.zip'
$extract = Join-Path $stage 'extract'
New-Item -ItemType Directory -Path $extract -Force | Out-Null

try {
    Write-Host 'Downloading Espressif ESP8266 GCC 8.4.0...'
    Invoke-WebRequest -Uri $toolchainUrl -OutFile $archive
    Expand-Archive -LiteralPath $archive -DestinationPath $extract

    $source = Join-Path $extract 'xtensa-lx106-elf'
    $sourceCompiler = Join-Path $source 'bin\xtensa-lx106-elf-gcc.exe'
    $sourceRuntime = Join-Path $source 'xtensa-lx106-elf\lib\libc_nano.a'
    if (!(Test-Path -LiteralPath $sourceCompiler) -or !(Test-Path -LiteralPath $sourceRuntime)) {
        throw 'Downloaded toolchain is incomplete.'
    }

    Write-PackageMetadata $source

    New-Item -ItemType Directory -Path $packagesDir -Force | Out-Null
    if (Test-Path -LiteralPath $packageDir) {
        $suffix = Get-Date -Format 'yyyyMMddHHmmss'
        $backup = Join-Path $packagesDir ("toolchain-xtensa@backup-$suffix")
        Move-Item -LiteralPath $packageDir -Destination $backup
        Write-Host "Previous toolchain preserved at $backup"
    }
    Move-Item -LiteralPath $source -Destination $packageDir
    Move-IncompleteCompetitors
    & $compiler --version | Select-Object -First 1
} finally {
    if (Test-Path -LiteralPath $stage) {
        Remove-Item -LiteralPath $stage -Recurse -Force
    }
}
