#pragma once

#include <Modules/ReplayRecorderModule.h>
#include <Utils/Replay/ReplayFile.h>
#include <Utils/Replay/ReplayMessages.h>
#include <Utils/Replay/ReplayState.h>

#include <GWCA/Packets/StoC.h>
#include <GWCA/Utilities/Hook.h>

// The Replay Recorder's state shared by its recording, snapshot and playback files. Not an API - the module owns all of this.
namespace ReplayRecorder {
    using namespace ReplayMessages;

    using Clock = std::chrono::steady_clock;

    using GW::Packet::StoC::PacketBase;

    using ReplayState::Message;

    inline ReplayRecorderModule::Settings settings;

    inline ReplayFile::MessageTable message_table;

    inline uint64_t message_table_digest = 0;

    constexpr uint32_t viewpoint_message_id = CharToClient172;

    inline uintptr_t game_viewpoint_handler = 0;

    inline std::mutex recording_mutex;

    inline ReplayFile::Writer writer;

    inline std::filesystem::path recording_path;

    inline uint32_t chapter_map_file = 0;

    inline GW::Packet::StoC::PostProcess post_process{};

    // The char client's per-character data (AgentGetNotifyParams): what an agent wears, in s2c 110's slot order (CharCliOnEquipUpdate, CharCliAgentGetEquippedItemId).
    struct CharacterData {
        uint8_t h0000[0x24];
        uint32_t worn[9];
    };

    struct MapPoint {
        float x;
        float y;
        uint32_t plane;
        uint32_t h000C;
    };

    // The agent engine's agent of the server's world (IAgent::CAgent, AgAgent.cpp), as GWCA's AgentContext::agent_movement lists them: what 41, 42, 40, 43, 39 and 44 set.
    struct WalkingAgent {
        uint8_t h0000[0x10];
        uint32_t agent_id;
        uint8_t h0014[0x08];
        uint32_t def;
        uint8_t h0020[0x28];
        uint32_t stop_time;
        uint8_t h004C[0x0C];
        uint32_t point_time;
        float max_speed;
        float move_speed;
        uint8_t h0064[0x14];
        MapPoint point;
        MapPoint leg;
        uint32_t target_agent;
        MapPoint target;
        uint32_t h00AC;
        float velocity_x;
        float velocity_y;
        uint8_t h00B8[0x0C];
        uint32_t facing;
    };
    static_assert(offsetof(WalkingAgent, def) == offsetof(GW::AgentMovement, agentDef) && offsetof(WalkingAgent, point) == 0x78 && offsetof(WalkingAgent, velocity_x) == 0xB0 && offsetof(WalkingAgent, facing) == 0xC4);

    using AgentNotifyParams_pt = int(__cdecl*)(uint32_t agent_id, uint32_t* definition, const CharacterData** params);

    inline AgentNotifyParams_pt AgentNotifyParams_Func = nullptr;

    using ObserveGame_pt = void(__cdecl*)(uint32_t game_key);

    inline ObserveGame_pt ObserveGame_Func = nullptr;

    inline GW::HookEntry observe_hook_entry;

    inline bool observe_applied = false;

    inline std::atomic_bool emulating_packet = false;

    inline std::atomic_bool playing_replay = false;

    inline std::string observe_status;

    enum class Playback {
        Off,
        Transfer,
        Connecting,
        Acknowledging,
        Loading,
        Playing,
        Ending,
    };

    inline bool playback_armed = false;

    inline std::string playback_status;

    inline std::atomic<Playback> playback = Playback::Off;

    inline std::atomic_bool playback_stop_requested = false;

    uint32_t ServerTime();
    ReplayFile::Motion MotionOf(const WalkingAgent& walking, uint32_t clock);
    // Every line is flushed: the game can end without the toolbox's exit, and a replay's last lines tell why.
    void LogLine(void (*log)(const char* format, ...), const std::string& text);
    std::filesystem::path RecordingsFolder();
    std::vector<Message> ItemDetails(uint32_t item_id);
    ReplayState::World Snapshot();
    void DisarmPlayback();
    std::string ArmPlayback();
    void UpdatePlayback();
    void DropListedRows();
    std::string StartListing();
} // namespace ReplayRecorder
