@echo off
chcp 65001 >nul
REM ===========================================================================
REM build_plugin.bat - сборка плагина Wild Commander (SDCC/Z80)
REM
REM frontend/plugin/src -> frontend/plugin/obj -> release/UMPLAYER.WMF
REM ===========================================================================

setlocal

cd /d "%~dp0..\frontend\plugin"

if not exist obj mkdir obj
REM   -mz80          архитектура
REM   --sdcccall 1   регистровое соглашение вызова, то же у src/lib/wc_api/*.s
REM   --no-std-crt0  свой crt0 в src/asm/crt0.s
REM   --opt-code-size размер важнее скорости: Z80 занят опросом платы
set CFLAGS=-mz80 --sdcccall 1 --no-std-crt0 --opt-code-size -I src -I ../common --disable-warning 85

set ASFLAGS=-plosff

echo [1/5] Сборка wc_api.lib...
call src\lib\wc_api\build_lib.bat >nul
if errorlevel 1 ( echo FAIL wc_api.lib && goto :err )

echo [2/5] Компиляция C файлов...
sdcc %CFLAGS% -c src\main.c       -o obj\main.rel       >nul
if errorlevel 1 ( echo FAIL main.c && goto :err )

sdcc %CFLAGS% -c ..\common\bus_client.c -o obj\bus_client.rel >nul
if errorlevel 1 ( echo FAIL bus_client.c && goto :err )

sdcc %CFLAGS% -c src\clk.c -o obj\clk.rel >nul
if errorlevel 1 ( echo FAIL clk.c && goto :err )


echo [3/5] Сборка ASM файлов...
sdasz80 %ASFLAGS% obj\crt0.rel   src\asm\crt0.s      >nul
if errorlevel 1 ( echo FAIL crt0.s && goto :err )

sdasz80 %ASFLAGS% obj\txtlib.rel src\asm\txtlib.s >nul
if errorlevel 1 ( echo FAIL txtlib.s && goto :err )

echo [4/5] Линковка...
REM Слот 0x8000-0xBFFF (16 КБ): код с 0x8000, данные сразу за ним. Выход за
REM 0xBFFF ловит проверка карты ниже.
sdcc -mz80 --no-std-crt0 --out-fmt-ihx --code-loc 0x8000 --data-loc 0 ^
    obj\crt0.rel ^
    obj\main.rel ^
    obj\bus_client.rel ^
    obj\clk.rel ^
    obj\wc_api.lib ^
    obj\txtlib.rel ^
    -o obj\plugin.ihx >nul
if errorlevel 1 ( echo FAIL link && goto :err )

powershell -NoProfile -Command ^
  "$m=Get-Content obj\plugin.map -Raw;" ^
  "function gs($p){if($m -match '([0-9a-f]{8})\s+'+$p){return $Matches[1]}return '00000000'};" ^
  "$secs=@('CODE','DATA','GSINIT','GSFINAL','INITIALIZED','HOME','INITIALIZER');" ^
  "Write-Host '';" ^
  "Write-Host '  Section        Start    End      Size';" ^
  "Write-Host '  -------------- -------- -------- ----------';" ^
  "$lastEnd=0;$codeEnd=0;$dataStart=0;" ^
  "foreach($s in $secs){" ^
  "  $st=[Convert]::ToInt32((gs ('s__'+$s)),16);" ^
  "  $ln=[Convert]::ToInt32((gs ('l__'+$s)),16);" ^
  "  if($ln -eq 0){continue};" ^
  "  $en=$st+$ln-1;" ^
  "  if($en -gt $lastEnd){$lastEnd=$en};" ^
  "  if($s -eq 'CODE'){$codeEnd=$en};" ^
  "  if($s -eq 'DATA'){$dataStart=$st};" ^
  "  Write-Host ('  _'+$s.PadRight(14)+' 0x'+$st.ToString('X4')+'   0x'+$en.ToString('X4')+'   '+$ln.ToString().PadLeft(5)+' bytes')" ^
  "};" ^
  "$free=0xBFFF-$lastEnd;" ^
  "Write-Host '  -------------- -------- -------- ----------';" ^
  "Write-Host ('  Last byte: 0x'+$lastEnd.ToString('X4')+'   Free before C000: '+$free+' bytes');" ^
  "if($codeEnd -ge $dataStart -and $dataStart -gt 0){Write-Host ('  *** ERROR: _CODE (end 0x'+$codeEnd.ToString('X4')+') overlaps _DATA (start 0x'+$dataStart.ToString('X4')+')! ***') -ForegroundColor Red; exit 1};" ^
  "if($lastEnd -ge 0xC000){Write-Host '  *** ERROR: sections overflow past 0xBFFF! ***' -ForegroundColor Red; exit 1};"
if errorlevel 1 goto :err

echo [5/5] Генерация WMF...
if not exist ..\..\release mkdir ..\..\release\
node "%~dp0tools\ihx2wmf.js" obj\plugin.ihx ..\..\release\UMPLAYER.WMF
if errorlevel 1 ( echo FAIL ihx2wmf && goto :err )
copy /y ..\..\release\UMPLAYER.WMF ..\..\SD\WC\UMPLAYER.WMF >nul
if errorlevel 1 ( echo FAIL copy to SD\WC && goto :err )

echo.
echo === BUILD OK ===
goto :end

:err
echo.
echo === BUILD FAILED ===
exit /b 1

:end
endlocal
