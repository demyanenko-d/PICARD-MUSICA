@echo off
REM ===========================================================================
REM build_lib.bat - Сборка статической библиотеки wc_api.lib
REM
REM Каждая функция WC API - отдельный .rel модуль.
REM Линковщик SDCC подтягивает из .lib только модули с нужными символами.
REM ===========================================================================

setlocal

cd /d "%~dp0"

set ASFLAGS=-plosff
set BUILDDIR=..\..\..\obj\wc_api
set LIBFILE=..\..\..\obj\wc_api.lib

REM --- Папка для .rel файлов ---
if not exist "%BUILDDIR%" mkdir "%BUILDDIR%"

REM --- Сборка всех .s -> .rel ---
for %%f in (*.s) do (
    sdasz80 %ASFLAGS% "%BUILDDIR%\%%~nf.rel" "%%f" >nul
    if errorlevel 1 ( echo FAIL %%f && exit /b 1 )
)

REM --- Удалить старую библиотеку ---
if exist "%LIBFILE%" del "%LIBFILE%"

REM --- Создать .lib архив (Windows cmd не раскрывает *.rel для sdar) ---
setlocal enabledelayedexpansion
set RELFILES=
for %%f in ("%BUILDDIR%\*.rel") do set RELFILES=!RELFILES! "%%f"
sdar -rc "%LIBFILE%" %RELFILES% >nul
if errorlevel 1 ( echo FAIL sdar && exit /b 1 )
endlocal

echo   wc_api.lib OK
exit /b 0
