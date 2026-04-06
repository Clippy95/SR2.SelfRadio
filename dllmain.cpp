// dllmain.cpp : Defines the entry point for the DLL application.
#include "pch.h"

void set_uint(uintptr_t ptr, uintptr_t addr_lo, uintptr_t addr_hi)
{
    const uint32_t value = static_cast<uint32_t>(ptr);

    *reinterpret_cast<uint16_t*>(addr_lo) = static_cast<uint16_t>(value & 0xFFFF);
    *reinterpret_cast<uint16_t*>(addr_hi) = static_cast<uint16_t>((value >> 16) & 0xFFFF);
}

uintptr_t get_uint(uintptr_t addr_lo, uintptr_t addr_hi)
{
    const uint32_t lo = *reinterpret_cast<uint16_t*>(addr_lo);
    const uint32_t hi = *reinterpret_cast<uint16_t*>(addr_hi);

    return static_cast<uintptr_t>(lo | (hi << 16));
}

BYTE havok_pasued() {
    return (*(BYTE*)0x2526D28 || *(BYTE*)0x2527CB6);
}

const FMOD_VECTOR* object_get_pos(uintptr_t* obj) {
    if (obj) {

        // idk
    //    FMOD_VECTOR obj_pos = {
    //*(float*)obj + 0x14,  // X
    //*(float*)obj + 0x1C,  // Y
    //*(float*)obj + 0x18   // Z
    //    };

        return (FMOD_VECTOR*)(obj + 0x14);
    }
    return nullptr; // return default if null
}

FMOD_VECTOR player_get_pos() {
    FMOD_VECTOR playerPos = {
    *(float*)0x25F5BB4,  // X
    *(float*)0x25F5BBC,  // Y
    *(float*)0x25F5BB8   // Z
    };
    return playerPos;
}

class CSelfRadio {
public:
    struct Flags {
        uint8_t is_playing : 1;
        uint8_t object_alive : 1;
        uint8_t is_2d : 1;
        uint8_t pending_start : 1;
        uint8_t pending_stop : 1;
        uint8_t reserved : 3;
    } flags{};

    uint32_t vehicle_handle = 0;

    int current_track_index = -1;
    uint32_t playback_seed = 0;

    uint64_t start_at_ms = 0;         // delayed start after station switch
    uint64_t track_started_at_ms = 0; // when current track began
    uint32_t seek_ms = 0;             // current seek position if resumed

    float volume_scale = 1.0f;        // final effective scalar you send to FMOD
    float user_lpf = 0.0f;            // if you want to mirror muffling/submersion

    FMOD_VECTOR object_pos{};
    FMOD_VECTOR object_vel{};

    FMOD::Channel* channel = nullptr;
    FMOD::Sound* current_sound = nullptr;
};


struct play_inst
{
    unsigned int instance;
    unsigned int anim_inst;
    int emotion_handle;
};


struct __declspec(align(4)) radio_inst
{
    unsigned __int16 station;
    unsigned __int16 last_station;
    unsigned __int16 play_index;
    unsigned __int16 processed_frame;
    play_inst station_inst;
    play_inst overlap_inst;
    play_inst noise_inst;
    play_inst station_switch;
    float owner_volume_scale;
    float player_volume_scale;
    float overlap_volume_scale;
    float noise_volume_scale;
    float noise_volume_level;
    struct rflags
    {
        unsigned __int8 m_is_blocked : 1;
        unsigned __int8 m_disallow_block : 1;
        unsigned __int8 m_disallow_unblock : 1;
        unsigned __int8 m_play_track : 1;
        unsigned __int8 m_play_ovrlp : 1;
        unsigned __int8 m_was_in_range : 1;
        unsigned __int8 m_forced_by_hijack : 1;
    };
    rflags flags;

};


CSelfRadio* vehicle_get_selfradio(uintptr_t vehicle) {

    return (CSelfRadio*)get_uint(vehicle + 0x4A, vehicle + 0xA9);

}

radio_inst* vehicle_get_radio_inst(uintptr_t vehicle) 
{
    if (vehicle) {
        return (radio_inst*)(vehicle + 0x8D24);
    }
    return nullptr;
}


uintptr_t vehicle_construct_og;
uintptr_t __fastcall vehicle_construct(uintptr_t thisa) {
    auto obj = thiscall_call<uintptr_t>(vehicle_construct_og,thisa);
    if (obj) {
        auto test = new CSelfRadio();
        set_uint((uintptr_t)test, obj + 0x4A, obj + 0xA9);
    }
    return obj;
}

bool is_radio_station_self_radio(radio_inst* radioi)
{
    // temp
    const int self_radio_Station = 50;
    if (radioi) {
        radioi->station == self_radio_Station;
    }
    return false;
}

uintptr_t radio_tuner_update_og;
void radio_tuner_update_hook(uintptr_t vehicle) 
{
    cdecl_call<void>(radio_tuner_update_og, vehicle);
    auto CSRadio = vehicle_get_selfradio(vehicle);
    auto radioi = vehicle_get_radio_inst(vehicle);
    if (radioi && CSRadio)
    {
        if (radioi->last_station != radioi->station && is_radio_station_self_radio(radioi)) {
            CSRadio->flags.is_playing = 1;
        }
    }

}

void MainHook()
{
    InterceptCall(0xDB2142, vehicle_construct_og, vehicle_construct);
}

BOOL APIENTRY DllMain( HMODULE hModule,
                       DWORD  ul_reason_for_call,
                       LPVOID lpReserved
                     )
{
    switch (ul_reason_for_call)
    {
    case DLL_PROCESS_ATTACH:

    {
        MainHook();
        HMODULE moduleHandle;
        // idk why but this makes it not DETATCH prematurely
        GetModuleHandleEx(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS, (LPCTSTR)DllMain, &moduleHandle);
        break;
    }

    case DLL_THREAD_ATTACH:
    case DLL_THREAD_DETACH:
    case DLL_PROCESS_DETACH:
        break;
    }
    return TRUE;
}

