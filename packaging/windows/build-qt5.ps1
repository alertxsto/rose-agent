param(
    [Parameter(Mandatory=$true)][string]$WorkDirectory,
    [Parameter(Mandatory=$true)][string]$InstallDirectory,
    [int]$Jobs = 4
)
$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest
$version = '5.15.18'
$modules = @{
    qtbase = 'b7218518b03f42a3dde075f27bfdef30e236b95f6aae58db763f4453f5ce6e7d'
    qtsvg = '51e79736446896f264823436e38ff55b27d159d8c27b5d6601662cff381a44a9'
    qtimageformats = 'c6389a389381e9e4eaec466cbe2902e4c39cb1a53cf52b3545f3118cc37d5eaa'
}
function Invoke-Checked([string]$Program, [string[]]$Arguments) {
    & $Program @Arguments
    if ($LASTEXITCODE -ne 0) { throw "$Program failed with exit $LASTEXITCODE" }
}
New-Item -ItemType Directory -Force $WorkDirectory, $InstallDirectory | Out-Null
$WorkDirectory = (Resolve-Path $WorkDirectory).Path
$InstallDirectory = (Resolve-Path $InstallDirectory).Path
$sourceRoot = Join-Path $WorkDirectory 'sources'
New-Item -ItemType Directory -Force $sourceRoot | Out-Null
$jomArchive = Join-Path $WorkDirectory 'jom_1_1_4.zip'
if (!(Test-Path $jomArchive)) {
    Invoke-WebRequest 'https://download.qt.io/official_releases/jom/jom_1_1_4.zip' -OutFile $jomArchive
}
if ((Get-FileHash -Algorithm SHA256 $jomArchive).Hash.ToLowerInvariant() -ne 'd533c1ef49214229681e90196ed2094691e8c4a0a0bef0b2c901debcb562682b') {
    throw 'Pinned official jom checksum mismatch'
}
$jomDirectory = Join-Path $WorkDirectory 'jom'
if (!(Test-Path (Join-Path $jomDirectory 'jom.exe'))) {
    Expand-Archive $jomArchive $jomDirectory
}
foreach ($module in @('qtbase', 'qtsvg', 'qtimageformats')) {
    $name = "$module-everywhere-opensource-src-$version.zip"
    $archive = Join-Path $WorkDirectory $name
    if (!(Test-Path $archive)) {
        Invoke-WebRequest "https://download.qt.io/archive/qt/5.15/$version/submodules/$name" -OutFile $archive
    }
    if ((Get-FileHash -Algorithm SHA256 $archive).Hash.ToLowerInvariant() -ne $modules[$module]) {
        throw "Official Qt source checksum mismatch: $name"
    }
    $source = Join-Path $sourceRoot "$module-everywhere-src-$version"
    if (!(Test-Path $source)) { Expand-Archive -Path $archive -DestinationPath $sourceRoot }
    if (!(Test-Path $source)) { throw "Expected Qt source directory absent: $source" }
    $build = Join-Path $WorkDirectory "build-$module"
    New-Item -ItemType Directory -Force $build | Out-Null
    Push-Location $build
    try {
        if ($module -eq 'qtbase') {
            Invoke-Checked (Join-Path $source 'configure.bat') @(
                '-prefix', $InstallDirectory, '-opensource', '-confirm-license',
                '-release', '-shared', '-platform', 'win32-msvc',
                '-nomake', 'examples', '-nomake', 'tests',
                '-schannel', '-no-openssl', '-no-icu', '-no-opengl',
                '-qt-zlib', '-qt-libpng', '-qt-libjpeg', '-qt-pcre', '-qt-harfbuzz'
            )
        } else {
            Invoke-Checked (Join-Path $InstallDirectory 'bin/qmake.exe') @((Join-Path $source "$module.pro"))
        }
        Invoke-Checked (Join-Path $jomDirectory 'jom.exe') @('-j', [string]$Jobs)
        Invoke-Checked 'nmake.exe' @('/NOLOGO', 'install')
    } finally { Pop-Location }
}
Invoke-Checked (Join-Path $InstallDirectory 'bin/qmake.exe') @('-query', 'QT_VERSION')
if ($env:GITHUB_OUTPUT) {
    "qt-prefix=$InstallDirectory" | Out-File -Append -Encoding utf8 $env:GITHUB_OUTPUT
    "qt-sources=$sourceRoot" | Out-File -Append -Encoding utf8 $env:GITHUB_OUTPUT
}
