#!/usr/bin/env bash
set -euo pipefail
echo "[*] Installing official repo packages..."
pacman -Sy --needed --noconfirm vosk-api imgui vulkan-headers curl stb portaudio sdl2 libpulse gtk3 libappindicator-gtk3 libayatana-appindicator
echo "[✔] All packages installed, you should now be able to compile konamask."
