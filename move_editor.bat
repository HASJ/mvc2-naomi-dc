@echo off
setlocal
set "PYTHONPATH=%~dp0tools;%PYTHONPATH%"
where pythonw >nul 2>nul
if %ERRORLEVEL%==0 (
    start "" pythonw -m move_editor --repo "%~dp0." %*
) else (
    python -m move_editor --repo "%~dp0." %*
)
