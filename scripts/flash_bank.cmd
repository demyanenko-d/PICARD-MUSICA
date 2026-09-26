@echo off
REM ==========================================================================
REM  flash_bank.cmd [path-to-ssb]  --  flash the SF2 instrument bank over SWD
REM
REM  ASCII ONLY. cmd reads this file as CP866; UTF-8 Cyrillic here turns into
REM  garbage that breaks command parsing. Same rule as build.bat.
REM
REM  The bank is a SEPARATE flash region, not part of the firmware image.
REM  Reason: it is ~14 MB against the firmware's ~450 KB, and it changes once
REM  in a hundred builds. Baking it into the UF2 would make every ordinary
REM  SWD flash take a minute for nothing.
REM
REM  Offset must match SOUNDSINTH_BANK_FLASH_OFFSET in
REM  backend/ports/rp2350/firmware_config.h -- 1 MB. Firmware lives below
REM  it, the bank above, and nothing else is in flash.
REM
REM  Run this ONCE after baking a new bank. Ordinary firmware flashing
REM  (flash_zc.cmd / flash_plain.cmd) leaves the bank untouched: it writes
REM  only the sections the ELF declares, all of which sit below 1 MB.
REM
REM  Takes minutes: 14 MB over SWD is not fast. That is the whole point of
REM  keeping it out of the normal flash cycle.
REM ==========================================================================
setlocal

set "SSB=%~1"
if "%SSB%"=="" set "SSB=%~dp0..\release\banks\GeneralUser-GS.ssb"
if not exist "%SSB%" (
    echo *** flash_bank: not found: %SSB%
    echo     Bake one first:
    echo       node scripts\tools\bake_banks.js gu
    exit /b 1
)

REM 0x10100000 = XIP base 0x10000000 + SOUNDSINTH_BANK_FLASH_OFFSET 0x100000.
set "ADDR=0x10100000"

set "OOCD="
set "OD=%USERPROFILE%\.pico-sdk\openocd"
for /f "delims=" %%D in ('dir /b /o:n "%OD%" 2^>nul') do (
    if exist "%OD%\%%D\openocd.exe" set "OOCD=%OD%\%%D"
)
if not defined OOCD ( echo *** flash_bank: openocd.exe not found under %OD% & exit /b 1 )

for %%F in ("%SSB%") do echo Flashing bank %%~nxF (%%~zF bytes) at %ADDR% -- this takes minutes.

"%OOCD%\openocd.exe" -s "%OOCD%\scripts" ^
    -f interface/cmsis-dap.cfg ^
    -f target/rp2350.cfg ^
    -c "adapter speed 5000" ^
    -c "init" ^
    -c "reset init" ^
    -c "flash write_image erase \"%SSB:\=/%\" %ADDR% bin" ^
    -c "reset run" ^
    -c "shutdown"

if not "%ERRORLEVEL%"=="0" (
    echo.
    echo     Write failed. Our firmware reprograms QMI for PSRAM and OpenOCD
    echo     then cannot restore XIP -- same failure mode as flash_swd.cmd.
    echo     Doing a rescue reset and retrying once.
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
        -c "init" ^
        -c "reset init" ^
        -c "flash write_image erase \"%SSB:\=/%\" %ADDR% bin" ^
        -c "reset run" ^
        -c "shutdown"

    if not "%ERRORLEVEL%"=="0" (
        echo.
        echo *** flash_bank: FAILED ***
        exit /b 1
    )
)

echo.
echo === BANK FLASHED at %ADDR% ===
echo     The board prints "boot: bank ..." on the next start if it reads back.
exit /b 0
