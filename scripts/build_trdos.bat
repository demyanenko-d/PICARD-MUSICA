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
REM Board port numbers live in common/bus_client.h, not here.
set ASFLAGS=-plosgff

echo [1/5] Assembling the framework...
for %%F in (sd fat page file dir scr kbd irq font) do (
    sdasz80 %ASFLAGS% obj\%%F.rel %FW%\%%F.s >nul
    if errorlevel 1 ( echo FAIL %%F.s && exit /b 1 )
)

echo [2/5] Assembling startup and glue...
for %%F in (crt0 fw_glue) do (
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
REM No -b_FONT here on purpose. In the z80_fw test builds the font sits high
REM in page 2 because a flat 16K image has no room for it. Here the program
REM starts at 0x6000 and there is plenty, so the font lives inside the body
REM and travels with the file. Pinned high, it would simply never be loaded.
sdcc -mz80 --no-std-crt0 --out-fmt-ihx --code-loc 0x6000 --data-loc 0 ^
    obj\crt0.rel ^
    obj\main.rel ^
    obj\ui.rel ^
    obj\bus_client.rel ^
    obj\fw_glue.rel ^
    obj\file.rel obj\dir.rel obj\fat.rel obj\page.rel obj\sd.rel ^
    obj\scr.rel obj\kbd.rel obj\irq.rel obj\font.rel ^
    -o obj\player.ihx
if errorlevel 1 ( echo FAIL link && exit /b 1 )

echo [5/5] Packing into Hobeta...
makebin -s 65536 obj\player.ihx obj\player.full
if errorlevel 1 ( echo FAIL makebin && exit /b 1 )
node "%~dp0tools\cutbin.js" obj\player.full obj\player.bin 0x6000 obj\player.map
if errorlevel 1 ( echo FAIL cut && exit /b 1 )
if not exist ..\..\release mkdir ..\..\release\
node "%~dp0tools\mkhobeta.js" obj\player.bin ..\..\release\UNIMOD.$C UNIMOD 0x6000
if errorlevel 1 ( echo FAIL hobeta && exit /b 1 )

REM Snapshot for emulators: no disk image, no loader needed. Port 0x10
REM matches what main.c puts into fat_port_shadow at startup.
node "%~dp0tools\mksna.js" obj\player.bin ..\..\release\UNIMOD.sna 0x6000 0x6000 0x10
if errorlevel 1 ( echo FAIL sna && exit /b 1 )

echo === BUILD OK ===
endlocal
exit /b 0
