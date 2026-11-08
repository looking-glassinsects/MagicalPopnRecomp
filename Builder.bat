@echo off
rem Opens the Magical Pop'n Builder from the source code (needs Python 3). You can also drag a ROM onto this file.
rem Most people should use the ready-made Builder.exe from the releases page instead.
cd /d "%~dp0"
where pythonw >nul 2>nul && (start "" pythonw "builder\builder.pyw" %* & exit /b 0)
where pyw >nul 2>nul && (start "" pyw -3 "builder\builder.pyw" %* & exit /b 0)
echo The ready-made builder needs nothing installed:
echo   https://github.com/looking-glassinsects/MagicalPopnRecomp/releases/latest
echo.
echo To run the builder from the source instead, install Python 3 from https://www.python.org/downloads/
echo (tick "Add python.exe to PATH" during setup).
pause
