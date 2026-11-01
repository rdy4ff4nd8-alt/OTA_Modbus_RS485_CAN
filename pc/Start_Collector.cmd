@echo off
title Gateway Collector
cd /d "%~dp0.."
if not exist "C:\Python\Anaconda\python.exe" (
    echo Python not found: C:\Python\Anaconda\python.exe
    pause
    exit /b 1
)
echo Starting collector. Keep this window open while collecting data.
echo Database: %CD%\modbus_gateway.sqlite3
"C:\Python\Anaconda\python.exe" -u "%~dp0collector.py"
echo.
echo Collector stopped. Exit code: %ERRORLEVEL%
echo If an error is shown above, send a screenshot of this window.
pause
