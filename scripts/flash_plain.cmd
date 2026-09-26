@echo off
REM ==========================================================================
REM  Build and flash the firmware WITHOUT the Z-Controller emulator.
REM
REM    flash_plain.cmd            flash over SWD through the debug probe (default)
REM    flash_plain.cmd bootsel    flash through the BOOTSEL button instead
REM
REM  ASCII ONLY -- see tools\flash_swd.cmd.
REM
REM  SWD is the default because it needs nothing from the operator: no button,
REM  no re-plug, no waiting. The BOOTSEL path stays for when the probe is not
REM  at hand.
REM
REM  Uses the same build directory as build.bat, so the two variants stay
REM  incrementally rebuildable without trashing each other's CMake cache.
REM  SOUNDSINTH_ZCONTROLLER reaches the compiler as -D and overrides the
REM  default of SOUNDSINTH_RP2350_ZCONTROLLER (it sits under #ifndef).
REM ==========================================================================
setlocal
cd /d "%~dp0.."

echo.
echo === building firmware WITHOUT Z-Controller ===
cmake -S . -B build\plain -G Ninja -DSOUNDSINTH_ZCONTROLLER=0 >nul || goto :fail
cmake --build build\plain -j 8 || goto :fail

if /i "%~1"=="bootsel" (
    call "%~dp0tools\flash_uf2.cmd" "%~dp0..\build\plain\soundsinth_firmware.uf2" || goto :fail
) else (
    call "%~dp0tools\flash_swd.cmd" "%~dp0..\build\plain\soundsinth_firmware.elf" || goto :fail
)
endlocal
exit /b 0

:fail
echo.
echo *** FAILED, code %errorlevel% ***
endlocal
exit /b 1
