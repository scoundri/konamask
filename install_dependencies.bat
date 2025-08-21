# if vcpkg is not installed
# git clone https://github.com/microsoft/vcpkg.git C:\vcpkg
# cd C:\vcpkg
# .\bootstrap-vcpkg.bat
# install packages
vcpkg.exe install ^
    portaudio:x64-windows ^
    sdl2:x64-windows ^
    curl[core]:x64-windows ^
    vulkan-loader:x64-windows ^
    imgui[sdl2-binding,vulkan-binding]:x64-windows ^
    vulkan-headers:x64-windows ^
    nlohmann-json:x64-windows ^
    stb:x64-windows ^
    vosk-api:x64-windows ^
    espeak-ng:x64-windows

pause
