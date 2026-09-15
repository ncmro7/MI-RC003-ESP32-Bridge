@echo off
setlocal
title Build MI-RC003 Bridge Firmware
rem One-click firmware build: compiles and generates firmware for BOTH the
rem Windows standalone flasher (build\merged-flash.bin) and the web flasher
rem (webusb-config\flash\firmware\merged-flash.bin + manifest.json).
rem Usage: build-firmware.bat [-Profile n16r8|n8r2|n4r2] [-NoBuild] [-FlashMode dio] [-FlashFreq 80m] [-FlashSize 16MB]
powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0tools\build-firmware.ps1" %*
set EC=%ERRORLEVEL%
echo.
pause
endlocal & exit /b %EC%
