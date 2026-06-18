#!/usr/bin/env bash
set -euo pipefail

echo "[INSTALLING] Installing official repo packages..."
sudo pacman -Sy --needed --noconfirm vosk-api espeak-ng onnxruntime-cpu nlohmann-json vulkan-headers curl stb portaudio sdl2 libpulse gtk3 libappindicator-gtk3 libayatana-appindicator
echo "[CHECKING] Looking for an AUR helper..."

AUR_HELPERS=(yay paru pikaur trizen pacaur aurman aura)
AUR_HELPER=""

for helper in "${AUR_HELPERS[@]}"; do
    if command -v "$helper" >/dev/null 2>&1; then
        AUR_HELPER="$helper"
        break
    fi
done

if [[ -z "$AUR_HELPER" ]]; then
    echo "[ERROR] No AUR helper found (checked: ${AUR_HELPERS[*]})."
    echo "        imgui-full is an AUR package and cannot be installed with pacman alone."
    echo "        Install an AUR helper first, e.g.:"
    echo "          git clone https://aur.archlinux.org/yay.git && cd yay && makepkg -si"
    exit 1
fi

echo "[FOUND] Using '$AUR_HELPER' as the AUR helper."
"$AUR_HELPER" -S --needed --noconfirm imgui-full
echo "[SUCCESS] All packages installed, you should now be able to compile konamask."
