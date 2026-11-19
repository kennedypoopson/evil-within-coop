@echo off
rem Starts another copy of The Evil Within for two-copy co-op testing on one PC.
rem 1) In the copy that is already running (with ewcoop injected) press Ctrl+Shift+F1 to release the game's
rem    one-copy lock.
rem 2) Run this. The Steam variables let the game start directly instead of handing off to Steam.
setlocal
set "GAMEDIR=C:\Program Files (x86)\Steam\steamapps\common\TheEvilWithin"
set SteamAppId=268050
set SteamGameId=268050
set SteamOverlayGameId=268050
pushd "%GAMEDIR%"
rem r_letterBoxMode 0 + r_forceAspectRatio 0: no black bars (the game defaults to a 2.5:1 letterbox).
start "" "%GAMEDIR%\EvilWithin.exe" +com_allowconsole 1 +com_skipIntroVideo 1 +r_letterBoxMode 0 +r_forceAspectRatio 0
popd
