// dllmain.cpp : Defines the entry point for the DLL application.
#include "pch.h"
#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <string>
#include <string_view>
#include <unordered_set>
#include <vector>
#include <shobjidl.h>
#include <safetyhook.hpp>
#include "BlingMenu_public.h"
#include "buildnumber.h"
namespace fs = std::filesystem;

constexpr uint64_t kSelfRadioInitialStartDelayMs = 1500;

HMODULE g_dll_module = nullptr;

enum hud_message_region : __int32
{
    HUD_REGION_DEBUG = 0x0,
    HUD_REGION_HELP = 0x1,
    HUD_REGION_DIVERSION = 0x2,
    HUD_REGION_SUBTITLES = 0x3,
    HUD_REGION_CUTSCENE_HELP = 0x4,
    NUM_HUD_REGIONS = 0x5,
    HUD_REGION_DEFAULT = 0x1,
};


enum hud_message_priority : __int32
{
    HUD_MESSAGE_PRIORITY_LOW = 0x0,
    HUD_MESSAGE_PRIORITY_NORMAL = 0x1,
    HUD_MESSAGE_PRIORITY_HIGH = 0x2,
    HUD_MESSAGE_PRIORITY_CRITICAL = 0x3,
    HUD_MESSAGE_PRIORITY_SUPER_CRITICAL = 0x4,
};

enum game_audio_type : __int32
{
    GAT_ALL = 0xFFFFFFFF,
    GAT_FOLEY = 0x0,
    GAT_VOICE = 0x1,
    GAT_MUSIC = 0x2,
    GAT_AMBIENT = 0x3,
    NUM_GAME_AUDIO_TYPES = 0x4,
};

enum hud_message_sync_flags : __int32
{
    HUD_MESSAGE_SYNC_LOCAL = 0x1,
    HUD_MESSAGE_SYNC_REMOTE = 0x2,
    HUD_MESSAGE_SYNC_ALL = 0x3,
};

struct hud_message_params
{
    float duration;
    float delay;
    float repeat_delay;
    float fade_time;
    hud_message_priority priority;
    hud_message_region region;
    unsigned int group_id;
    bool sound;
    unsigned __int16 audio_id;
    game_audio_type audio_type;
    hud_message_sync_flags sync_flags;
};

hud_message_params Hud_message_CSelfRadio_params =
{
    7.f,0.f,0.f,2.4f,HUD_MESSAGE_PRIORITY_NORMAL,HUD_REGION_DIVERSION,0,false,-1,GAT_MUSIC,HUD_MESSAGE_SYNC_LOCAL
};

class CSelfRadio;

CSelfRadio* Ambient_CSelfRadio;

constexpr char kSelfRadioMenuPath[] = "Self Radio";

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
    static std::string GetLowercaseExtension(const fs::path& path)
    {
        if (!path.has_extension())
            return {};

        std::string ext = path.extension().string();
        std::transform(ext.begin(), ext.end(), ext.begin(), [](unsigned char ch) {
            return static_cast<char>(std::tolower(ch));
        });

        return ext;
    }

    static bool IsShortcutFile(const fs::path& path)
    {
        return GetLowercaseExtension(path) == ".lnk";
    }

    static bool TryResolveShortcutTarget(const fs::path& shortcut_path, fs::path& target_path)
    {
        HRESULT hr = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
        const bool should_uninitialize = SUCCEEDED(hr);
        if (FAILED(hr) && hr != RPC_E_CHANGED_MODE)
            return false;

        IShellLinkW* shell_link = nullptr;
        IPersistFile* persist_file = nullptr;
        wchar_t resolved_path[MAX_PATH]{};
        WIN32_FIND_DATAW find_data{};
        bool resolved = false;

        hr = CoCreateInstance(CLSID_ShellLink, nullptr, CLSCTX_INPROC_SERVER, IID_IShellLinkW, reinterpret_cast<void**>(&shell_link));
        if (FAILED(hr) || !shell_link)
            goto Cleanup;

        hr = shell_link->QueryInterface(IID_IPersistFile, reinterpret_cast<void**>(&persist_file));
        if (FAILED(hr) || !persist_file)
            goto Cleanup;

        hr = persist_file->Load(shortcut_path.c_str(), STGM_READ);
        if (FAILED(hr))
            goto Cleanup;

        hr = shell_link->GetPath(resolved_path, static_cast<int>(std::size(resolved_path)), &find_data, SLGP_RAWPATH);
        if (FAILED(hr) || resolved_path[0] == L'\0')
            goto Cleanup;

        target_path = fs::path(resolved_path);
        resolved = true;

    Cleanup:
        if (persist_file)
            persist_file->Release();
        if (shell_link)
            shell_link->Release();
        if (should_uninitialize)
            CoUninitialize();

        return resolved;
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

        system->set3DSettings(1.0f, 1.0f, 1.0f);
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

            const fs::path source_path = entry.path();
            fs::path song_path = source_path;
            if (IsShortcutFile(source_path) && !TryResolveShortcutTarget(source_path, song_path))
            {
                printf("[SelfRadio] Failed to resolve shortcut: %s\n", source_path.string().c_str());
                continue;
            }

            FMOD::Sound* sound = nullptr;
            const FMOD_RESULT result = m_system->createStream(
                song_path.string().c_str(),
                FMOD_CREATESTREAM | FMOD_LOOP_OFF,
                nullptr,
                &sound);

            if (result != FMOD_OK || !sound)
                continue;

            SelfRadioSong song{};
            song.name = MakeSongName(source_path);
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
bool g_self_radio_enable_3d = true;
bool g_self_radio_force_2d = false;
bool g_self_radio_force_3d = false;
bool g_self_radio_use_velocity = false;
bool g_self_radio_use_linear_rolloff = false;
float g_self_radio_min_distance = 2.0f;
float g_self_radio_max_distance = 45.0f;
float g_self_radio_3d_level = 1.0f;
float g_self_radio_3d_spread = 0.0f;
float g_self_radio_doppler_scale = 0.0f;
float g_self_radio_rolloff_scale = 1.0f;
uintptr_t g_debug_active_vehicle = 0;
bool g_debug_active_playing = false;
bool g_debug_active_is_2d = true;
int g_debug_active_track = -1;
float g_debug_emitter_x = 0.0f;
float g_debug_emitter_y = 0.0f;
float g_debug_emitter_z = 0.0f;
float g_debug_emitter_vx = 0.0f;
float g_debug_emitter_vy = 0.0f;
float g_debug_emitter_vz = 0.0f;
float g_debug_listener_x = 0.0f;
float g_debug_listener_y = 0.0f;
float g_debug_listener_z = 0.0f;
float g_debug_listener_vx = 0.0f;
float g_debug_listener_vy = 0.0f;
float g_debug_listener_vz = 0.0f;

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

FMOD_VECTOR ambient_get_pos(uintptr_t obj)
{
    FMOD_VECTOR pos{};
    if (obj)
    {
        pos.x = *(float*)(obj + 0x8);
        pos.y = *(float*)(obj + 0x10);
        pos.z = *(float*)(obj + 0xC);
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
        // idk if this is needed
        uint8_t is_ambient : 1;
        uint8_t reserved : 2;
    } flags{};

    // atm if 2D, this usually means player can control the track, skip and so on.
    bool isPlayerControlled()
    {
        return flags.is_2d && flags.object_alive;
    }

    uint32_t vehicle_handle = 0;

    int current_track_index = -1;
    uint32_t playback_seed = 0;

    uint64_t start_at_ms = 0;         // delayed start after station switch
    uint64_t track_started_at_ms = 0; // when current track began
    uint32_t seek_ms = 0;             // current seek position if resumed
    uintptr_t object = 0;
    uint64_t last_runtime_update_ms = 0;

    float volume_scale = 1.0f;        // final effective scalar you send to FMOD
    float user_lpf = 0.0f;            // if you want to mirror muffling/submersion

    FMOD_VECTOR object_pos{};
    FMOD_VECTOR object_vel{};
    FMOD_VECTOR previous_object_pos{};

    FMOD::Channel* channel = nullptr;
    FMOD::Sound* current_sound = nullptr;

    void Reset(bool stop_audio = false)
    {
        if (stop_audio && channel) {
            channel->stop();
        }

        bool was_ambient = flags.is_ambient;

        flags = {};

        if (was_ambient) {
            flags.is_ambient = 1;
        }

        vehicle_handle = 0;
        current_track_index = -1;
        playback_seed = 0;
        start_at_ms = 0;
        track_started_at_ms = 0;
        seek_ms = 0;
        object = 0;
        last_runtime_update_ms = 0;
        volume_scale = 1.0f;
        user_lpf = 0.0f;
        object_pos = {};
        object_vel = {};
        previous_object_pos = {};
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

double __declspec(naked) vehicle_audio_find_lpf_level(uintptr_t vehicle, int type_audio, int object_is_in_vehicle) {
    static const DWORD vehicle_audio_find_lpf_level_addr = 0x483B00;
    __asm {
        push ebp
        mov ebp, esp
        sub esp, __LOCAL_SIZE
        
        push eax

        mov eax, type_audio
        push object_is_in_vehicle
        push vehicle

        call vehicle_audio_find_lpf_level_addr

        pop eax

        mov esp, ebp
        pop ebp
        ret
    }
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

static FMOD_VECTOR fmod_vector_sub(const FMOD_VECTOR& lhs, const FMOD_VECTOR& rhs)
{
    FMOD_VECTOR result{};
    result.x = lhs.x - rhs.x;
    result.y = lhs.y - rhs.y;
    result.z = lhs.z - rhs.z;
    return result;
}

static FMOD_VECTOR fmod_vector_scale(const FMOD_VECTOR& value, float scalar)
{
    FMOD_VECTOR result{};
    result.x = value.x * scalar;
    result.y = value.y * scalar;
    result.z = value.z * scalar;
    return result;
}

static void self_radio_update_debug_from_csr(const CSelfRadio* csr)
{
    if (!csr)
        return;

    g_debug_active_vehicle = csr->object;
    g_debug_active_playing = csr->flags.is_playing != 0;
    g_debug_active_is_2d = csr->flags.is_2d != 0;
    g_debug_active_track = csr->current_track_index;
    g_debug_emitter_x = csr->object_pos.x;
    g_debug_emitter_y = csr->object_pos.y;
    g_debug_emitter_z = csr->object_pos.z;
    g_debug_emitter_vx = csr->object_vel.x;
    g_debug_emitter_vy = csr->object_vel.y;
    g_debug_emitter_vz = csr->object_vel.z;
}

uintptr_t sub_935B80;
void self_radio_refresh_runtime_state(uintptr_t vehicle, CSelfRadio* csr)
{
    if (!csr)
        return;

    const uint64_t now_ms = self_radio_now_ms();
    const FMOD_VECTOR new_pos = object_get_pos(vehicle);
    if (g_self_radio_use_velocity && csr->last_runtime_update_ms != 0 && now_ms > csr->last_runtime_update_ms)
    {
        const float delta_seconds = static_cast<float>(now_ms - csr->last_runtime_update_ms) / 1000.0f;
        if (delta_seconds > 0.0f)
            csr->object_vel = fmod_vector_scale(fmod_vector_sub(new_pos, csr->previous_object_pos), 1.0f / delta_seconds);
    }
    else
    {
        csr->object_vel = {};
    }

    csr->object_pos = new_pos;
    csr->previous_object_pos = new_pos;
    csr->last_runtime_update_ms = now_ms;

    bool is_2d = radio_tuner_should_be_2d_for_vehicle(vehicle) != 0;
    if (!g_self_radio_enable_3d || g_self_radio_force_2d)
        is_2d = true;
    else if (g_self_radio_force_3d)
        is_2d = false;

    csr->flags.is_2d = is_2d;
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
        channel->setMode(FMOD_3D | (g_self_radio_use_linear_rolloff ? FMOD_3D_LINEARROLLOFF : FMOD_3D_INVERSEROLLOFF));
        channel->set3DLevel(g_self_radio_3d_level);
        channel->set3DSpread(g_self_radio_3d_spread);
        channel->set3DAttributes(&csr->object_pos, &csr->object_vel);
        channel->set3DMinMaxDistance(g_self_radio_min_distance, g_self_radio_max_distance);
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
    if (g_debug_active_vehicle == csr->object)
        g_debug_active_playing = false;

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
    self_radio_update_debug_from_csr(csr);

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
            csr->channel->setMode(FMOD_3D | (g_self_radio_use_linear_rolloff ? FMOD_3D_LINEARROLLOFF : FMOD_3D_INVERSEROLLOFF));
            csr->channel->set3DLevel(g_self_radio_3d_level);
            csr->channel->set3DSpread(g_self_radio_3d_spread);
            csr->channel->set3DAttributes(&csr->object_pos, &csr->object_vel);
            csr->channel->set3DMinMaxDistance(g_self_radio_min_distance, g_self_radio_max_distance);
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

void Update_Ambient_CSelfRadio() {
    if (!Ambient_CSelfRadio)
        return;
    uintptr_t ambient = *(uintptr_t*)0x2574358;
    uintptr_t emitter = *(uintptr_t*)0x257435C;
    if (!emitter || !ambient) {
        Ambient_CSelfRadio->Reset(true)
;        return;
    }

    FMOD_VECTOR crib_radio_pos = ambient_get_pos(ambient);
    //printf("%f %f %f\n", crib_radio_pos.x, crib_radio_pos.y, crib_radio_pos.z);
    radio_inst* radio = *(radio_inst**)(emitter + 0x8);
    if (!radio) {
        Ambient_CSelfRadio->Reset(true);
        return;
    }



}

void gameplay_loop()
{
    cdecl_call(sub_935B80);
    Update_Ambient_CSelfRadio();
    g_cached_game_volume = get_game_volume();

    for (CSelfRadio* csr : g_self_radios)
    {
        if (!csr || !csr->channel)
            continue;

        csr->channel->setVolume(g_cached_game_volume);
    }

    if (auto* system = g_self_radio_song_library.GetSystem())
    {
        static FMOD_VECTOR last_listener_pos{};
        static uint64_t last_listener_update_ms = 0;
        const uint64_t now_ms = self_radio_now_ms();
        const FMOD_VECTOR listener_pos = player_get_pos();
        FMOD_VECTOR listener_vel{};
        if (g_self_radio_use_velocity && last_listener_update_ms != 0 && now_ms > last_listener_update_ms)
        {
            const float delta_seconds = static_cast<float>(now_ms - last_listener_update_ms) / 1000.0f;
            if (delta_seconds > 0.0f)
                listener_vel = fmod_vector_scale(fmod_vector_sub(listener_pos, last_listener_pos), 1.0f / delta_seconds);
        }

        float* matrix = (float*)0x025F5B38;
        FMOD_VECTOR listener_forward = { matrix[6], matrix[7], matrix[8] };
        FMOD_VECTOR listener_up = { matrix[3], matrix[4], matrix[5] };
        system->set3DSettings(g_self_radio_doppler_scale, 1.0f, g_self_radio_rolloff_scale);
        system->set3DListenerAttributes(0, &listener_pos, &listener_vel, &listener_forward, &listener_up);
        g_debug_listener_x = listener_pos.x;
        g_debug_listener_y = listener_pos.y;
        g_debug_listener_z = listener_pos.z;
        g_debug_listener_vx = listener_vel.x;
        g_debug_listener_vy = listener_vel.y;
        g_debug_listener_vz = listener_vel.z;
        last_listener_pos = listener_pos;
        last_listener_update_ms = now_ms;
        system->update();
    }
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

    csr->user_lpf = vehicle_audio_find_lpf_level(vehicle, 4, 1);


}

uintptr_t sub_9551F0;

void BlingMenuOptions() {
    if (BlingMenuLoad()) {
        BlingMenuAddCategory(kSelfRadioMenuPath);
        BlingMenuAddBool(kSelfRadioMenuPath, "Enable 3D", &g_self_radio_enable_3d, nullptr);
        BlingMenuAddBool(kSelfRadioMenuPath, "Force 2D", &g_self_radio_force_2d, nullptr);
        BlingMenuAddBool(kSelfRadioMenuPath, "Force 3D", &g_self_radio_force_3d, nullptr);
        BlingMenuAddBool(kSelfRadioMenuPath, "Use Velocity", &g_self_radio_use_velocity, nullptr);
        BlingMenuAddBool(kSelfRadioMenuPath, "Linear Rolloff", &g_self_radio_use_linear_rolloff, nullptr);
        BlingMenuAddFloat(kSelfRadioMenuPath, "3D Min Distance", &g_self_radio_min_distance, nullptr, 0.5f, 0.0f, 200.0f);
        BlingMenuAddFloat(kSelfRadioMenuPath, "3D Max Distance", &g_self_radio_max_distance, nullptr, 1.0f, 1.0f, 1000.0f);
        BlingMenuAddFloat(kSelfRadioMenuPath, "3D Level", &g_self_radio_3d_level, nullptr, 0.05f, 0.0f, 1.0f);
        BlingMenuAddFloat(kSelfRadioMenuPath, "3D Spread", &g_self_radio_3d_spread, nullptr, 1.0f, 0.0f, 360.0f);
        BlingMenuAddFloat(kSelfRadioMenuPath, "Doppler Scale", &g_self_radio_doppler_scale, nullptr, 0.05f, 0.0f, 5.0f);
        BlingMenuAddFloat(kSelfRadioMenuPath, "Rolloff Scale", &g_self_radio_rolloff_scale, nullptr, 0.05f, 0.0f, 5.0f);
        BlingMenuAddFuncStd(kSelfRadioMenuPath, "Reload Songs", []() {
            self_radio_reload_songs();
        });
        BlingMenuAddFuncStd(kSelfRadioMenuPath, "Print Debug", []() {
            printf("[SelfRadio] active_vehicle=%p playing=%d is_2d=%d track=%d\n",
                (void*)g_debug_active_vehicle, g_debug_active_playing, g_debug_active_is_2d, g_debug_active_track);
            printf("[SelfRadio] emitter_pos=(%.2f, %.2f, %.2f) emitter_vel=(%.2f, %.2f, %.2f)\n",
                g_debug_emitter_x, g_debug_emitter_y, g_debug_emitter_z,
                g_debug_emitter_vx, g_debug_emitter_vy, g_debug_emitter_vz);
            printf("[SelfRadio] listener_pos=(%.2f, %.2f, %.2f) listener_vel=(%.2f, %.2f, %.2f)\n",
                g_debug_listener_x, g_debug_listener_y, g_debug_listener_z,
                g_debug_listener_vx, g_debug_listener_vy, g_debug_listener_vz);
        });
        BlingMenuAddFuncCustomStd(kSelfRadioMenuPath, "Status", [](int) -> const char* {
            static char buffer[256];
            std::snprintf(buffer, sizeof(buffer), "veh=%p playing=%d 2d=%d track=%d",
                (void*)g_debug_active_vehicle, g_debug_active_playing, g_debug_active_is_2d, g_debug_active_track);
            return buffer;
        }, []() {});
        BlingMenuAddFuncCustomStd(kSelfRadioMenuPath, "Emitter Pos", [](int) -> const char* {
            static char buffer[256];
            std::snprintf(buffer, sizeof(buffer), "(%.2f, %.2f, %.2f)", g_debug_emitter_x, g_debug_emitter_y, g_debug_emitter_z);
            return buffer;
        }, []() {});
        BlingMenuAddFuncCustomStd(kSelfRadioMenuPath, "Emitter Vel", [](int) -> const char* {
            static char buffer[256];
            std::snprintf(buffer, sizeof(buffer), "(%.2f, %.2f, %.2f)", g_debug_emitter_vx, g_debug_emitter_vy, g_debug_emitter_vz);
            return buffer;
        }, []() {});
        BlingMenuAddFuncCustomStd(kSelfRadioMenuPath, "Listener Pos", [](int) -> const char* {
            static char buffer[256];
            std::snprintf(buffer, sizeof(buffer), "(%.2f, %.2f, %.2f)", g_debug_listener_x, g_debug_listener_y, g_debug_listener_z);
            return buffer;
        }, []() {});
        BlingMenuAddFuncCustomStd(kSelfRadioMenuPath, "Listener Vel", [](int) -> const char* {
            static char buffer[256];
            std::snprintf(buffer, sizeof(buffer), "(%.2f, %.2f, %.2f)", g_debug_listener_vx, g_debug_listener_vy, g_debug_listener_vz);
            return buffer;
        }, []() {});

        BlingMenuAddFunc(kSelfRadioMenuPath, "version r" BUILD_NUMBER_STR, NULL);
        BlingMenuAddFunc(kSelfRadioMenuPath, "commit " COMMIT_HASH, NULL);
        BlingMenuAddFunc(kSelfRadioMenuPath, BUILD_TIME_UTC, NULL);

    }
}

void late_init()
{
    cdecl_call(sub_9551F0);
    Ambient_CSelfRadio = new CSelfRadio();
    Ambient_CSelfRadio->flags.is_ambient = 1;
    self_radio_register(Ambient_CSelfRadio);
    self_radio_init();

    BlingMenuOptions();

    self_radio_Station = thiscall_call<int>(0x4904F0, "SELF RADIO");
    printf("SELF RAIDO1!! %d\n\n\n\n\n\n\n\n\n\n\n\n\n\n", self_radio_Station);
}

uintptr_t object_free_this_addr;
SafetyHookInline vehicle_free_thisD;
SafetyHookInline vehicle_delete_thisD;
void FreeCSRadio(uintptr_t obj) 
{
    auto CSRadio = vehicle_get_selfradio(obj);
    if (CSRadio) {
        CSRadio->Reset(true);
        CSRadio->flags.object_alive = 0;
    }
}

void __fastcall object_delete_this_hook(uintptr_t obj, void*, int unknown) {
    FreeCSRadio(obj);
    vehicle_delete_thisD.unsafe_thiscall(obj,unknown);
}

void __fastcall object_free_this_hook(uintptr_t obj) {
    FreeCSRadio(obj);
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
    vehicle_delete_thisD = safetyhook::create_inline(0xAA4490, object_delete_this_hook);

    static auto vehicle_kill_audio_hook = safetyhook::create_mid(0x483570, [](SafetyHookContext& ctx) {
        FreeCSRadio(ctx.eax);
        });

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

