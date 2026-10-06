@echo off
rem Starts Sekiro with sekicraft.dll through me3 (Arxan neutralized, offline).
rem Log: sekiro\build\sekicraft.log
cd /d "%~dp0"
chcp 65001 >nul
me3\bin\me3.exe launch -p sekicraft.me3 --disable-arxan true --online false %*
