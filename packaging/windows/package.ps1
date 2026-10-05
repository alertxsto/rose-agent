param(
    [Parameter(Mandatory=$true)][string]$BuildDirectory,
    [Parameter(Mandatory=$true)][string]$QtPrefix,
    [Parameter(Mandatory=$true)][string]$QtSources,
    [Parameter(Mandatory=$true)][ValidateSet('x86','x64')][string]$Architecture,
    [Parameter(Mandatory=$true)][string]$OutputDirectory,
    [string]$Version = '0.1.0'
)
$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest
function Invoke-Checked([string]$Program, [string[]]$Arguments) {
    & $Program @Arguments
    if ($LASTEXITCODE -ne 0) { throw "$Program failed with exit $LASTEXITCODE" }
}
$root = (Resolve-Path (Join-Path $PSScriptRoot '../..')).Path
$build = (Resolve-Path $BuildDirectory).Path
$qt = (Resolve-Path $QtPrefix).Path
$source = (Resolve-Path $QtSources).Path
New-Item -ItemType Directory -Force $OutputDirectory | Out-Null
$output = (Resolve-Path $OutputDirectory).Path
$stage = Join-Path $output "RoseAgent-$Version-windows-$Architecture"
if (Test-Path $stage) { throw "Refusing nonempty/existing stage: $stage" }
New-Item -ItemType Directory $stage | Out-Null
foreach ($name in @('rose-agent.exe','rose-cli.exe')) {
    Copy-Item (Join-Path $build "bin/$name") $stage
}
if ($Architecture -eq 'x86') {
    foreach ($name in @('Core','Gui','Widgets','Network','Concurrent','PrintSupport','Svg')) {
        Copy-Item (Join-Path $qt "bin/Qt5$name.dll") $stage
    }
    foreach ($plugin in @('platforms/qwindows.dll','imageformats/qjpeg.dll','imageformats/qsvg.dll','imageformats/qwebp.dll')) {
        $destination = Join-Path $stage "plugins/$plugin"
        New-Item -ItemType Directory -Force (Split-Path $destination) | Out-Null
        Copy-Item (Join-Path $qt "plugins/$plugin") $destination
    }
    $style = Join-Path $qt 'plugins/styles/qwindowsvistastyle.dll'
    if (Test-Path $style) {
        New-Item -ItemType Directory -Force (Join-Path $stage 'plugins/styles') | Out-Null
        Copy-Item $style (Join-Path $stage 'plugins/styles')
    }
} else {
    Invoke-Checked (Join-Path $qt 'bin/windeployqt.exe') @('--release','--no-translations','--no-compiler-runtime','--dir',$stage,(Join-Path $stage 'rose-agent.exe'))
    # WebP is supplied by the qtimageformats addon, not QtBase itself.
    if (!(Test-Path (Join-Path $stage 'imageformats/qwebp.dll'))) {
        Copy-Item (Join-Path $qt 'plugins/imageformats/qwebp.dll') (Join-Path $stage 'imageformats')
    }
}
$crtGeneration = if ($Architecture -eq 'x86') { '142' } else { '143' }
$crtRoot = Join-Path $env:VSINSTALLDIR 'VC/Redist/MSVC'
$crtVersions = @(Get-ChildItem $crtRoot -Directory | Where-Object { $Architecture -ne 'x86' -or $_.Name -like '14.29.*' } | Sort-Object Name -Descending)
$crt = $null
foreach ($candidate in $crtVersions) {
    $directory = Join-Path $candidate.FullName "$Architecture/Microsoft.VC$crtGeneration.CRT"
    if (Test-Path $directory) { $crt = $directory; break }
}
if (!$crt) { throw "App-local VC$crtGeneration CRT unavailable (legacy x86 requires exact v14214.29)" }
Copy-Item (Join-Path $crt '*.dll') $stage
if ($Architecture -eq 'x86') {
    $ucrt = Join-Path ${env:ProgramFiles(x86)} 'Windows Kits/10/Redist/10.0.19041.0/ucrt/DLLs/x86'
    if (!(Test-Path (Join-Path $ucrt 'ucrtbase.dll'))) { throw 'Windows8.1-compatible SDK19041 app-local UCRT is required' }
    Copy-Item (Join-Path $ucrt '*.dll') $stage
    "[Paths]`r`nPrefix=.`r`nPlugins=plugins`r`n" | Set-Content -Encoding ascii (Join-Path $stage 'qt.conf')
} else {
    "[Paths]`r`nPrefix=.`r`nPlugins=.`r`n" | Set-Content -Encoding ascii (Join-Path $stage 'qt.conf')
}
foreach ($name in @('README.md','ROADMAP.md','CHANGELOG.md','LICENSE')) {
    Copy-Item (Join-Path $root $name) $stage
}
New-Item -ItemType Directory -Force (Join-Path $stage 'docs') | Out-Null
foreach ($directory in @('usage','format','compatibility','releases')) {
    Copy-Item (Join-Path $root "docs/$directory") (Join-Path $stage 'docs') -Recurse
}
Invoke-Checked 'python' @((Join-Path $root 'packaging/copy-third-party-notices.py'),'--sources',$source,'--destination',(Join-Path $stage 'third-party'),'--version',$(if($Architecture -eq 'x86'){'5.15.18'}else{'6.8.3'}))
@"
Microsoft Visual C++ runtime: dynamically linked app-local VC$crtGeneration CRT.
Build/distribution provenance: Visual Studio licensed GitHub-hosted runner.
Legacy x86 uses v14214.29 CRT and Windows SDK10.0.19041 app-local Universal CRT;
no latest VC143 redistributable is installed into Windows8.1.
https://learn.microsoft.com/cpp/windows/redistributing-visual-cpp-files
https://learn.microsoft.com/cpp/windows/universal-crt-deployment
Qt5 x86 uses Windows Schannel and the OS certificate store, not OpenSSL1.1.
Windows8.1 is out of Microsoft support; this package does not repair that OS.
"@ | Set-Content -Encoding utf8 (Join-Path $stage 'third-party/Microsoft-runtime-NOTICE.txt')
Invoke-Checked 'python' @((Join-Path $root 'packaging/windows/verify-pe.py'),'--directory',$stage,'--architecture',$Architecture)
$uninstall = @()
foreach ($file in Get-ChildItem $stage -File -Recurse) {
    $relative = $file.FullName.Substring($stage.Length + 1)
    if ($relative.Contains('$') -or $relative.Contains('"')) { throw 'Unsafe package file name for NSIS' }
    $uninstall += 'Delete "$INSTDIR\' + $relative + '"'
}
$uninstall += 'Delete "$INSTDIR\uninstall-files.nsh"'
foreach ($directory in Get-ChildItem $stage -Directory -Recurse | Sort-Object { $_.FullName.Length } -Descending) {
    $uninstall += 'RMDir "$INSTDIR\' + $directory.FullName.Substring($stage.Length + 1) + '"'
}
$uninstall | Set-Content -Encoding utf8 (Join-Path $stage 'uninstall-files.nsh')
$installer = Join-Path $output "RoseAgent-$Version-windows-$Architecture-setup.exe"
$makensis = Join-Path ${env:ProgramFiles(x86)} 'NSIS/makensis.exe'
if (!(Test-Path $makensis)) { throw 'NSIS makensis is required' }
Invoke-Checked $makensis @("/DVERSION=$Version","/DARCH=$Architecture","/DSTAGE=$stage","/DOUTPUT=$installer",(Join-Path $root 'packaging/windows/installer.nsi'))
Compress-Archive -Path (Join-Path $stage '*') -DestinationPath (Join-Path $output "RoseAgent-$Version-windows-$Architecture-portable.zip")
if ($env:GITHUB_OUTPUT) {
    "stage=$stage" | Out-File -Append -Encoding utf8 $env:GITHUB_OUTPUT
}
