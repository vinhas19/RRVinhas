Vinhasxyz — Rob Run overlay

Requirements:
- ROB RUN.exe running
- WebView2 Runtime installed (standard on Windows 11, else: https://developer.microsoft.com/microsoft-edge/webview2/)
- Windows 10 or 11, 64-bit
- Administrator rights (the exe prompts for elevation)

How to run:
1. Start ROB RUN.exe and reach the main menu.
2. Run RobRunAIO.exe as administrator.
3. Wait for the launcher to map the driver and start the overlay.
4. F1 = settings menu, Insert = toggle overlay, End = quit.

The launcher extracts the overlay and driver to a payload\ folder beside itself on first run. Settings are saved in CoreRR.cfg beside the overlay.

Troubleshooting:
- "Virtualization-based security is active": disable Core Isolation / Memory Integrity in Windows Security, then reboot.
- "Waiting for game" for more than 5 minutes: the game is not at the main menu yet, or the game process name is not ROB RUN.exe.
- Blank menu: install the WebView2 Runtime.
