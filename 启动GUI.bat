@echo off
rem Launch the GUI without a console window. Double-click this file.
cd /d "%~dp0"

where pyw >nul 2>nul
if not errorlevel 1 (
    start "" pyw "mp3_renamer_gui.py"
    exit /b
)

where pythonw >nul 2>nul
if not errorlevel 1 (
    start "" pythonw "mp3_renamer_gui.py"
    exit /b
)

echo pythonw not found, falling back to python (a console window will appear) ...
python "mp3_renamer_gui.py"
if errorlevel 1 pause
