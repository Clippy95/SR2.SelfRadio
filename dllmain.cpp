// dllmain.cpp : Defines the entry point for the DLL application.
#include "pch.h"
#include <algorithm>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <string>
#include <string_view>
#include <unordered_set>
#include <vector>
#include <safetyhook.hpp>
#include "BlingMenu_public.h"
namespace fs = std::filesystem;

constexpr uint64_t kSelfRadioInitialStartDelayMs = 1500;

HMODULE g_dll_module = nullptr;
class CSelfRadio;

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
        printf("[SelfRadio] Reloading songs...\n");
        LoadSongsFromDirectory(GetExeDirectory() / "SelfRadio", seen_dirs);
        LoadSongsFromDirectory(GetDllDirectoryS() / "SelfRadio", seen_dirs);

        std::sort(m_songs.begin(), m_songs.end(), [](const SelfRadioSong& lhs, const SelfRadioSong& rhs) {
            if (lhs.name != rhs.name)
                return lhs.name < rhs.name;
            return lhs.path.string() < rhs.path.string();
        });

        printf("[SelfRadio] Loaded %zu song(s)\n", m_songs.size());
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

        printf("[SelfRadio] Scanning: %s\n", directory.string().c_str());

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
            printf("[SelfRadio] Found song: %s (%s)\n", song.name.c_str(), song.path.string().c_str());
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
std::vector<CSelfRadio*> g_self_radios;

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

static uint64_t self_radio_now_ms()
{
    using namespace std::chrono;
    return duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count();
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

FMOD_VECTOR object_get_pos(uintptr_t obj)
{
    FMOD_VECTOR pos{};
    if (obj)
    {
        pos.x = *(float*)(obj + 0x14);
        pos.y = *(float*)(obj + 0x1C);
        pos.z = *(float*)(obj + 0x18);
    }
    return pos;
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
    uintptr_t object = 0;

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
        object = 0;
        volume_scale = 1.0f;
        user_lpf = 0.0f;
        object_pos = {};
        object_vel = {};
        channel = nullptr;
        current_sound = nullptr;
    }

};

void self_radio_register(CSelfRadio* csr)
{
    if (!csr)
        return;

    if (std::find(g_self_radios.begin(), g_self_radios.end(), csr) == g_self_radios.end())
        g_self_radios.push_back(csr);
}

void self_radio_unregister_dead()
{
    std::erase_if(g_self_radios, [](CSelfRadio* csr) {
        return csr == nullptr || !csr->flags.object_alive;
    });
}


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
        return (radio_inst*)(vehicle + 0x8D38);
    }
    return nullptr;
}


uintptr_t vehicle_construct_og;
uintptr_t __fastcall vehicle_construct(uintptr_t thisa) {
    auto obj = thiscall_call<uintptr_t>(vehicle_construct_og,thisa);
    if (obj) {
        auto test = new CSelfRadio();
        test->object = obj;
        test->flags.object_alive = 1;
        test->playback_seed = static_cast<uint32_t>(obj);
        self_radio_register(test);
        set_uint((uintptr_t)test, obj + 0x4A, obj + 0xA9);
    }
    return obj;
}
int self_radio_Station = -1;
bool is_radio_station_self_radio(radio_inst* radioi)
{
    // temp

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
        mov result, eax
        pop esi
    }
    return result;
}
bool* game_focus = (bool*)0x252A406;
float g_cached_game_volume = 0.0f;
float get_game_volume()
{
    if (*game_focus == false || havok_paused() ) {
        return 0.f;
    }

    float gameMusicVol = *(float*)0x00EE34E0 / 3.0f;
    return gameMusicVol;

}
uintptr_t sub_935B80;
void self_radio_refresh_runtime_state(uintptr_t vehicle, CSelfRadio* csr)
{
    if (!csr)
        return;

    csr->flags.is_2d = radio_tuner_should_be_2d_for_vehicle(vehicle) != 0;
    csr->object_pos = object_get_pos(vehicle);
    csr->volume_scale = 1.0f;
}

bool self_radio_start_playback(uintptr_t vehicle, CSelfRadio* csr, radio_inst* radioi)
{
    auto* system = g_self_radio_song_library.GetSystem();
    const auto& songs = self_radio_get_songs();
    if (!system || songs.empty() || !csr || !radioi)
        return false;

    if (csr->current_track_index < 0 || csr->current_track_index >= static_cast<int>(songs.size()))
        csr->current_track_index = static_cast<int>(csr->playback_seed % songs.size());

    const SelfRadioSong& song = songs[csr->current_track_index];
    if (!song.sound)
        return false;

    FMOD::Channel* channel = nullptr;
    if (system->playSound(song.sound, nullptr, true, &channel) != FMOD_OK || !channel)
        return false;

    // Dynamic state is refreshed from the vehicle every radio update.
    self_radio_refresh_runtime_state(vehicle, csr);

    if (csr->flags.is_2d)
    {
        channel->setMode(FMOD_2D);
    }
    else
    {
        channel->setMode(FMOD_3D);
        channel->set3DAttributes(&csr->object_pos, &csr->object_vel);
        channel->set3DMinMaxDistance(2.0f, 45.0f);
    }

    channel->setVolume(g_cached_game_volume);
    if (csr->seek_ms)
        channel->setPosition(csr->seek_ms, FMOD_TIMEUNIT_MS);
    channel->setPaused(false);

    csr->channel = channel;
    csr->current_sound = song.sound;
    csr->track_started_at_ms = self_radio_now_ms() - csr->seek_ms;
    csr->flags.is_playing = 1;
    csr->flags.pending_start = 0;
    csr->flags.pending_stop = 0;
    return true;
}

void self_radio_stop_playback(CSelfRadio* csr, bool preserve_seek = false)
{
    if (!csr)
        return;

    if (csr->channel)
    {
        if (preserve_seek)
        {
            unsigned int position_ms = 0;
            if (csr->channel->getPosition(&position_ms, FMOD_TIMEUNIT_MS) == FMOD_OK)
                csr->seek_ms = position_ms;
        }

        csr->channel->stop();
    }

    csr->channel = nullptr;
    csr->current_sound = nullptr;
    csr->flags.is_playing = 0;
    csr->flags.pending_start = 0;
    csr->flags.pending_stop = 0;

    if (!preserve_seek)
        csr->seek_ms = 0;
}

void self_radio_update(CSelfRadio* csr, bool switched_to_self_radio = false)
{
    if (!csr || !csr->flags.object_alive || !csr->object)
        return;

    const uintptr_t vehicle = csr->object;
    auto* radioi = vehicle_get_radio_inst(vehicle);
    if (!radioi)
        return;

    const auto& songs = self_radio_get_songs();
    if (songs.empty() || !g_self_radio_song_library.GetSystem())
    {
        self_radio_stop_playback(csr);
        return;
    }

    const bool is_self_station = is_radio_station_self_radio(radioi);
    const uint64_t now_ms = self_radio_now_ms();

    if (!is_self_station)
    {
        if (csr->flags.is_playing || csr->flags.pending_start)
            self_radio_stop_playback(csr, true);
        return;
    }

    // This is per-vehicle, per-update state, not one-time start state.
    self_radio_refresh_runtime_state(vehicle, csr);

    if (csr->current_track_index < 0 || csr->current_track_index >= static_cast<int>(songs.size()))
        csr->current_track_index = static_cast<int>(csr->playback_seed % songs.size());

    if (csr->channel)
    {
        bool channel_playing = false;
        if (csr->channel->isPlaying(&channel_playing) != FMOD_OK)
            channel_playing = false;

        if (!channel_playing)
        {
            csr->channel = nullptr;
            csr->current_sound = nullptr;
            csr->flags.is_playing = 0;
            csr->seek_ms = 0;
            csr->current_track_index = (csr->current_track_index + 1) % static_cast<int>(songs.size());
            csr->flags.pending_start = 1;
            csr->start_at_ms = now_ms;
        }
    }

    if (csr->flags.is_playing && csr->channel)
    {
        if (csr->flags.is_2d)
        {
            csr->channel->setMode(FMOD_2D);
        }
        else
        {
            csr->channel->setMode(FMOD_3D);
            csr->channel->set3DAttributes(&csr->object_pos, &csr->object_vel);
        }

        unsigned int position_ms = 0;
        if (csr->channel->getPosition(&position_ms, FMOD_TIMEUNIT_MS) == FMOD_OK)
            csr->seek_ms = position_ms;
        return;
    }

    if (!csr->flags.pending_start)
    {
        csr->flags.pending_start = 1;
        csr->start_at_ms = now_ms + (switched_to_self_radio ? kSelfRadioInitialStartDelayMs : 0);
    }

    if (csr->flags.pending_start && now_ms >= csr->start_at_ms)
        self_radio_start_playback(vehicle, csr, radioi);
}

void gameplay_loop()
{
    cdecl_call(sub_935B80);
    g_cached_game_volume = get_game_volume();

    for (CSelfRadio* csr : g_self_radios)
    {
        if (!csr || !csr->channel)
            continue;

        csr->channel->setVolume(g_cached_game_volume);
    }

    if (auto* system = g_self_radio_song_library.GetSystem())
        system->update();
}

void radio_tuner_update_hook(uintptr_t vehicle) 
{
    auto radioi_before = vehicle_get_radio_inst(vehicle);
    const bool switched_to_self_radio =
        radioi_before &&
        is_radio_station_self_radio(radioi_before) &&
        radioi_before->last_station != radioi_before->station;

    cdecl_call<void>(radio_tuner_update_og, vehicle);
    auto csr = vehicle_get_selfradio(vehicle);
    if (!csr)
        return;

    csr->flags.object_alive = 1;
    csr->object = vehicle;

    self_radio_update(csr, switched_to_self_radio);

}

uintptr_t sub_9551F0;

void BlingMenuOptions() {
    if (BlingMenuLoad()) {
        
    }
}

void late_init()
{
    cdecl_call(sub_9551F0);
    self_radio_init();

    self_radio_Station = thiscall_call<int>(0x4904F0, "SELF RADIO");
    printf("SELF RAIDO1!! %d\n\n\n\n\n\n\n\n\n\n\n\n\n\n", self_radio_Station);
}

uintptr_t object_free_this_addr;
SafetyHookInline vehicle_free_thisD;

void __fastcall object_free_this_hook(uintptr_t obj) {
    auto CSRadio = vehicle_get_selfradio(obj);
    if (CSRadio) {
        CSRadio->Reset(true);
        CSRadio->flags.object_alive = 0;
    }
    vehicle_free_thisD.unsafe_thiscall(obj);
}



void vehicle_create_callback(uintptr_t obj)
{
    auto CSRadio = vehicle_get_selfradio(obj);
    if (CSRadio) {
        CSRadio->flags.object_alive = 1;
        CSRadio->object = obj;
        CSRadio->playback_seed = static_cast<uint32_t>(obj);
        self_radio_register(CSRadio);
    }
}

void ensure_console() {
    AllocConsole();
    freopen("CONOUT$", "w", stdout);
    freopen("CONOUT$", "w", stderr);
}

void* vehicle_create_callback_addr = vehicle_create_callback;

void MainHook()
{
    ensure_console();
    InterceptCall(0xDB2142, vehicle_construct_og, vehicle_construct);
    InterceptCall(0x5202A2, sub_9551F0, late_init);
    //InterceptCall(0xAA4FD6, object_free_this_addr, object_free_this_hook);
    //InterceptCall(0xAA4FF7, object_free_this_addr, object_free_this_hook);

    vehicle_free_thisD = safetyhook::create_inline(0xAA4A40, object_free_this_hook);

    InterceptCall(0x5205FB, sub_935B80, gameplay_loop);

    InterceptCall(0x489A51, radio_tuner_update_og, radio_tuner_update_hook);

    Patch<void*>((0xAE2B0B + 1), &vehicle_create_callback_addr);
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
        //if (ul_reason_for_call == DLL_PROCESS_DETACH)
        //    g_self_radio_song_library.Shutdown();
        break;
    }
    return TRUE;
}

