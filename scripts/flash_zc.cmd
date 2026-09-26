@echo off
REM ==========================================================================
REM  Build and flash the firmware WITH the Z-Controller emulator.
REM
REM    flash_zc.cmd            flash over SWD through the debug probe (default)
REM    flash_zc.cmd bootsel    flash through the BOOTSEL button instead
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
echo === building firmware WITH Z-Controller ===
cmake -S . -B build\zc -G Ninja -DSOUNDSINTH_ZCONTROLLER=1 >nul || goto :fail
cmake --build build\zc -j 8 || goto :fail

if /i "%~1"=="bootsel" (
    call "%~dp0tools\flash_uf2.cmd" "%~dp0..\build\zc\soundsinth_firmware.uf2" || goto :fail
) else (
    call "%~dp0tools\flash_swd.cmd" "%~dp0..\build\zc\soundsinth_firmware.elf" || goto :fail
)
endlocal
exit /b 0

:fail
echo.
echo *** FAILED, code %errorlevel% ***
endlocal
exit /b 1
