# konamask - a voice masking utility
### (ReadMe version: Pre-Release 1.0)
## In short:
konamask is a (currently) **Linux-only** application that:
1) captures microphone input,
2) performs speech recognition (converts input to text),
3) then outputs masked speech trough a virtual microphone.

All manageable from a simple user interface.

The goal is privacy: replace your voice in real time.

Included configuration and customizability of the voice `input`, `output` and `user interface` is simple and begginer friendly.

## Features
- Speech-to-text (real-time voice recognition) (using Vosk-API) for fluid input.
- Text-to-speech (voice output pipeline) for masking your voice.
- GUI for control and masking configuration, selecting models, and tuning parameters.
- System integration on Linux (PulseAudio / appindicator support) for minimal disruption while not in use.

## Required libraries
these packages must be installed before building
- Required for voice transformation: 
`vosk-api`
`portaudio`
`libpulse`
- Required for rendering: 
`vulkan-headers`
`sdl2`
`stb`
`imgui`
- Miscellaneous: 
`curl`
`gtk3`
`libappindicator-gtk3`
`libayatana-appindicator`

Note: the precise package names your distribution uses may differ slightly; see the platform-specific notes below.

## Platform notes & package availability

### Arch Linux (recommended / primary target)
```
sudo pacman -S --needed vosk-api portaudio vulkan-headers curl stb gtk3 libpulse libayatana-appindicator sdl2
```
**SDL2** on Arch: the official repo may provide `sdl2-compat` to satisfy `sdl2` dependencies - check `pacman -Ss sdl2`.

### Debian / Ubuntu (example packages)
```
sudo apt install build-essential cmake pkg-config libsdl2-dev libgl1-mesa-dev libvulkan-dev vulkan-sdk libportaudio2 portaudio19-dev libpulse-dev libgtk-3-dev libayatana-appindicator3-dev libcurl4-openssl-dev libstb-dev libnlohmann-json-dev
```
On recent Ubuntu releases `libayatana-appindicator3-dev` is the package that replaces the older `libappindicator` development package. If you get dependency conflicts, select `libayatana-appindicator3-dev`.

**Vosk-API**: Vosk may not be packaged in all distributions. The upstream project documents installing language models and client bindings (Python) and offers sources; for system integration on Debian/Ubuntu you may need to build or install a Vosk dev package or use the provided pip/bindings.
