@echo off
setlocal
REM ===========================================================================
REM  Release build: everything shipped to the user ends up in release\
REM
REM  ASCII ONLY. cmd reads this file as CP866; UTF-8 Cyrillic here turns
REM  into garbage that breaks command parsing. Same rule as the other
REM  .bat files in this tree.
REM
REM  Nine artefacts:
REM    1. UNIMOD.$C                  TR-DOS application (Hobeta, for a real machine)
REM    2. UNIMOD.sna                 the same app as a 128K snapshot, for emulators
REM    3. UMPLAYER.WMF               Wild Commander plugin for TS-Config
REM    4. soundsinth_zcontroller.uf2 firmware WITH the Z-Controller emulator
REM    5. soundsinth_plain.uf2       firmware WITHOUT it
REM    6. bank.uf2                   instrument bank for flash, on its own
REM    7. GeneralUser-GS.LICENSE.txt licence of the SoundFont in that bank
REM    8. SGM.ssb                    bigger bank FOR THE SD CARD (30 MB, does not fit flash)
REM    9. Timbres-of-Heaven.ssb      the widest one, also for the card (50 MB)
REM
REM  Firmware and bank are separate images: half a megabyte against sixteen.
REM  A firmware update writes only its own blocks and leaves the bank in
REM  place; the bank goes in once. See step [6/8].
REM
REM  The snapshot needs neither a disk image nor a loader: the app comes up
REM  at once. With an emulator that does Z-Controller and a card image this
REM  covers the file list, sorting and navigation -- everything that does
REM  not need the board itself.
REM
REM  Both firmwares come from ONE source tree, built in build\zc and build\plain.
REM  SOUNDSINTH_ZCONTROLLER reaches the compiler as -D and overrides the
REM  default of SOUNDSINTH_RP2350_ZCONTROLLER (it sits under #ifndef).
REM  Separate build dirs (not a reconfigure in place) keep
REM  both variants incrementally rebuildable without trashing each other's
REM  cache. Every build directory in this repo lives under build\.
REM ===========================================================================

cd /d "%~dp0.."
set RELEASE=%~dp0..\release
set BANKS=%RELEASE%\banks
set PICARD=%RELEASE%\picard

echo.
echo === [1/8] release directory ===
if not exist "%RELEASE%" mkdir "%RELEASE%"
if not exist "%BANKS%" mkdir "%BANKS%"
if not exist "%PICARD%" mkdir "%PICARD%"
if exist "%PICARD%\*.uf2" del /q "%PICARD%\*.uf2"

echo.
echo === [2/8] TR-DOS application ===
call "%~dp0build_trdos.bat" || goto :fail

echo.
echo === [3/8] Wild Commander plugin ===
call "%~dp0build_plugin.bat" || goto :fail

echo.
echo === [4/8] firmware WITH Z-Controller ===
cmake -S . -B build\zc -G Ninja -DSOUNDSINTH_ZCONTROLLER=1 >nul || goto :fail
cmake --build build\zc -j 8 || goto :fail
copy /y "build\zc\soundsinth_firmware.uf2" "%PICARD%\soundsinth_zcontroller.uf2" >nul || goto :fail

echo.
echo === [5/8] firmware WITHOUT Z-Controller ===
cmake -S . -B build\plain -G Ninja -DSOUNDSINTH_ZCONTROLLER=0 >nul || goto :fail
cmake --build build\plain -j 8 || goto :fail
copy /y "build\plain\soundsinth_firmware.uf2" "%PICARD%\soundsinth_plain.uf2" >nul || goto :fail
echo.
echo === [6/8] bank image ===
REM  The bank ships as its OWN .uf2, apart from the firmware.
REM
REM  Firmware changes often, the bank almost never, and the bank is 16 MB
REM  against half a megabyte of firmware. Kept apart, an ordinary update
REM  writes only the firmware blocks; the bank lives above 1 MB and a
REM  firmware .uf2 never reaches there, so it survives untouched. Same
REM  reason debug flashing over SWD (flash_zc.cmd / flash_plain.cmd) has
REM  always left the bank alone.
REM
REM  No bank baked yet: the release still builds, just without the bank
REM  image, and the board says so on boot. Bake it:
REM  node scripts\tools\bake_banks.js
REM
REM  The image is made by picotool, the SDK's own tool, and not by hand.
REM  The bootrom takes only blocks whose payload is exactly 256 bytes and
REM  drops a short one without a word: the tail never reaches flash, the
REM  received count falls one short of the declared one, and the board does
REM  not reboot after the copy -- the drive just stays mounted. picotool
REM  pads the last block; uf2_check.js makes sure of it afterwards.
REM
REM  The absolute block (errata RP2350-E10) picotool adds when combining
REM  two images, but on a plain convert it prints the message and writes
REM  nothing. It is copied over from the firmware image, which carries one.
if not exist "%BANKS%\GeneralUser-GS.ssb" (
    echo    no banks\GeneralUser-GS.ssb -- release ships WITHOUT the bank image, .mid will not play
    goto :banks_done
)

set "PICOTOOL="
set "PD=%USERPROFILE%\.pico-sdk\picotool"
for /f "delims=" %%D in ('dir /b /o:n "%PD%" 2^>nul') do (
    if exist "%PD%\%%D\picotool\picotool.exe" set "PICOTOOL=%PD%\%%D\picotool\picotool.exe"
)
if not defined PICOTOOL ( echo *** picotool.exe not found under %PD% & goto :fail )

REM  0x10100000 = XIP base + SOUNDSINTH_BANK_FLASH_OFFSET, same as flash_bank.cmd.
"%PICOTOOL%" uf2 convert "%BANKS%\GeneralUser-GS.ssb" -t bin "%PICARD%\bank.uf2" -o 0x10100000 --family rp2350-arm-s || goto :fail
node "%~dp0tools\uf2_abs_block.js" "%PICARD%\bank.uf2" "%~dp0..\build\zc\soundsinth_firmware.uf2" || goto :fail
node "%~dp0tools\uf2_check.js" "%PICARD%\bank.uf2" || goto :fail
:banks_done

node "%~dp0tools\uf2_check.js" "%PICARD%\soundsinth_zcontroller.uf2" || goto :fail
node "%~dp0tools\uf2_check.js" "%PICARD%\soundsinth_plain.uf2" || goto :fail

REM  [7/8] Licences for the banks that are present.
REM
REM  The banks themselves are baked straight into release\banks\ by
REM  scripts\tools\bake_banks.js; this step only puts the licences beside them.
REM  SGM and Timbres do NOT go into flash and cannot: 30 and 50 MB against
REM  16 MB of flash total. The board reads them from the SD card
REM  (SOUNDSINTH_BANK_SD_PATH); the flashed GeneralUser is the fallback.
echo.
echo === [7/8] bank licences ===
for %%B in (GeneralUser-GS SGM Timbres-of-Heaven) do (
    if exist "%BANKS%\%%B.ssb" (
        copy /y "%~dp0..\SF2\%%B.LICENSE.txt" "%BANKS%\%%B.LICENSE.txt" >nul || goto :fail
    ) else (
        echo    no banks\%%B.ssb -- bake it: node scripts\tools\bake_banks.js
    )
)

REM  [8/8] SD card image from SD\ -- it already has the fresh plugin in WC\.
echo.
echo === [8/8] SD card image ===
call "%~dp0build_sd.bat" || goto :fail
echo.
echo === DONE: release\ ===
for %%F in ("%RELEASE%\*") do echo    %%~nxF  %%~zF bytes
for %%F in ("%PICARD%\*") do echo    picard\%%~nxF  %%~zF bytes
for %%F in ("%BANKS%\*") do echo    banks\%%~nxF  %%~zF bytes
endlocal
exit /b 0

:fail
echo.
echo *** BUILD FAILED, code %errorlevel% ***
endlocal
exit /b 1
