# SPDX-License-Identifier: GPL-3.0-only WITH GPL-3.0-linking-exception
# Installs the JieLi 2.5.3 toolchain on a CI Windows machine with no clicks.
# The installer's format is not documented, so this tries the common silent
# installer flags first, then falls back to unpacking it with 7-Zip.
# Prints the pi32 folder (the one holding bin/clang.exe) to $env:GITHUB_OUTPUT.
$ErrorActionPreference = 'Stop'
$url = 'https://jl-update.oss-cn-shenzhen.aliyuncs.com/2.5.3.exe'
$expectedClang = 'd4bc97cf421c1ccc373dd8924ce88720ff7457b3f1478d15df4aeda6ae923bce'
$target = 'C:\JL'

function Find-Pi32 {
    foreach ($root in @($target, 'C:\JL_extract', "$env:ProgramFiles", "${env:ProgramFiles(x86)}", "$env:LOCALAPPDATA")) {
        if (!$root -or !(Test-Path $root)) { continue }
        $hit = Get-ChildItem -Path $root -Recurse -Filter clang.exe -ErrorAction SilentlyContinue |
            Where-Object { $_.Directory.Name -eq 'bin' -and (Test-Path (Join-Path $_.Directory.FullName 'pi32v2-ld.exe')) } |
            Select-Object -First 1
        if ($hit) { return $hit.Directory.Parent.FullName }
    }
    return $null
}

function Try-Installer($installer, [string[]]$arguments, $label) {
    Write-Host "Trying $label installer flags: $($arguments -join ' ')"
    $p = Start-Process -FilePath $installer -ArgumentList $arguments -PassThru
    if (!$p.WaitForExit(900000)) { Write-Host "  timed out, stopping it"; $p | Stop-Process -Force; return }
    Write-Host "  exit code $($p.ExitCode)"
}

$pi32 = Find-Pi32
if (!$pi32) {
    $installer = Join-Path $env:RUNNER_TEMP 'jl-2.5.3.exe'
    if (!(Test-Path $installer)) {
        Write-Host "Downloading $url"
        curl.exe --fail --location --retry 3 --output $installer $url
        if ($LASTEXITCODE) { throw "Download failed ($LASTEXITCODE)" }
    }
    Write-Host ("Installer: {0:N0} bytes, SHA-256 {1}" -f (Get-Item $installer).Length, (Get-FileHash $installer -Algorithm SHA256).Hash)
    Write-Host '7-Zip view of the installer:'
    & 7z l $installer | Select-Object -First 25 | ForEach-Object { Write-Host "  $_" }

    Try-Installer $installer @('/VERYSILENT','/SUPPRESSMSGBOXES','/NORESTART','/SP-',"/DIR=$target") 'Inno Setup'
    $pi32 = Find-Pi32
    if (!$pi32) {
        Try-Installer $installer @('/S',"/D=$target") 'NSIS'
        $pi32 = Find-Pi32
    }
    if (!$pi32) {
        Write-Host 'Silent install did not produce clang.exe; unpacking with 7-Zip instead.'
        & 7z x $installer '-oC:\JL_extract' -y | Out-Null
        # Nested archives are common in these installers; unpack one level more.
        Get-ChildItem 'C:\JL_extract' -Recurse -Include *.7z,*.zip,*.cab -ErrorAction SilentlyContinue | ForEach-Object {
            & 7z x $_.FullName ("-o" + $_.FullName + '_x') -y | Out-Null
        }
        $pi32 = Find-Pi32
    }
}
if (!$pi32) {
    Write-Host 'Could not find bin/clang.exe. Folders created:'
    foreach ($d in @($target, 'C:\JL_extract')) { if (Test-Path $d) { Get-ChildItem $d -Recurse -Depth 3 | Select-Object -First 80 | ForEach-Object { Write-Host "  $($_.FullName)" } } }
    throw 'JieLi toolchain install failed.'
}
$hash = (Get-FileHash (Join-Path $pi32 'bin/clang.exe') -Algorithm SHA256).Hash.ToLowerInvariant()
Write-Host "Toolchain: $pi32 (clang.exe $hash)"
if ($hash -ne $expectedClang) { throw "clang.exe SHA-256 does not match the 2.5.3 toolchain the SDK expects." }
"pi32=$pi32" | Out-File -FilePath $env:GITHUB_OUTPUT -Append -Encoding utf8
