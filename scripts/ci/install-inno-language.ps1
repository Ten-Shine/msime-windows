# Put the Chinese language file where the Inno Setup compiler will actually look for it.
#
# msime_setup.iss declares a chinesesimplified language pointing at
# compiler:Languages\ChineseSimplified.isl, and the Inno Setup on the runner does not ship that
# file, so ISCC aborts on the [Languages] section. Pinned by commit and checksum rather than
# tracking a branch, so the installer's Chinese text cannot change under a rebuild.
#
# "Where ISCC.exe is" is not the answer. On the windows-2025 image Inno Setup comes from Chocolatey
# and `Get-Command ISCC.exe` resolves to the shim at C:\ProgramData\Chocolatey\bin\ISCC.exe, so
# deriving the directory from it installed the file to C:\ProgramData\Chocolatey\bin\Languages\.
# ISPP resolves compiler: against the real installation, which is why every release build reported
# "Installed ... ChineseSimplified.isl" and then failed two steps later with "Couldn't open include
# file c:\program files (x86)\inno setup 6\Languages\ChineseSimplified.isl".
#
# So find the installation by a file only it has, and refuse to guess. Getting this wrong silently
# is what made it cost a whole release cycle to notice.
#
# Inno Setup 7 is probed before 6: build-installer.yml's package job installs the 64-bit 7.1.0
# edition (machine scope, so its uninstall key lands in the 64-bit registry view) before running
# this script, while ci.yml and release.yml still run against the hosted Chocolatey 6.7.1. Both
# editions' installations carry the marker files. Since 7.0.2 Simplified Chinese is an official
# translation, a 7.1 installation already ships ChineseSimplified.isl and the download below is
# skipped; the 6.x runners do not ship it, which is what the pin is for.
$ErrorActionPreference = 'Stop'

# The pin lives here rather than in the workflow so that ci.yml can run this script as a regression
# check without keeping a second copy of the URL and digest in step with this one.
$url = 'https://raw.githubusercontent.com/jrsoftware/issrc/1ae7bf81dc0d2013235dfe4bb0b6f4e4a0b6b25c/Files/Languages/ChineseSimplified.isl'
$expected = 'e0b0b350e2245f3c5e65586dfe43d574f6e7f06f2261149aba284954b3fc9a8d'

# Present in the installation directory and absent from a shim directory.
$markers = @('ISPPBuiltins.iss', 'Default.isl')

$candidates = [System.Collections.Generic.List[string]]::new()
# Inno Setup 7 first: the x64 edition writes 'Inno Setup 7_is1' to the 64-bit view, a 32-bit
# edition or a user-scope install lands elsewhere, so probe every view before falling back to 6.
foreach ($key in @(
        'HKLM:\SOFTWARE\Microsoft\Windows\CurrentVersion\Uninstall\Inno Setup 7_is1',
        'HKLM:\SOFTWARE\WOW6432Node\Microsoft\Windows\CurrentVersion\Uninstall\Inno Setup 7_is1',
        'HKCU:\SOFTWARE\Microsoft\Windows\CurrentVersion\Uninstall\Inno Setup 7_is1',
        'HKLM:\SOFTWARE\Microsoft\Windows\CurrentVersion\Uninstall\Inno Setup 7 (32-bit)_is1',
        'HKLM:\SOFTWARE\WOW6432Node\Microsoft\Windows\CurrentVersion\Uninstall\Inno Setup 7 (32-bit)_is1',
        'HKCU:\SOFTWARE\Microsoft\Windows\CurrentVersion\Uninstall\Inno Setup 7 (32-bit)_is1',
        'HKLM:\SOFTWARE\WOW6432Node\Microsoft\Windows\CurrentVersion\Uninstall\Inno Setup 6_is1',
        'HKLM:\SOFTWARE\Microsoft\Windows\CurrentVersion\Uninstall\Inno Setup 6_is1')) {
    $location = (Get-ItemProperty -Path $key -Name 'InstallLocation' -ErrorAction SilentlyContinue).InstallLocation
    if ($location) { $candidates.Add($location.TrimEnd('\')) }
}
foreach ($name in @('Inno Setup 7', 'Inno Setup 7 (32-bit)', 'Inno Setup 6')) {
    foreach ($base in @(${env:ProgramFiles(x86)}, $env:ProgramFiles, (Join-Path $env:LOCALAPPDATA 'Programs'))) {
        if ($base) { $candidates.Add((Join-Path $base $name)) }
    }
}
foreach ($command in @(Get-Command 'ISCC.exe' -CommandType Application -ErrorAction SilentlyContinue)) {
    $candidates.Add((Split-Path $command.Source -Parent))
}

$root = $candidates |
    Where-Object { $_ -and (Test-Path $_) } |
    Where-Object { $directory = $_; $markers | Where-Object { Test-Path (Join-Path $directory $_) } } |
    Select-Object -First 1

if (-not $root) {
    throw "Could not find the Inno Setup installation. Looked in: $($candidates -join '; ')"
}
Write-Host "Inno Setup installation: $root"

$languages = Join-Path $root 'Languages'
$target = Join-Path $languages 'ChineseSimplified.isl'

if (Test-Path $target) {
    Write-Host "Already present: $target"
    exit 0
}

New-Item -ItemType Directory -Path $languages -Force | Out-Null
Invoke-WebRequest -Uri $url -OutFile $target -MaximumRetryCount 3 -RetryIntervalSec 5

$actual = (Get-FileHash $target -Algorithm SHA256).Hash.ToLower()
if ($actual -ne $expected) {
    Remove-Item $target -Force
    throw "ChineseSimplified.isl checksum mismatch: expected $expected, got $actual"
}

Write-Host "Installed $target ($((Get-Item $target).Length) bytes)"
