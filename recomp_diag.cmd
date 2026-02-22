@echo off
setlocal EnableDelayedExpansion

if "%~1"=="" (
    echo Usage: recomp_diag.cmd ^<path_to_recompiled_exe^> [diag options...]
    echo Example: recomp_diag.cmd PokemonSilver\pokemonsilver_recompiled.exe --max-instr 5000000
    exit /b 1
)

set "EXE=%~1"
shift

set "SCRIPT_DIR=%~dp0"
if not exist "%SCRIPT_DIR%tools\recomp_diag.py" (
    set "SCRIPT_DIR=%CD%\"
)

set "EXTRA_ARGS="
:collect_args
if "%~1"=="" goto run
set "EXTRA_ARGS=!EXTRA_ARGS! "%~1""
shift
goto collect_args

:run
python "%SCRIPT_DIR%tools\recomp_diag.py" --exe "%EXE%" !EXTRA_ARGS!
exit /b %ERRORLEVEL%
