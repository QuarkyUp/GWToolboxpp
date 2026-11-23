#include "stdafx.h"

#include <GWCA/Context/CharContext.h>
#include <GWCA/Context/PreGameContext.h>

#include <GWCA/Constants/UIMessages.h>

#include <GWCA/GameEntities/Agent.h>
#include <GWCA/GameEntities/Map.h>
#include <GWCA/GameEntities/Match.h>

#include <GWCA/Managers/AgentMgr.h>
#include <GWCA/Managers/MapMgr.h>
#include <GWCA/Managers/MemoryMgr.h>
#include <GWCA/Managers/StoCMgr.h>
#include <GWCA/Managers/UIMgr.h>

#include <GWCA/Packets/StoC.h>

#include <GWCA/Utilities/Hooker.h>
#include <GWCA/Utilities/Scanner.h>

#include <GWToolbox.h>
#include <ImGuiAddons.h>
#include <Logger.h>
#include <Modules/ReplayRecorderModule.h>
#include <Modules/Resources.h>
#include <Utils/EncString.h>
#include <Utils/ReplayFile.h>
#include <Utils/ReplayObserve.h>
#include <Utils/ReplayWire.h>
#include <Utils/StoCHandlers.h>
#include <Utils/TextUtils.h>

namespace {
    using Clock = std::chrono::steady_clock;
    using GW::Packet::StoC::PacketBase;

    constexpr auto idle_timeout = std::chrono::seconds(60);
    constexpr auto flush_interval = std::chrono::seconds(5);
    constexpr auto registration_retry_interval = std::chrono::seconds(1);

    ReplayRecorderModule::Settings settings;

    GW::HookEntry stoc_hook_entry;
    bool callbacks_registered = false;
    ReplayFile::MessageTable message_table;
    uint64_t message_table_digest = 0;
    constexpr uint32_t viewpoint_message_id = 172;
    uintptr_t game_viewpoint_handler = 0;
    Clock::time_point next_registration_attempt;

    std::mutex recording_mutex;
    ReplayFile::Writer writer;
    ReplayFile::Header header;
    std::filesystem::path recording_path;
    Clock::time_point recording_start;
    Clock::time_point last_record_at;
    Clock::time_point last_flush_at;
    std::vector<uint8_t> body;
    std::vector<uint8_t> pending_chat;
    uint32_t pending_chat_time_ms = 0;
    bool has_pending_chat = false;
    std::optional<ReplayFile::NameStripper> name_stripper;
    std::string status = "Waiting for the next map";

    using ObserveGame_pt = void(__cdecl*)(uint32_t game_key);
    using GameProps_pt = uint8_t*(__cdecl*)();
    ObserveGame_pt ObserveGame_Func = nullptr;
    ObserveGame_pt ObserveGame_Ret = nullptr;
    GameProps_pt GameProps_Func = nullptr;
    GW::HookEntry observe_hook_entry;
    bool observe_applied = false;
    std::atomic_bool emulating_rows = false;
    std::atomic_bool playing_replay = false;
    std::set<uint32_t> server_keys;
    std::map<uint32_t, ReplayObserve::Row> listed_rows;
    std::set<std::filesystem::path> logged_left_out;
    std::string observe_status;

    std::filesystem::path RecordingsFolder()
    {
        return settings.folder.empty() ? Resources::GetSettingsFolderPath() / L"replays" : std::filesystem::path(TextUtils::StringToWString(settings.folder));
    }

    uint32_t ElapsedMs(const Clock::time_point since)
    {
        return static_cast<uint32_t>(std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - since).count());
    }

    std::string RecordingName()
    {
        return TextUtils::WStringToString(recording_path.filename().wstring());
    }

    void FinishRecording()
    {
        if (!writer.IsOpen()) {
            return;
        }
        has_pending_chat = false;
        header.duration_ms = ElapsedMs(recording_start);
        header.flags |= ReplayFile::Complete;
        const auto size = writer.Size();
        if (header.record_count == 0) {
            writer.Close(header);
            std::error_code error;
            std::filesystem::remove(recording_path, error);
            status = std::format("{} removed: no message was recorded", RecordingName());
            Log::Log("Replay Recorder: %s", status.c_str());
            return;
        }
        if (!writer.Close(header)) {
            status = std::format("Could not finish {}", RecordingName());
            Log::Error("Replay Recorder: %s", status.c_str());
            return;
        }
        status = std::format("Saved {}: {} messages, {} KB, {} layout errors{}", RecordingName(), header.record_count, size / 1024, header.layout_errors,
                             header.flags & ReplayFile::SizeCapReached ? " (size cap reached)" : "");
        Log::Info("Replay Recorder: %s", status.c_str());
    }

    void StartRecording(const GW::Packet::StoC::GameSrvTransfer* transfer)
    {
        const auto map_id = static_cast<GW::Constants::MapID>(transfer->map_id);
        const auto map_info = GW::Map::GetMapInfo(map_id);
        if (!map_info || GWToolbox::ShouldDisableToolbox(map_id)) {
            status = std::format("Not recording map {}: PvP or unknown", transfer->map_id);
            return;
        }
        const auto explorable = transfer->is_explorable != 0;
        if (!(explorable ? settings.record_explorables : settings.record_outposts) || message_table.empty()) {
            status = std::format("Not recording map {}", transfer->map_id);
            return;
        }
        const auto folder = RecordingsFolder();
        std::wstring error;
        if (!Resources::EnsureFolderExists(folder, error)) {
            status = TextUtils::WStringToString(error);
            Log::Error("Replay Recorder: %s", status.c_str());
            return;
        }
        recording_path = folder / std::format(L"{}_map{}.gwrec", TextUtils::StringToWString(TextUtils::FilenameTimestamp()), transfer->map_id);
        header = {};
        header.client_build = GW::MemoryMgr::GetGWVersion();
        header.table_digest = message_table_digest;
        header.start_time_unix_ms = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count();
        header.map_id = transfer->map_id;
        header.map_region_type = static_cast<uint32_t>(map_info->type);
        header.is_explorable = explorable;
        header.region = transfer->region;
        header.flags = (settings.strip_names_and_chat ? ReplayFile::NamesStripped | ReplayFile::ChatDropped : 0) | (settings.strip_own_names ? ReplayFile::OwnNamesStripped : 0);
        strncpy(header.toolbox_version, GWTOOLBOXDLL_VERSION GWTOOLBOXDLL_VERSION_BETA, sizeof(header.toolbox_version) - 1);
        if (!writer.Open(recording_path, header, message_table)) {
            status = std::format("Could not create {}", RecordingName());
            Log::Error("Replay Recorder: %s", status.c_str());
            return;
        }
        const auto char_context = GW::GetCharContext();
        name_stripper.reset();
        if (settings.strip_names_and_chat || settings.strip_own_names) {
            name_stripper.emplace(settings.strip_names_and_chat, settings.strip_own_names, char_context ? std::wstring(char_context->player_name) : std::wstring());
        }
        has_pending_chat = false;
        recording_start = last_record_at = last_flush_at = Clock::now();
        status = std::format("Recording {}", RecordingName());
    }

    void WriteRecord(const uint32_t time_ms, const std::vector<uint8_t>& bytes)
    {
        const auto size_cap = static_cast<uint64_t>(settings.size_cap_mb) << 20;
        if (writer.Size() + bytes.size() + 6 > size_cap) {
            header.flags |= ReplayFile::SizeCapReached;
            FinishRecording();
            return;
        }
        if (!writer.Append(time_ms, bytes.data(), bytes.size())) {
            FinishRecording();
            return;
        }
        header.record_count++;
        last_record_at = Clock::now();
    }

    void PackRecord(const PacketBase* packet, std::vector<uint8_t>& out)
    {
        const auto& fields = message_table[packet->header];
        if (!ReplayFile::PackMessage(packet->header, fields.data(), static_cast<uint32_t>(fields.size()), reinterpret_cast<const uint8_t*>(packet), out,
                                     name_stripper ? name_stripper->For(static_cast<uint16_t>(packet->header)) : ReplayFile::TextFilter())) {
            out = {static_cast<uint8_t>(packet->header), static_cast<uint8_t>(packet->header >> 8)};
            header.layout_errors++;
        }
    }

    void OnServerMessage(GW::HookStatus*, PacketBase* packet, void*)
    {
        if (emulating_rows || playing_replay) {
            return;
        }
        std::scoped_lock lock(recording_mutex);
        if (packet->header == GAME_SMSG_TRANSFER_GAME_SERVER_INFO) {
            FinishRecording();
            StartRecording(static_cast<const GW::Packet::StoC::GameSrvTransfer*>(packet));
            return;
        }
        if (!writer.IsOpen() || packet->header >= message_table.size()) {
            return;
        }
        if (packet->header == GAME_SMSG_INSTANCE_LOAD_INFO) {
            const auto info = static_cast<const GW::Packet::StoC::InstanceLoadInfo*>(packet);
            if (info->is_observer) {
                writer.Close(header);
                std::error_code error;
                std::filesystem::remove(recording_path, error);
                status = "Not recording: observing a match";
                return;
            }
            header.district = info->district;
            header.language = info->language;
        }
        const auto time_ms = ElapsedMs(recording_start);
        if (header.flags & ReplayFile::ChatDropped) {
            switch (packet->header) {
                case GAME_SMSG_CHAT_MESSAGE_CORE:
                    PackRecord(packet, pending_chat);
                    pending_chat_time_ms = time_ms;
                    has_pending_chat = true;
                    return;
                case GAME_SMSG_CHAT_MESSAGE_GLOBAL:
                case GAME_SMSG_CHAT_MESSAGE_LOCAL:
                    has_pending_chat = false;
                    return;
                case GAME_SMSG_SPEECH_BUBBLE: {
                    const auto agent = GW::Agents::GetAgentByID(static_cast<const GW::Packet::StoC::SpeechBubble*>(packet)->agent_id);
                    const auto living = agent ? agent->GetAsAgentLiving() : nullptr;
                    if (living && living->IsPlayer()) {
                        return;
                    }
                    break;
                }
                default:
                    break;
            }
            if (has_pending_chat) {
                has_pending_chat = false;
                WriteRecord(pending_chat_time_ms, pending_chat);
                if (!writer.IsOpen()) {
                    return;
                }
            }
        }
        PackRecord(packet, body);
        WriteRecord(time_ms, body);
    }

    void RegisterCallbacks()
    {
        if (Clock::now() < next_registration_attempt) {
            return;
        }
        next_registration_attempt = Clock::now() + registration_retry_interval;
        const auto table = GetStoCHandlerArray();
        if (!(table && table->size())) {
            return;
        }
        message_table.clear();
        for (const auto& handler : *table) {
            message_table.emplace_back(handler.fields, handler.fields + handler.field_count);
        }
        message_table_digest = ReplayFile::TableDigest(message_table);
        if (!game_viewpoint_handler && table->size() > viewpoint_message_id) {
            // GWCA puts its dispatcher in a header's handler_func once a callback is registered on it (PacketLoggerWindow.cpp's InitStoC): the game's own is read first.
            game_viewpoint_handler = reinterpret_cast<uintptr_t>((*table)[viewpoint_message_id].handler_func);
        }
        for (uint32_t message_id = 0; message_id < message_table.size(); message_id++) {
            if (!GW::StoC::RegisterPacketCallback(&stoc_hook_entry, message_id, OnServerMessage, nullptr, 0x9000)) {
                GW::StoC::RemoveCallbacks(&stoc_hook_entry);
                message_table.clear();
                return;
            }
        }
        callbacks_registered = true;
    }

    using GameConnect_pt = void(__cdecl*)(uint32_t protocol, const uint8_t* address, const void* data, uint32_t size);
    using Goodbye_pt = void(__cdecl*)(uint32_t reason);
    using InstanceAck_pt = void(__cdecl*)(uint8_t* mission);
    using MapLoaded_pt = void(__cdecl*)();
    using WorldMapToggle_pt = void(__cdecl*)(uint32_t game_view, int open);
    using MissionMapCreate_pt = void(__fastcall*)(uint32_t* world_map);
    using AgentAdvance_pt = int(__cdecl*)(uint32_t elapsed_ms);
    using PostNetEvent_pt = uint32_t(__cdecl*)(void* context, uint32_t connection, uint32_t kind, const void* data, uint32_t size);

    constexpr uint32_t replay_connection_id = 0x7E500001;
    constexpr uint16_t closed_port = 1;
    constexpr uint8_t address_family_ipv4 = 2;
    constexpr uint32_t net_event_connected = 5;
    constexpr uint32_t net_event_closed = 6;
    constexpr uint32_t net_event_data = 8;
    constexpr uint32_t mission_context_offset = 0x44;
    constexpr uint32_t mission_flags_offset = 0x190;
    constexpr uint32_t mission_join_value_offset = 0x194;
    constexpr uint32_t mission_join_map_offset = 0x198;
    constexpr uint32_t instance_acknowledged = 0xC0;
    constexpr uint32_t join_pending = 0x20;
    constexpr uint32_t transfer_kept = 0x10;
    constexpr uint32_t msgconn_mode_offset = 0x60;
    constexpr uint32_t msgconn_receive_cipher_offset = 0x7C;
    constexpr uint32_t msgconn_encrypted = 2;
    constexpr auto connect_timeout = std::chrono::seconds(15);
    constexpr auto load_timeout = std::chrono::seconds(120);
    constexpr auto close_timeout = std::chrono::seconds(20);
    const std::vector<uint32_t> transfer_fields = {0x1a6, 0x1805, 0x404, 0x104, 0x204, 0x104, 0x404, 0x104};
    const std::vector<uint32_t> viewpoint_fields = {viewpoint_message_id, 0x10, 0x104};
    constexpr auto restart_settle = std::chrono::seconds(3);
    constexpr auto transfer_wait = std::chrono::seconds(60);

    struct ViewpointMessage {
        uint32_t header = viewpoint_message_id;
        uint32_t agent = 0;
        uint32_t select = 0;
    };

    struct TransferMessage {
        uint32_t header = GAME_SMSG_TRANSFER_GAME_SERVER_INFO;
        uint8_t address[24] = {};
        uint32_t game_type = 0;
        uint32_t territory = 0;
        uint32_t map_id = 0;
        uint32_t map_type = 0;
        uint32_t key = 0;
        uint32_t last_byte = 0;
    };
    // The 422 handler (0x0084dbe0) reads +0x30, a field GWCA's GameSrvTransfer does not have.
    static_assert(sizeof(TransferMessage) == 0x34);

    struct ConnectedEvent {
        uint32_t socket = 0;
        uint32_t server = 0;
        uint8_t address[24] = {};
        uint32_t protocol = 0;
        uint32_t connection = replay_connection_id;
        uint8_t secret[ReplayWire::ready_secret_size] = {};
    };
    static_assert(sizeof(ConnectedEvent) == 0x3C && offsetof(ConnectedEvent, connection) == 0x24 && offsetof(ConnectedEvent, secret) == 0x28);

    enum class Playback {
        Off,
        Transfer,
        Connecting,
        Acknowledging,
        Loading,
        Playing,
        Ending,
    };

    GameConnect_pt GameConnect_Func = nullptr;
    GameConnect_pt GameConnect_Ret = nullptr;
    Goodbye_pt Goodbye_Func = nullptr;
    Goodbye_pt Goodbye_Ret = nullptr;
    InstanceAck_pt InstanceAck_Func = nullptr;
    InstanceAck_pt InstanceAck_Ret = nullptr;
    MapLoaded_pt MapLoaded_Func = nullptr;
    MapLoaded_pt MapLoaded_Ret = nullptr;
    WorldMapToggle_pt WorldMapToggle_Func = nullptr;
    WorldMapToggle_pt WorldMapToggle_Ret = nullptr;
    MissionMapCreate_pt MissionMapCreate_Func = nullptr;
    MissionMapCreate_pt MissionMapCreate_Ret = nullptr;
    AgentAdvance_pt AgentAdvance_Func = nullptr;
    AgentAdvance_pt AgentAdvance_Ret = nullptr;
    PostNetEvent_pt PostNetEvent_Func = nullptr;
    uint8_t** game_connection_global = nullptr;
    void** net_context_global = nullptr;
    bool playback_armed = false;
    std::string playback_status;

    std::atomic<Playback> playback = Playback::Off;
    std::atomic_bool playback_paused = false;
    std::atomic_int playback_speed = 1;
    std::atomic_bool playback_stop_requested = false;
    ReplayWire::Plan plan;
    ReplayWire::Rc4 send_cipher;
    std::filesystem::path playing_path;
    ReplayObserve::Row playing_row;
    std::atomic_bool playback_ended = false;
    std::atomic_bool restart_pending = false;
    bool viewpoint_available = false;
    std::atomic_uint32_t followed_agent = 0;
    uint32_t last_target = 0;
    bool world_map_refusal_logged = false;
    bool mission_map_refusal_logged = false;
    bool join_wait_logged = false;
    std::optional<Clock::time_point> outpost_settled_since;
    TransferMessage transfer;
    bool transfer_due = false;
    bool connection_open = false;
    bool stop_sent = false;
    uint8_t* replay_connection = nullptr;
    size_t next_post = 0;
    double played_ms = 0;
    Clock::time_point stage_since;
    Clock::time_point last_update;

    std::mt19937 RandomBytes()
    {
        std::random_device device;
        return std::mt19937(device());
    }

    void PostNetEvent(const uint32_t kind, const uint32_t connection, const void* data, const uint32_t size)
    {
        PostNetEvent_Func(*net_context_global, connection, kind, data, size);
    }

    uint8_t* MissionContext()
    {
        const auto properties = GameProps_Func ? GameProps_Func() : nullptr;
        return properties ? *reinterpret_cast<uint8_t**>(properties + mission_context_offset) : nullptr;
    }

    uint32_t MissionFlags()
    {
        const auto mission = MissionContext();
        return mission ? *reinterpret_cast<uint32_t*>(mission + mission_flags_offset) : 0;
    }

    uint32_t JoinedMap()
    {
        const auto mission = MissionContext();
        return mission ? *reinterpret_cast<uint32_t*>(mission + mission_join_map_offset) : 0;
    }

    void LogPlayback(const std::string& step)
    {
        Log::Log("Replay Recorder: %s", step.c_str());
        Log::FlushFile();
    }

    void PostStage(const ReplayWire::Stage stage)
    {
        LogPlayback(std::format("posting {}", ReplayWire::StageName(stage)));
        for (; next_post < plan.posts.size() && plan.posts[next_post].stage == stage; next_post++) {
            auto bytes = plan.posts[next_post].bytes;
            send_cipher.Apply(bytes.data(), bytes.size());
            PostNetEvent(net_event_data, replay_connection_id, bytes.data(), bytes.size());
        }
    }

    void EnterStage(const Playback stage)
    {
        playback = stage;
        playing_replay = stage != Playback::Off;
        stage_since = Clock::now();
    }

    void FinishPlayback(const std::string& why)
    {
        Log::Info("Replay Recorder: %s", why.c_str());
        Log::FlushFile();
        playback_status = why;
        plan = {};
        connection_open = false;
        replay_connection = nullptr;
        transfer_due = false;
        stop_sent = false;
        playback_stop_requested = false;
        EnterStage(Playback::Off);
    }

    void CloseReplayConnection(const std::string& why)
    {
        if (connection_open) {
            connection_open = false;
            PostNetEvent(net_event_closed, replay_connection_id, nullptr, 0);
        }
        FinishPlayback(why);
    }

    void StopObserving()
    {
        if (stop_sent) {
            return;
        }
        stop_sent = true;
        LogPlayback("stop observing asked of the game");
        EnterStage(Playback::Ending);
        GW::UI::SendUIMessage(GW::UI::UIMessage::kMessage_0x1000017d);
    }

    void __cdecl OnGameConnect(const uint32_t protocol, const uint8_t* address, const void* data, const uint32_t size)
    {
        GW::Hook::EnterHook();
        if (playback != Playback::Transfer || protocol != 0 || memcmp(address, transfer.address, 8) != 0) {
            GameConnect_Ret(protocol, address, data, size);
            return GW::Hook::LeaveHook();
        }
        auto random = RandomBytes();
        ConnectedEvent connected;
        memcpy(connected.address, address, sizeof(connected.address));
        uint8_t ready[ReplayWire::ready_record_size] = {ReplayWire::ready_record_type, ReplayWire::ready_record_size};
        for (auto& byte : connected.secret) {
            byte = static_cast<uint8_t>(random());
        }
        for (size_t i = 2; i < sizeof(ready); i++) {
            ready[i] = static_cast<uint8_t>(random());
        }
        LogPlayback("the game's connect replaced: the replay's connection and its ready record posted");
        // A kind-5 event with no socket gives the client the MsgConn its own playback runs on (GcSrv.cpp, MsgConn's null-socket branches).
        PostNetEvent(net_event_connected, 0, &connected, sizeof(connected));
        PostNetEvent(net_event_data, replay_connection_id, ready, sizeof(ready));
        connection_open = true;
        EnterStage(Playback::Connecting);
        GW::Hook::LeaveHook();
    }

    void __cdecl OnGoodbye(const uint32_t reason)
    {
        GW::Hook::EnterHook();
        Goodbye_Ret(reason);
        if (connection_open) {
            CloseReplayConnection(std::format("Left the replay of {}", TextUtils::WStringToString(playing_path.filename().wstring())));
        }
        GW::Hook::LeaveHook();
    }

    void __cdecl OnInstanceAck(uint8_t* mission)
    {
        GW::Hook::EnterHook();
        InstanceAck_Ret(mission);
        if (playback == Playback::Acknowledging && (*reinterpret_cast<uint32_t*>(mission + mission_flags_offset) & instance_acknowledged) == instance_acknowledged) {
            LogPlayback("c2s 145 due: the instance acknowledged");
            PostStage(ReplayWire::Stage::Load);
            EnterStage(Playback::Loading);
        }
        GW::Hook::LeaveHook();
    }

    void __cdecl OnMapLoaded()
    {
        GW::Hook::EnterHook();
        MapLoaded_Ret();
        if (playback == Playback::Loading) {
            LogPlayback("c2s 144 due: the replay's map loaded");
            PostStage(ReplayWire::Stage::Entry);
            played_ms = 0;
            last_update = Clock::now();
            EnterStage(Playback::Playing);
        }
        GW::Hook::LeaveHook();
    }

    void __cdecl OnWorldMapToggle(const uint32_t game_view, const int open)
    {
        GW::Hook::EnterHook();
        const auto char_context = GW::GetCharContext();
        const auto left_map = char_context ? GW::Map::GetMapInfo(char_context->current_map_id) : nullptr;
        const auto replay_map = GW::Map::GetMapInfo(static_cast<GW::Constants::MapID>(playing_row.header.map_id));
        // The world map draws the world of the map left for the replay (+0x238) over the explored areas the replay's s2c 139 gave, and asserts they match (GmMapView.cpp:1731).
        if (open && playback != Playback::Off && !(left_map && replay_map && left_map->continent == replay_map->continent)) {
            if (!world_map_refusal_logged) {
                world_map_refusal_logged = true;
                Log::Info("Replay Recorder: the world map stays closed during this replay: it would show the continent of map %u, the one left for the replay, over the explored areas of map %u's",
                          char_context ? static_cast<uint32_t>(char_context->current_map_id) : 0, playing_row.header.map_id);
                Log::FlushFile();
            }
            return GW::Hook::LeaveHook();
        }
        WorldMapToggle_Ret(game_view, open);
        GW::Hook::LeaveHook();
    }

    void __fastcall OnMissionMapCreate(uint32_t* world_map)
    {
        GW::Hook::EnterHook();
        // While observing, the mission map draws the map's image (AreaInfo::file_id, CMapWindow's observer branch), which only PvP maps have: a PvE replay's would be black.
        if (playback != Playback::Off) {
            if (!mission_map_refusal_logged) {
                mission_map_refusal_logged = true;
                Log::Info("Replay Recorder: the mission map stays closed during a replay: the game's observer mode draws it from a PvP map's image, which a PvE map has none of");
                Log::FlushFile();
            }
            return GW::Hook::LeaveHook();
        }
        MissionMapCreate_Ret(world_map);
        GW::Hook::LeaveHook();
    }

    int __cdecl OnAgentAdvance(uint32_t elapsed_ms)
    {
        GW::Hook::EnterHook();
        // The agents' clock alone (AgApi.cpp's AgentAdvance: its trackers and timer queue); ArenaNet's connections run on the event timers, untouched.
        if (playback == Playback::Playing) {
            elapsed_ms = playback_paused ? 0 : elapsed_ms * static_cast<uint32_t>(playback_speed.load());
            played_ms = std::min(played_ms + elapsed_ms, static_cast<double>(plan.duration_ms));
        }
        const auto result = AgentAdvance_Ret(elapsed_ms);
        GW::Hook::LeaveHook();
        return result;
    }

    void FollowViewpoint(const uint32_t agent_id)
    {
        ViewpointMessage message{.agent = agent_id};
        emulating_rows = true;
        GW::StoC::EmulatePacket(reinterpret_cast<PacketBase*>(&message));
        emulating_rows = false;
        followed_agent = agent_id;
        LogPlayback(std::format("the view follows agent {} (s2c 172 emulated, as an observer's server sends it)", agent_id));
    }

    void DisarmPlayback()
    {
        for (const auto hooked : {reinterpret_cast<void*>(GameConnect_Func), reinterpret_cast<void*>(Goodbye_Func), reinterpret_cast<void*>(InstanceAck_Func), reinterpret_cast<void*>(MapLoaded_Func),
                                  reinterpret_cast<void*>(WorldMapToggle_Func), reinterpret_cast<void*>(MissionMapCreate_Func), reinterpret_cast<void*>(AgentAdvance_Func)}) {
            if (hooked) {
                GW::Hook::RemoveHook(hooked);
            }
        }
        GameConnect_Func = nullptr;
        Goodbye_Func = nullptr;
        InstanceAck_Func = nullptr;
        MapLoaded_Func = nullptr;
        WorldMapToggle_Func = nullptr;
        MissionMapCreate_Func = nullptr;
        AgentAdvance_Func = nullptr;
        viewpoint_available = false;
        playback_armed = false;
    }

    std::string ArmPlayback()
    {
        const auto find = [](const char* pattern, const char* mask) {
            return GW::Scanner::Find(pattern, mask, 0);
        };
        const auto game_connect = find("\x55\x8B\xEC\xE8\x00\x00\x00\x00\x85\xC0\x0F\x85\x00\x00\x00\x00\x53\x56\x57\xFF\x75\x08\xE8", "xxxx????xxxx????xxxxxxx");
        const auto goodbye = find("\x55\x8B\xEC\x83\x3D\x00\x00\x00\x00\x00\x56\x8B\x75\x08\x75\x00\x8B\xC6\xF7\xD8\x1B\xC0\x83\xE0\x06", "xxxxx?????xxxxx?xxxxxxxxx");
        const auto post_net_event = find("\x55\x8B\xEC\x56\x8B\x75\x08\x85\xF6\x75\x00\x68\xDB\x07\x00\x00\xBA\x00\x00\x00\x00\xB9", "xxxxxxxxxx?xxxxxx????x");
        const auto net_callback = find("\x55\x8B\xEC\x83\xEC\x48\xA1\x00\x00\x00\x00\x33\xC5\x89\x45\xFC\x53\x8B\x5D\x0C\x56\x8B\x75\x08", "xxxxxxx????xxxxxxxxxxxxx");
        const auto ready_step = find(
            "\x55\x8B\xEC\x83\xEC\x28\xA1\x00\x00\x00\x00\x33\xC5\x89\x45\xFC\x8B\x45\x0C\x53\x8B\x5D\x08\x89\x5D\xDC\x89\x45\xD8\x83\x7B\x60\x01\x0F\x85\x00\x00\x00\x00\x8B\x45\x10\x80\x78\x01\x16\x0F\x85\x00\x00\x00\x00\x56\x57\x8D\x53\x69",
            "xxxxxxx????xxxxxxxxxxxxxxxxxxxxxxxx????xxxxxxxxx????xxxxx");
        const auto ready_cipher = find("\x6A\x14\x8D\x73\x7C\xC7\x43\x60\x02\x00\x00\x00\x8B\xCE\xE8\x00\x00\x00\x00\x8B\x43\x64\x8D\xBB\x84\x01\x00\x00", "xxxxxxxxxxxxxxx????xxxxxxxxx");
        const auto receive = find(
            "\x55\x8B\xEC\x51\xA1\x00\x00\x00\x00\x33\xC5\x89\x45\xFC\x53\x56\x57\xE8\x00\x00\x00\x00\x8B\x5D\x10\x85\xC0\x74\x0E\x8D\x43\x34\x50\x6A\x00\xE8\x00\x00\x00\x00\x83\xC4\x08\x8B\x4D\x08\x83\x79\x34\x00\x74\x7A\x8B\x7D\x0C\x83\x79\x60\x02",
            "xxxxx????xxxxxxxxx????xxxxxxxxxxxxxx????xxxxxxxxxxxxxxxxxxx");
        const auto receive_cipher = find("\x53\x8D\x4F\x7C\xE8\x00\x00\x00\x00\xFF\x75\x14\x8D\x04\x1E\x89\x77\x44\x57\x89\x47\x48\xE8", "xxxxx????xxxxxxxxxxxxxx");
        const auto instance_ack = find("\x55\x8B\xEC\x8B\x45\x08\x8B\x88\x90\x01\x00\x00\x8B\xC1\x25\xC0\x00\x00\x00\x3C\xC0\x75\x00\xF6\xC1\x01", "xxxxxxxxxxxxxxxxxxxxxx?xxx");
        const auto map_loaded = find("\x55\x8B\xEC\x81\xEC\x48\x01\x00\x00\xA1\x00\x00\x00\x00\x33\xC5\x89\x45\xFC\x8D\x45\xE8\xC7\x45\xE4\x90\x00\x00\x00", "xxxxxxxxxx????xxxxxxxxxxxxxxx");
        const auto transfer_handler = find("\x55\x8B\xEC\x8B\x45\x08\x83\xEC\x08\x8B\x40\x20\x84\xC0\x79\x00\x83\xC8\x80\x56\x50\xE8", "xxxxxxxxxxxxxxx?xxxxxx");
        const auto world_map_toggle = find("\x55\x8B\xEC\x56\x57\x8B\x7D\x08\x68\x95\x00\x00\x00\x57\xE8\x00\x00\x00\x00\x83\xC4\x08\x8B\xF0", "xxxxxxxxxxxxxxx????xxxxx");
        const auto mission_map_create = find("\x55\x8B\xEC\x83\xEC\x24\x56\x57\x8B\xF9\x6A\x04\xFF\x37\xE8\x00\x00\x00\x00\x6A\x00\x50\xE8", "xxxxxxxxxxxxxxx????xxxx");
        const auto agent_advance = find("\x55\x8B\xEC\x83\x7D\x08\x00\x7D\x00\x6A\x37\xBA\x00\x00\x00\x00\xB9\x00\x00\x00\x00\xE8", "xxxxxxxx?xxx????x????x");
        const auto observe_agent = find("\x55\x8B\xEC\xE8\x00\x00\x00\x00\xFF\x75\x08\x8B\x48\x2C\x81\xC1\x44\x06\x00\x00\xE8\x00\x00\x00\x00\x83\x7D\x0C\x00", "xxxx????xxxxxxxxxxxxx????xxxx");
        const std::pair<const char*, bool> checks[] = {
            {"the game connect", game_connect != 0},
            {"the game connection's goodbye", goodbye != 0},
            {"the network event post", post_net_event != 0},
            {"the network callback", net_callback != 0},
            {"the connection's ready step", ready_step != 0 && ready_cipher == ready_step + 0xA5},
            {"the connection's receive", receive != 0 && receive_cipher == receive + 0x89},
            {"the instance acknowledgement", instance_ack != 0},
            {"the map loaded message", map_loaded != 0},
            {"the transfer handler", transfer_handler != 0},
            {"the world map's toggle", world_map_toggle != 0},
            {"the mission map's creation", mission_map_create != 0},
            {"the Observe window's hook", ObserveGame_Func != nullptr},
            {"the game's properties", GameProps_Func != nullptr},
            {"the transfer message's layout", message_table.size() > GAME_SMSG_TRANSFER_GAME_SERVER_INFO && message_table[GAME_SMSG_TRANSFER_GAME_SERVER_INFO] == transfer_fields},
        };
        for (const auto& [name, found] : checks) {
            if (!found) {
                return std::format("Playback not available: {} is not found in this client", name);
            }
        }
        game_connection_global = *reinterpret_cast<uint8_t***>(goodbye + 0x4B);
        net_context_global = *reinterpret_cast<void***>(net_callback + 0x50);
        PostNetEvent_Func = reinterpret_cast<PostNetEvent_pt>(post_net_event);
        GameConnect_Func = reinterpret_cast<GameConnect_pt>(game_connect);
        Goodbye_Func = reinterpret_cast<Goodbye_pt>(goodbye);
        InstanceAck_Func = reinterpret_cast<InstanceAck_pt>(instance_ack);
        MapLoaded_Func = reinterpret_cast<MapLoaded_pt>(map_loaded);
        WorldMapToggle_Func = reinterpret_cast<WorldMapToggle_pt>(world_map_toggle);
        MissionMapCreate_Func = reinterpret_cast<MissionMapCreate_pt>(mission_map_create);
        AgentAdvance_Func = reinterpret_cast<AgentAdvance_pt>(agent_advance);
        if (GW::Hook::CreateHook(&GameConnect_Func, OnGameConnect, &GameConnect_Ret) != 0 || GW::Hook::CreateHook(&Goodbye_Func, OnGoodbye, &Goodbye_Ret) != 0 ||
            GW::Hook::CreateHook(&InstanceAck_Func, OnInstanceAck, &InstanceAck_Ret) != 0 || GW::Hook::CreateHook(&MapLoaded_Func, OnMapLoaded, &MapLoaded_Ret) != 0 ||
            GW::Hook::CreateHook(&WorldMapToggle_Func, OnWorldMapToggle, &WorldMapToggle_Ret) != 0 || GW::Hook::CreateHook(&MissionMapCreate_Func, OnMissionMapCreate, &MissionMapCreate_Ret) != 0 ||
            (AgentAdvance_Func && GW::Hook::CreateHook(&AgentAdvance_Func, OnAgentAdvance, &AgentAdvance_Ret) != 0)) {
            DisarmPlayback();
            return "Playback not available: its hooks could not be made";
        }
        for (const auto hooked : {reinterpret_cast<void*>(GameConnect_Func), reinterpret_cast<void*>(Goodbye_Func), reinterpret_cast<void*>(InstanceAck_Func), reinterpret_cast<void*>(MapLoaded_Func),
                                  reinterpret_cast<void*>(WorldMapToggle_Func), reinterpret_cast<void*>(MissionMapCreate_Func), reinterpret_cast<void*>(AgentAdvance_Func)}) {
            if (hooked) {
                GW::Hook::EnableHooks(hooked);
            }
        }
        // s2c 172's handler passes {agent, u8} to the char client's observe call, which sets the viewpoint the camera follows (0x00811a20, x23-live, x25-live).
        const auto follow_missing = message_table.size() <= viewpoint_message_id || message_table[viewpoint_message_id] != viewpoint_fields ? "s2c 172's fields are not {agent, u8} in this client"
                                    : !observe_agent                                                                                     ? "the char client's observe call is not found in this client"
                                    : !game_viewpoint_handler                                                                            ? "s2c 172's handler was not read before the recorder's callbacks"
                                    : memcmp(reinterpret_cast<const void*>(game_viewpoint_handler), "\x55\x8B\xEC\x8B\x45\x08\xFF\x70\x08\xFF\x70\x04\xE8", 13) != 0
                                        ? "s2c 172's handler is not the viewpoint handler's code (or another module replaced it before the recorder read it)"
                                    : GW::Scanner::FunctionFromNearCall(game_viewpoint_handler + 12) != observe_agent ? "s2c 172's handler does not call the char client's observe call"
                                                                                                                       : "";
        viewpoint_available = !*follow_missing;
        playback_armed = true;
        return std::format("Ready: Observe on one of your recordings plays it{}{}", viewpoint_available ? "" : std::format(" (following other agents is not available: {})", follow_missing),
                           AgentAdvance_Func ? "" : " (pause and speeds run on the toolbox's clock: the agents' clock is not found in this client)");
    }

    void StartPlayback(const ReplayObserve::Row& row)
    {
        ReplayFile::Contents contents;
        std::string error;
        auto made = ReplayFile::Read(row.path, contents, error) ? ReplayWire::MakePlan(contents) : ReplayWire::Plan{.error = error};
        const auto name = TextUtils::WStringToString(row.path.filename().wstring());
        if (made.error.empty() && made.map_type != ReplayWire::game_map_type) {
            made.error = std::format("its map type is {}, not a game map's: the observer's party window asserts the observed map is one", made.map_type);
        }
        if (!made.error.empty()) {
            playback_status = std::format("{} cannot be played: {}", name, made.error);
            Log::Error("Replay Recorder: %s", playback_status.c_str());
            Log::FlushFile();
            GW::UI::SendUIMessage(GW::UI::UIMessage::kMessage_0x1000010d);
            return;
        }
        plan = std::move(made);
        playing_path = row.path;
        playing_row = row;
        playback_ended = false;
        followed_agent = plan.controlled_agent;
        last_target = 0;
        world_map_refusal_logged = false;
        mission_map_refusal_logged = false;
        join_wait_logged = false;
        next_post = 0;
        played_ms = 0;
        playback_paused = false;
        transfer = {};
        const uint8_t loopback[] = {address_family_ipv4, 0, closed_port >> 8, closed_port & 0xFF, 127, 0, 0, 1};
        memcpy(transfer.address, loopback, sizeof(loopback));
        // MissionClient::GameJoin keeps the 422's +0x1c at +0x194; it reaches only the connect data our hook never sends and the reconnect record.
        const auto mission = MissionContext();
        transfer.game_type = mission ? *reinterpret_cast<uint32_t*>(mission + mission_join_value_offset) : 0;
        const auto joined_map = JoinedMap();
        transfer.territory = contents.header.region;
        transfer.map_id = contents.header.map_id;
        transfer.map_type = contents.header.is_explorable;
        transfer.key = RandomBytes()();
        transfer_due = true;
        playback_status = std::format("Playing {}", name);
        std::string changes;
        for (const auto& change : plan.changes) {
            changes += change.kind == ReplayWire::ChangeKind::LeftOut ? std::format("; left out: {} records from record {} ({})", change.count, change.record, change.why)
                                                                      : std::format("; record {}: 410's observer byte set ({})", change.record, change.why);
        }
        LogPlayback(
            std::format(
                "{}: {} posts, the recording as recorded from record {} ({} ms), its stream timed from its 31 at {} ms, {} ms long, controlled agent {}, {} of the recorder's own records{}", playback_status, plan.posts.size(), plan.first_record,
                plan.start_ms, plan.entry_ms, plan.duration_ms, plan.controlled_agent, plan.own_records_streamed, changes
            )
        );
        LogPlayback(std::format("transfer to map {}; the 422's +0x1c from the outpost's own join: {:#010x} (the join's map {}, the map now {})", transfer.map_id, transfer.game_type, joined_map, static_cast<uint32_t>(GW::Map::GetMapID())));
        EnterStage(Playback::Transfer);
    }

    void UpdatePlayback()
    {
        const auto now = Clock::now();
        const auto waited = now - stage_since;
        const auto stop = playback_stop_requested || !settings.play_from_observe_window || !settings.list_in_observe_window;
        switch (playback.load()) {
            case Playback::Off:
                return;
            case Playback::Transfer: {
                const auto flags = MissionFlags();
                if (transfer_due) {
                    // The 422 handler only keeps a transfer while a join is pending (+0x190 & 0x20) and runs it when that join ends (event 0x1e, MsCliGame.cpp).
                    if (flags & (join_pending | transfer_kept)) {
                        if (!join_wait_logged) {
                            join_wait_logged = true;
                            LogPlayback(std::format("waiting for the game's own join to end before the transfer (mission flags {:#x})", flags));
                        }
                        if (waited > connect_timeout || stop) {
                            FinishPlayback(std::format("The game was still joining a map (mission flags {:#x}): the replay was not started", flags));
                        }
                        return;
                    }
                    transfer_due = false;
                    stage_since = now;
                    emulating_rows = true;
                    GW::StoC::EmulatePacket(reinterpret_cast<PacketBase*>(&transfer));
                    emulating_rows = false;
                    LogPlayback(std::format("the transfer (s2c 422) emulated: mission flags {:#x} before, {:#x} after, the join's map {}", flags, MissionFlags(), JoinedMap()));
                }
                else if (waited > (flags & (join_pending | transfer_kept) ? transfer_wait : connect_timeout)) {
                    FinishPlayback(std::format("The game did not start the replay's connection (mission flags {:#x}, the join's map {}, a game connection {})", flags, JoinedMap(),
                                               *game_connection_global ? "open" : "none"));
                }
                return;
            }
            case Playback::Connecting: {
                const auto connection = *game_connection_global;
                if (connection && *reinterpret_cast<uint32_t*>(connection + msgconn_mode_offset) == msgconn_encrypted) {
                    if (*reinterpret_cast<uint32_t*>(connection) != 0) {
                        CloseReplayConnection("The game's connection is not the replay's: playback stopped");
                        return;
                    }
                    replay_connection = connection;
                    memcpy(&send_cipher, connection + msgconn_receive_cipher_offset, sizeof(send_cipher));
                    LogPlayback("the replay's connection is ready: its receive cipher copied");
                    EnterStage(Playback::Acknowledging);
                    PostStage(ReplayWire::Stage::BeforeLoad);
                }
                else if (waited > connect_timeout || stop) {
                    CloseReplayConnection("The replay's connection did not become ready");
                }
                return;
            }
            case Playback::Acknowledging:
            case Playback::Loading:
                if (*game_connection_global != replay_connection) {
                    FinishPlayback("The replay's connection is gone");
                }
                else if (waited > load_timeout || stop) {
                    CloseReplayConnection("The replay's map did not load");
                }
                return;
            case Playback::Playing: {
                const auto elapsed = std::chrono::duration<double, std::milli>(now - last_update).count();
                last_update = now;
                if (stop) {
                    Log::Info("Replay Recorder: %s", restart_pending ? "playback restarting: back to the outpost, then the same recording from its start" : "playback stopped: back to the outpost");
                    Log::FlushFile();
                    StopObserving();
                    return;
                }
                if (!AgentAdvance_Func && !playback_paused) {
                    played_ms = std::min(played_ms + elapsed * playback_speed, static_cast<double>(plan.duration_ms));
                }
                for (; next_post < plan.posts.size() && plan.posts[next_post].time_ms <= played_ms; next_post++) {
                    auto bytes = plan.posts[next_post].bytes;
                    send_cipher.Apply(bytes.data(), bytes.size());
                    PostNetEvent(net_event_data, replay_connection_id, bytes.data(), bytes.size());
                }
                if (next_post == plan.posts.size() && !playback_ended) {
                    playback_ended = true;
                    playback_paused = true;
                    played_ms = plan.duration_ms;
                    Log::Info("Replay Recorder: the replay has ended: paused on its last moment until Stop or Restart");
                    Log::FlushFile();
                }
                const auto target = GW::Agents::GetTargetId();
                if (viewpoint_available && settings.follow_selected_agent && target != last_target) {
                    last_target = target;
                    if (target && target != followed_agent && GW::Agents::GetAgentByID(target)) {
                        FollowViewpoint(target);
                    }
                }
                return;
            }
            case Playback::Ending:
                if (waited > close_timeout) {
                    CloseReplayConnection("The game did not leave the replay: its connection closed by the toolbox");
                }
                return;
        }
    }

    void __cdecl OnObserveGame(const uint32_t game_key)
    {
        GW::Hook::EnterHook();
        std::unique_lock lock(recording_mutex);
        const auto found = listed_rows.find(game_key);
        if (found == listed_rows.end()) {
            lock.unlock();
            ObserveGame_Ret(game_key);
            return GW::Hook::LeaveHook();
        }
        const auto row = found->second;
        lock.unlock();
        if (playback_armed && playback == Playback::Off) {
            StartPlayback(row);
            return GW::Hook::LeaveHook();
        }
        const auto& map_name = Resources::GetMapName(static_cast<GW::Constants::MapID>(row.header.map_id))->wstring();
        const auto answer = std::format(L"Replay {}: {}, {} long. Turn on \"Play recordings from the Observe window\" in the Replay Recorder's settings to play it.", row.path.filename().wstring(),
                                        map_name.empty() ? std::format(L"map {}", row.header.map_id) : map_name, ReplayObserve::Duration(row.header.duration_ms));
        Log::InfoW(L"%ls", answer.c_str());
        GW::UI::SendUIMessage(GW::UI::UIMessage::kMessage_0x1000010d);
        GW::Hook::LeaveHook();
    }

    void OnObserveRow(GW::HookStatus*, PacketBase* packet, void*)
    {
        if (emulating_rows) {
            return;
        }
        std::scoped_lock lock(recording_mutex);
        server_keys.insert(reinterpret_cast<const ReplayObserve::RowMessage*>(packet)->key);
    }

    void OnObserveRowsCommitted(GW::HookStatus*, PacketBase*, void*)
    {
        if (emulating_rows) {
            return;
        }
        std::vector<ReplayObserve::RowMessage> messages;
        {
            std::scoped_lock lock(recording_mutex);
            listed_rows.clear();
            const auto taken_keys = std::exchange(server_keys, {});
            if (message_table.empty()) {
                observe_status = "Not listed: the client's message table is not read yet";
                return;
            }
            const auto now = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count();
            auto scan = ReplayObserve::ScanFolder(RecordingsFolder(), GW::MemoryMgr::GetGWVersion(), message_table_digest, taken_keys, now);
            for (auto& row : scan.rows) {
                if (auto error = ReplayObserve::CheckRow(message_table, row.message); !error.empty()) {
                    scan.left_out.push_back({row.path, std::move(error)});
                    continue;
                }
                messages.push_back(row.message);
                listed_rows.emplace(row.message.key, std::move(row));
            }
            for (const auto& [path, reason] : scan.left_out) {
                if (logged_left_out.insert(path).second) {
                    Log::Log("Replay Recorder: %s is not listed in the Observe window: %s", TextUtils::WStringToString(path.filename().wstring()).c_str(), reason.c_str());
                }
            }
            observe_status = std::format("{} recordings listed in the Observe window, {} left out (the reasons are in the toolbox's log)", messages.size(), scan.left_out.size());
        }
        emulating_rows = true;
        for (auto& message : messages) {
            Resources::GetMapName(static_cast<GW::Constants::MapID>(message.map_id));
            GW::StoC::EmulatePacket(reinterpret_cast<PacketBase*>(&message));
        }
        emulating_rows = false;
    }

    void DropListedRows()
    {
        std::unique_lock lock(recording_mutex);
        const auto listed = std::exchange(listed_rows, {});
        server_keys.clear();
        lock.unlock();
        const auto char_context = GW::GetCharContext();
        if (listed.empty() || !char_context) {
            return;
        }
        const auto in_client_table = std::ranges::any_of(char_context->observer_matches, [&listed](const GW::ObserverMatch* match) {
            return match && listed.contains(match->match_id);
        });
        if (!in_client_table) {
            return;
        }
        // An empty list committed clears the client's table: without the hook, Observe on a row of ours would send its key to the server.
        PacketBase commit{ReplayObserve::rows_committed_message_id};
        emulating_rows = true;
        GW::StoC::EmulatePacket(&commit);
        emulating_rows = false;
    }

    std::string StartListing()
    {
        const auto observe_game = GW::Scanner::Find("\x55\x8B\xEC\xE8\x00\x00\x00\x00\xFF\x75\x08\x8B\x48\x44\x81\xC1\x58\x02\x00\x00\xE8\x00\x00\x00\x00\x85\xC0", "xxxx????xxxxxxxxxxxxx????xx");
        ObserveGame_Func = reinterpret_cast<ObserveGame_pt>(observe_game);
        GameProps_Func = observe_game ? reinterpret_cast<GameProps_pt>(GW::Scanner::FunctionFromNearCall(observe_game + 3)) : nullptr;
        if (!ObserveGame_Func || GW::Hook::CreateHook(&ObserveGame_Func, OnObserveGame, &ObserveGame_Ret) != 0) {
            ObserveGame_Func = nullptr;
            Log::Log("Replay Recorder: the game's Observe function is not found in this client; recordings are not listed");
            return "Not listed: the game's Observe function is not found in this client";
        }
        GW::Hook::EnableHooks(ObserveGame_Func);
        if (!GW::StoC::RegisterPacketCallback(&observe_hook_entry, ReplayObserve::row_message_id, OnObserveRow) || !GW::StoC::RegisterPacketCallback(&observe_hook_entry, ReplayObserve::rows_committed_message_id, OnObserveRowsCommitted)) {
            GW::StoC::RemoveCallbacks(&observe_hook_entry);
            GW::Hook::RemoveHook(ObserveGame_Func);
            ObserveGame_Func = nullptr;
            return "Not listed: the Observe window's messages cannot be watched";
        }
        return "Waiting for the Observe window";
    }
}

void ReplayRecorderModule::Initialize()
{
    ToolboxModule::Initialize();
    SettingsRegistry::Register(this, settings);
    std::scoped_lock lock(recording_mutex);
    RegisterCallbacks();
}

ReplayRecorderModule::PlaybackView ReplayRecorderModule::GetPlayback() const
{
    const auto stage = playback.load();
    return {
        .active = stage != Playback::Off || restart_pending,
        .playing = stage == Playback::Playing,
        .ended = playback_ended,
        .paused = playback_paused,
        .restarting = restart_pending,
        .can_follow = viewpoint_available,
        .speed = playback_speed,
        .played_ms = static_cast<uint32_t>(played_ms),
        .length_ms = plan.duration_ms,
        .recorder_agent = plan.controlled_agent,
        .followed_agent = followed_agent,
        .name = TextUtils::WStringToString(playing_path.filename().wstring()),
        .status = playback_status,
    };
}

void ReplayRecorderModule::SetPlaybackPaused(const bool paused)
{
    playback_paused = paused && !playback_ended;
}

void ReplayRecorderModule::SetPlaybackSpeed(const int speed)
{
    playback_speed = std::clamp(speed, 1, 8);
}

void ReplayRecorderModule::StopPlayback()
{
    restart_pending = false;
    playback_stop_requested = true;
}

void ReplayRecorderModule::RestartPlayback()
{
    restart_pending = true;
    playback_stop_requested = true;
}

void ReplayRecorderModule::FollowAgent(const uint32_t agent_id)
{
    if (viewpoint_available && playback == Playback::Playing && agent_id) {
        FollowViewpoint(agent_id);
    }
}

bool* ReplayRecorderModule::FollowSelectedAgent()
{
    return &settings.follow_selected_agent;
}

void ReplayRecorderModule::SignalTerminate()
{
    playback_stop_requested = true;
    restart_pending = false;
    GW::StoC::RemoveCallbacks(&stoc_hook_entry);
    GW::StoC::RemoveCallbacks(&observe_hook_entry);
    DropListedRows();
    std::scoped_lock lock(recording_mutex);
    FinishRecording();
    callbacks_registered = false;
}

bool ReplayRecorderModule::CanTerminate()
{
    return playback == Playback::Off;
}

void ReplayRecorderModule::Terminate()
{
    ToolboxModule::Terminate();
    if (playback != Playback::Off) {
        // The client's exit closes the socket of every GcSrv entry (0x004920f0) and asserts on ours, which has none: it must be gone first.
        CloseReplayConnection("The toolbox closed during a replay: the replay's connection closed before the game's own");
    }
    DisarmPlayback();
    if (ObserveGame_Func) {
        GW::Hook::RemoveHook(ObserveGame_Func);
        ObserveGame_Func = nullptr;
    }
    observe_applied = false;
}

void ReplayRecorderModule::Update(float)
{
    if (settings.list_in_observe_window != observe_applied) {
        observe_applied = settings.list_in_observe_window;
        std::string applied_status;
        if (observe_applied) {
            applied_status = StartListing();
        }
        else {
            GW::StoC::RemoveCallbacks(&observe_hook_entry);
            DropListedRows();
            if (ObserveGame_Func) {
                GW::Hook::RemoveHook(ObserveGame_Func);
                ObserveGame_Func = nullptr;
            }
        }
        std::scoped_lock lock(recording_mutex);
        observe_status = std::move(applied_status);
    }
    UpdatePlayback();
    const auto play = settings.play_from_observe_window && settings.list_in_observe_window && ObserveGame_Func;
    const auto settled = GW::Map::GetInstanceType() == GW::Constants::InstanceType::Outpost && GW::Map::GetIsMapLoaded() && !GW::Map::GetIsObserving() && GameProps_Func &&
                         !(MissionFlags() & (join_pending | transfer_kept));
    if (!settled) {
        outpost_settled_since.reset();
    }
    else if (!outpost_settled_since) {
        outpost_settled_since = Clock::now();
    }
    if (restart_pending && playback == Playback::Off) {
        if (!play || !playback_armed) {
            restart_pending = false;
        }
        else if (outpost_settled_since && Clock::now() - *outpost_settled_since > restart_settle) {
            restart_pending = false;
            LogPlayback("restart: back in an outpost whose join has ended");
            StartPlayback(playing_row);
        }
    }
    if (play && !playback_armed && playback_status.empty()) {
        playback_status = ArmPlayback();
        LogPlayback(playback_status);
    }
    else if (!play && playback == Playback::Off && (playback_armed || !playback_status.empty())) {
        DisarmPlayback();
        playback_status.clear();
    }
    std::scoped_lock lock(recording_mutex);
    if (!callbacks_registered) {
        RegisterCallbacks();
    }
    if (!writer.IsOpen()) {
        return;
    }
    const auto now = Clock::now();
    if (now - last_record_at > idle_timeout || GW::GetPreGameContext()) {
        FinishRecording();
        return;
    }
    if (now - last_flush_at > flush_interval) {
        writer.Flush();
        last_flush_at = now;
    }
}

void ReplayRecorderModule::LoadSettings(SettingsDoc& doc, ToolboxIni* legacy)
{
    ToolboxModule::LoadSettings(doc, legacy);
    doc.GetStruct(Name(), settings);
}

void ReplayRecorderModule::SaveSettings(SettingsDoc& doc)
{
    ToolboxModule::SaveSettings(doc);
    doc.SetStruct(Name(), settings);
}

void ReplayRecorderModule::DrawSettingsInternal()
{
    std::scoped_lock lock(recording_mutex);
    ImGui::Checkbox("Record explorable areas", &settings.record_explorables);
    ImGui::Checkbox("Record outposts", &settings.record_outposts);
    ImGui::Checkbox("Keep other players' names and chat out of recordings", &settings.strip_names_and_chat);
    ImGui::ShowHelp("Other players' names become \"Player 1\", \"Player 2\"...; player chat and speech bubbles are left out.");
    ImGui::Checkbox("Keep my own names out of recordings", &settings.strip_own_names);
    ImGui::ShowHelp("Your character's and account's names become \"Recorder\", your mercenary heroes' names \"Mercenary 1\"... With either setting on, guild names become \"Guild 1\"... and guild tags \"G1\"...");
    ImGui::InputText("Folder", settings.folder, MAX_PATH);
    ImGui::SameLine();
    if (ImGui::Button("Open")) {
        const auto folder = RecordingsFolder();
        if (Resources::EnsureFolderExists(folder)) {
            ShellExecuteW(nullptr, L"open", folder.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
        }
    }
    if (settings.folder.empty()) {
        ImGui::TextDisabled("Empty: %s", TextUtils::WStringToString(RecordingsFolder().wstring()).c_str());
    }
    if (ImGui::InputInt("Size cap per recording (MB)", &settings.size_cap_mb)) {
        settings.size_cap_mb = std::clamp(settings.size_cap_mb, 1, 4096);
    }
    ImGui::TextDisabled("PvP maps are never recorded. A recording starts on entering a map and ends on leaving it.");
    ImGui::Checkbox("List recordings in the Observe window", &settings.list_in_observe_window);
    ImGui::ShowHelp("Recordings made on this client build appear in the Observe window (B, in an outpost) under \"Custom Event (0)\". Choosing one is answered here; nothing is sent to the server.");
    if (!observe_status.empty()) {
        ImGui::TextDisabled("%s", observe_status.c_str());
    }
    ImGui::Checkbox("Play recordings from the Observe window", &settings.play_from_observe_window);
    ImGui::ShowHelp("Observe on one of your recordings plays it in the game's observer mode, fed by the toolbox: nothing of the replay goes to the server. The Replay window shows while it plays; its Stop, or stop observing (B, then Yes), brings you back to your outpost. Needs the list above.");
    if (!playback_status.empty()) {
        ImGui::TextDisabled("%s", playback_status.c_str());
    }
    if (playback != Playback::Off) {
        ImGui::TextDisabled("A replay is playing: its controls are in the Replay window.");
    }
    if (writer.IsOpen()) {
        ImGui::Text("Recording %s: %u messages, %llu KB, %u layout errors", RecordingName().c_str(), header.record_count, writer.Size() / 1024, header.layout_errors);
    }
    else {
        ImGui::Text("%s", status.c_str());
    }
}
