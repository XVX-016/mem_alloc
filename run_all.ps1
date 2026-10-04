# PowerShell equivalent of run_all.sh (Windows opens .sh files in the editor).
$ErrorActionPreference = "Stop"
Set-Location $PSScriptRoot

$env:Path = "C:\msys64\mingw64\bin;C:\msys64\usr\bin;" + $env:Path

$make = Get-Command mingw32-make -ErrorAction SilentlyContinue
if (-not $make) { $make = Get-Command make -ErrorAction SilentlyContinue }
if (-not $make) {
    Write-Error "make or mingw32-make not found. Add C:\msys64\mingw64\bin to PATH."
}

function Invoke-Built([string]$stem) {
    $exe = Join-Path $PSScriptRoot "build\$stem.exe"
    $bin = Join-Path $PSScriptRoot "build\$stem"
    if (Test-Path $exe) { & $exe }
    elseif (Test-Path $bin) { & $bin }
    else { throw "missing $stem" }
}

Write-Host "==> build (Release -O2)"
& $make.Source -j 4 all
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }

Write-Host "`n==> tests"
Invoke-Built tests
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }

$asanOk = "skipped"
$tsanOk = "skipped"

Write-Host "`n==> AddressSanitizer + UBSan (separate target)"
& $make.Source asan
if ($LASTEXITCODE -eq 0) {
    Invoke-Built tests_asan
    if ($LASTEXITCODE -eq 0) { $asanOk = "passed" } else { $asanOk = "failed or unsupported" }
} else {
    $asanOk = "failed or unsupported"
    Write-Host "warning: ASan build/run did not succeed (common on some Windows toolchains)"
}

Write-Host "`n==> ThreadSanitizer (separate target)"
& $make.Source tsan
if ($LASTEXITCODE -eq 0) {
    Invoke-Built tests_tsan
    if ($LASTEXITCODE -eq 0) { $tsanOk = "passed" } else { $tsanOk = "failed or unsupported" }
} else {
    $tsanOk = "failed or unsupported"
    Write-Host "warning: TSan build/run did not succeed (often unavailable on MinGW/Windows)"
}

New-Item -ItemType Directory -Force -Path (Join-Path $PSScriptRoot "build") | Out-Null

Write-Host "`n==> bench_threadpool"
Invoke-Built bench_threadpool | Tee-Object -FilePath (Join-Path $PSScriptRoot "build\bench_threadpool.out")

Write-Host "`n==> bench_allocator"
Invoke-Built bench_allocator | Tee-Object -FilePath (Join-Path $PSScriptRoot "build\bench_allocator.out")

Write-Host "`n========== SUMMARY =========="
Write-Host "tests:          passed"
Write-Host "asan+ubsan:     $asanOk"
Write-Host "tsan:           $tsanOk"
Write-Host ""
Write-Host "----- thread pool -----"
Get-Content (Join-Path $PSScriptRoot "build\bench_threadpool.out")
Write-Host ""
Write-Host "----- allocator -----"
Get-Content (Join-Path $PSScriptRoot "build\bench_allocator.out")
Write-Host "=============================="
