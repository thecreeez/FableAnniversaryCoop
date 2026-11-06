@echo off
setlocal EnableExtensions
title Theresa visual preview
powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0tools\theresa\play-theresa.ps1" %*
if errorlevel 1 pause
endlocal
