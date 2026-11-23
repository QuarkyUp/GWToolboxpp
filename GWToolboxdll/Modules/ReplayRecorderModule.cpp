#include "stdafx.h"

#include <GWCA/Context/CharContext.h>

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
    constexpr wchar_t literal_start = 0x107;
    constexpr wchar_t literal_end = 0x1;

    ReplayRecorderModule::Settings settings;

    GW::HookEntry stoc_hook_entry;
    bool callbacks_registered = false;
    ReplayFile::MessageTable message_table;
    uint64_t message_table_digest = 0;
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
    std::wstring own_name;
    std::unordered_map<std::wstring, std::wstring> player_labels;
    std::string status = "Waiting for the next map";

    using ObserveGame_pt = void(__cdecl*)(uint32_t game_key);
    ObserveGame_pt ObserveGame_Func = nullptr;
    ObserveGame_pt ObserveGame_Ret = nullptr;
    GW::HookEntry observe_hook_entry;
    bool observe_applied = false;
    std::atomic_bool emulating_rows = false;
    std::atomic_bool playing_replay = false;
    uint32_t last_game_type = 0;
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

    bool IsPlainText(const std::wstring_view text)
    {
        return !text.empty() && std::ranges::all_of(text, [](const wchar_t c) {
            return c >= 0x20 && c < 0x100;
        });
    }

    void StripPlayerNames(std::wstring& text)
    {
        const auto label_of = [](const std::wstring_view name) {
            if (name == own_name) {
                return std::wstring(name);
            }
            auto& label = player_labels[std::wstring(name)];
            if (label.empty()) {
                label = std::format(L"Player {}", player_labels.size());
            }
            return label;
        };
        if (IsPlainText(text)) {
            text = label_of(text);
            return;
        }
        std::wstring stripped;
        size_t position = 0;
        for (auto start = text.find(literal_start); start != std::wstring::npos; start = text.find(literal_start, position)) {
            const auto end = text.find(literal_end, start + 1);
            if (end == std::wstring::npos) {
                break;
            }
            stripped.append(text, position, start + 1 - position);
            const auto literal = std::wstring_view(text).substr(start + 1, end - start - 1);
            stripped += IsPlainText(literal) ? label_of(literal) : std::wstring(literal);
            position = end;
        }
        stripped.append(text, position);
        text = std::move(stripped);
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
        header.flags = settings.strip_names_and_chat ? ReplayFile::NamesStripped | ReplayFile::ChatDropped : 0;
        strncpy(header.toolbox_version, GWTOOLBOXDLL_VERSION GWTOOLBOXDLL_VERSION_BETA, sizeof(header.toolbox_version) - 1);
        if (!writer.Open(recording_path, header, message_table)) {
            status = std::format("Could not create {}", RecordingName());
            Log::Error("Replay Recorder: %s", status.c_str());
            return;
        }
        const auto char_context = GW::GetCharContext();
        own_name = char_context ? char_context->player_name : L"";
        player_labels.clear();
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
        const auto names_stripped = (header.flags & ReplayFile::NamesStripped) != 0;
        if (!ReplayFile::PackMessage(packet->header, fields.data(), static_cast<uint32_t>(fields.size()), reinterpret_cast<const uint8_t*>(packet), out,
                                     names_stripped ? ReplayFile::TextFilter(StripPlayerNames) : ReplayFile::TextFilter())) {
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
            last_game_type = static_cast<const GW::Packet::StoC::GameSrvTransfer*>(packet)->token1;
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
    using PostNetEvent_pt = uint32_t(__cdecl*)(void* context, uint32_t connection, uint32_t kind, const void* data, uint32_t size);

    constexpr uint32_t replay_connection_id = 0x7E500001;
    constexpr uint16_t closed_port = 1;
    constexpr uint8_t address_family_ipv4 = 2;
    constexpr uint32_t net_event_connected = 5;
    constexpr uint32_t net_event_closed = 6;
    constexpr uint32_t net_event_data = 8;
    constexpr uint32_t mission_flags_offset = 0x190;
    constexpr uint32_t instance_acknowledged = 0xC0;
    constexpr uint32_t msgconn_mode_offset = 0x60;
    constexpr uint32_t msgconn_receive_cipher_offset = 0x7C;
    constexpr uint32_t msgconn_encrypted = 2;
    constexpr auto connect_timeout = std::chrono::seconds(15);
    constexpr auto load_timeout = std::chrono::seconds(120);
    constexpr auto close_timeout = std::chrono::seconds(20);
    const std::vector<uint32_t> transfer_fields = {0x1a6, 0x1805, 0x404, 0x104, 0x204, 0x104, 0x404, 0x104};

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

    void PostStage(const ReplayWire::Stage stage)
    {
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
            PostStage(ReplayWire::Stage::Entry);
            played_ms = 0;
            last_update = Clock::now();
            EnterStage(Playback::Playing);
        }
        GW::Hook::LeaveHook();
    }

    void DisarmPlayback()
    {
        for (const auto hooked : {reinterpret_cast<void*>(GameConnect_Func), reinterpret_cast<void*>(Goodbye_Func), reinterpret_cast<void*>(InstanceAck_Func), reinterpret_cast<void*>(MapLoaded_Func)}) {
            if (hooked) {
                GW::Hook::RemoveHook(hooked);
            }
        }
        GameConnect_Func = nullptr;
        Goodbye_Func = nullptr;
        InstanceAck_Func = nullptr;
        MapLoaded_Func = nullptr;
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
            {"the Observe window's hook", ObserveGame_Func != nullptr},
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
        if (GW::Hook::CreateHook(&GameConnect_Func, OnGameConnect, &GameConnect_Ret) != 0 || GW::Hook::CreateHook(&Goodbye_Func, OnGoodbye, &Goodbye_Ret) != 0 ||
            GW::Hook::CreateHook(&InstanceAck_Func, OnInstanceAck, &InstanceAck_Ret) != 0 || GW::Hook::CreateHook(&MapLoaded_Func, OnMapLoaded, &MapLoaded_Ret) != 0) {
            DisarmPlayback();
            return "Playback not available: its hooks could not be made";
        }
        for (const auto hooked : {reinterpret_cast<void*>(GameConnect_Func), reinterpret_cast<void*>(Goodbye_Func), reinterpret_cast<void*>(InstanceAck_Func), reinterpret_cast<void*>(MapLoaded_Func)}) {
            GW::Hook::EnableHooks(hooked);
        }
        playback_armed = true;
        return "Ready: Observe on one of your recordings plays it";
    }

    void StartPlayback(const ReplayObserve::Row& row)
    {
        ReplayFile::Contents contents;
        std::string error;
        auto made = ReplayFile::Read(row.path, contents, error) ? ReplayWire::MakePlan(contents) : ReplayWire::Plan{.error = error};
        const auto name = TextUtils::WStringToString(row.path.filename().wstring());
        if (!made.error.empty()) {
            playback_status = std::format("{} cannot be played: {}", name, made.error);
            Log::Error("Replay Recorder: %s", playback_status.c_str());
            GW::UI::SendUIMessage(GW::UI::UIMessage::kMessage_0x1000010d);
            return;
        }
        plan = std::move(made);
        playing_path = row.path;
        next_post = 0;
        played_ms = 0;
        playback_paused = false;
        transfer = {};
        const uint8_t loopback[] = {address_family_ipv4, 0, closed_port >> 8, closed_port & 0xFF, 127, 0, 0, 1};
        memcpy(transfer.address, loopback, sizeof(loopback));
        transfer.game_type = last_game_type;
        transfer.territory = contents.header.region;
        transfer.map_id = contents.header.map_id;
        transfer.map_type = contents.header.is_explorable;
        transfer.key = RandomBytes()();
        transfer_due = true;
        playback_status = std::format("Playing {}", name);
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
            case Playback::Transfer:
                if (transfer_due) {
                    transfer_due = false;
                    stage_since = now;
                    emulating_rows = true;
                    GW::StoC::EmulatePacket(reinterpret_cast<PacketBase*>(&transfer));
                    emulating_rows = false;
                }
                else if (waited > connect_timeout) {
                    FinishPlayback("The game did not start the replay's connection");
                }
                return;
            case Playback::Connecting: {
                const auto connection = *game_connection_global;
                if (connection && *reinterpret_cast<uint32_t*>(connection + msgconn_mode_offset) == msgconn_encrypted) {
                    if (*reinterpret_cast<uint32_t*>(connection) != 0) {
                        CloseReplayConnection("The game's connection is not the replay's: playback stopped");
                        return;
                    }
                    replay_connection = connection;
                    memcpy(&send_cipher, connection + msgconn_receive_cipher_offset, sizeof(send_cipher));
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
                if (!playback_paused) {
                    played_ms += elapsed * playback_speed;
                }
                for (; next_post < plan.posts.size() && plan.posts[next_post].time_ms <= played_ms && !stop; next_post++) {
                    auto bytes = plan.posts[next_post].bytes;
                    send_cipher.Apply(bytes.data(), bytes.size());
                    PostNetEvent(net_event_data, replay_connection_id, bytes.data(), bytes.size());
                }
                if (next_post == plan.posts.size() || stop) {
                    Log::Info("Replay Recorder: %s", stop ? "playback stopped: back to the outpost" : "the replay has ended: back to the outpost");
                    StopObserving();
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
        ObserveGame_Func = reinterpret_cast<ObserveGame_pt>(GW::Scanner::Find("\x55\x8B\xEC\xE8\x00\x00\x00\x00\xFF\x75\x08\x8B\x48\x44\x81\xC1\x58\x02\x00\x00\xE8\x00\x00\x00\x00\x85\xC0", "xxxx????xxxxxxxxxxxxx????xx"));
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

void ReplayRecorderModule::SignalTerminate()
{
    playback_stop_requested = true;
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
    if (play && !playback_armed && playback_status.empty()) {
        playback_status = ArmPlayback();
        Log::Log("Replay Recorder: %s", playback_status.c_str());
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
    if (now - last_record_at > idle_timeout) {
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
    ImGui::ShowHelp("Observe on one of your recordings plays it in the game's observer mode, fed by the toolbox: nothing of the replay goes to the server. Stop observing (B, then Yes) brings you back to your outpost. Needs the list above.");
    if (!playback_status.empty()) {
        ImGui::TextDisabled("%s", playback_status.c_str());
    }
    if (playback == Playback::Playing) {
        ImGui::Text("%s / %s", TextUtils::WStringToString(ReplayObserve::Duration(static_cast<uint32_t>(played_ms))).c_str(), TextUtils::WStringToString(ReplayObserve::Duration(plan.duration_ms)).c_str());
        ImGui::SameLine();
        if (ImGui::Button(playback_paused ? "Resume" : "Pause")) {
            playback_paused = !playback_paused;
        }
        for (const auto speed : {1, 2, 4}) {
            ImGui::SameLine();
            if (ImGui::RadioButton(std::format("x{}", speed).c_str(), playback_speed == speed)) {
                playback_speed = speed;
            }
        }
        ImGui::SameLine();
        if (ImGui::Button("Stop")) {
            playback_stop_requested = true;
        }
    }
    if (writer.IsOpen()) {
        ImGui::Text("Recording %s: %u messages, %llu KB, %u layout errors", RecordingName().c_str(), header.record_count, writer.Size() / 1024, header.layout_errors);
    }
    else {
        ImGui::Text("%s", status.c_str());
    }
}
