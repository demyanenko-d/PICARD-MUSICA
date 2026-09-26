@echo off
setlocal
REM ===========================================================================
REM  Release build: everything shipped to the user ends up in release\
REM
REM  ASCII ONLY. cmd reads this file as CP866; UTF-8 Cyrillic here turns
REM  into garbage that breaks command parsing. Same rule as the other
REM  .bat files in this tree.
REM
REM  Eight artefacts:
REM    1. UNIMOD.$C                  TR-DOS application (Hobeta, for a real machine)
REM    2. UNIMOD.sna                 the same app as a 128K snapshot, for emulators
REM    3. UMPLAYER.WMF               Wild Commander plugin for TS-Config
REM    4. soundsinth_zcontroller.uf2 firmware WITH the Z-Controller emulator
REM    5. soundsinth_plain.uf2       firmware WITHOUT it
REM    6. GeneralUser-GS.LICENSE.txt licence of the SoundFont baked into both
REM    7. SGM.ssb                    bigger bank FOR THE SD CARD (30 MB, does not fit flash)
REM    8. Timbres-of-Heaven.ssb      the widest one, also for the card (50 MB)
REM
REM  Both firmware images carry the baked instrument bank inside them, so a
REM  release is one file to flash. Debug builds do not -- see step [6/8].
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
echo === [6/8] embedding the instrument bank ===
REM  RELEASE images carry the bank inside the .uf2: whoever installs a
REM  release flashes one file and is done. Debug flashing (flash_zc.cmd /
REM  flash_plain.cmd, over SWD from the ELF) stays bank-free and fast --
REM  the bank sits above 1 MB and nothing in the ELF reaches there, so an
REM  ordinary SWD flash leaves an already-written bank alone.
REM
REM  No bank baked yet: the release still builds, just without it, and the
REM  board says so on boot. Bake it: node scripts\tools\bake_banks.js
if not exist "%BANKS%\GeneralUser-GS.ssb" (
    echo    no banks\GeneralUser-GS.ssb -- release ships WITHOUT the bank, .mid will not play
) else (
    node "%~dp0tools\uf2_embed_bank.js" "%PICARD%\soundsinth_zcontroller.uf2" "%BANKS%\GeneralUser-GS.ssb" "%PICARD%\soundsinth_zcontroller.uf2" || goto :fail
    node "%~dp0tools\uf2_embed_bank.js" "%PICARD%\soundsinth_plain.uf2" "%BANKS%\GeneralUser-GS.ssb" "%PICARD%\soundsinth_plain.uf2" || goto :fail
)

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
