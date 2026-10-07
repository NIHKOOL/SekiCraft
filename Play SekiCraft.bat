@echo off
rem SekiCraft: starts Sekiro with the SekiCraft DLL (through me3); Sekiro then starts Minecraft.
rem What Minecraft to start: sekiro\build\sekicraft.ini (written on the first run).
cd /d "%~dp0tools"
chcp 65001 >nul
me3\bin\me3.exe launch -p sekicraft.me3 --disable-arxan true --online false %*
