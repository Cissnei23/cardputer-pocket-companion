# Pocket Player + Claude for M5Stack Cardputer ADV

An Arduino/PlatformIO project for the M5Stack Cardputer ADV. It combines an
offline microSD music player with an optional Wi-Fi client for a small Claude
Code bridge running on your computer.

## Features

- Plays local MP3 and PCM WAV files from the microSD card, through the built-in
  speaker or a wired 3.5 mm headset.
- Song list, pause/resume, volume, and three playback modes: stop after one song,
  repeat one, or play songs in order.
- Music works without Wi-Fi. The device only asks to connect when you open the
  Claude conversations screen.
- Optional Claude chat, conversation list, model selection, and five-hour and
  weekly usage indicators through the local bridge.

## Hardware and tools

- M5Stack Cardputer ADV
- FAT32 microSD card
- USB-C data cable
- Visual Studio Code with the PlatformIO IDE extension
- For Claude chat only: a computer on the same trusted Wi-Fi network, Node.js,
  and Claude Code already installed and signed in

## Build and flash

1. Clone this repository and open its folder in VS Code.
2. Install PlatformIO IDE if it is not already installed. PlatformIO reads the
   board and library configuration from `platformio.ini`.
3. Build with PlatformIO, then connect the Cardputer by USB-C and use **Upload**.
4. Put songs in a folder named `music` at the root of the microSD card. The
   firmware scans that folder when opening the music library.

Use legally obtained, unprotected audio files. No music files are included in
this repository.

## Optional Claude bridge

The firmware can be used as a music player without setting up the bridge. To
enable chat, set up the computer and device on the same private network.

1. Copy `include/pocket_claude_secrets.example.h` to
   `include/pocket_claude_secrets.h`, and copy
   `include/pocket_claude_config.example.h` to
   `include/pocket_claude_config.h`.
2. Generate a long random token. Put the same token in the firmware secrets
   header and in `claude-cardputer-bridge/.bridge-token.ps1`, copied from
   `.bridge-token.example.ps1`.
3. In `pocket_claude_config.h`, set the bridge URL to the computer's current
   private-network IP address, for example `http://192.168.1.20:8765`.
4. Make sure Node.js and Claude Code are installed and Claude Code is signed in
   on that computer.
5. In PowerShell, run `claude-cardputer-bridge/start-bridge.ps1`. If Windows
   Firewall asks, allow access only on private networks. Do not forward port
   8765 from your router or expose the bridge to the public internet.
6. Rebuild and flash the firmware. When opening Claude from the Cardputer,
   select the same Wi-Fi network and enter its password if prompted.

The bridge uses the local Claude Code login on the computer to handle chat and
usage requests. Conversation IDs and Claude Code session IDs are saved locally
in `claude-cardputer-bridge/conversations.json`; that file is ignored by Git.
Treat the bridge token as a password. The bridge is intended for a trusted local
network and uses HTTP, so do not use it on untrusted networks.

## Keys

| Screen | Keys |
| --- | --- |
| Home | `Tab` changes model; `C` opens Claude; `M` opens music |
| Music list | `W` / `S` moves; `Enter` plays; `C` returns home |
| Player | `P` pause/resume; `M` changes playback mode; `B` returns |
| Finished | `R` replays; `B` returns to list; `M` changes playback mode |
| Claude conversations | `W` / `S` moves; `Enter` opens; `N` starts new; `D` deletes; `Fn+B` returns |
| Claude chat | Type to compose; `Enter` sends; `Tab` cycles models; `Fn+B` returns |

## Privacy

Local credentials, bridge token, bridge URL configuration, chat history, build
files, and local working materials are excluded by `.gitignore`. Never commit a
real token, password, OAuth credential, conversation history, or personal audio
file. The example configuration files contain placeholders only.

