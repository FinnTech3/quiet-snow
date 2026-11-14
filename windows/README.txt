Quiet Snow for Windows
======================

Run QuietSnow.exe. A snowflake appears in the tray (under the ^ arrow if Windows hides it). Right-click it for the menu.
There is no installer and nothing runs in the background after you quit it.

The menu has two places for the snow: "Over all apps and screens" (the default) and "Desktop only", where it hides behind windows.

The menu also has a snow type, an optional pause hotkey (Ctrl+Alt+Shift+S), Copy diagnostics (for bug reports), Check for
updates (opens the release page in your browser) and Reset settings. It hides itself while Windows is in presentation mode.

Remove it: right-click the tray snowflake, choose Quit, then delete this folder. If you switched on "Open at login",
switch it off first (same menu). Your settings live under HKEY_CURRENT_USER\Software\FinnTech3\QuietSnow.

What it touches: it draws a see-through, click-through window over each display, and reads where your windows are
(position and size only, never their contents). It makes no network connections and needs no admin rights.

Source, licence and checks: https://github.com/FinnTech3/quiet-snow
The SHA256 sums for this release are in SHA256SUMS-Windows.txt on the release page.
