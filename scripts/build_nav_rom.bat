@echo off
REM Build the BDI image navigator ROM: 8 KB the board puts at 0x0000 from
REM the NMI fetch at 0x0066 until the RETN at 0x1FFE.
REM
REM ASCII ONLY. cmd reads this file as CP866; UTF-8 Cyrillic here turns
REM into garbage that breaks command parsing.
REM
REM Memory map (see frontend/nav_rom/crt0.s):
REM   0x0000 code, 0x0066 entry   0x1700 font, 2 KB   0x1FFE RETN
REM   0x2000 exchange page        0x2800 data         0x3C00 guard, stack
REM
REM The page layout comes from backend/player/nav/nav_page.h.
REM Output is a C header with the image, compiled into the firmware.

setlocal
cd /d "%~dp0..\frontend\nav_rom"

if not exist obj mkdir obj

set FW=..\z80_fw
set CFLAGS=-mz80 --sdcccall 1 --no-std-crt0 --opt-code-size --disable-warning 85
set ASFLAGS=-plosgff
set PAGE_H=..\..\backend\player\nav\nav_page.h

echo [1/5] Page layout from the board header...
node "%~dp0tools\mkcdefs.js" %PAGE_H% obj\nav_page.h NP
if errorlevel 1 ( echo FAIL nav_page.h && exit /b 1 )
node "%~dp0tools\mkcdefs.js" %PAGE_H% obj\nav_page.inc NP
if errorlevel 1 ( echo FAIL nav_page.inc && exit /b 1 )

echo [2/5] Assembling...
for %%F in (scr kbd font) do (
    sdasz80 %ASFLAGS% obj\%%F.rel %FW%\%%F.s >nul
    if errorlevel 1 ( echo FAIL %%F.s && exit /b 1 )
)
for %%F in (crt0 nav_io) do (
    sdasz80 %ASFLAGS% obj\%%F.rel %%F.s
    if errorlevel 1 ( echo FAIL %%F.s && exit /b 1 )
)

echo [3/5] Compiling C...
sdcc %CFLAGS% -c main.c -o obj\main.rel
if errorlevel 1 ( echo FAIL main.c && exit /b 1 )

echo [4/5] Linking...
REM crt0 goes first: its _CODE holds the entry at 0x0066.
sdcc -mz80 --no-std-crt0 --out-fmt-ihx --code-loc 0x0000 --data-loc 0x2800 ^
    -Wl-b_FONT=0x1700 -Wl-b_EXIT=0x1FFE ^
    obj\crt0.rel obj\main.rel obj\nav_io.rel ^
    obj\scr.rel obj\kbd.rel obj\font.rel ^
    -o obj\nav_rom.ihx
if errorlevel 1 ( echo FAIL link && exit /b 1 )

echo [5/5] Packing the image...
makebin -s 65536 obj\nav_rom.ihx obj\nav_rom.full
if errorlevel 1 ( echo FAIL makebin && exit /b 1 )
node "%~dp0tools\cutbin.js" obj\nav_rom.full obj\nav_rom.bin 0x0000
if errorlevel 1 ( echo FAIL cut && exit /b 1 )
REM Entry LD (nn),SP at 0x0066 and the exit RETN at 0x1FFE: the board
REM swaps the ROM in and out on exactly these fetches.
node "%~dp0tools\checkbytes.js" obj\nav_rom.bin 66=ed,73 1ffe=ed,45
if errorlevel 1 ( echo FAIL vectors && exit /b 1 )
REM Data must stay below the guard at 0x3C00.
node "%~dp0tools\mkromimage.js" obj\nav_rom.bin ^
    ..\..\backend\ports\rp2350\hostlink\rom_images\rom_image_nav.h ^
    NavRom 8192 "Source: frontend/nav_rom, built by scripts/build_nav_rom.bat." ^
    obj\nav_rom.map 3C00
if errorlevel 1 ( echo FAIL image && exit /b 1 )

echo === BUILD OK ===
endlocal
exit /b 0
