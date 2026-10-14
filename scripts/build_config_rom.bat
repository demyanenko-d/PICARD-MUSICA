@echo off
REM Build the configuration ROM: an 8 KB image the board substitutes at
REM 0x0000 through the DivMMC mechanism.
REM
REM ASCII ONLY. cmd reads this file as CP866; UTF-8 Cyrillic here turns
REM into garbage that breaks command parsing.
REM
REM Memory map (see frontend/config_rom/crt0.s):
REM   0x0000 reset            0x0038 IM 1        0x0066 NMI
REM   0x0068 code             0x1800 font, 2 KB
REM   0x2000 settings page    0x4000 screen
REM   0x6000 data, upward     0x7FFF stack, downward
REM
REM Output is a C header with the image, compiled into the firmware.

setlocal
cd /d "%~dp0..\frontend\config_rom"

if not exist obj mkdir obj

set FW=..\z80_fw
set BUS=..\common
set CFLAGS=-mz80 --sdcccall 1 --no-std-crt0 --opt-code-size -I . -I %BUS% --disable-warning 85
set ASFLAGS=-plosgff

echo [1/5] Assembling the framework...
for %%F in (scr kbd font) do (
    sdasz80 %ASFLAGS% obj\%%F.rel %FW%\%%F.s >nul
    if errorlevel 1 ( echo FAIL %%F.s && exit /b 1 )
)

echo [2/5] Assembling startup and glue...
for %%F in (crt0 fw_glue) do (
    sdasz80 %ASFLAGS% obj\%%F.rel %%F.s >nul
    if errorlevel 1 ( echo FAIL %%F.s && exit /b 1 )
)

echo [3/5] Compiling C...
for %%F in (main cfgbus) do (
    sdcc %CFLAGS% -c %%F.c -o obj\%%F.rel
    if errorlevel 1 ( echo FAIL %%F.c && exit /b 1 )
)

echo [4/5] Linking...
REM Vectors live inside _CODE with assembler-computed padding. Pinned areas
REM are laid out in the linker's own order and _GSINIT lands on top of the
REM code. The font sits in the last two kilobytes of the image.
sdcc -mz80 --no-std-crt0 --out-fmt-ihx --code-loc 0x0000 --data-loc 0x6000 ^
    -Wl-b_FONT=0x1800 ^
    obj\crt0.rel ^
    obj\main.rel ^
    obj\cfgbus.rel ^
    obj\fw_glue.rel ^
    obj\scr.rel obj\kbd.rel obj\font.rel ^
    -o obj\config_rom.ihx
if errorlevel 1 ( echo FAIL link && exit /b 1 )

echo [5/5] Packing the image...
makebin -s 65536 obj\config_rom.ihx obj\config_rom.full
if errorlevel 1 ( echo FAIL makebin && exit /b 1 )
REM Without the map on purpose: it would also count _DATA at 0x6000, which
REM lives in the machine RAM and has no place in the image.
node "%~dp0tools\cutbin.js" obj\config_rom.full obj\config_rom.bin 0x0000
if errorlevel 1 ( echo FAIL cut && exit /b 1 )
REM The vector layout rests on counted padding, so it is verified here:
REM 0x0000 DI, 0x0038 PUSH AF, 0x0066 RETN. Miscounting is otherwise only
REM visible on the machine.
node "%~dp0tools\checkbytes.js" obj\config_rom.bin 0=f3 38=f5 66=ed,45
if errorlevel 1 ( echo FAIL vectors && exit /b 1 )
node "%~dp0tools\mkromimage.js" obj\config_rom.bin ^
    ..\..\backend\ports\rp2350\hostlink\rom_images\rom_image_config.h ^
    ConfigRom 8192 "Source: frontend/config_rom, built by scripts/build_config_rom.bat." ^
    obj\config_rom.map
if errorlevel 1 ( echo FAIL image && exit /b 1 )

echo === BUILD OK ===
endlocal
exit /b 0
