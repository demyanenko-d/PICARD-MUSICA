@echo off
REM Build the TR-DOS application: navigator + player.
REM
REM ASCII ONLY. cmd reads this file as CP866; UTF-8 Cyrillic here turns
REM into garbage that breaks command parsing.
REM
REM Everything loads at 0x6000, where TR-DOS puts it. Output is a Hobeta
REM file ready to be written to a TRD image.
REM
REM Memory map:
REM   0x6000 code, data, font     0xB600 IM2 table
REM   0xBF00 stack                0xC000 paging window

setlocal
cd /d "%~dp0..\frontend\trdos"

if not exist obj mkdir obj

set FW=..\z80_fw
set BUS=..\common
set CFLAGS=-mz80 --sdcccall 1 --no-std-crt0 --opt-code-size -I . -I %BUS% --disable-warning 85
REM Board trace over UART is off: the screen is right there.
set CFLAGS=%CFLAGS% -DBUS_TRACE=0
REM Level, spectrum and track metadata for the player screen.
set CFLAGS=%CFLAGS% -DBUS_SCOPE=1
REM Board port numbers live in common/bus_client.h, not here.
set ASFLAGS=-plosgff

echo [1/5] Assembling the framework...
REM No scr: the 8x8 text output of the framework wants 8 bytes per glyph;
REM here the font is stored by rows (ui6.s) and ui.c draws all text.
for %%F in (sd fat page file dir kbd irq) do (
    sdasz80 %ASFLAGS% obj\%%F.rel %FW%\%%F.s >nul
    if errorlevel 1 ( echo FAIL %%F.s && exit /b 1 )
)

echo [2/5] Assembling startup, glue and screen resources...
REM ui_res.s: deck prerender, 6x8 font (it replaces the 8x8 font of the
REM framework as _fw_font), 7-segment and 4x8 digits. Made from res\.
node "%~dp0tools\mkui.js" res obj\ui_res.s
if errorlevel 1 ( echo FAIL mkui && exit /b 1 )
sdasz80 %ASFLAGS% obj\ui_res.rel obj\ui_res.s >nul
if errorlevel 1 ( echo FAIL ui_res.s && exit /b 1 )
for %%F in (crt0 fw_glue ui6 dots dirsort) do (
    sdasz80 %ASFLAGS% obj\%%F.rel %%F.s >nul
    if errorlevel 1 ( echo FAIL %%F.s && exit /b 1 )
)

echo [3/5] Compiling C...
for %%F in (main ui) do (
    sdcc %CFLAGS% -c %%F.c -o obj\%%F.rel
    if errorlevel 1 ( echo FAIL %%F.c && exit /b 1 )
)
sdcc %CFLAGS% -c %BUS%\bus_client.c -o obj\bus_client.rel
if errorlevel 1 ( echo FAIL bus_client.c && exit /b 1 )

echo [4/5] Linking...
REM The font is stored by rows, a 256-byte page per glyph row, so it must
REM start on a page boundary: 7 pages at 0xAF00..0xB5FF, right below the
REM IM 2 table and still inside the loaded body. Code and data must end
REM below 0xAF00; cutbin.js fails the build if areas overlap.
sdcc -mz80 --no-std-crt0 --out-fmt-ihx --code-loc 0x6000 --data-loc 0 -Wl-b_FONT=0xAF00 ^
    obj\crt0.rel ^
    obj\main.rel ^
    obj\ui.rel ^
    obj\bus_client.rel ^
    obj\fw_glue.rel ^
    obj\file.rel obj\dir.rel obj\fat.rel obj\page.rel obj\sd.rel ^
    obj\kbd.rel obj\irq.rel obj\ui6.rel obj\dots.rel obj\dirsort.rel obj\ui_res.rel ^
    -o obj\player.ihx
if errorlevel 1 ( echo FAIL link && exit /b 1 )

echo [5/5] Packing into Hobeta...
makebin -s 65536 obj\player.ihx obj\player.full
if errorlevel 1 ( echo FAIL makebin && exit /b 1 )
node "%~dp0tools\cutbin.js" obj\player.full obj\player.bin 0x6000 obj\player.map
if errorlevel 1 ( echo FAIL cut && exit /b 1 )
if not exist ..\..\release mkdir ..\..\release\
node "%~dp0tools\mkhobeta.js" obj\player.bin ..\..\release\ugly.$c ugly 0x6000
if errorlevel 1 ( echo FAIL hobeta && exit /b 1 )

REM Snapshot for emulators: no disk image, no loader needed. Port 0x10
REM matches what main.c puts into fat_port_shadow at startup.
node "%~dp0tools\mksna.js" obj\player.bin ..\..\release\ugly.sna 0x6000 0x6000 0x10
if errorlevel 1 ( echo FAIL sna && exit /b 1 )

echo === BUILD OK ===
endlocal
exit /b 0
