$ErrorActionPreference = 'Stop'

$projectRoot = Split-Path -Parent $MyInvocation.MyCommand.Path
Push-Location $projectRoot
try {
    cmake --preset windows-release
    if ($LASTEXITCODE -ne 0) { throw "CMake configuration failed" }
    cmake --build --preset windows-release --target frame_sender
    if ($LASTEXITCODE -ne 0) { throw "FrameSender build failed" }
}
finally {
    Pop-Location
}

Write-Host "Built: $projectRoot\build\Release\frame_sender.exe"
