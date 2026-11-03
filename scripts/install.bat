@echo off
rem Install Nano Script into %USERPROFILE%\.ns by running install.ps1.
rem
rem   curl -fsSLO https://liamlangli.com/ns/install.bat && install.bat
rem
rem Windows 10 and later ship both curl and PowerShell.
setlocal
if not defined NS_RELEASE_URL set "NS_RELEASE_URL=https://liamlangli.com/ns"
powershell -NoProfile -ExecutionPolicy Bypass -Command "irm '%NS_RELEASE_URL%/install.ps1' | iex"
if errorlevel 1 exit /b 1
endlocal
