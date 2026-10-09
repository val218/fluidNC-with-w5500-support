@echo off
setlocal EnableExtensions
title dpCREATOR SD card drive
rem ===========================================================================
rem  dpCREATOR / FluidNC: show the board's SD card as a drive in This PC.
rem
rem  Double-click this file (do NOT "Run as administrator").
rem  The WebUI (Files > PC drive) downloads a copy with your board IP filled in.
rem    dpcreator-sd-drive.cmd                     asks for the board IP
rem    dpcreator-sd-drive.cmd 192.168.10.104      board IP, drive S:
rem    dpcreator-sd-drive.cmd 192.168.10.104 Z    board IP, drive Z:
rem    dpcreator-sd-drive.cmd remove              remove drive + auto-reconnect
rem
rem  What it does:
rem   1. Once, with one Windows admin prompt: WebClient (the Windows WebDAV
rem      client) starts automatically, and its 50 MB file size limit is lifted.
rem   2. Maps http://<board>/sd to a drive letter, named "dpCREATOR SD".
rem   3. Adds a hidden sign-in task (Startup folder) that reconnects the drive.
rem ===========================================================================

set "DEFAULT_IP=192.168.10.104"
rem ASK_IP=0: use DEFAULT_IP without asking (the WebUI download fills it in).
set "ASK_IP=1"
set "BOARD=%~1"
set "LETTER=%~2"
if "%LETTER%"=="" set "LETTER=S"
set "LABEL=dpCREATOR SD"
set "STARTUP=%APPDATA%\Microsoft\Windows\Start Menu\Programs\Startup"
set "STARTUP_FILE=%STARTUP%\dpCREATOR SD drive.vbs"
set "WEBCLIENT_KEY=HKLM\SYSTEM\CurrentControlSet\Services\WebClient\Parameters"

rem Mapping as administrator would hide the drive from your normal Explorer.
net session >nul 2>&1
if not errorlevel 1 (
    echo This window is running as administrator.
    echo Close it and just double-click the script instead; it asks for admin
    echo rights itself only for the one step that needs them.
    goto :end
)

if /i "%BOARD%"=="remove" goto :remove

if "%BOARD%"=="" if "%ASK_IP%"=="1" set /p "BOARD=Board IP address [%DEFAULT_IP%]: "
if "%BOARD%"=="" set "BOARD=%DEFAULT_IP%"
set "URL=http://%BOARD%/sd"

echo.
echo [1/4] Checking the board at %BOARD% ...
ping -n 2 -w 1500 %BOARD% >nul
if errorlevel 1 (
    echo   No answer from %BOARD%. Check the cable, power and IP ^($Ethernet/Status^).
    goto :end
)
echo   OK

echo [2/4] Windows WebDAV client ...
set "NEED_ADMIN=0"
sc qc WebClient | find /i "AUTO_START" >nul || set "NEED_ADMIN=1"
reg query "%WEBCLIENT_KEY%" /v FileSizeLimitInBytes 2>nul | find /i "0xffffffff" >nul || set "NEED_ADMIN=1"
if "%NEED_ADMIN%"=="1" (
    echo   One-time setup needs administrator rights: answer YES on the next prompt.
    powershell -NoProfile -ExecutionPolicy Bypass -Command "try { Start-Process cmd.exe -Verb RunAs -Wait -WindowStyle Hidden -ArgumentList '/c sc config WebClient start= auto & reg add %WEBCLIENT_KEY% /v FileSizeLimitInBytes /t REG_DWORD /d 4294967295 /f & net stop WebClient & net start WebClient' } catch { exit 1 }"
    if errorlevel 1 (
        echo   Admin prompt was cancelled. The drive may still work, but WebClient
        echo   will not start by itself and files over 50 MB will not copy.
    )
)
sc query WebClient | find /i "RUNNING" >nul || (
    rem WebClient is "trigger start": touching a WebDAV path starts it too.
    dir "\\%BOARD%@80\sd" >nul 2>&1
)
echo   OK

echo [3/4] Mapping %LETTER%: to %URL% ...
net use %LETTER%: /delete /y >nul 2>&1
net use %LETTER%: %URL% /persistent:yes
if errorlevel 1 (
    echo   Mapping failed. Check that http://%BOARD% opens the dpCREATOR WebUI and
    echo   that the SD card shows in its Files panel, then restart the PC once
    echo   and run this again.
    goto :end
)
rem Drive name shown in This PC (both key spellings Windows uses for WebDAV).
for %%K in ("##%BOARD%#sd" "##%BOARD%@80#sd" "##%BOARD%@80#DavWWWRoot#sd") do (
    reg add "HKCU\Software\Microsoft\Windows\CurrentVersion\Explorer\MountPoints2\%%~K" /v _LabelFromReg /t REG_SZ /d "%LABEL%" /f >nul 2>&1
)
echo   OK

echo [4/4] Reconnect at sign-in ...
> "%STARTUP_FILE%" echo ' dpCREATOR: reconnect the FluidNC SD card drive at sign-in (made by dpcreator-sd-drive.cmd)
>>"%STARTUP_FILE%" echo WScript.Sleep 15000
>>"%STARTUP_FILE%" echo CreateObject("WScript.Shell").Run "cmd /c net use %LETTER%: >nul 2>&1 || net use %LETTER%: %URL% /persistent:yes", 0, False
echo   OK

echo.
echo Done. %LETTER%: "%LABEL%" is in This PC under "Network locations".
echo Copy G-code onto it; the board builds the pendant .viz files by itself.
start "" explorer.exe %LETTER%:\
goto :end

:remove
set "LETTER=%~2"
if "%LETTER%"=="" set "LETTER=S"
net use %LETTER%: /delete /y
del "%STARTUP_FILE%" >nul 2>&1
echo Removed %LETTER%: and the sign-in reconnect.

:end
echo.
pause
endlocal
