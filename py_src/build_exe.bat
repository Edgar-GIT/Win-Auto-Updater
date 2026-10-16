@echo off
REM Build MacroHelper.exe on Windows x64 (run from py_src\).
setlocal
cd /d "%~dp0"

if not exist ".venv\Scripts\python.exe" (
  py -3.12 -m venv .venv 2>nul || py -3.11 -m venv .venv
)
call .venv\Scripts\activate.bat
python -m pip install --upgrade pip
python -m pip install -r requirements.txt pyinstaller

pyinstaller --noconfirm MacroHelper.spec
echo.
echo Output: %cd%\dist\MacroHelper.exe
echo Copy it next to SingleUpdate.exe on the USB stick.
endlocal
