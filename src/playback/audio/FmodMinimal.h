#pragma once

// Minimal FMOD 2.02 ABI declarations for the export-capture DSP. The project does not ship fmod headers; only the
// subset of the plug-in ABI we touch is declared here, matched against the 2.02 docs. Kept private to the audio module.

#include <cstdint>
#include <cstring>

namespace playback::audio::fmod {

using FMOD_RESULT      = int;
using FMOD_BOOL        = int;
using FMOD_CHANNELMASK = unsigned int;
using FMOD_SPEAKERMODE = int;

#define PB_F_CALLBACK __stdcall

constexpr unsigned int FMOD_PLUGIN_SDK_VERSION      = 110;
constexpr int          FMOD_CHANNELCONTROL_DSP_HEAD = -1;
constexpr int          FMOD_CHANNELCONTROL_DSP_TAIL = -3;
constexpr FMOD_RESULT  FMOD_OK                      = 0;
constexpr int          FMOD_OUTPUTTYPE_NOSOUND_NRT  = 4;

struct FMOD_DSP_STATE {
    void*            instance;
    void*            plugindata;
    FMOD_CHANNELMASK channelmask;
    FMOD_SPEAKERMODE source_speakermode;
    float*           sidechaindata;
    int              sidechainchannels;
    void*            functions; // FMOD_DSP_STATE_FUNCTIONS*, unused
    int              systemobject;
};

// Resolved fmod.dll C API entry points, filled by hooks::install() so GetProcAddress stays in one place. All pointers
// are void* because the project's FMOD types are opaque.
struct CApi {
    int (*getOutput)(void*, int*){};
    int (*setOutput)(void*, int){};
    int (*getDriver)(void*, int*){};
    int (*setDriver)(void*, int){};
    int (*getChannelsPlaying)(void*, int*, int*){};
    int (*getMasterChannelGroup)(void*, void**){};
    int (*createDSP)(void*, void const* desc, void** dsp){};
    int (*groupGetNumDSPs)(void*, int*){};
    int (*groupGetDSPIndex)(void*, void*, int*){};
    int (*groupAddDSP)(void*, int index, void* dsp){};
    int (*groupRemoveDSP)(void*, void*){};
    int (*groupStop)(void*){};
    int (*dspRelease)(void*){};
};

using FMOD_DSP_CREATE_CALLBACK  = FMOD_RESULT(PB_F_CALLBACK*)(FMOD_DSP_STATE*);
using FMOD_DSP_RELEASE_CALLBACK = FMOD_RESULT(PB_F_CALLBACK*)(FMOD_DSP_STATE*);
using FMOD_DSP_RESET_CALLBACK   = FMOD_RESULT(PB_F_CALLBACK*)(FMOD_DSP_STATE*);
using FMOD_DSP_READ_CALLBACK    = FMOD_RESULT(PB_F_CALLBACK*)(
    FMOD_DSP_STATE*,
    float*       inbuffer,
    float*       outbuffer,
    unsigned int length,
    int          inchannels,
    int*         outchannels
);
using FMOD_DSP_PROCESS_CALLBACK =
    FMOD_RESULT(PB_F_CALLBACK*)(FMOD_DSP_STATE*, unsigned int, void const*, void*, FMOD_BOOL, int);

// Only the leading fields we set are spelled out; the rest stay zero via aggregate init. The struct is passed to FMOD
// by pointer, so the full 2.02 layout must be present. Unused trailing callbacks are kept as opaque pointers.
struct FMOD_DSP_DESCRIPTION {
    unsigned int              pluginsdkversion;
    char                      name[32];
    unsigned int              version;
    int                       numinputbuffers;
    int                       numoutputbuffers;
    FMOD_DSP_CREATE_CALLBACK  create;
    FMOD_DSP_RELEASE_CALLBACK release;
    FMOD_DSP_RESET_CALLBACK   reset;
    FMOD_DSP_READ_CALLBACK    read;
    FMOD_DSP_PROCESS_CALLBACK process;
    void*                     setposition;
    int                       numparameters;
    void*                     paramdesc;
    void*                     setparameterfloat;
    void*                     setparameterint;
    void*                     setparameterbool;
    void*                     setparameterdata;
    void*                     getparameterfloat;
    void*                     getparameterint;
    void*                     getparameterbool;
    void*                     getparameterdata;
    void*                     shouldiprocess;
    void*                     userdata;
    void*                     sys_register;
    void*                     sys_deregister;
    void*                     sys_mix;
};

} // namespace playback::audio::fmod
