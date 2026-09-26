@echo off
REM ==========================================================================
REM  flash_swd.cmd <path-to-elf>   --  flash over SWD through a debug probe
REM
REM  ASCII ONLY. cmd reads this file as CP866; UTF-8 Cyrillic here turns
REM  into garbage that breaks command parsing. Same rule as build.bat.
REM
REM  This is the fast path and the default one: no BOOTSEL button, no USB
REM  re-plug, no waiting. The probe is a Raspberry Pi Debug Probe (or any
REM  picoprobe build new enough to speak CMSIS-DAP v2, VID 2E8A PID 000C).
REM
REM  Feeds OpenOCD the ELF, not the UF2 -- "program" wants a real image with
REM  section addresses, and the ELF is what the build already produces next
REM  to the UF2.
REM
REM  5000 kHz is measured working on this board. Drop it if the probe wires
REM  are long or the link turns flaky.
REM
REM  See flash_uf2.cmd for the BOOTSEL fallback and for why exit codes
REM  are compared against "0" rather than tested with "if errorlevel".
REM ==========================================================================
setlocal

set "ELF=%~1"
if "%ELF%"=="" ( echo *** flash_swd: no ELF given *** & exit /b 1 )
if not exist "%ELF%" ( echo *** flash_swd: not found: %ELF% *** & exit /b 1 )

REM OpenOCD ships with the SDK install; take the newest version present.
set "OOCD="
set "OD=%USERPROFILE%\.pico-sdk\openocd"
for /f "delims=" %%D in ('dir /b /o:n "%OD%" 2^>nul') do (
    if exist "%OD%\%%D\openocd.exe" set "OOCD=%OD%\%%D"
)
if not defined OOCD ( echo *** flash_swd: openocd.exe not found under %OD% *** & exit /b 1 )

echo Flashing over SWD: %ELF%
"%OOCD%\openocd.exe" -s "%OOCD%\scripts" ^
    -f interface/cmsis-dap.cfg ^
    -f target/rp2350.cfg ^
    -c "adapter speed 5000" ^
    -c "program \"%ELF:\=/%\" verify" ^
    -c "reset run" ^
    -c "shutdown"
if "%ERRORLEVEL%"=="0" goto :done

echo.
echo     Load failed. Our firmware reprograms QMI for PSRAM, and OpenOCD
echo     then cannot restore XIP. Doing a rescue reset and retrying once.
echo.
"%OOCD%\openocd.exe" -s "%OOCD%\scripts" ^
    -f interface/cmsis-dap.cfg ^
    -f target/rp2350-rescue.cfg ^
    -c "init" ^
    -c "shutdown"

"%OOCD%\openocd.exe" -s "%OOCD%\scripts" ^
    -f interface/cmsis-dap.cfg ^
    -f target/rp2350.cfg ^
    -c "adapter speed 5000" ^
    -c "program \"%ELF:\=/%\" verify" ^
    -c "reset run" ^
    -c "shutdown"

if not "%ERRORLEVEL%"=="0" (
    echo.
    echo *** flash_swd: FAILED ***
    echo     Is the probe plugged in and wired to SWDIO/SWCLK/GND?
    echo     No probe at hand: pass "bootsel" to flash_zc.cmd / flash_plain.cmd
    echo     to go through the BOOTSEL button instead.
    exit /b 1
)

:done
echo.
echo === FLASHED over SWD: %ELF% ===
exit /b 0
