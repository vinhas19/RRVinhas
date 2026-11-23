@echo off
rem ===========================================================================
rem  restart.bat - RR dev restart: stop, clean runtime state, rebuild, launch.
rem
rem  Phases
rem    1  stop RobRunPort.exe / RobRunAIO.exe, settle 2 s
rem    2  delete runtime state (logs, masq tracking, %TEMP% copies, payload exe)
rem    3  build_port.bat then build_aio.bat (the AIO embeds the overlay)
rem    4  verify: sizes, timestamps, hash of the build against the AIO payload
rem    5  offer to launch RobRunAIO.exe (its manifest raises the UAC prompt)
rem
rem  Never deleted: 1_run\CoreRR.cfg (user settings), 2_src_port\CoreRR.cfg,
rem  the built exes outside 1_run\payload, and the driver files.
rem  Safe to run twice. Needs no elevation itself.
rem ===========================================================================
setlocal EnableExtensions
title RR Restart

rem --paths--------------------------------------------------------------------
rem  %~dp0 always ends with a backslash; ROOT is kept without one so every
rem  derived path below has exactly one separator.
set "ROOT=%~dp0"
if "%ROOT:~-1%"=="\" set "ROOT=%ROOT:~0,-1%"
set "RUN=%ROOT%\1_run"
set "PAY=%RUN%\payload"
set "SRC=%ROOT%\2_src_port"
set "CFG=%RUN%\CoreRR.cfg"
set "PORT=%SRC%\RobRunPort.exe"
set "RUNPORT=%RUN%\RobRunPort.exe"
set "PAYPORT=%PAY%\RobRunPort.exe"
set "AIO=%RUN%\RobRunAIO.exe"

set "REMOVED=0"
set "LOCKED=0"

pushd "%ROOT%" >nul 2>nul

echo ============================================================
echo  RR Restart
echo  root: %ROOT%
echo ============================================================
echo.
if not exist "%SRC%\build_port.bat" goto no_src
if not exist "%SRC%\build_aio.bat" goto no_src

rem ===========================================================================
echo --- PHASE 1/5  STOP ---------------------------------------
rem ===========================================================================
call :kill_one "RobRunPort.exe"
call :kill_one "RobRunAIO.exe"
rem  The AIO copies the overlay to %TEMP% under a masquerade name (masq.txt,
rem  masq_overlay.txt). That copy is invisible to taskkill /IM and is usually
rem  elevated, so it can only be stopped from an elevated prompt: detect it and
rem  report instead of guessing at PIDs.
call :masq_check "%RUN%\masq.txt"
call :masq_check "%PAY%\masq_overlay.txt"
echo   waiting 2 s for handles to close ...
timeout /t 2 /nobreak >nul 2>nul

rem ===========================================================================
echo.
echo --- PHASE 2/5  CLEAN --------------------------------------
rem ===========================================================================
rem  Config files are deliberately absent from this list: 1_run\CoreRR.cfg is
rem  the user's settings. Only the payload copy of the AIO config is removed.
call :del_one "%RUN%\robrun_runtime.txt"
call :del_one "%RUN%\robrun_chain.txt"
call :del_one "%RUN%\status.log"
call :del_one "%RUN%\discovery.log"
call :del_one "%RUN%\hotkeys.log"
call :del_one "%RUN%\crash.log"
call :del_one "%RUN%\wv2_error.log"
call :del_one "%RUN%\selftest.log"
call :del_one "%RUN%\masq.txt"
call :del_one "%RUN%\masq_overlay.txt"
call :del_one "%PAY%\robrun_runtime.txt"
call :del_one "%PAY%\robrun_chain.txt"
call :del_one "%PAY%\status.log"
call :del_one "%PAY%\discovery.log"
call :del_one "%PAY%\hotkeys.log"
call :del_one "%PAY%\crash.log"
call :del_one "%PAY%\wv2_error.log"
call :del_one "%PAY%\selftest.log"
call :del_one "%PAY%\masq_overlay.txt"
call :del_one "%PAY%\CoreRR.cfg"
call :del_one "%PAYPORT%"
call :del_one "%PAY%\RobRunAIO.exe"
if defined TEMP call :del_one "%TEMP%\robrun_runtime.txt"
if defined TEMP call :del_one "%TEMP%\robrun_chain.txt"
if defined TEMP call :del_one "%TEMP%\status.log"
if defined TEMP call :del_one "%TEMP%\discovery.log"
if defined TEMP call :del_one "%TEMP%\hotkeys.log"
if defined TEMP call :del_one "%TEMP%\crash.log"
if defined TEMP call :del_one "%TEMP%\wv2_error.log"
if defined TEMP call :del_one "%TEMP%\CoreRR.cfg"
if defined TEMP call :del_one "%TEMP%\menu_harness.html"
if defined TEMP for %%f in ("%TEMP%\masq*.txt") do call :del_one "%%~ff"
echo.
echo   cleaned %REMOVED% file(s), %LOCKED% locked
rem ===========================================================================
echo.
echo --- PHASE 3/5  REBUILD ------------------------------------
rem ===========================================================================
cd /d "%SRC%"
if errorlevel 1 goto no_src
echo   [1/2] build_port.bat  -^> 2_src_port\RobRunPort.exe + 1_run\RobRunPort.exe
call "%SRC%\build_port.bat"
if errorlevel 1 goto build_failed
echo.
echo   [2/2] build_aio.bat   -^> 1_run\RobRunAIO.exe (embeds the new overlay)
call "%SRC%\build_aio.bat"
if errorlevel 1 goto aio_failed

rem ===========================================================================
echo.
echo --- PHASE 4/5  VERIFY -------------------------------------
rem ===========================================================================
call :size_of "%PORT%"
call :size_of "%RUNPORT%"
call :size_of "%PAYPORT%"
call :size_of "%AIO%"
echo.
call :compare "%PORT%" "%RUNPORT%" "build vs 1_run copy"
call :compare "%PORT%" "%PAYPORT%" "build vs AIO payload"

rem ===========================================================================
echo.
echo --- PHASE 5/5  LAUNCH -------------------------------------
rem ===========================================================================
if not exist "%AIO%" goto no_aio
choice /c yn /n /m "Launch RobRunAIO now? (Y/N) " 2>nul
if errorlevel 255 goto no_input
if errorlevel 2 goto done
if errorlevel 1 goto launch
goto done

:no_input
rem  choice returns 255 when there is no console (stdin redirected or launched by
rem  a tool): treat that as "no", never as a launch.
echo   no console input - not launching; start 1_run\RobRunAIO.exe yourself.
goto done

:launch
rem  RobRunAIO.exe carries requireAdministrator, so this start raises UAC; this
rem  script itself stays unelevated. /d keeps the working folder at 1_run.
echo   starting 1_run\RobRunAIO.exe  (accept the UAC prompt) ...
start "" /d "%RUN%" "%AIO%"
goto done

rem ===========================================================================
:done
cd /d "%ROOT%"
call :read_cfg
echo.
echo   Reminder: 1_run\CoreRR.cfg keeps your settings.
echo             debug_slots=%DBG%  (1 = slot debug on, 0 = off)
echo.
popd >nul 2>nul
exit /b 0
rem ===========================================================================
rem  helper: stop one image by name, report what actually happened
rem ===========================================================================
:kill_one
set "K_NAME=%~1"
tasklist /fi "IMAGENAME eq %K_NAME%" 2>nul | find /i "%K_NAME%" >nul
if errorlevel 1 goto kill_none
taskkill /f /im "%K_NAME%" >nul 2>nul
tasklist /fi "IMAGENAME eq %K_NAME%" 2>nul | find /i "%K_NAME%" >nul
if errorlevel 1 goto kill_ok
echo   still alive : %K_NAME%  (elevated - run this from an admin prompt to stop it)
exit /b 0
:kill_ok
echo   stopped     : %K_NAME%
exit /b 0
:kill_none
echo   not running : %K_NAME%
exit /b 0

rem ===========================================================================
rem  helper: the masqueraded overlay is not matched by taskkill /IM, so check the
rem  recorded path and only report it (killing "%TEMP%\WmiPrvSE.exe" by name
rem  would also hit the real Windows host process).
rem ===========================================================================
:masq_check
if not exist "%~1" exit /b 0
set "M_PATH="
for /f "usebackq delims=" %%p in ("%~1") do if not defined M_PATH set "M_PATH=%%p"
if not defined M_PATH exit /b 0
set "M_NAME="
for %%n in ("%M_PATH%") do set "M_NAME=%%~nxn"
if not defined M_NAME exit /b 0
tasklist /fi "IMAGENAME eq %M_NAME%" 2>nul | find /i "%M_NAME%" >nul
if errorlevel 1 exit /b 0
echo   still alive : %M_NAME%  (masqueraded overlay at %M_PATH%)
exit /b 0

rem ===========================================================================
rem  helper: delete one file, count it, never fail the script
rem ===========================================================================
:del_one
if not exist "%~1" exit /b 0
del /f /q "%~1" >nul 2>nul
if not exist "%~1" goto del_ok
set /a LOCKED+=1
echo   locked   %~1
exit /b 0
:del_ok
set /a REMOVED+=1
echo   removed  %~1
exit /b 0

rem ===========================================================================
rem  helper: size and timestamp of one file
rem ===========================================================================
:size_of
if not exist "%~1" goto size_missing
for %%f in ("%~1") do echo   %%~nxf  %%~zf bytes  %%~tf  (%%~dpf)
exit /b 0
:size_missing
echo   missing   %~1  (expected after a clean; recreated by the build or the AIO)
exit /b 0

rem ===========================================================================
rem  helper: SHA-256 of one file into SHA (certutil - no PowerShell needed)
rem ===========================================================================
:sha256
set "SHA="
for /f "skip=1 tokens=*" %%h in ('certutil -hashfile "%~1" SHA256 2^>nul') do if not defined SHA set "SHA=%%h"
if not defined SHA exit /b 1
set "SHA=%SHA: =%"
exit /b 0

rem ===========================================================================
rem  helper: compare two files by hash, warn on mismatch but keep going
rem ===========================================================================
:compare
if not exist "%~1" goto cmp_no_a
if not exist "%~2" goto cmp_no_b
call :sha256 "%~1"
set "C_H1=%SHA%"
call :sha256 "%~2"
set "C_H2=%SHA%"
if /i "%C_H1%"=="%C_H2%" goto cmp_same
echo   MISMATCH  %~3
echo     %~nx1  %C_H1%
echo     %~nx2  %C_H2%
echo     warning: the AIO re-extracts its embedded payload on launch, so the
echo              next run still uses its own copy - rebuild the AIO to sync.
exit /b 0
:cmp_same
echo   match     %~3  %C_H1%
exit /b 0
:cmp_no_a
echo   skipped   %~3  (missing %~nx1)
exit /b 0
:cmp_no_b
echo   skipped   %~3  (missing %~nx2 - the AIO extracts it on launch)
exit /b 0

rem ===========================================================================
rem  helper: read debug_slots out of 1_run\CoreRR.cfg
rem ===========================================================================
:read_cfg
set "DBG=missing"
if not exist "%CFG%" exit /b 0
set "DBG=unset"
for /f "tokens=1,2 delims== " %%a in ('findstr /r /i /c:"^debug_slots[ =]" "%CFG%" 2^>nul') do set "DBG=%%b"
if not defined DBG set "DBG=unset"
exit /b 0

rem ===========================================================================
rem  failures
rem ===========================================================================
:no_aio
echo   NOT FOUND: %AIO%
echo   check that 1_run exists and holds RobRunAIO.exe.
goto done

:no_src
echo.
echo ERROR: build scripts not found under "%SRC%".
echo        restart.bat must sit in the repo root next to 1_run and 2_src_port.
popd >nul 2>nul
pause
exit /b 1

:build_failed
echo.
echo BUILD FAILED: build_port.bat returned an error - overlay not rebuilt.
popd >nul 2>nul
pause
exit /b 1

:aio_failed
echo.
echo AIO BUILD FAILED: build_aio.bat returned an error.
echo 1_run\RobRunPort.exe is the new overlay, but 1_run\RobRunAIO.exe still
echo embeds the previous one; do not launch until this build succeeds.
popd >nul 2>nul
pause
exit /b 1


