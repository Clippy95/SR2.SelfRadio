// dllmain.cpp : Defines the entry point for the DLL application.
#include "pch.h"
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cwctype>
#include <filesystem>
#include <string>
#include <string_view>
#include <unordered_set>
#include <vector>
#include <shobjidl.h>
#include <safetyhook.hpp>
#include "BlingMenu_public.h"
#include "buildnumber.h"
#include <random>
#include "IniReader.h"
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
    2.4f,0.f,0.f,0.4f,HUD_MESSAGE_PRIORITY_NORMAL,HUD_REGION_DEBUG,0,false,-1,GAT_MUSIC,HUD_MESSAGE_SYNC_LOCAL
};


struct radio_flags
{
    unsigned __int16 m_is_police : 1;
    unsigned __int16 m_is_fbi : 1;
    unsigned __int16 m_is_pirate : 1;
    unsigned __int16 m_not_selectable : 1;
    unsigned __int16 m_is_customizable : 1;
    unsigned __int16 m_disabled_in_interface : 1;
    unsigned __int16 m_dont_display_station : 1;
    unsigned __int16 m_track_delayed : 1;
    unsigned __int16 m_track_immediately : 1;
    unsigned __int16 m_ovrlp_delayed : 1;
    unsigned __int16 m_ovrlp_immediately : 1;
    unsigned __int16 m_ovrlp_can_be_updated : 1;
    unsigned __int16 m_ovrlp_is_queued : 1;
    unsigned __int16 m_is_selfradio : 1;
};


// In-memory XML node layout used by the radio table parser.
struct xml_node
{
    const char* name;
    xml_node* next;
    xml_node* elements;
    const char* text;
};
static_assert(sizeof(xml_node) == 0x10);

static void self_radio_add_station(xml_node* table)
{
    if (!table)
        return;

    unsigned int station_count = 0;
    xml_node** tail = &table->elements;
    for (; *tail; tail = &(*tail)->next)
    {
        const auto* entry = *tail;
        if (!entry->name || _stricmp(entry->name, "Station"))
            continue;

        ++station_count;
        for (const auto* field = entry->elements; field; field = field->next)
        {
            if (field->name && !_stricmp(field->name, "Name")
                && field->text && !_stricmp(field->text, "SELF RADIO"))
                return; // Keep an existing entry from an older/custom radio.xtbl.
        }
    }

    if (station_count == 0 || station_count >= 0xFFFF)
        return;

    // The game allocates one entry per station, including the one we add.
    std::vector<bool> used_slots(station_count + 1, false);
    for (const auto* entry = table->elements; entry; entry = entry->next)
    {
        if (!entry->name || _stricmp(entry->name, "Station"))
            continue;

        for (const auto* field = entry->elements; field; field = field->next)
        {
            if (field->name && !_stricmp(field->name, "Slot") && field->text)
            {
                const auto index = std::strtoul(field->text, nullptr, 10);
                if (index < used_slots.size())
                    used_slots[index] = true;
            }
        }
    }

    const auto free_slot = std::find(used_slots.begin(), used_slots.end(), false);
    if (free_slot == used_slots.end())
        return;
    const auto slot_index = static_cast<unsigned int>(free_slot - used_slots.begin());

    // Append before the game counts/allocates it, so all existing IDs stay intact.
    // xml_table_close rewinds its temporary pool; it does not free these nodes.
    static char slot_text[16];
    static xml_node flag{ "Flag", nullptr, nullptr, "Self Radio" };
    static xml_node flags{ "Flags", nullptr, &flag, nullptr };
    static xml_node slot{ "Slot", &flags, nullptr, slot_text };
    static xml_node playlist{ "Playlist", &slot, nullptr, nullptr };
    static xml_node name{ "Name", &playlist, nullptr, "SELF RADIO" };
    static xml_node station{ "Station", nullptr, &name, nullptr };
    std::snprintf(slot_text, sizeof(slot_text), "%u", slot_index);
    station.next = nullptr;
    *tail = &station;
}


int __declspec(naked) hud_message_asm(const wchar_t* message_text, hud_message_params* a2) {
    __asm {
        push ebp
        mov ebp, esp
        sub esp, __LOCAL_SIZE


        mov eax, message_text
        push eax
        mov edi, a2
        push edi

        mov edx, 0x0079CD40
        call edx

        mov esp, ebp
        pop ebp
        ret
    }
}

void hud_message(const wchar_t* message_text, hud_message_params* params)
{
    __asm pushad
    hud_message_asm(message_text, params);
    __asm popad
}

static std::string path_to_utf8_string(const fs::path& path)
{
    const auto utf8 = path.u8string();
    return std::string(utf8.begin(), utf8.end());
}

static std::wstring utf8_to_wide(std::string_view value)
{
    if (value.empty())
        return {};

    int wide_length = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, value.data(), static_cast<int>(value.size()), nullptr, 0);
    if (wide_length <= 0)
        wide_length = MultiByteToWideChar(CP_UTF8, 0, value.data(), static_cast<int>(value.size()), nullptr, 0);
    if (wide_length <= 0)
        return std::wstring(value.begin(), value.end());

    std::wstring wide(static_cast<size_t>(wide_length), L'\0');
    if (MultiByteToWideChar(CP_UTF8, 0, value.data(), static_cast<int>(value.size()), wide.data(), wide_length) <= 0)
        return std::wstring(value.begin(), value.end());

    return wide;
}

static FMOD_RESULT self_radio_create_stream_for_path(FMOD::System* system, const fs::path& path, FMOD_MODE mode, FMOD::Sound** sound)
{
    if (!system || !sound)
        return FMOD_ERR_INVALID_PARAM;

    const std::string utf8_path = path_to_utf8_string(path);
    if (utf8_path.empty())
        return FMOD_ERR_INVALID_PARAM;

    return system->createStream(utf8_path.c_str(), mode, nullptr, sound);
}

bool g_self_radio_show_current_playing = true;
static void self_radio_notify_track(const std::string& name)
{
    if (!g_self_radio_show_current_playing)
        return;
    static std::string last_notified;
    if (name == last_notified)
        return;
    last_notified = name;

    static wchar_t wide_buf[256];
    std::wstring wide = L"Self Radio now playing: ";
    wide += utf8_to_wide(name);
    wcsncpy(wide_buf, wide.c_str(), 255);
    wide_buf[255] = L'\0';

    hud_message(wide_buf, &Hud_message_CSelfRadio_params);
}

class CSelfRadio;

CSelfRadio* Ambient_CSelfRadio;

constexpr char kSelfRadioMenuPath[] = "Self Radio";

struct SelfRadioSong
{
    std::string name;
    fs::path path;
    uint32_t length_ms = 0;
};

static void self_radio_show_exception(const char* context, const char* what)
{
    static char buf[1024];
    std::snprintf(buf, sizeof(buf),
        "SelfRadio encountered an error in: %s\n\n"
        "Error: %s\n\n"
        "Songs will not be loaded from this path.\n"
        "Check that your SelfRadio folder exists and is accessible.",
        context, what);
    MessageBoxA(nullptr, buf, "SelfRadio - Error", MB_OK | MB_ICONERROR | MB_SETFOREGROUND);
}

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

        std::unordered_set<std::wstring> seen_dirs;
        printf("[SelfRadio] Reloading songs...\n");

        try
        {
            if (GetExeDirectory() != GetDllDirectoryS())
                LoadSongsFromDirectory(GetDllDirectoryS() / "SelfRadio", seen_dirs);
        }
        catch (const std::exception& e)
        {
            printf("[SelfRadio] Exception in LoadSongsFromDirectory: %s\n", e.what());
            self_radio_show_exception("LoadSongsFromDirectory", e.what());
        }
        catch (...)
        {
            printf("[SelfRadio] Unknown exception in LoadSongsFromDirectory\n");
            self_radio_show_exception("LoadSongsFromDirectory", "Unknown exception");
        }

        std::sort(m_songs.begin(), m_songs.end(), [](const SelfRadioSong& lhs, const SelfRadioSong& rhs) {
            if (lhs.name != rhs.name)
                return lhs.name < rhs.name;
            return lhs.path.native() < rhs.path.native();
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
    static std::wstring GetLowercaseExtension(const fs::path& path)
    {
        if (!path.has_extension())
            return {};

        std::wstring ext = path.extension().wstring();
        std::transform(ext.begin(), ext.end(), ext.begin(), [](wchar_t ch) {
            return static_cast<wchar_t>(std::towlower(ch));
            });

        return ext;
    }

    static bool IsShortcutFile(const fs::path& path)
    {
        return GetLowercaseExtension(path) == L".lnk";
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

    static std::wstring NormalizePathString(const fs::path& path)
    {
        std::wstring value = path.lexically_normal().native();
        std::transform(value.begin(), value.end(), value.begin(), [](wchar_t ch) {
            return static_cast<wchar_t>(std::towlower(ch));
            });
        return value;
    }

    static std::string MakeSongName(const fs::path& path)
    {
        const auto utf8 = path.stem().u8string();
        return std::string(utf8.begin(), utf8.end());
    }

    static fs::path GetModuleDirectory(HMODULE module)
    {
        std::wstring buffer(MAX_PATH, L'\0');
        for (;;)
        {
            const DWORD copied = GetModuleFileNameW(module, buffer.data(), static_cast<DWORD>(buffer.size()));
            if (copied == 0)
                return {};

            if (copied < buffer.size() - 1)
            {
                buffer.resize(copied);
                return fs::path(buffer).parent_path();
            }

            buffer.resize(buffer.size() * 2);
        }
    }

    static FMOD_RESULT CreateStreamForPath(FMOD::System* system, const fs::path& path, FMOD_MODE mode, FMOD::Sound** sound)
    {
        return self_radio_create_stream_for_path(system, path, mode, sound);
    }

    fs::path GetExeDirectory() const
    {
        return GetModuleDirectory(nullptr);
    }

    fs::path GetDllDirectoryS() const
    {
        return GetModuleDirectory(m_module);
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

    void LoadSongsFromDirectory(const fs::path& directory, std::unordered_set<std::wstring>& seen_dirs)
    {
        try
        {
            if (directory.empty())
                return;

            // Wrap exists/is_directory individually — these can throw on bad paths
            bool dir_exists = false;
            bool is_dir = false;
            try { dir_exists = fs::exists(directory); }
            catch (...) { printf("[SelfRadio] exists() threw for: %s\n", path_to_utf8_string(directory).c_str()); return; }
            try { is_dir = fs::is_directory(directory); }
            catch (...) { printf("[SelfRadio] is_directory() threw for: %s\n", path_to_utf8_string(directory).c_str()); return; }

            if (!dir_exists || !is_dir)
                return;

            const std::wstring normalized_dir = NormalizePathString(directory);
            if (!seen_dirs.emplace(normalized_dir).second)
                return;

            printf("[SelfRadio] Scanning: %s\n", path_to_utf8_string(directory).c_str());

            fs::directory_iterator it;
            try { it = fs::directory_iterator(directory); }
            catch (const std::exception& e)
            {
                printf("[SelfRadio] Failed to open directory '%s': %s\n", path_to_utf8_string(directory).c_str(), e.what());
                return;
            }

            for (const fs::directory_entry& entry : it)
            {
                try
                {
                    if (!entry.is_regular_file())
                        continue;

                    const fs::path source_path = entry.path();
                    fs::path song_path = source_path;
                    if (IsShortcutFile(source_path) && !TryResolveShortcutTarget(source_path, song_path))
                    {
                        printf("[SelfRadio] Failed to resolve shortcut: %s\n", path_to_utf8_string(source_path).c_str());
                        continue;
                    }

                    FMOD::Sound* sound = nullptr;
                    const FMOD_RESULT result = CreateStreamForPath(m_system, song_path, FMOD_CREATESTREAM | FMOD_LOOP_OFF, &sound);

                    if (result != FMOD_OK || !sound)
                        continue;

                    SelfRadioSong song{};
                    song.name = MakeSongName(source_path);
                    song.path = song_path;
                    unsigned int length_ms = 0;
                    if (sound->getLength(&length_ms, FMOD_TIMEUNIT_MS) == FMOD_OK)
                        song.length_ms = length_ms;
                    sound->release();
                    printf("[SelfRadio] Found song: %s (%s)\n", song.name.c_str(), path_to_utf8_string(song.path).c_str());
                    m_songs.push_back(std::move(song));
                }
                catch (const std::exception& e)
                {
                    printf("[SelfRadio] Exception processing entry: %s\n", e.what());
                }
                catch (...)
                {
                    printf("[SelfRadio] Unknown exception processing entry\n");
                }
            }
        }
        catch (const std::exception& e)
        {
            printf("[SelfRadio] Exception in LoadSongsFromDirectory: %s\n", e.what());
            self_radio_show_exception("LoadSongsFromDirectory", e.what());
        }
        catch (...)
        {
            printf("[SelfRadio] Unknown exception in LoadSongsFromDirectory\n");
            self_radio_show_exception("LoadSongsFromDirectory", "Unknown exception");
        }
    }

    void ReleaseSongs()
    {
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
float g_self_radio_volume = 1.f;
bool g_self_radio_enable_3d = true;
bool g_self_radio_can_npc_select = true;
bool g_self_radio_sync_all = false;
bool g_self_radio_random_start = false;
bool g_self_radio_shuffle = false;
bool g_self_radio_force_2d = false;
bool g_self_radio_force_3d = false;
bool g_self_radio_use_velocity = false;
bool g_self_radio_use_linear_rolloff = false;
float g_self_radio_min_distance = 2.0f;
float g_self_radio_max_distance = 45.0f;
float g_self_radio_vehicle_box_extent = 44.0f;
float g_self_radio_ambient_box_extent = 100.0f;
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

struct SelfRadioStationState
{
    bool active = false;
    bool pending_start = false;
    int track_index = -1;
    uint32_t seek_ms = 0;
    uint64_t start_at_ms = 0;
    uint64_t last_update_ms = 0;
};

SelfRadioStationState g_self_radio_station{};
bool g_self_radio_sync_all_prev = false;

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
    uint32_t* audio_paused = (uint32_t*)0x2526B44;
    return (*(BYTE*)0x2526D28 || *(BYTE*)0x2527CB6) || *audio_paused > 0;
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
static std::mt19937 rng{ std::random_device{}() };
static uint32_t self_radio_random_seed()
{

    static std::uniform_int_distribution<uint32_t> dist;
    return dist(rng);
}

static int self_radio_random_track(int current_index, int count)
{
    if (count <= 1)
        return 0;

    std::uniform_int_distribution<int> dist(0, count - 2);

    // Pick from all tracks except current, then offset past it to avoid repeat.
    int idx = dist(rng);
    if (idx >= current_index)
        idx++;
    return idx;
}

static int self_radio_next_track_index(int current_index, size_t count)
{
    const int track_count = static_cast<int>(count);
    if (track_count <= 0)
        return -1;

    if (g_self_radio_shuffle)
        return self_radio_random_track(current_index, track_count);

    if (current_index < 0 || current_index >= track_count)
        return 0;

    return (current_index + 1) % track_count;
}

static void self_radio_station_advance_after_track_end(uint64_t now_ms)
{
    const auto& songs = self_radio_get_songs();
    if (!g_self_radio_station.active || songs.empty())
        return;

    g_self_radio_station.track_index = self_radio_next_track_index(g_self_radio_station.track_index, songs.size());
    g_self_radio_station.seek_ms = 0;
    g_self_radio_station.pending_start = false;
    g_self_radio_station.start_at_ms = now_ms;
    g_self_radio_station.last_update_ms = now_ms;

    if (g_self_radio_station.track_index >= 0)
        self_radio_notify_track(songs[g_self_radio_station.track_index].name);
}

class CSelfRadio {
public:
    struct Flags {
        uint8_t is_playing : 1;
        uint8_t object_alive : 1;
        uint8_t is_2d : 1;
        uint8_t pending_start : 1;
        uint8_t pending_stop : 1;
        uint8_t is_ambient : 1;
        uint8_t reserved : 2;
    } flags{};

    bool isPlayerControlled()
    {
        return (flags.is_2d && flags.object_alive) || flags.is_ambient;
    }

    uint32_t vehicle_handle = 0;

    int current_track_index = -1;
    uint32_t playback_seed = self_radio_random_seed();

    uint64_t start_at_ms = 0;
    uint64_t track_started_at_ms = 0;
    uint32_t seek_ms = 0;
    bool synced_to_station = false;
    uintptr_t object = 0;
    uint64_t last_runtime_update_ms = 0;

    float volume_scale = 1.0f;
    float user_lpf = 0.0f;

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
        if (current_sound) {
            current_sound->release();
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
        synced_to_station = false;
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

std::unordered_map<uintptr_t, CSelfRadio> g_ambient_states;
std::vector<uintptr_t> g_ambient_insertion_order;
size_t g_ambient_eviction_index = 0;

constexpr size_t kAmbientPoolInitial = 64;
constexpr size_t kAmbientPoolMax = 256;

CSelfRadio& ambient_state_get_or_create(uintptr_t ambient_ptr)
{
    auto it = g_ambient_states.find(ambient_ptr);
    if (it != g_ambient_states.end())
        return it->second;

    if (g_ambient_states.size() >= kAmbientPoolMax)
    {
        uintptr_t evict = g_ambient_insertion_order[g_ambient_eviction_index];
        // Stop audio on evicted entry before removing
        g_ambient_states[evict].Reset(true);
        g_ambient_states.erase(evict);
        g_ambient_insertion_order[g_ambient_eviction_index] = ambient_ptr;
        g_ambient_eviction_index = (g_ambient_eviction_index + 1) % kAmbientPoolMax;
    }
    else
    {
        g_ambient_insertion_order.push_back(ambient_ptr);
    }

    CSelfRadio& state = g_ambient_states[ambient_ptr];
    state.flags.is_ambient = 1;
    state.playback_seed = self_radio_random_seed();
    return state;
}

static void ambient_csr_save_and_stop(CSelfRadio& csr)
{
    if (csr.flags.is_playing && csr.channel)
    {
        unsigned int pos = 0;
        if (csr.channel->getPosition(&pos, FMOD_TIMEUNIT_MS) == FMOD_OK)
            csr.seek_ms = pos;
        csr.channel->stop();
        csr.channel = nullptr;
        csr.flags.is_playing = 0;
        csr.flags.pending_start = 0;
    }

    if (csr.current_sound)
    {
        csr.current_sound->release();
        csr.current_sound = nullptr;
    }
}

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
    auto obj = thiscall_call<uintptr_t>(vehicle_construct_og, thisa);
    if (obj) {
        auto test = new CSelfRadio();
        test->object = obj;
        test->flags.object_alive = 1;
        test->playback_seed = self_radio_random_seed();
        self_radio_register(test);
        set_uint((uintptr_t)test, obj + 0x4A, obj + 0xA9);
    }
    return obj;
}
int self_radio_Station = -1;
bool is_radio_station_self_radio(radio_inst* radioi)
{
    if (radioi) {
        return radioi->station == self_radio_Station;
    }
    return false;
}

uintptr_t radio_tuner_update_og;

uintptr_t radio_tuner_should_be_2d_for_vehicle_addr = 0x48C500;
BOOL radio_tuner_should_be_2d_for_vehicle(uintptr_t vehicle) {
    BOOL result;
    __asm {
        push esi
        mov esi, vehicle
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
    if (*game_focus == false || havok_paused()) {
        return 0.f;
    }

    float gameMusicVol = *(float*)0x00EE34E0;

    return gameMusicVol * g_self_radio_volume;

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

static bool self_radio_is_within_listener_box(const FMOD_VECTOR& emitter_pos, float half_extent)
{
    if (half_extent <= 0.0f)
        return true;

    const FMOD_VECTOR listener_pos = player_get_pos();
    return std::fabs(emitter_pos.x - listener_pos.x) <= half_extent
        && std::fabs(emitter_pos.y - listener_pos.y) <= half_extent
        && std::fabs(emitter_pos.z - listener_pos.z) <= half_extent;
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

    if (g_self_radio_sync_all && g_self_radio_station.active && g_self_radio_station.track_index >= 0)
    {
        csr->current_track_index = g_self_radio_station.track_index;
        csr->seek_ms = g_self_radio_station.pending_start ? 0 : g_self_radio_station.seek_ms;
    }
    else if (csr->current_track_index < 0 || csr->current_track_index >= static_cast<int>(songs.size()))
        csr->current_track_index = static_cast<int>(csr->playback_seed % songs.size());

    const SelfRadioSong& song = songs[csr->current_track_index];
    FMOD::Sound* sound = nullptr;
    if (self_radio_create_stream_for_path(system, song.path, FMOD_CREATESTREAM | FMOD_LOOP_OFF, &sound) != FMOD_OK || !sound)
        return false;

    FMOD::Channel* channel = nullptr;
    if (system->playSound(sound, nullptr, true, &channel) != FMOD_OK || !channel)
    {
        sound->release();
        return false;
    }

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
    csr->current_sound = sound;
    csr->track_started_at_ms = self_radio_now_ms() - csr->seek_ms;
    csr->flags.is_playing = 1;
    csr->flags.pending_start = 0;
    csr->flags.pending_stop = 0;
    csr->synced_to_station = g_self_radio_sync_all && g_self_radio_station.active;
    if (csr->flags.is_2d)
        self_radio_notify_track(song.name);
    return true;
}

// Variant for ambient: always 3D, uses an explicit world position instead of object_get_pos().
bool self_radio_start_playback_ambient(CSelfRadio* csr, const FMOD_VECTOR& world_pos)
{
    auto* system = g_self_radio_song_library.GetSystem();
    const auto& songs = self_radio_get_songs();
    if (!system || songs.empty() || !csr)
        return false;

    if (g_self_radio_sync_all && g_self_radio_station.active && g_self_radio_station.track_index >= 0)
    {
        csr->current_track_index = g_self_radio_station.track_index;
        csr->seek_ms = g_self_radio_station.pending_start ? 0 : g_self_radio_station.seek_ms;
    }
    else if (csr->current_track_index < 0 || csr->current_track_index >= static_cast<int>(songs.size()))
        csr->current_track_index = static_cast<int>(csr->playback_seed % songs.size());

    const SelfRadioSong& song = songs[csr->current_track_index];
    FMOD::Sound* sound = nullptr;
    if (self_radio_create_stream_for_path(system, song.path, FMOD_CREATESTREAM | FMOD_LOOP_OFF, &sound) != FMOD_OK || !sound)
        return false;

    FMOD::Channel* channel = nullptr;
    if (system->playSound(sound, nullptr, true, &channel) != FMOD_OK || !channel)
    {
        sound->release();
        return false;
    }

    // Ambient crib radio is always 3D.
    const FMOD_VECTOR zero_vel{};
    channel->setMode(FMOD_3D | (g_self_radio_use_linear_rolloff ? FMOD_3D_LINEARROLLOFF : FMOD_3D_INVERSEROLLOFF));
    channel->set3DLevel(g_self_radio_3d_level);
    channel->set3DSpread(g_self_radio_3d_spread);
    channel->set3DAttributes(&world_pos, &zero_vel);
    channel->set3DMinMaxDistance(g_self_radio_min_distance, g_self_radio_max_distance);

    channel->setVolume(g_cached_game_volume);
    if (csr->seek_ms)
        channel->setPosition(csr->seek_ms, FMOD_TIMEUNIT_MS);
    channel->setPaused(false);

    csr->object_pos = world_pos;
    csr->object_vel = zero_vel;
    csr->channel = channel;
    csr->current_sound = sound;
    csr->track_started_at_ms = self_radio_now_ms() - csr->seek_ms;
    csr->flags.is_playing = 1;
    csr->flags.is_2d = 0; // ambient is always 3D
    csr->flags.pending_start = 0;
    csr->flags.pending_stop = 0;
    csr->synced_to_station = g_self_radio_sync_all && g_self_radio_station.active;
    if (!csr->flags.is_2d)
        self_radio_notify_track(song.name);
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
    if (csr->current_sound)
    {
        csr->current_sound->release();
        csr->current_sound = nullptr;
    }
    csr->flags.is_playing = 0;
    csr->flags.pending_start = 0;
    csr->flags.pending_stop = 0;
    if (g_debug_active_vehicle == csr->object)
        g_debug_active_playing = false;

    if (!preserve_seek)
        csr->seek_ms = 0;
}

static uint32_t self_radio_get_track_length_ms(int track_index)
{
    const auto& songs = self_radio_get_songs();
    if (track_index < 0 || track_index >= static_cast<int>(songs.size()))
        return 0;

    return songs[track_index].length_ms;
}

static bool self_radio_is_station_candidate(const CSelfRadio* csr)
{
    if (!csr)
        return false;

    if (csr->flags.is_ambient)
        return (csr->flags.is_playing && csr->channel) || csr->flags.pending_start;

    if (!csr->flags.object_alive || !csr->object)
        return false;

    radio_inst* radioi = vehicle_get_radio_inst(csr->object);
    if (!radioi || !is_radio_station_self_radio(radioi))
        return false;

    return (csr->flags.is_playing && csr->channel) || csr->flags.pending_start;
}

static CSelfRadio* self_radio_find_station_source()
{
    CSelfRadio* preferred_2d = nullptr;
    CSelfRadio* any_playing = nullptr;
    CSelfRadio* any_pending = nullptr;

    auto consider = [&](CSelfRadio* csr)
        {
            if (!self_radio_is_station_candidate(csr))
                return;

            if (csr->flags.is_playing && csr->channel)
            {
                if (csr->flags.is_2d && !preferred_2d)
                    preferred_2d = csr;

                if (!any_playing)
                    any_playing = csr;
            }
            else if (!any_pending)
            {
                any_pending = csr;
            }
        };

    for (CSelfRadio* csr : g_self_radios)
        consider(csr);

    for (auto& [ptr, csr] : g_ambient_states)
        consider(&csr);

    if (preferred_2d)
        return preferred_2d;
    if (any_playing)
        return any_playing;
    return any_pending;
}

static void self_radio_station_clear()
{
    g_self_radio_station = {};
}

static void self_radio_station_bootstrap_from(CSelfRadio* source)
{
    if (!source || source->current_track_index < 0)
        return;

    g_self_radio_station.active = true;
    g_self_radio_station.track_index = source->current_track_index;
    g_self_radio_station.seek_ms = source->seek_ms;
    if (source->flags.is_playing && source->channel)
    {
        unsigned int live_pos_ms = 0;
        if (source->channel->getPosition(&live_pos_ms, FMOD_TIMEUNIT_MS) == FMOD_OK)
            g_self_radio_station.seek_ms = live_pos_ms;
    }
    g_self_radio_station.pending_start = !(source->flags.is_playing && source->channel) && source->flags.pending_start;
    g_self_radio_station.start_at_ms = g_self_radio_station.pending_start ? source->start_at_ms : self_radio_now_ms();
    g_self_radio_station.last_update_ms = self_radio_now_ms();

    if (!g_self_radio_station.pending_start)
    {
        const auto& songs = self_radio_get_songs();
        if (g_self_radio_station.track_index >= 0 && g_self_radio_station.track_index < static_cast<int>(songs.size()))
            self_radio_notify_track(songs[g_self_radio_station.track_index].name);
    }
}

static void self_radio_station_update()
{
    const uint64_t now_ms = self_radio_now_ms();
    const bool paused = havok_paused();

    if (!g_self_radio_sync_all)
    {
        self_radio_station_clear();
        g_self_radio_sync_all_prev = false;
        return;
    }

    if (!g_self_radio_sync_all_prev)
    {
        self_radio_station_clear();
        if (CSelfRadio* source = self_radio_find_station_source())
            self_radio_station_bootstrap_from(source);
    }
    else if (!g_self_radio_station.active)
    {
        if (CSelfRadio* source = self_radio_find_station_source())
            self_radio_station_bootstrap_from(source);
    }

    g_self_radio_sync_all_prev = true;

    if (!g_self_radio_station.active)
        return;

    if (g_self_radio_station.pending_start)
    {
        if (now_ms < g_self_radio_station.start_at_ms)
            return;

        g_self_radio_station.pending_start = false;
        g_self_radio_station.seek_ms = 0;
        g_self_radio_station.last_update_ms = now_ms;

        const auto& songs = self_radio_get_songs();
        if (g_self_radio_station.track_index >= 0 && g_self_radio_station.track_index < static_cast<int>(songs.size()))
            self_radio_notify_track(songs[g_self_radio_station.track_index].name);
        return;
    }

    if (paused)
    {
        g_self_radio_station.last_update_ms = now_ms;
        return;
    }

    if (g_self_radio_station.last_update_ms == 0)
        g_self_radio_station.last_update_ms = now_ms;

    const uint64_t delta_ms = now_ms - g_self_radio_station.last_update_ms;
    g_self_radio_station.last_update_ms = now_ms;
    g_self_radio_station.seek_ms += static_cast<uint32_t>(delta_ms);

    const auto& songs = self_radio_get_songs();
    while (g_self_radio_station.active && !songs.empty())
    {
        const uint32_t length_ms = self_radio_get_track_length_ms(g_self_radio_station.track_index);
        if (length_ms == 0 || g_self_radio_station.seek_ms < length_ms)
            break;

        g_self_radio_station.seek_ms -= length_ms;
        g_self_radio_station.track_index = self_radio_next_track_index(g_self_radio_station.track_index, songs.size());
        self_radio_notify_track(songs[g_self_radio_station.track_index].name);
    }
}

static void self_radio_station_apply_to(CSelfRadio* csr, uint64_t now_ms)
{
    if (!g_self_radio_sync_all || !g_self_radio_station.active || !csr)
        return;

    if (g_self_radio_station.track_index < 0)
        return;

    if (g_self_radio_station.pending_start)
    {
        if (csr->channel)
            self_radio_stop_playback(csr, false);

        csr->current_track_index = g_self_radio_station.track_index;
        csr->seek_ms = 0;
        csr->flags.pending_start = 1;
        csr->start_at_ms = g_self_radio_station.start_at_ms;
        csr->synced_to_station = true;
        return;
    }

    const int previous_track_index = csr->current_track_index;
    csr->current_track_index = g_self_radio_station.track_index;
    csr->seek_ms = g_self_radio_station.seek_ms;

    if (csr->flags.is_playing && csr->channel)
    {
        if (!csr->synced_to_station || previous_track_index != g_self_radio_station.track_index)
        {
            self_radio_stop_playback(csr, false);
            csr->flags.pending_start = 1;
            csr->start_at_ms = now_ms;
        }
    }
    else
    {
        csr->flags.pending_start = 1;
        csr->start_at_ms = now_ms;
    }

    csr->synced_to_station = true;
}

static uint32_t self_radio_random_start_seek(uint32_t length_ms)
{
    if (length_ms == 0)
        return 0;


    static std::uniform_real_distribution<float> dist(0.0f, 1.0f);

    // Take the minimum of two uniform samples — biases toward the first half.
    const float a = dist(rng);
    const float b = dist(rng);
    const float t = min(a, b);

    return static_cast<uint32_t>(t * static_cast<float>(length_ms));
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
        csr->synced_to_station = false;
        return;
    }

    self_radio_refresh_runtime_state(vehicle, csr);
    self_radio_update_debug_from_csr(csr);

    const bool within_listener_box = csr->flags.is_2d || self_radio_is_within_listener_box(csr->object_pos, g_self_radio_vehicle_box_extent);
    if (!within_listener_box)
    {
        if (csr->flags.is_playing || csr->channel)
            self_radio_stop_playback(csr, true);
        else
            csr->flags.pending_start = 0;
        return;
    }

    if (g_self_radio_sync_all && g_self_radio_station.active)
        self_radio_station_apply_to(csr, now_ms);
    else
    {
        csr->synced_to_station = false;
        if (csr->current_track_index < 0 || csr->current_track_index >= static_cast<int>(songs.size()))
        csr->current_track_index = static_cast<int>(csr->playback_seed % songs.size());
    }

    if (csr->channel)
    {
        bool channel_playing = false;
        if (csr->channel->isPlaying(&channel_playing) != FMOD_OK)
            channel_playing = false;

        if (!channel_playing)
        {
            csr->channel = nullptr;
            if (csr->current_sound)
            {
                csr->current_sound->release();
                csr->current_sound = nullptr;
            }
            csr->flags.is_playing = 0;
            if (g_self_radio_sync_all && g_self_radio_station.active)
            {
                if (csr->synced_to_station
                    && !g_self_radio_station.pending_start
                    && csr->current_track_index == g_self_radio_station.track_index)
                {
                    self_radio_station_advance_after_track_end(now_ms);
                }

                csr->seek_ms = g_self_radio_station.pending_start ? 0 : g_self_radio_station.seek_ms;
                csr->current_track_index = g_self_radio_station.track_index;
                csr->flags.pending_start = 1;
                csr->start_at_ms = g_self_radio_station.pending_start ? g_self_radio_station.start_at_ms : now_ms;
            }
            else
            {
                csr->seek_ms = 0;
                csr->current_track_index = self_radio_next_track_index(csr->current_track_index, songs.size());
                csr->flags.pending_start = 1;
                csr->start_at_ms = now_ms;
            }
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
        if (g_self_radio_sync_all && g_self_radio_station.active)
            csr->start_at_ms = g_self_radio_station.pending_start ? g_self_radio_station.start_at_ms : now_ms;
        else
        {
            // Random start position when enabled — applies to ALL vehicles (2D and 3D),
            // only on the non-sync path, only on a genuine cold start.
            if (g_self_radio_random_start && csr->seek_ms == 0)
            {
                const uint32_t length_ms = self_radio_get_track_length_ms(csr->current_track_index);
                csr->seek_ms = self_radio_random_start_seek(length_ms);
            }
            const bool apply_delay = switched_to_self_radio;
            csr->start_at_ms = now_ms + (apply_delay ? kSelfRadioInitialStartDelayMs : 0);
        }
    }

    if (csr->flags.pending_start && now_ms >= csr->start_at_ms)
        self_radio_start_playback(vehicle, csr, radioi);
}

// Helper: skip the currently-playing CSelfRadio to another track (+1 or -1).
void self_radio_skip_track(CSelfRadio* csr, int delta)
{
    if (!csr)
        return;

    const auto& songs = self_radio_get_songs();
    if (songs.empty())
        return;

    const int n = static_cast<int>(songs.size());

    if (g_self_radio_sync_all)
    {
        if (!g_self_radio_station.active)
            self_radio_station_bootstrap_from(csr);

        if (!g_self_radio_station.active)
            return;

        g_self_radio_station.track_index = ((g_self_radio_station.track_index + delta) % n + n) % n;
        g_self_radio_station.seek_ms = 0;
        g_self_radio_station.pending_start = false;
        g_self_radio_station.last_update_ms = self_radio_now_ms();
        self_radio_notify_track(songs[g_self_radio_station.track_index].name);
        return;
    }

    if (csr->channel)
    {
        csr->channel->stop();
        csr->channel = nullptr;
        if (csr->current_sound)
        {
            csr->current_sound->release();
            csr->current_sound = nullptr;
        }
    }

    csr->flags.is_playing = 0;
    csr->flags.pending_start = 0;
    csr->seek_ms = 0;

    if (csr->current_track_index < 0 || csr->current_track_index >= n)
        csr->current_track_index = 0;

    csr->current_track_index = ((csr->current_track_index + delta) % n + n) % n;

    // Queue immediate start.
    csr->flags.pending_start = 1;
    csr->start_at_ms = self_radio_now_ms();

    printf("[SelfRadio] Skipping to track %d (%s)\n",
        csr->current_track_index, songs[csr->current_track_index].name.c_str());
}

// Returns the first player-controlled CSelfRadio that is currently active.
CSelfRadio* self_radio_get_player_controlled()
{
    // Check all active ambient states
    for (auto& [ptr, csr] : g_ambient_states)
    {
        if (csr.flags.is_playing || csr.flags.pending_start)
            return &csr;
    }

    for (CSelfRadio* csr : g_self_radios)
    {
        if (!csr || !csr->isPlayerControlled())
            continue;
        if (csr->flags.is_playing || csr->flags.pending_start)
            return csr;
    }

    return nullptr;
}

void Update_Ambient_CSelfRadio()
{
    uintptr_t ambient = *(uintptr_t*)0x2574358;
    uintptr_t emitter = *(uintptr_t*)0x257435C;

    if (!emitter || !ambient)
    {
        for (auto& [ptr, csr] : g_ambient_states)
            ambient_csr_save_and_stop(csr);
        return;
    }

    radio_inst* radio = *(radio_inst**)(emitter + 0x8);
    if (!radio)
    {
        for (auto& [ptr, csr] : g_ambient_states)
            ambient_csr_save_and_stop(csr);
        return;
    }

    const auto& songs = self_radio_get_songs();
    if (songs.empty() || !g_self_radio_song_library.GetSystem())
        return;

    const bool is_self_station = is_radio_station_self_radio(radio);

    CSelfRadio& csr = ambient_state_get_or_create(ambient);

    if (!is_self_station)
    {
        ambient_csr_save_and_stop(csr);
        csr.synced_to_station = false;
        return;
    }

    // --- On Self Radio ---

    const FMOD_VECTOR world_pos = ambient_get_pos(ambient);
    csr.object_pos = world_pos;
    const uint64_t now_ms = self_radio_now_ms();

    const bool ambient_is_2d = !g_self_radio_enable_3d || g_self_radio_force_2d;
    const bool within_listener_box = ambient_is_2d || self_radio_is_within_listener_box(world_pos, g_self_radio_ambient_box_extent);
    if (!within_listener_box)
    {
        if (csr.flags.is_playing || csr.channel)
            ambient_csr_save_and_stop(csr);
        else
            csr.flags.pending_start = 0;
        return;
    }

    if (g_self_radio_sync_all && g_self_radio_station.active)
        self_radio_station_apply_to(&csr, now_ms);
    else
    {
        csr.synced_to_station = false;
        if (csr.current_track_index < 0 ||
            csr.current_track_index >= static_cast<int>(songs.size()))
        {
            csr.current_track_index =
                static_cast<int>(csr.playback_seed % songs.size());
        }
    }

    if (csr.flags.is_playing && csr.channel)
    {
        const FMOD_VECTOR zero_vel{};

        if (!g_self_radio_enable_3d || g_self_radio_force_2d)
        {
            csr.channel->setMode(FMOD_2D);
            csr.flags.is_2d = 1;
        }
        else
        {
            csr.channel->setMode(FMOD_3D | (g_self_radio_use_linear_rolloff ? FMOD_3D_LINEARROLLOFF : FMOD_3D_INVERSEROLLOFF));
            csr.channel->set3DLevel(g_self_radio_3d_level);
            csr.channel->set3DSpread(g_self_radio_3d_spread);
            csr.channel->set3DAttributes(&world_pos, &zero_vel);
            csr.channel->set3DMinMaxDistance(g_self_radio_min_distance, g_self_radio_max_distance);
            csr.flags.is_2d = 0;
        }

        bool channel_playing = false;
        if (csr.channel->isPlaying(&channel_playing) != FMOD_OK)
            channel_playing = false;

        if (!channel_playing)
        {
            csr.channel = nullptr;
            if (csr.current_sound)
            {
                csr.current_sound->release();
                csr.current_sound = nullptr;
            }
            csr.flags.is_playing = 0;
            if (g_self_radio_sync_all && g_self_radio_station.active)
            {
                if (csr.synced_to_station
                    && !g_self_radio_station.pending_start
                    && csr.current_track_index == g_self_radio_station.track_index)
                {
                    self_radio_station_advance_after_track_end(now_ms);
                }

                csr.seek_ms = g_self_radio_station.pending_start ? 0 : g_self_radio_station.seek_ms;
                csr.current_track_index = g_self_radio_station.track_index;
                csr.flags.pending_start = 1;
                csr.start_at_ms = g_self_radio_station.pending_start ? g_self_radio_station.start_at_ms : now_ms;
            }
            else
            {
                csr.seek_ms = 0;

                const int n = static_cast<int>(songs.size());
                csr.current_track_index = self_radio_next_track_index(csr.current_track_index, n);

                csr.flags.pending_start = 1;
                csr.start_at_ms = now_ms;
            }
            // fall through to start below
        }
        else
        {
            unsigned int position_ms = 0;
            if (csr.channel->getPosition(&position_ms, FMOD_TIMEUNIT_MS) == FMOD_OK)
                csr.seek_ms = position_ms;
            return;
        }
    }

    if (!csr.flags.pending_start)
    {
        csr.flags.pending_start = 1;
        if (g_self_radio_sync_all && g_self_radio_station.active)
            csr.start_at_ms = g_self_radio_station.pending_start ? g_self_radio_station.start_at_ms : now_ms;
        else
        {
            if (g_self_radio_random_start && csr.seek_ms == 0)
            {
                const uint32_t length_ms = self_radio_get_track_length_ms(csr.current_track_index);
                csr.seek_ms = self_radio_random_start_seek(length_ms);
            }
            csr.start_at_ms = now_ms;
        }
    }

    if (now_ms >= csr.start_at_ms)
        self_radio_start_playback_ambient(&csr, world_pos);
}

void gameplay_loop()
{
    cdecl_call(sub_935B80);
    self_radio_station_update();
    Update_Ambient_CSelfRadio();
    g_cached_game_volume = get_game_volume();

    const bool paused = havok_paused();

    for (CSelfRadio* csr : g_self_radios)
    {
        if (!csr || !csr->channel)
            continue;

        csr->channel->setVolume(g_cached_game_volume);
        csr->channel->setPaused(paused);
    }

    for (auto& [ptr, csr] : g_ambient_states)
    {
        if (!csr.channel)
            continue;
        csr.channel->setVolume(g_cached_game_volume);
        csr.channel->setPaused(paused);
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

    if (!radio_tuner_should_be_2d_for_vehicle(vehicle))
        csr->user_lpf = vehicle_audio_find_lpf_level(vehicle, 4, 1);
    else
        csr->user_lpf = 0.f;
}

uintptr_t sub_9551F0;

static bool g_self_radio_hide_in_selector = true;

void BlingMenuOptions() {
    CIniReader ini;

    g_self_radio_volume = std::clamp(ini.ReadFloat("OPTIONS", "Volume", 0.45f),0.f,4.f);
    g_self_radio_sync_all = ini.ReadBoolean("OPTIONS", "Sync All", false);
    g_self_radio_shuffle = ini.ReadBoolean("OPTIONS", "Shuffle", false);
    g_self_radio_random_start = ini.ReadBoolean("OPTIONS", "Random Start", false);
    g_self_radio_min_distance = ini.ReadFloat("OPTIONS", "3D Min Distance", 2.f);
    g_self_radio_max_distance = ini.ReadFloat("OPTIONS", "3D Max Distance", 45.f);
    g_self_radio_can_npc_select = ini.ReadBoolean("OPTIONS", "Can NPCs Select Self Radio", true);
    g_self_radio_hide_in_selector = ini.ReadBoolean("OPTIONS", "Hide In Selector", true);
    g_self_radio_show_current_playing = ini.ReadBoolean("OPTIONS", "Show current playing", true);
    if (BlingMenuLoad()) {
        BlingMenuAddCategory(kSelfRadioMenuPath);
        BlingMenuAddFloat(kSelfRadioMenuPath, "Volume", &g_self_radio_volume, []() {CIniReader ini; ini.WriteFloat("OPTIONS", "Volume", g_self_radio_volume); }, 0.05, 0.f, 4.f);
        BlingMenuAddBool(kSelfRadioMenuPath, "Enable 3D", &g_self_radio_enable_3d, nullptr);
        BlingMenuAddBool(kSelfRadioMenuPath, "Can NPC select", &g_self_radio_can_npc_select, nullptr);
        BlingMenuAddBool(kSelfRadioMenuPath, "Sync All", &g_self_radio_sync_all, nullptr);
        BlingMenuAddBool(kSelfRadioMenuPath, "Shuffle", &g_self_radio_shuffle, nullptr);
        BlingMenuAddBool(kSelfRadioMenuPath, "Force 2D", &g_self_radio_force_2d, nullptr);
        BlingMenuAddBool(kSelfRadioMenuPath, "Force 3D", &g_self_radio_force_3d, nullptr);
        BlingMenuAddBool(kSelfRadioMenuPath, "Use Velocity", &g_self_radio_use_velocity, nullptr);
        BlingMenuAddBool(kSelfRadioMenuPath, "Linear Rolloff", &g_self_radio_use_linear_rolloff, nullptr);
        BlingMenuAddFloat(kSelfRadioMenuPath, "Vehicle Box Extent", &g_self_radio_vehicle_box_extent, nullptr, 1.0f, 0.0f, 500.0f);
        BlingMenuAddFloat(kSelfRadioMenuPath, "Ambient Box Extent", &g_self_radio_ambient_box_extent, nullptr, 1.0f, 0.0f, 500.0f);
        BlingMenuAddFloat(kSelfRadioMenuPath, "3D Min Distance", &g_self_radio_min_distance, nullptr, 0.5f, 0.0f, 200.0f);
        BlingMenuAddFloat(kSelfRadioMenuPath, "3D Max Distance", &g_self_radio_max_distance, nullptr, 1.0f, 1.0f, 1000.0f);
        BlingMenuAddFloat(kSelfRadioMenuPath, "3D Level", &g_self_radio_3d_level, nullptr, 0.05f, 0.0f, 1.0f);
        BlingMenuAddFloat(kSelfRadioMenuPath, "3D Spread", &g_self_radio_3d_spread, nullptr, 1.0f, 0.0f, 360.0f);
        BlingMenuAddFloat(kSelfRadioMenuPath, "Doppler Scale", &g_self_radio_doppler_scale, nullptr, 0.05f, 0.0f, 5.0f);
        BlingMenuAddFloat(kSelfRadioMenuPath, "Rolloff Scale", &g_self_radio_rolloff_scale, nullptr, 0.05f, 0.0f, 5.0f);
        BlingMenuAddFuncStd(kSelfRadioMenuPath, "Reload Songs", []() {
            self_radio_reload_songs();
            });

        // Prev / Next track (only active when player-controlled)
        BlingMenuAddFuncCustomStd(kSelfRadioMenuPath, "Prev Track", [](int) -> const char* {
            auto* csr = self_radio_get_player_controlled();
            return csr ? "< Prev" : "(not player-controlled)";
            }, []() {
                auto* csr = self_radio_get_player_controlled();
                if (csr) self_radio_skip_track(csr, -1);
                });

            BlingMenuAddFuncCustomStd(kSelfRadioMenuPath, "Next Track", [](int) -> const char* {
                auto* csr = self_radio_get_player_controlled();
                return csr ? "Next >" : "(not player-controlled)";
                }, []() {
                    auto* csr = self_radio_get_player_controlled();
                    if (csr) self_radio_skip_track(csr, +1);
                    });

                BlingMenuAddFuncCustomStd(kSelfRadioMenuPath, "Now Playing", [](int) -> const char* {
                    static char buffer[256];
                    const auto& songs = self_radio_get_songs();
                    auto* csr = self_radio_get_player_controlled();
                    if (!csr || songs.empty())
                        std::snprintf(buffer, sizeof(buffer), "(nothing)");
                    else
                    {
                        const int idx = csr->current_track_index;
                        if (idx >= 0 && idx < static_cast<int>(songs.size()))
                            std::snprintf(buffer, sizeof(buffer), "%s", songs[idx].name.c_str());
                        else
                            std::snprintf(buffer, sizeof(buffer), "(unknown)");
                    }
                    return buffer;
                    }, []() {});

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
                BlingMenuAddInt(kSelfRadioMenuPath, "Hud_message_CSelfRadio_params.region", (int*)&Hud_message_CSelfRadio_params.region, NULL, 1, HUD_REGION_DEBUG, NUM_HUD_REGIONS);
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

    g_ambient_states.reserve(kAmbientPoolInitial);
    g_ambient_insertion_order.reserve(kAmbientPoolInitial);

    const auto station = thiscall_call<uint16_t>(0x4904F0, "SELF RADIO");
    const auto off_station = thiscall_call<uint16_t>(0x4904F0, "OFF");
    self_radio_Station = station != off_station ? station : -1;
    printf("[SelfRadio] Station ID: %d\n", self_radio_Station);
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
    vehicle_delete_thisD.unsafe_thiscall(obj, unknown);
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
        CSRadio->playback_seed = self_radio_random_seed();
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

    vehicle_free_thisD = safetyhook::create_inline(0xAA4A40, object_free_this_hook);
    vehicle_delete_thisD = safetyhook::create_inline(0xAA4490, object_delete_this_hook);

    static auto vehicle_kill_audio_hook = safetyhook::create_mid(0x483570, [](SafetyHookContext& ctx) {
        FreeCSRadio(ctx.eax);
        });

    InterceptCall(0x5205FB, sub_935B80, gameplay_loop);

    static auto crib_radio_hack = safetyhook::create_mid(0x474B84, [](SafetyHookContext& ctx) {


        uintptr_t emitter = *(uintptr_t*)0x257435C;


        if (emitter) {
            radio_inst* radio = *(radio_inst**)(emitter + 0x8);
            if (is_radio_station_self_radio(radio)) {
                ctx.eip = 0x474BBB;
            }
        }

        });

    InterceptCall(0x489A51, radio_tuner_update_og, radio_tuner_update_hook);

    Patch<void*>((0xAE2B0B + 1), &vehicle_create_callback_addr);

    static auto add_self_radio_station = safetyhook::create_mid(0x49025F, [](SafetyHookContext& ctx) {
        self_radio_add_station(reinterpret_cast<xml_node*>(ctx.eax));
        });

    static auto new_radio_parse_flags = safetyhook::create_mid(0x48FB19, [](SafetyHookContext& ctx) {

        const char* radio_flag = (const char*)ctx.eax;
        radio_flags* flags = (radio_flags*)(ctx.ebp + 0x3C);
        if (!strcmp("Self Radio",radio_flag)) 
        {

            flags->m_is_selfradio = 1;
        }

        });

    static auto vint_populate_playlist_gnere = safetyhook::create_mid(0x777721, [](SafetyHookContext& ctx) {

        radio_flags* flags = (radio_flags*)(ctx.edi + 0x3C);
        if (g_self_radio_hide_in_selector && flags->m_is_selfradio)
        {

            ctx.eip = 0x77780F;
        }

        });

    static auto radio_tuner_npc_can_select_station_midhook = safetyhook::create_mid(0x48E624, [](SafetyHookContext& ctx) {

        radio_flags flags = (radio_flags)(ctx.eax);

        if (!g_self_radio_can_npc_select && flags.m_is_selfradio) {
            ctx.eip = 0x48E621;
        }

        });


}

BOOL APIENTRY DllMain(HMODULE hModule,
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
