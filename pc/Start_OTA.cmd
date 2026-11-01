@echo off
title Gateway OTA Release
cd /d "%~dp0.."
if not exist "C:\Python\Anaconda\python.exe" (
    echo Python not found: C:\Python\Anaconda\python.exe
    pause
    exit /b 1
)
"C:\Python\Anaconda\python.exe" -u "%~dp0start_ota.py"
echo.
echo OTA launcher ended. If an older OTA server is serving this release, keep that server running.
echo Otherwise keep this window open during the upgrade.
pause
