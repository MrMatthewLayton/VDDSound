@echo off
setlocal
REM ===========================================================================
REM  install.bat  -  register vddsound.dll as an NTVDM VDD and deploy the
REM                  freshly built DLL on the XP VM.
REM
REM  One script does both jobs (both idempotent, safe to re-run every build):
REM    * deploy   - kill the resident NTVDM (it holds the old DLL open),
REM                 copy the DLL that sits beside this script into C:\vddsound.
REM    * register - point HKLM\...\VirtualDeviceDrivers\VDD at that DLL so
REM                 NTVDM loads it at startup (this folds in the old install.reg
REM                 via reg.exe). Takes effect on the next NTVDM launch.
REM
REM  Lives in the shared folder next to vddsound.dll (the build script publishes
REM  both here). Run it on the VM as Administrator. No paths to edit.
REM ===========================================================================

set "SRC=%~dp0vddsound.dll"
set "DEST=C:\vddsound\vddsound.dll"
set "LOG=C:\vddsound\trace.log"
set "VDDKEY=HKLM\SYSTEM\CurrentControlSet\Control\VirtualDeviceDrivers"

echo.
echo [1/5] Killing NTVDM (it holds the old DLL open)...
taskkill /F /IM ntvdm.exe >nul 2>&1
ping -n 2 127.0.0.1 >nul
taskkill /F /IM ntvdm.exe >nul 2>&1
ping -n 2 127.0.0.1 >nul
tasklist /FI "IMAGENAME eq ntvdm.exe" 2>nul | find /I "ntvdm.exe" >nul
if not errorlevel 1 (echo     ERROR: NTVDM is STILL running. Close all DOS boxes, then rerun.& echo.& pause& exit /b 1)
echo     ok - NTVDM not running.

echo [2/5] Checking source DLL...
if not exist "%SRC%" (echo     ERROR: not found next to this script: %SRC%& echo.& pause& exit /b 1)
echo     ok - %SRC%
echo     source build tag (what the shared folder is handing us):
findstr /C:"vddsound build [" "%SRC%"

echo [3/5] Clearing old trace log...
if not exist "C:\vddsound" mkdir "C:\vddsound"
if exist "%LOG%" del /Q "%LOG%"

echo [4/5] Copying new DLL...
copy /Y "%SRC%" "%DEST%" >nul
if errorlevel 1 (echo     ERROR: copy failed - an NTVDM is probably still holding the DLL.& echo.& pause& exit /b 1)

echo [5/5] Registering vddsound as an NTVDM VDD...
REM  Sets the VDD value (REG_MULTI_SZ) to our DLL path. NOTE: this REPLACES the
REM  value. On a stock XP SP3 it is empty, so this is fine. If you have OTHER
REM  VDDs registered, back up / merge by hand instead - the current value is
REM  echoed below before we overwrite it.
echo     (current VDD value, if any:)
reg query "%VDDKEY%" /v VDD 2>nul | find /I "VDD"
reg add "%VDDKEY%" /v VDD /t REG_MULTI_SZ /d "%DEST%" /f >nul
if errorlevel 1 (echo     ERROR: reg add failed - run this script as Administrator.& echo.& pause& exit /b 1)
echo     ok - VDD = %DEST%

echo.
echo     Deployed OK:
for %%F in ("%DEST%") do echo       %%~tF   %%~zF bytes
echo     DEPLOYED build tag at %DEST% (this is what will load - confirm it is latest):
findstr /C:"vddsound build [" "%DEST%"
echo.
echo  Registration takes effect on the next NTVDM launch. Just run your DOS
echo  program, then open %LOG%.
echo  The FIRST log line must show the SAME tag printed just above.
echo  If it shows an older tag or none (DLL not loading), reboot once and retry.
echo.
pause
endlocal
