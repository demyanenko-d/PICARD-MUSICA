@echo off
REM ==========================================================================
REM  flash_uf2.cmd <path-to-uf2>   --  load one UF2 onto the board
REM
REM  ASCII ONLY. cmd reads this file as CP866; UTF-8 Cyrillic here turns
REM  into garbage that breaks command parsing. Same rule as build.bat.
REM
REM  The firmware logs over the PHYSICAL UART (GPIO32/33), not USB CDC, so
REM  there is no reset endpoint for picotool to poke: the board cannot be
REM  rebooted into BOOTSEL from here. Holding the button is the only way,
REM  which is why this waits instead of failing outright.
REM
REM  NEVER use "if errorlevel 1" on picotool. With no board attached it
REM  returns -7, and "if errorlevel N" is a SIGNED >= test: -7 >= 1 is false,
REM  so the check reads as success and the script cheerfully reports a flash
REM  that never happened. Measured, not guessed. Compare against "0" instead.
REM ==========================================================================
setlocal

set "UF2=%~1"
if "%UF2%"=="" ( echo *** flash_uf2: no UF2 given *** & exit /b 1 )
if not exist "%UF2%" ( echo *** flash_uf2: not found: %UF2% *** & exit /b 1 )

REM picotool ships with the SDK install; take the newest version present.
set "PICOTOOL="
set "PT=%USERPROFILE%\.pico-sdk\picotool"
for /f "delims=" %%D in ('dir /b /o:n "%PT%" 2^>nul') do (
    if exist "%PT%\%%D\picotool\picotool.exe" set "PICOTOOL=%PT%\%%D\picotool\picotool.exe"
)
if not defined PICOTOOL ( echo *** flash_uf2: picotool.exe not found under %PT% *** & exit /b 1 )

REM Already in BOOTSEL? Then skip the prompt entirely.
"%PICOTOOL%" info >nul 2>&1
if "%ERRORLEVEL%"=="0" goto :load

echo.
echo     Hold BOOTSEL and plug the board in (or tap RESET while holding it).
echo     Waiting up to 60 seconds...
echo.
set /a TRIES=0
:wait
set /a TRIES+=1
if %TRIES% gtr 60 ( echo *** flash_uf2: no board in BOOTSEL mode after 60 s *** & exit /b 1 )
ping -n 2 127.0.0.1 >nul
"%PICOTOOL%" info >nul 2>&1
if not "%ERRORLEVEL%"=="0" goto :wait

:load
echo Loading %UF2% ...
REM -x runs the firmware right after loading, so the board comes up without
REM a manual power cycle.
"%PICOTOOL%" load -x "%UF2%"
if not "%ERRORLEVEL%"=="0" ( echo. & echo *** flash_uf2: load FAILED *** & exit /b 1 )

echo.
echo === FLASHED: %UF2% ===
exit /b 0
