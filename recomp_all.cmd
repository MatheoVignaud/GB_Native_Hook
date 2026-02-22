@echo off
setlocal EnableDelayedExpansion

if "%~1"=="" (
    echo Usage: recomp_all.cmd ^<rom.gb^> [recomp_build options...]
    echo Example: recomp_all.cmd Pokemons.gb --mode debug --mingw C:/MinGW
    exit /b 1
)

set "ROM=%~1"
shift

set "SCRIPT_DIR=%~dp0"
if not exist "%SCRIPT_DIR%tools\recomp_build.py" (
    set "SCRIPT_DIR=%CD%\"
)

set "EXTRA_ARGS="
:collect_args
if "%~1"=="" goto run
set "EXTRA_ARGS=!EXTRA_ARGS! "%~1""
shift
goto collect_args

:run
python "%SCRIPT_DIR%tools\recomp_build.py" --rom "%ROM%" !EXTRA_ARGS!
exit /b %ERRORLEVEL%
