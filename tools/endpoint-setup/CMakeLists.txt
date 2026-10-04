# Standalone build for the corresponding sources included in the portable ZIP.
cmake_minimum_required(VERSION 3.22)
project(SoundeeEndpointSetup LANGUAGES CXX)
set(CMAKE_CXX_STANDARD 20)
add_executable(SoundeeEndpointSetup EndpointSetup.cpp equalizerapo/DeviceAPOInfo.cpp
    equalizerapo/AbstractAPOInfo.cpp equalizerapo/VoicemeeterAPOInfo.cpp
    equalizerapo/helpers/RegistryHelper.cpp equalizerapo/helpers/StringHelper.cpp)
target_include_directories(SoundeeEndpointSetup PRIVATE equalizerapo)
target_compile_definitions(SoundeeEndpointSetup PRIVATE UNICODE _UNICODE)
target_link_libraries(SoundeeEndpointSetup PRIVATE ole32 oleaut32 uuid advapi32 shlwapi shell32 authz version)
