Set-StrictMode -Version Latest
$ErrorActionPreference = "Stop"

$RootDir = Split-Path -Parent $PSScriptRoot
$BuildDir = Join-Path $RootDir "build"
$BinDir = Join-Path $HOME "bin"
$StartupScript = Join-Path $BinDir "git-gencommit-env.ps1"

cmake -S $RootDir -B $BuildDir -DCMAKE_BUILD_TYPE=Release
cmake --build $BuildDir --config Release --parallel

if (-not (Test-Path $BinDir)) {
    New-Item -Path $BinDir -ItemType Directory -Force | Out-Null
}

$ExeCandidates = @(
    Join-Path (Join-Path $BuildDir "Release") "git-gencommit.exe",
    Join-Path $BuildDir "git-gencommit.exe",
    Join-Path $BuildDir "git-gencommit"
)

$BuiltExe = $null
foreach ($p in $ExeCandidates) {
    if (Test-Path $p) {
        $BuiltExe = $p
        break
    }
}

if (-not $BuiltExe) {
    throw "Built executable not found in $BuildDir"
}

$Target = Join-Path $BinDir "git-gencommit.exe"
Copy-Item -Path $BuiltExe -Destination $Target -Force

$StartupScriptContent = @'
$env:PATH = "$HOME\bin;$env:PATH"
'@
Set-Content -Path $StartupScript -Value $StartupScriptContent -Encoding UTF8

Write-Host "Installed: $Target"
Write-Host "Created startup script: $StartupScript"
Write-Host "Add this line to your PowerShell profile (`$PROFILE):"
Write-Host "  . `"$HOME\bin\git-gencommit-env.ps1`""

if (-not [Console]::IsInputRedirected -and -not [Console]::IsOutputRedirected) {
    Write-Host "Launching configuration wizard..."
    & $Target config
}
else {
    Write-Host "Run this once in a terminal to configure providers:"
    Write-Host "  git gencommit config"
}
