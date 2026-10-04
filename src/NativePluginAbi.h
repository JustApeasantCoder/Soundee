#pragma once
#include <cstdint>
// Minimal VST2 binary interface used by Equalizer APO's plug-in NativeHost. No
// Steinberg SDK, editor, JUCE runtime, or playback device is required here.
struct NativeEffect;
using NativeDispatch = intptr_t (__cdecl *)(NativeEffect*, int32_t, int32_t, intptr_t, void*, float);
using NativeProcess = void (__cdecl *)(NativeEffect*, float**, float**, int32_t);
using NativeHost = NativeDispatch;
struct NativeEffect {
    int32_t magic = 0x56737450;
    NativeDispatch dispatcher = nullptr;
    NativeProcess process = nullptr;
    void (__cdecl *setParameter)(NativeEffect*, int32_t, float) = nullptr;
    float (__cdecl *getParameter)(NativeEffect*, int32_t) = nullptr;
    int32_t programs = 1, parameters = 2, inputs = 2, outputs = 2, flags = (1 << 4) | (1 << 5);
    void* reserved1 = nullptr; void* reserved2 = nullptr;
    int32_t delay = 0, quality1 = 0, quality2 = 0;
    float ratio = 1;
    void* object = nullptr; void* user = nullptr;
    int32_t id = 0x53654551, version = 100;
    NativeProcess processReplacing = nullptr;
    void* processDoubleReplacing = nullptr;
    char future[56]{};
};
static_assert(sizeof(NativeEffect) == 192);
