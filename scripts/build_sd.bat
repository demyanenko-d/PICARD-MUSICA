@echo off
REM Build the SD card image: SD\ -> build\sd.img (FAT32, 100 MB).
REM
REM ASCII ONLY. cmd reads this file as CP866.
REM
REM The image holds Wild Commander with the plugin and SD\test_music.
REM The PC FatFs test mounts it, emulators take it as the card.

setlocal
cd /d "%~dp0.."

if not exist build mkdir build
if exist build\sd.img del /q build\sd.img

"%~dp0robimg.exe" -p="build\sd.img" -s=102400 -C="SD" >nul
if not exist build\sd.img ( echo FAIL robimg && exit /b 1 )

for %%F in (build\sd.img) do echo build\sd.img  %%~zF bytes
endlocal
exit /b 0
