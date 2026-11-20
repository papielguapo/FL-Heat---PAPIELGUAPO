$ErrorActionPreference = 'Stop'
$source = Join-Path $PSScriptRoot 'FL HEAT.vst3'
if (-not (Test-Path -LiteralPath (Join-Path $source 'Contents\x86_64-win\FL HEAT.vst3'))) {
    $source = Join-Path $PSScriptRoot 'Binaries\FL HEAT.vst3'
}
if (-not (Test-Path -LiteralPath (Join-Path $source 'Contents\x86_64-win\FL HEAT.vst3'))) {
    throw 'No se encontró el bundle FL HEAT.vst3.'
}

$vstRoot = Join-Path $env:CommonProgramFiles 'VST3'
$target  = Join-Path $vstRoot 'FL HEAT.vst3'

New-Item -ItemType Directory -Path $vstRoot -Force | Out-Null
Copy-Item -LiteralPath $source -Destination $vstRoot -Recurse -Force

Write-Host "Instalado correctamente en: $target" -ForegroundColor Green
