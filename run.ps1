param(
    [ValidateSet("run", "debug")]
    [string]$Action = "run",

    [ValidateSet("auto", "gcc", "clang")]
    [string]$Compiler = "auto",

    [switch]$Reconfigure
)

$ErrorActionPreference = "Stop"

$ProjectRoot = Split-Path -Parent $MyInvocation.MyCommand.Path
$BuildDir = Join-Path $ProjectRoot "build"
$CacheFile = Join-Path $BuildDir "CMakeCache.txt"
$Generator = "Ninja"
$Clang64Bin = "E:\msys2\clang64\bin"
$LlvmBin = "E:\LLVM\bin"
$MsysBin = "E:\msys2\ucrt64\bin"

function Add-ToPath([string]$Dir) {
    if ((Test-Path $Dir) -and -not (($env:PATH -split ';') -contains $Dir)) {
        $env:PATH = "$Dir;$env:PATH"
    }
}

function Resolve-CommandPath([string[]]$Names) {
    foreach ($Name in $Names) {
        $Command = Get-Command $Name -ErrorAction SilentlyContinue | Select-Object -First 1
        if ($Command) {
            return $Command.Source
        }
    }
    return $null
}

function Get-ProjectName {
    $CMakeLists = Join-Path $ProjectRoot "CMakeLists.txt"
    if (-not (Test-Path $CMakeLists)) {
        return $null
    }

    $Match = Select-String -Path $CMakeLists -Pattern 'project\s*\(\s*([A-Za-z0-9_.-]+)' | Select-Object -First 1
    if ($Match) {
        return $Match.Matches[0].Groups[1].Value
    }

    return $null
}

function Get-CachedGenerator {
    if (-not (Test-Path $CacheFile)) {
        return $null
    }

    $Match = Select-String -Path $CacheFile -Pattern '^CMAKE_GENERATOR:INTERNAL=(.+)$' | Select-Object -First 1
    if ($Match) {
        return $Match.Matches[0].Groups[1].Value
    }

    return $null
}

function Resolve-CompilerPair([string]$SelectedCompiler) {
    switch ($SelectedCompiler) {
        "clang" {
            return @{
                C = Join-Path $Clang64Bin "clang.exe"
                CXX = Join-Path $Clang64Bin "clang++.exe"
            }
        }
        "gcc" {
            Add-ToPath $MsysBin
            return @{
                C = Resolve-CommandPath @("gcc", "cc")
                CXX = Resolve-CommandPath @("g++", "c++")
            }
        }
        default {
            $AutoClang = Join-Path $Clang64Bin "clang.exe"
            $AutoClangxx = Join-Path $Clang64Bin "clang++.exe"

            if ((Test-Path $AutoClang) -and (Test-Path $AutoClangxx)) {
                return @{
                    C = $AutoClang
                    CXX = $AutoClangxx
                }
            }

            return @{
                C = $null
                CXX = $null
            }
        }
    }
}

Add-ToPath $Clang64Bin
Add-ToPath $LlvmBin
Add-ToPath $MsysBin

$CachedGenerator = Get-CachedGenerator
if ($CachedGenerator -and $CachedGenerator -ne $Generator -and -not $Reconfigure) {
    Write-Host "Generator changed from '$CachedGenerator' to '$Generator', clearing cached CMake configuration..."
    $Reconfigure = $true
}

if ($Compiler -ne "auto" -and -not $Reconfigure -and (Test-Path $CacheFile)) {
    Write-Host "Compiler change requested, clearing cached CMake configuration..."
    $Reconfigure = $true
}

if ($Reconfigure) {
    Remove-Item $CacheFile -Force -ErrorAction SilentlyContinue
    Remove-Item (Join-Path $BuildDir "CMakeFiles") -Recurse -Force -ErrorAction SilentlyContinue
    Remove-Item (Join-Path $BuildDir "build.ninja") -Force -ErrorAction SilentlyContinue
    Remove-Item (Join-Path $BuildDir "rules.ninja") -Force -ErrorAction SilentlyContinue
    Remove-Item (Join-Path $BuildDir ".ninja_deps") -Force -ErrorAction SilentlyContinue
    Remove-Item (Join-Path $BuildDir ".ninja_log") -Force -ErrorAction SilentlyContinue
    Remove-Item (Join-Path $BuildDir "Makefile") -Force -ErrorAction SilentlyContinue
}

if (-not (Test-Path $CacheFile)) {
    Write-Host "Configuring project..."
    $ConfigureArgs = @("-S", $ProjectRoot, "-B", $BuildDir, "-G", $Generator)
    $CompilerPair = Resolve-CompilerPair $Compiler

    if ($CompilerPair.C -and $CompilerPair.CXX) {
        $ConfigureArgs += "-DCMAKE_C_COMPILER=$($CompilerPair.C)"
        $ConfigureArgs += "-DCMAKE_CXX_COMPILER=$($CompilerPair.CXX)"
    } elseif ($Compiler -ne "auto") {
        throw "Requested compiler '$Compiler' is not available in PATH."
    }

    cmake @ConfigureArgs
    if ($LASTEXITCODE -ne 0) {
        exit $LASTEXITCODE
    }
}

Write-Host "Building project..."
cmake --build $BuildDir --parallel
if ($LASTEXITCODE -ne 0) {
    exit $LASTEXITCODE
}

$ProjectName = Get-ProjectName
$Target = $null

if ($ProjectName) {
    $ExpectedTarget = Join-Path $BuildDir "$ProjectName.exe"
    if (Test-Path $ExpectedTarget) {
        $Target = Get-Item $ExpectedTarget
    }
}

if (-not $Target) {
    $Target = Get-ChildItem $BuildDir -Filter *.exe |
        Sort-Object LastWriteTime -Descending |
        Select-Object -First 1
}

if (-not $Target) {
    Write-Error "No executable found in $BuildDir"
}

Push-Location $BuildDir
try {
    if ($Action -eq "debug") {
        & gdb $Target.Name
    } else {
        & ".\$($Target.Name)"
    }
    exit $LASTEXITCODE
} finally {
    Pop-Location
}
