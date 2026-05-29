@echo off
setlocal
REM ===========================================================================
REM  redeploy.bat  -  swap in a freshly built vddsound.dll on the XP VM.
REM
REM  Lives in the shared folder next to vddsound.dll (the build script publishes
REM  both here). Run it on the VM: it kills the resident NTVDM (which holds the
REM  old DLL open), confirms it is gone, clears the log, and copies the DLL that
REM  sits beside this script into C:\vddsound. No paths to edit.
REM ===========================================================================

set "SRC=%~dp0vddsound.dll"
set "DEST=C:\vddsound\vddsound.dll"
set "LOG=C:\vddsound\trace.log"

echo.
echo [1/4] Killing NTVDM (it holds the old DLL open)...
taskkill /F /IM ntvdm.exe >nul 2>&1
ping -n 2 127.0.0.1 >nul
taskkill /F /IM ntvdm.exe >nul 2>&1
ping -n 2 127.0.0.1 >nul
tasklist /FI "IMAGENAME eq ntvdm.exe" 2>nul | find /I "ntvdm.exe" >nul
if not errorlevel 1 (echo     ERROR: NTVDM is STILL running. Close all DOS boxes, then rerun.& echo.& pause& exit /b 1)
echo     ok - NTVDM not running.

echo [2/4] Checking source DLL...
if not exist "%SRC%" (echo     ERROR: not found next to this script: %SRC%& echo.& pause& exit /b 1)
echo     ok - %SRC%

echo [3/4] Clearing old trace log...
if not exist "C:\vddsound" mkdir "C:\vddsound"
if exist "%LOG%" del /Q "%LOG%"

echo [4/4] Copying new DLL...
copy /Y "%SRC%" "%DEST%" >nul
if errorlevel 1 (echo     ERROR: copy failed - an NTVDM is probably still holding the DLL.& echo.& pause& exit /b 1)

echo.
echo     Deployed OK:
for %%F in ("%DEST%") do echo       %%~tF   %%~zF bytes
echo.
echo  Now run your DOS program, then open %LOG%.
echo  The FIRST log line shows the build tag, e.g. [b5-userhook].
echo  If it shows an older tag or none, the new DLL did not load.
echo.
pause
endlocal
