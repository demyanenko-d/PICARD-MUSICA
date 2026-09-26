@echo off
REM Build z80_fw and its tests into flat binaries for the emulator.
REM
REM ASCII ONLY. cmd reads this file as CP866; UTF-8 Cyrillic here turns
REM into garbage that breaks command parsing.
REM
REM Layout: library sources here, tests in tests\, objects in obj\.
REM
REM Everything loads at 0x8000 and runs from there. Flat .bin rather than
REM .ihx: the emulator just maps it at the load address. 16K is cut out,
REM because the result buffers live above 0x9FFF.
REM
REM fat.rel needs page.rel (paging lives there), so every link that pulls
REM in fat.rel must pull in page.rel too.

setlocal
cd /d "%~dp0..\frontend\z80_fw"

if not exist obj mkdir obj

echo [1/4] Assembling library...
for %%F in (sd fat page file dir scr kbd irq font) do (
    sdasz80 -plosgff obj\%%F.rel %%F.s
    if errorlevel 1 ( echo FAIL %%F.s && exit /b 1 )
)

echo [2/4] Assembling tests...
for %%F in (test_sd test_fat test_dir test_dirtab test_ext test_file test_fwd test_load test_pre test_speed test_ui) do (
    sdasz80 -plosgff obj\%%F.rel tests\%%F.s
    if errorlevel 1 ( echo FAIL tests\%%F.s && exit /b 1 )
)

REM Without --data-loc above _CODE the linker
REM silently laid _DATA on top of _CODE: nothing complained, the program just
REM wandered off. Omitting it is worse still - sdld then puts _DATA at 0x8000,
REM right on the code. So it stays explicit, with room to grow, and step 4
REM checks the result every build.
REM
REM   0x8000 code, 5.6K of room   0x9600 data   0x9F00 stack   0xA000 results
set LINK=sdcc -mz80 --no-std-crt0 --out-fmt-ihx --code-loc 0x8000 --data-loc 0x9600
set LIB=obj\file.rel obj\dir.rel obj\fat.rel obj\page.rel obj\sd.rel
set UILIB=obj\scr.rel obj\kbd.rel obj\irq.rel obj\font.rel
REM Font is 2K and will not fit under the data area, so it gets its own
REM segment high in page 2, right below the paging window.
set FONTLOC=-Wl-b_FONT=0xB800

echo [3/4] Linking...

REM test_sd.s hardcodes its sector buffer at 0x9000, so its data must NOT
REM go there: sd.s data would land right on top of the buffer and the first
REM read would wipe the card type.
sdcc -mz80 --no-std-crt0 --out-fmt-ihx --code-loc 0x8000 --data-loc 0x8600 ^
    obj\test_sd.rel obj\sd.rel -o obj\test_sd.ihx
if errorlevel 1 ( echo FAIL link test_sd && exit /b 1 )

%LINK% obj\test_fwd.rel obj\sd.rel -o obj\test_fwd.ihx
if errorlevel 1 ( echo FAIL link test_fwd && exit /b 1 )

REM The rest just take the whole library: the linker drops what is unused.
for %%P in (test_fat test_dir test_dirtab test_ext test_file test_load test_pre test_speed) do (
    %LINK% obj\%%P.rel %LIB% -o obj\%%P.ihx
    if errorlevel 1 ( echo FAIL link %%P && exit /b 1 )
)

%LINK% %FONTLOC% obj\test_ui.rel %UILIB% -o obj\test_ui.ihx
if errorlevel 1 ( echo FAIL link test_ui && exit /b 1 )

echo [4/4] Flattening and checking the memory map...
for %%P in (test_sd test_fat test_dir test_dirtab test_ext test_file test_fwd test_load test_pre test_speed test_ui) do (
    call :flatten %%P
    if errorlevel 1 exit /b 1
    call :checkmap %%P
    if errorlevel 1 exit /b 1
)

echo === BUILD OK ===
endlocal
exit /b 0

REM makebin emits the whole 64K space; cut out the chunk from 0x8000 and
REM throw the 64K intermediate away - eight of those are half a megabyte
REM of nothing.
:flatten
makebin -s 65536 obj\%1.ihx obj\%1.full
if errorlevel 1 ( echo FAIL makebin %1 && exit /b 1 )
powershell -NoProfile -Command ^
  "$b=[IO.File]::ReadAllBytes('obj\%1.full');" ^
  "$out=New-Object byte[] 0x4000;" ^
  "[Array]::Copy($b,0x8000,$out,0,0x4000);" ^
  "[IO.File]::WriteAllBytes('obj\%1.bin',$out);" ^
  "Write-Host ('  obj\%1.bin - ' + $out.Length + ' bytes at 0x8000')"
if errorlevel 1 ( echo FAIL cut %1 && exit /b 1 )
del obj\%1.full
exit /b 0

REM Areas placed by hand overlap silently: sdld says nothing and the program
REM just wanders off, so it is checked.
REM Everything must fit below the stack at 0x9F00; result buffers start at
REM 0xA000.
:checkmap
powershell -NoProfile -Command ^
  "$m = Get-Content 'obj\%1.map';" ^
  "$c = $m | Select-String '^_CODE\s';" ^
  "$d = $m | Select-String '^_DATA\s';" ^
  "if (-not $c -or -not $d) { Write-Host '  %1: no areas in map'; exit 1 }" ^
  "$cf = [regex]::Match($c[0].Line,'([0-9A-F]{8})\s+([0-9A-F]{8})');" ^
  "$df = [regex]::Match($d[0].Line,'([0-9A-F]{8})\s+([0-9A-F]{8})');" ^
  "$ce = [Convert]::ToInt32($cf.Groups[1].Value,16) + [Convert]::ToInt32($cf.Groups[2].Value,16);" ^
  "$ds = [Convert]::ToInt32($df.Groups[1].Value,16);" ^
  "$de = $ds + [Convert]::ToInt32($df.Groups[2].Value,16);" ^
  "if ($ce -gt $ds) { Write-Host ('  %1: CODE ends 0x{0:X4} but DATA starts 0x{1:X4} - OVERLAP' -f $ce,$ds); exit 1 }" ^
  "if ($de -gt 0x9F00) { Write-Host ('  %1: DATA ends 0x{0:X4}, past the stack at 0x9F00' -f $de); exit 1 }"
if errorlevel 1 ( echo FAIL memory map %1 && exit /b 1 )
exit /b 0
