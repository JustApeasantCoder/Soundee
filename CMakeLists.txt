cmake_minimum_required(VERSION 3.22)
file(STRINGS "${CMAKE_CURRENT_SOURCE_DIR}/VERSION" SOUNDEE_VERSION LIMIT_COUNT 1)
project(Soundee VERSION ${SOUNDEE_VERSION} LANGUAGES C CXX)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
if(MSVC)
    # Packages must also run on machines without the Visual C++ redistributable.
    set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
endif()
if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/vendor/JUCE/CMakeLists.txt")
    add_subdirectory(vendor/JUCE EXCLUDE_FROM_ALL)
else()
    include(FetchContent)
    FetchContent_Declare(JUCE GIT_REPOSITORY https://github.com/juce-framework/JUCE.git
        GIT_TAG be29c81492b6151c8ea8d14c840e1311963b3a83)
    FetchContent_MakeAvailable(JUCE)
endif()
configure_file("${JUCE_SOURCE_DIR}/LICENSE.md" "${CMAKE_CURRENT_BINARY_DIR}/licenses/JUCE-LICENSE.md" COPYONLY)
juce_add_gui_app(Soundee PRODUCT_NAME Soundee COMPANY_NAME Soundee
    BUNDLE_ID com.soundee.desktop VERSION ${PROJECT_VERSION}
    ICON_BIG assets/branding/soundee.png ICON_SMALL assets/branding/soundee.png)
juce_generate_juce_header(Soundee)
juce_add_binary_data(SoundeeAssets SOURCES assets/branding/soundee.png)
if(WIN32)
    # JUCE's resource rule omits Info.txt from its dependencies. Rebuild Windows
    # version metadata when VERSION changes during an incremental package build.
    get_target_property(SOUNDEE_INFO_FILE Soundee JUCE_INFO_FILE)
    get_target_property(SOUNDEE_GENERATED_DIR Soundee JUCE_GENERATED_SOURCES_DIRECTORY)
    add_custom_command(OUTPUT "${SOUNDEE_GENERATED_DIR}/Soundee_resources.rc"
        APPEND DEPENDS "${SOUNDEE_INFO_FILE}")
endif()
target_sources(Soundee PRIVATE src/Main.cpp src/Profile.cpp src/Filters.cpp
    src/Diagnostics.cpp src/MainComponent.cpp src/SystemEq.cpp src/ParametricEq.cpp
    src/ListeningProfiles.cpp src/AudioMeter.cpp src/MeterPanel.cpp src/ProfileControls.cpp src/MainView.cpp src/FeatureChecks.cpp
    src/NativeBridge.cpp src/DesktopIntegration.cpp src/ProfileArchive.cpp src/ResponseComparison.cpp src/EqWorkflow.cpp src/AdvancedChecks.cpp src/ReviewRegressionChecks.cpp)
target_compile_definitions(Soundee PRIVATE JUCE_WEB_BROWSER=0 JUCE_USE_CURL=0
    JUCE_APPLICATION_NAME_STRING="$<TARGET_PROPERTY:Soundee,JUCE_PRODUCT_NAME>"
    JUCE_APPLICATION_VERSION_STRING="${PROJECT_VERSION}"
    SOUNDEE_PROJECT_ROOT="${CMAKE_CURRENT_SOURCE_DIR}")
target_link_libraries(Soundee PRIVATE SoundeeAssets juce::juce_gui_extra juce::juce_audio_formats juce::juce_dsp juce::juce_cryptography
    juce::juce_recommended_config_flags)
if(MSVC)
    target_compile_options(Soundee PRIVATE /utf-8)
endif()

add_library(SoundeeDSP SHARED src/NativePlugin.cpp)
target_compile_features(SoundeeDSP PRIVATE cxx_std_20)
set_target_properties(SoundeeDSP PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/Soundee_artefacts")
if(MSVC)
    target_compile_options(SoundeeDSP PRIVATE /O2 /utf-8)
    set_property(TARGET SoundeeDSP PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
endif()
add_dependencies(Soundee SoundeeDSP)
add_executable(SoundeeBackendProbe tools/backend-probe/probe.cpp)
target_compile_features(SoundeeBackendProbe PRIVATE cxx_std_20)
target_compile_definitions(SoundeeBackendProbe PRIVATE UNICODE _UNICODE NOMINMAX)
target_link_libraries(SoundeeBackendProbe PRIVATE ole32 uuid propsys advapi32)
set_target_properties(SoundeeBackendProbe PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/Soundee_artefacts")
add_dependencies(Soundee SoundeeBackendProbe)

add_executable(SoundeeEndpointSetup src/EndpointSetup.cpp
    third_party/equalizerapo/DeviceAPOInfo.cpp third_party/equalizerapo/AbstractAPOInfo.cpp
    third_party/equalizerapo/VoicemeeterAPOInfo.cpp
    third_party/equalizerapo/helpers/RegistryHelper.cpp third_party/equalizerapo/helpers/StringHelper.cpp)
target_include_directories(SoundeeEndpointSetup PRIVATE third_party/equalizerapo)
target_compile_definitions(SoundeeEndpointSetup PRIVATE UNICODE _UNICODE)
target_link_libraries(SoundeeEndpointSetup PRIVATE ole32 oleaut32 uuid advapi32 shlwapi shell32 authz version)
set_target_properties(SoundeeEndpointSetup PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/Soundee_artefacts")
add_dependencies(Soundee SoundeeEndpointSetup)
enable_testing()
set(SOUNDEE_TEST_PROFILE "${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures/raw/stereo-v3.swproj")
add_test(NAME Soundee.Core COMMAND $<TARGET_FILE:Soundee> --self-test
    "${SOUNDEE_TEST_PROFILE}"
    "${CMAKE_CURRENT_BINARY_DIR}/qa")
set_tests_properties(Soundee.Core PROPERTIES TIMEOUT 120)
add_test(NAME Soundee.UI COMMAND $<TARGET_FILE:Soundee> --ui-test
    "${SOUNDEE_TEST_PROFILE}"
    "${CMAKE_CURRENT_BINARY_DIR}/qa")
set_tests_properties(Soundee.UI PROPERTIES TIMEOUT 120)
add_test(NAME Soundee.SystemEq COMMAND $<TARGET_FILE:Soundee> --system-eq-test
    "${SOUNDEE_TEST_PROFILE}"
    "${CMAKE_CURRENT_BINARY_DIR}/qa")
set_tests_properties(Soundee.SystemEq PROPERTIES TIMEOUT 60)
set(SOUNDEE_PERSONAL_PROFILE "" CACHE FILEPATH "Optional private calibration profile for additional local validation")
if(SOUNDEE_PERSONAL_PROFILE)
    add_test(NAME Soundee.PersonalProfile COMMAND $<TARGET_FILE:Soundee> --self-test
        "${SOUNDEE_PERSONAL_PROFILE}" "${CMAKE_CURRENT_BINARY_DIR}/qa-personal")
    set_tests_properties(Soundee.PersonalProfile PROPERTIES TIMEOUT 120)
endif()
