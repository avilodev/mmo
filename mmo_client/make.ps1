<#
.SYNOPSIS
    Build the client from PowerShell.

.DESCRIPTION
    Same job as make.bat, for PowerShell. The tree builds under MSYS2/MinGW64,
    so this locates MSYS2 and puts its two bin directories in front of PATH for
    the build only -- $env:PATH is restored on the way out, so the calling
    session is unchanged.

    Both directories are needed, in this order:

      mingw64\bin  gcc, pkg-config, mingw32-make -- the toolchain that produces
                   native Windows binaries, and must win over any other gcc.
      usr\bin      sh, rm, mkdir, cp -- the Makefile's recipes are Unix ones.
                   Without sh on PATH, mingw32-make runs recipes through
                   cmd.exe instead, where "mkdir -p" fails with "The syntax of
                   the command is incorrect" before anything is compiled.

    NOTE: running a .ps1 requires the execution policy to allow it. If you see
    "running scripts is disabled on this system", either use make.bat -- which
    execution policy does not apply to and which does exactly the same thing --
    or allow local scripts once with:

        Set-ExecutionPolicy -Scope CurrentUser RemoteSigned

    PowerShell does not search the current directory for commands, so this is
    invoked with a leading .\ :

        .\make.ps1            # builds everything
        .\make.ps1 clean
        .\make.ps1 game

    Every argument is passed straight through to mingw32-make.
#>

$ErrorActionPreference = 'Stop'

$msys2Home = @(
    $(if ($env:MSYS2_ROOT) { $env:MSYS2_ROOT }),
    'C:\msys64',
    (Join-Path $env:SystemDrive 'msys64'),
    (Join-Path $env:USERPROFILE 'msys64')
) | Where-Object { $_ -and (Test-Path (Join-Path $_ 'mingw64\bin\mingw32-make.exe')) } |
    Select-Object -First 1

if (-not $msys2Home -and -not (Get-Command mingw32-make -ErrorAction SilentlyContinue)) {
    Write-Host "ERROR: mingw32-make was not found." -ForegroundColor Red
    Write-Host ""
    Write-Host "The client builds with the MSYS2 MinGW64 toolchain. Install MSYS2, then"
    Write-Host "from an MSYS2 shell:"
    Write-Host ""
    Write-Host "    pacman -S mingw-w64-x86_64-gcc mingw-w64-x86_64-make mingw-w64-x86_64-openssl"
    Write-Host ""
    Write-Host "If MSYS2 lives somewhere other than C:\msys64, point MSYS2_ROOT at it:"
    Write-Host ""
    Write-Host '    $env:MSYS2_ROOT = "D:\msys64"'
    Write-Host ""
    exit 1
}

$savedPath = $env:PATH
try {
    if ($msys2Home) {
        $env:PATH = (Join-Path $msys2Home 'mingw64\bin') + ';' +
                    (Join-Path $msys2Home 'usr\bin') + ';' + $env:PATH
    }

    if (-not (Get-Command sh -ErrorAction SilentlyContinue)) {
        Write-Host "ERROR: mingw32-make was found but the MSYS2 Unix tools were not." -ForegroundColor Red
        Write-Host ""
        Write-Host "The Makefile's recipes use sh, rm, mkdir and cp. Without them on PATH"
        Write-Host "the build fails on the first recipe with 'The syntax of the command is"
        Write-Host "incorrect'. Expected them in $msys2Home\usr\bin."
        Write-Host ""
        exit 1
    }

    & mingw32-make @args
    exit $LASTEXITCODE
}
finally {
    $env:PATH = $savedPath
}
