@echo off
rem Flash P4Box (v1 chips) with the esptool that the Arduino esp32 package installed.
cd /d "%~dp0"
set ET=
for /d %%D in ("%LOCALAPPDATA%\Arduino15\packages\esp32\tools\esptool_py\*") do if exist "%%D\esptool.exe" set ET=%%D\esptool.exe
if "%ET%"=="" (echo esptool.exe not found - install the esp32 board package in Arduino IDE first & pause & exit /b 1)
set /p PORT=COM port of the panel (e.g. COM5): 
"%ET%" --chip esp32p4 -p %PORT% -b 921600 erase_flash
"%ET%" --chip esp32p4 -p %PORT% -b 921600 write_flash 0x0 P4Box_v1_full.bin
pause
