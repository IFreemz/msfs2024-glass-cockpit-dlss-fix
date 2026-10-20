Glass Cockpit DLSS Fix - standalone version (early beta)
=========================================================

Removes DLSS and FSR ghosting from the glass cockpit screens in MSFS 2024. No ReShade needed.

EARLY BETA: if anything goes wrong, especially a crash, please report it (link below) and use the ReShade add-on
version in the meantime, which is the proven, more stable choice for now. Use one version, not both.

Install
-------
1. Copy dxgi.dll into the folder that contains FlightSimulator2024.exe:
   - Steam: ...\steamapps\common\Microsoft Flight Simulator 2024\ (or ...\steamapps\common\MSFS2024\)
   - Microsoft Store / Xbox app: usually C:\XboxGames\Microsoft Flight Simulator 2024\Content\
   If Windows asks whether to replace an existing dxgi.dll there, DON'T: another mod (usually ReShade or OptiScaler)
   already uses that name. Rename this dxgi.dll to version.dll or winmm.dll instead (whichever isn't taken there):
   it works the same under those names. Install it only once, under one name.
2. Select DLSS or FSR in the sim's graphics settings and load a flight.
3. Press Ctrl+Shift+F6: the settings window opens and says "Working: fixing N screens".

Settings and hotkeys
--------------------
Ctrl+Shift+F6   settings window (the hotkey can be changed in the window)
Ctrl+Shift+F9   fix on/off
Ctrl+Shift+F11  green tint on every fixed screen
Bug reports (capture, logs) are in the settings window.
Settings are saved in glass-cockpit-dlss-fix.ini next to dxgi.dll.

Update: replace dxgi.dll (or version.dll / winmm.dll, if you renamed it) with the new one (your settings stay).
Uninstall: delete it and the glass-cockpit-dlss-fix.* files next to it.

Bug reports: https://github.com/IFreemz/msfs2024-glass-cockpit-dlss-fix/issues
(attach glass-cockpit-dlss-fix.log and glass-cockpit-dlss-fix.old.log from the game folder)

Glass Cockpit DLSS Fix by IFreemz - https://github.com/IFreemz/msfs2024-glass-cockpit-dlss-fix
Standalone version by Jason Tsorlinis (https://github.com/jtsorlinis). Thanks, Jason!
Licence: GPL-3.0-only with additional terms, see LICENSE and NOTICE.md. Source code: the GitHub page above.
Official downloads: the GitHub releases and the flightsim.to page only.
