$ErrorActionPreference = 'Stop'

$projectRoot = Split-Path -Parent $MyInvocation.MyCommand.Path
& "$projectRoot\build.ps1"

Push-Location $projectRoot
try {
    New-Item -ItemType Directory -Force -Path "$projectRoot\dist" | Out-Null
    cpack --config "$projectRoot\build\CPackConfig.cmake" -C Release -G ZIP -B "$projectRoot\dist"
    if ($LASTEXITCODE -ne 0) { throw "FrameSender packaging failed" }
}
finally {
    Pop-Location
}

Write-Host "Package written under: $projectRoot\dist"
