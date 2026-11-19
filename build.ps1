param([string]$Compiler = 'g++', [string]$ResourceCompiler = 'windres')
$ErrorActionPreference = 'Stop'
$flags = @('-std=c++17', '-Os', '-Wall', '-Wextra', '-Wpedantic', '-static', '-s', '-DUNICODE', '-D_UNICODE')
Push-Location -LiteralPath $PSScriptRoot
try {
    & $Compiler @flags 'fitbuds.cpp' '-lbthprops' '-lws2_32' '-o' 'fitbuds.exe'
    if ($LASTEXITCODE -ne 0) { throw 'CLI build failed' }
    & $ResourceCompiler -i 'fitbuds_tray.rc' -O coff -o 'fitbuds_tray.res.o'
    if ($LASTEXITCODE -ne 0) { throw 'Icon resource build failed' }
    & $Compiler @flags '-municode' '-mwindows' 'fitbuds_tray.cpp' 'fitbuds_tray.res.o' '-lbthprops' '-lws2_32' '-lshell32' '-o' 'FitBudsTray.exe'
    if ($LASTEXITCODE -ne 0) { throw 'Tray build failed' }
} finally {
    Pop-Location
}
