// dllmain.cpp : Defines the entry point for the DLL application.
#include "pch.h"
#include <algorithm>
#include <filesystem>
#include <string>
#include <string_view>
#include <unordered_set>
#include <vector>

namespace fs = std::filesystem;

HMODULE g_dll_module = nullptr;

struct SelfRadioSong
{
    std::string name;
    fs::path path;
    FMOD::Sound* sound = nullptr;
};

class SelfRadioSongLibrary
{
public:
    bool Init(HMODULE module)
    {
        m_module = module;

        if (!EnsureFMOD())
            return false;

        ReloadSongs();
        m_initialized = true;
        return true;
    }

    size_t ReloadSongs()
    {
        ReleaseSongs();

        if (!EnsureFMOD())
            return 0;

        std::unordered_set<std::string> seen_dirs;
        LoadSongsFromDirectory(GetExeDirectory() / "SelfRadio", seen_dirs);
        LoadSongsFromDirectory(GetDllDirectoryS() / "SelfRadio", seen_dirs);

        std::sort(m_songs.begin(), m_songs.end(), [](const SelfRadioSong& lhs, const SelfRadioSong& rhs) {
            if (lhs.name != rhs.name)
                return lhs.name < rhs.name;
            return lhs.path.string() < rhs.path.string();
        });

        return m_songs.size();
    }

    void Shutdown()
    {
        ReleaseSongs();

        if (m_system)
        {
            m_system->close();
            m_system->release();
            m_system = nullptr;
        }

        m_initialized = false;
    }

    const std::vector<SelfRadioSong>& GetSongs() const
    {
        return m_songs;
    }

    FMOD::System* GetSystem() const
    {
        return m_system;
    }

    bool IsInitialized() const
    {
        return m_initialized;
    }

private:
    static bool IsMp3File(const fs::path& path)
    {
        if (!path.has_extension())
            return false;

        std::string ext = path.extension().string();
        std::transform(ext.begin(), ext.end(), ext.begin(), [](unsigned char ch) {
            return static_cast<char>(std::tolower(ch));
        });

        return ext == ".mp3";
    }

    static std::string NormalizePathString(const fs::path& path)
    {
        std::string value = path.lexically_normal().string();
        std::transform(value.begin(), value.end(), value.begin(), [](unsigned char ch) {
            return static_cast<char>(std::tolower(ch));
        });
        return value;
    }

    static std::string MakeSongName(const fs::path& path)
    {
        return path.stem().string();
    }

    fs::path GetExeDirectory() const
    {
        char buffer[MAX_PATH]{};
        GetModuleFileNameA(nullptr, buffer, static_cast<DWORD>(std::size(buffer)));
        return fs::path(buffer).parent_path();
    }

    fs::path GetDllDirectoryS() const
    {
        char buffer[MAX_PATH]{};
        GetModuleFileNameA(m_module, buffer, static_cast<DWORD>(std::size(buffer)));
        return fs::path(buffer).parent_path();
    }

    bool EnsureFMOD()
    {
        if (m_system)
            return true;

        FMOD::System* system = nullptr;
        if (FMOD::System_Create(&system) != FMOD_OK || !system)
            return false;

        if (system->init(512, FMOD_INIT_NORMAL, nullptr) != FMOD_OK)
        {
            system->release();
            return false;
        }

        m_system = system;
        return true;
    }

    void LoadSongsFromDirectory(const fs::path& directory, std::unordered_set<std::string>& seen_dirs)
    {
        if (directory.empty() || !fs::exists(directory) || !fs::is_directory(directory))
            return;

        const std::string normalized_dir = NormalizePathString(directory);
        if (!seen_dirs.emplace(normalized_dir).second)
            return;

        for (const fs::directory_entry& entry : fs::directory_iterator(directory))
        {
            if (!entry.is_regular_file())
                continue;

            const fs::path song_path = entry.path();
            if (!IsMp3File(song_path))
                continue;

            FMOD::Sound* sound = nullptr;
            const FMOD_RESULT result = m_system->createStream(
                song_path.string().c_str(),
                FMOD_CREATESTREAM | FMOD_LOOP_OFF,
                nullptr,
                &sound);

            if (result != FMOD_OK || !sound)
                continue;

            SelfRadioSong song{};
            song.name = MakeSongName(song_path);
            song.path = song_path;
            song.sound = sound;
            m_songs.push_back(std::move(song));
        }
    }

    void ReleaseSongs()
    {
        for (SelfRadioSong& song : m_songs)
        {
            if (song.sound)
            {
                song.sound->release();
                song.sound = nullptr;
            }
        }

        m_songs.clear();
    }

private:
    HMODULE m_module = nullptr;
    FMOD::System* m_system = nullptr;
    std::vector<SelfRadioSong> m_songs;
    bool m_initialized = false;
};

SelfRadioSongLibrary g_self_radio_song_library;

bool self_radio_init()
{
    return g_self_radio_song_library.Init(g_dll_module);
}

size_t self_radio_reload_songs()
{
    return g_self_radio_song_library.ReloadSongs();
}

const std::vector<SelfRadioSong>& self_radio_get_songs()
{
    return g_self_radio_song_library.GetSongs();
}

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

BYTE havok_paused() {
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

    void Reset(bool stop_audio = false)
    {
        if (stop_audio && channel) {
            channel->stop();
        }

        flags = {};
        vehicle_handle = 0;
        current_track_index = -1;
        playback_seed = 0;
        start_at_ms = 0;
        track_started_at_ms = 0;
        seek_ms = 0;
        volume_scale = 1.0f;
        user_lpf = 0.0f;
        object_pos = {};
        object_vel = {};
        channel = nullptr;
        current_sound = nullptr;
    }

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
        return radioi->station == self_radio_Station;
    }
    return false;
}

uintptr_t radio_tuner_update_og;

uintptr_t radio_tuner_should_be_2d_for_vehicle_addr = 0x48C500;
int radio_tuner_should_be_2d_for_vehicle(uintptr_t vehicle) {
    int result;
    __asm {
        push esi
        mov esi,vehicle
        call radio_tuner_should_be_2d_for_vehicle_addr
        mov result eax
        pop esi
    }
    return result;
}
bool* game_focus = (bool*)0x252A406;
float get_game_volume()
{
    if (!*game_focus || havok_paused()) {
        return 0.f;
    }

    float gameMusicVol = *(float*)0x00EE34E0 / 4.0f;
    return gameMusicVol;

}



void radio_tuner_update_hook(uintptr_t vehicle) 
{
    cdecl_call<void>(radio_tuner_update_og, vehicle);
    auto CSRadio = vehicle_get_selfradio(vehicle);
    auto radioi = vehicle_get_radio_inst(vehicle);
    if (radioi && CSRadio && CSRadio->flags.object_alive)
    {
        if (radioi->last_station != radioi->station && is_radio_station_self_radio(radioi)) {

        }
    }

}

uintptr_t sub_9551F0;
void late_init()
{
    cdecl_call(sub_9551F0);
    self_radio_init();
}

uintptr_t object_free_this_addr;
void __fastcall object_free_this_hook(uintptr_t obj) {
    auto CSRadio = vehicle_get_selfradio(obj);
    if (CSRadio) {
        CSRadio->Reset(true);
    }
}

void MainHook()
{
    InterceptCall(0xDB2142, vehicle_construct_og, vehicle_construct);
    InterceptCall(0x9551F0, sub_9551F0, late_init);
    InterceptCall(0xAA4FD6, object_free_this_addr, object_free_this_hook);
    InterceptCall(0xAA4FF7, object_free_this_addr, object_free_this_hook);
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
        g_dll_module = hModule;
        MainHook();
        HMODULE moduleHandle;
        // idk why but this makes it not DETATCH prematurely
        GetModuleHandleEx(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS, (LPCTSTR)DllMain, &moduleHandle);
        break;
    }

    case DLL_THREAD_ATTACH:
    case DLL_THREAD_DETACH:
    case DLL_PROCESS_DETACH:
        if (ul_reason_for_call == DLL_PROCESS_DETACH)
            g_self_radio_song_library.Shutdown();
        break;
    }
    return TRUE;
}

