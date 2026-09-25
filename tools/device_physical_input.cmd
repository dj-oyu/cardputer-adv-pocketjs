@echo off
rem Runs device_physical_input.py under the ESP-IDF Python, which is the one
rem with pyserial. Exists because "python" on this machine resolves to
rem PlatformIO's interpreter (CLAUDE.md: do not mix the two), and the check is
rem meant to be typed by whoever is pressing the keys.
rem
rem   tools\device_physical_input.cmd COM3
setlocal
set IDF_PY=C:\Espressif\tools\python\v6.0.1\venv\Scripts\python.exe
if not exist "%IDF_PY%" (
  echo ESP-IDF python not found at %IDF_PY%
  exit /b 2
)
set PORT=%1
if "%PORT%"=="" set PORT=COM3
"%IDF_PY%" "%~dp0device_physical_input.py" --port %PORT% %2 %3 %4
