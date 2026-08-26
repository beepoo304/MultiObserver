@echo off
setlocal
powershell.exe -NoProfile -ExecutionPolicy Bypass -File "%~dp0install.ps1"
if errorlevel 1 (
    echo.
    echo MultiObserver installer FAILED.
    exit /b 1
)
echo.
echo MultiObserver installer completed successfully.
endlocal
