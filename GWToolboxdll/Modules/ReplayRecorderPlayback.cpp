#include "stdafx.h"

#include <GWCA/Constants/UIMessages.h>
#include <GWCA/Context/CharContext.h>
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
#include <Logger.h>
#include <Modules/ReplayRecorderInternal.h>
#include <Modules/ReplayRecorderModule.h>
#include <Modules/Resources.h>
#include <Utils/Replay/ReplayFile.h>
#include <Utils/Replay/ReplayMessages.h>
#include <Utils/Replay/ReplayObserve.h>
#include <Utils/Replay/ReplayWire.h>
#include <Utils/TextUtils.h>
#include "stdafx.h"

namespace ReplayRecorder {
    namespace {
        // The game connection's MsgConn (MsgConnConnectAccept's allocation): no socket in playback, the mode 2 once ready, the receive cipher (t03, t09).
        struct MsgConn {
            uint32_t socket;
            uint8_t h0004[0x5C];
            uint32_t mode;
            uint8_t h0064[0x18];
            ReplayWire::Rc4 receive_cipher;
        };
        static_assert(offsetof(MsgConn, mode) == 0x60 && offsetof(MsgConn, receive_cipher) == 0x7C);

        ObserveGame_pt ObserveGame_Ret = nullptr;

        std::set<uint32_t> server_keys;

        std::map<uint32_t, ReplayObserve::Row> listed_rows;

        std::set<std::filesystem::path> logged_left_out;

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

        constexpr uint32_t instance_acknowledged = 0xC0;

        constexpr uint32_t join_pending = 0x20;

        constexpr uint32_t transfer_kept = 0x10;

        constexpr uint32_t msgconn_encrypted = 2;

        constexpr auto connect_timeout = std::chrono::seconds(15);

        constexpr auto load_timeout = std::chrono::seconds(120);

        constexpr auto close_timeout = std::chrono::seconds(20);

        const std::vector<uint32_t> transfer_fields = {0x1a6, 0x1805, 0x404, 0x104, 0x204, 0x104, 0x404, 0x104};

        const std::vector<uint32_t> viewpoint_fields = {viewpoint_message_id, 0x10, 0x104};

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

        MsgConn** game_connection_global = nullptr;

        void** net_context_global = nullptr;

        std::atomic_bool playback_paused = false;

        std::atomic_int playback_speed = 1;

        ReplayWire::Plan plan;

        ReplayWire::Rc4 send_cipher;

        std::filesystem::path playing_path;

        ReplayFile::Contents replay;

        constexpr uint32_t no_seek = UINT32_MAX;

        std::atomic_uint32_t seek_request = no_seek;

        std::string playback_note;

        std::atomic_bool playback_ended = false;

        bool paused_by_end = false;

        bool viewpoint_available = false;

        std::atomic_uint32_t followed_agent = 0;

        uint32_t last_target = 0;

        bool world_map_refusal_logged = false;

        bool mission_map_refusal_logged = false;

        bool join_wait_logged = false;

        TransferMessage transfer;

        bool transfer_due = false;

        bool connection_open = false;

        bool stop_sent = false;

        MsgConn* replay_connection = nullptr;

        size_t next_post = 0;

        std::vector<ReplayWire::Post> placement;

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

        uint32_t MissionFlags()
        {
            const auto char_context = GW::GetCharContext();
            return char_context ? char_context->world_flags : 0;
        }

        uint32_t JoinedMap()
        {
            const auto char_context = GW::GetCharContext();
            return char_context ? static_cast<uint32_t>(char_context->map_id) : 0;
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
            replay = {};
            placement.clear();
            seek_request = no_seek;
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
            // A load while observing: the 422's join closes the current connection by this goodbye (GameJoin), and the connect hook makes the next one.
            if (connection_open && playback == Playback::Transfer) {
                connection_open = false;
                PostNetEvent(net_event_closed, replay_connection_id, nullptr, 0);
                LogPlayback("the game's join closed the previous load's connection");
            }
            else if (connection_open) {
                CloseReplayConnection(std::format("Left the replay of {}", TextUtils::WStringToString(playing_path.filename().wstring())));
            }
            GW::Hook::LeaveHook();
        }

        void __cdecl OnInstanceAck(uint8_t* mission)
        {
            GW::Hook::EnterHook();
            InstanceAck_Ret(mission);
            if (playback == Playback::Acknowledging && (MissionFlags() & instance_acknowledged) == instance_acknowledged) {
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
                LogPlayback(std::format("c2s 144 due: the replay's map loaded; the stream to {} ms after the keyframe posted at once", plan.seek_ms));
                PostStage(ReplayWire::Stage::Entry);
                played_ms = std::min(plan.seek_ms, plan.duration_ms);
                last_update = Clock::now();
                EnterStage(Playback::Playing);
            }
            GW::Hook::LeaveHook();
        }

        void __cdecl OnWorldMapToggle(const uint32_t game_view, const int open)
        {
            GW::Hook::EnterHook();
            // The world map draws the world of the map left for the replay (+0x238) over explored areas a keyframe does not hold, and asserts they match (GmMapView.cpp:1731).
            if (open && playback != Playback::Off) {
                if (!world_map_refusal_logged) {
                    world_map_refusal_logged = true;
                    Log::Info("Replay Recorder: the world map stays closed during a replay: it would show the map left for the replay over explored areas a keyframe does not hold");
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
            // The agents' clock alone (AgApi.cpp's GAME_SMSG_AGENT_MOVEMENT_TICK: its trackers and timer queue); ArenaNet's connections run on the event timers, untouched.
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

        std::string TimeText(const uint32_t time_ms)
        {
            return TextUtils::WStringToString(ReplayObserve::Duration(time_ms));
        }

        void LoadKeyframe(ReplayWire::Plan made)
        {
            plan = std::move(made);
            next_post = 0;
            placement.clear();
            played_ms = 0;
            playback_ended = false;
            followed_agent = plan.controlled_agent;
            last_target = 0;
            join_wait_logged = false;
            transfer = {};
            const uint8_t loopback[] = {address_family_ipv4, 0, closed_port >> 8, closed_port & 0xFF, 127, 0, 0, 1};
            memcpy(transfer.address, loopback, sizeof(loopback));
            // MissionClient::GameJoin keeps the 422's +0x1c at +0x194; it reaches only the connect data our hook never sends and the reconnect record.
            const auto char_context = GW::GetCharContext();
            transfer.game_type = char_context ? char_context->token1 : 0;
            const auto& chapter = replay.chapters[plan.chapter];
            transfer.territory = replay.header.region;
            transfer.map_id = chapter.map_id;
            transfer.map_type = chapter.is_explorable;
            transfer.key = RandomBytes()();
            transfer_due = true;
            std::string changes;
            for (const auto& change : plan.changes) {
                changes += change.kind == ReplayWire::ChangeKind::LeftOut     ? std::format("; left out: {} records from record {} ({})", change.count, change.record, change.why)
                           : change.kind == ReplayWire::ChangeKind::BroughtIn ? std::format("; brought in: {} records before record {} ({})", change.count, change.record, change.why)
                                                                              : std::format("; message {}: 410's observer byte set ({})", change.record, change.why);
            }
            LogPlayback(
                std::format(
                    "{}: chapter {} (map {}) from keyframe {} at {} ms, the stream to {} ms posted at once, then {} placement messages; {} posts, {} keyframe messages, the recording from record {} to record {}, {} ms long, controlled agent {}{}",
                    connection_open ? "a load while observing" : "the replay's first load", plan.chapter + 1, chapter.map_id, plan.keyframe + 1, plan.start_ms, plan.start_ms + plan.seek_ms, plan.placed, plan.posts.size(), plan.keyframe_records,
                    plan.first_record, plan.end_record, plan.duration_ms, plan.controlled_agent, changes
                )
            );
            LogPlayback(std::format("transfer to map {}; the 422's +0x1c from the outpost's own join: {:#010x} (the join's map {}, the map now {})", transfer.map_id, transfer.game_type, JoinedMap(), static_cast<uint32_t>(GW::Map::GetMapID())));
            EnterStage(Playback::Transfer);
        }

        void NoteSkipped(const size_t first_chapter, const size_t end_chapter)
        {
            for (auto chapter = first_chapter; chapter < end_chapter && chapter < replay.chapters.size(); chapter++) {
                const auto& skipped = replay.chapters[chapter];
                const auto end_ms = chapter + 1 < replay.chapters.size() ? replay.chapters[chapter + 1].time_ms : skipped.time_ms;
                playback_note = std::format(
                    "Skipped chapter {} (map {}, {}): {}", chapter + 1, skipped.map_id, TimeText(end_ms - skipped.time_ms),
                    skipped.is_explorable ? "none of its keyframes can be played, the reasons are in the toolbox's log" : "an outpost, which the game's observer mode does not play yet"
                );
                LogPlayback(playback_note);
            }
        }

        bool Seek(const uint32_t time_ms)
        {
            auto made = ReplayWire::MakeSeekPlan(replay, time_ms);
            if (!made.error.empty()) {
                playback_note = std::format("Cannot go to {}: {}", TimeText(time_ms), made.error);
                LogPlayback(playback_note);
                return false;
            }
            playback_note.clear();
            if (playback_ended && paused_by_end) {
                playback_paused = false;
            }
            playback_ended = false;
            paused_by_end = false;
            // Forward in the part of the chapter played: its stream to the time, posted at once, gives the state a load of the later keyframe would (replay_check --seek).
            if (ReplayWire::SeeksInPlace(plan, static_cast<uint32_t>(played_ms), made)) {
                const auto from_ms = plan.start_ms + static_cast<uint32_t>(played_ms);
                played_ms = std::min(made.start_ms + made.seek_ms - plan.start_ms, plan.duration_ms);
                placement = ReplayWire::PlacementPosts(replay, plan, static_cast<uint32_t>(played_ms));
                LogPlayback(
                    std::format(
                        "seek to {} ms: forward from {} ms in chapter {}, the stream from keyframe {} posted at once, no load, then {} placement messages", made.start_ms + made.seek_ms, from_ms, plan.chapter + 1, plan.keyframe + 1,
                        std::accumulate(placement.begin(), placement.end(), size_t{0}, [](const size_t sum, const ReplayWire::Post& post) {
                            return sum + post.records;
                        })
                    )
                );
                return false;
            }
            const auto after = std::ranges::upper_bound(replay.chapters, time_ms, {}, &ReplayFile::Chapter::time_ms) - replay.chapters.begin();
            NoteSkipped(static_cast<size_t>(std::max<std::ptrdiff_t>(after - 1, 0)), made.chapter);
            LoadKeyframe(std::move(made));
            return true;
        }

        void StartPlayback(const ReplayObserve::Row& row)
        {
            std::string error;
            replay = {};
            auto made = ReplayFile::Read(row.path, replay, error) ? ReplayWire::MakeSeekPlan(replay, 0) : ReplayWire::Plan{.error = error};
            const auto name = TextUtils::WStringToString(row.path.filename().wstring());
            if (!made.error.empty()) {
                replay = {};
                playback_status = std::format("{} cannot be played: {}", name, made.error);
                Log::Error("Replay Recorder: %s", playback_status.c_str());
                Log::FlushFile();
                GW::UI::SendUIMessage(GW::UI::UIMessage::kMessage_0x1000010d);
                return;
            }
            playing_path = row.path;
            world_map_refusal_logged = false;
            mission_map_refusal_logged = false;
            playback_note.clear();
            playback_paused = false;
            paused_by_end = false;
            seek_request = no_seek;
            playback_status = std::format("Playing {}", name);
            LoadKeyframe(std::move(made));
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
            if (!row.keyframe) {
                Log::InfoW(L"Replay %ls: it holds no explorable map with a keyframe; the game's observer mode does not play outposts yet.", row.path.filename().wstring().c_str());
                GW::UI::SendUIMessage(GW::UI::UIMessage::kMessage_0x1000010d);
                return GW::Hook::LeaveHook();
            }
            if (playback_armed && playback == Playback::Off) {
                StartPlayback(row);
                return GW::Hook::LeaveHook();
            }
            const auto& map_name = Resources::GetMapName(static_cast<GW::Constants::MapID>(row.header.map_id))->wstring();
            const auto answer = std::format(
                L"Replay {}: {}, {} long. Turn on \"Play recordings from the Observe window\" in the Replay Recorder's settings to play it.", row.path.filename().wstring(), map_name.empty() ? std::format(L"map {}", row.header.map_id) : map_name,
                ReplayObserve::Duration(row.header.duration_ms)
            );
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
                auto scan = ReplayObserve::ScanFolder(RecordingsFolder(), GW::MemoryMgr::GetGWVersion(), message_table_digest, taken_keys, now, writer.IsOpen() ? recording_path : std::filesystem::path());
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

        struct PlaybackHook {
            void** target;
            GW::Hook::HookTarget detour;
            void** original;
        };

        // Every hook but the agents' clock is required: ArmPlayback finds them first.
        const PlaybackHook playback_hooks[] = {
            {reinterpret_cast<void**>(&GameConnect_Func), OnGameConnect, reinterpret_cast<void**>(&GameConnect_Ret)},          {reinterpret_cast<void**>(&Goodbye_Func), OnGoodbye, reinterpret_cast<void**>(&Goodbye_Ret)},
            {reinterpret_cast<void**>(&InstanceAck_Func), OnInstanceAck, reinterpret_cast<void**>(&InstanceAck_Ret)},          {reinterpret_cast<void**>(&MapLoaded_Func), OnMapLoaded, reinterpret_cast<void**>(&MapLoaded_Ret)},
            {reinterpret_cast<void**>(&WorldMapToggle_Func), OnWorldMapToggle, reinterpret_cast<void**>(&WorldMapToggle_Ret)}, {reinterpret_cast<void**>(&MissionMapCreate_Func), OnMissionMapCreate, reinterpret_cast<void**>(&MissionMapCreate_Ret)},
            {reinterpret_cast<void**>(&AgentAdvance_Func), OnAgentAdvance, reinterpret_cast<void**>(&AgentAdvance_Ret)},
        };

    } // namespace

    void LogPlayback(const std::string& step)
    {
        Log::Log("Replay Recorder: %s", step.c_str());
        Log::FlushFile();
    }

    void DisarmPlayback()
    {
        for (const auto& hook : playback_hooks) {
            if (*hook.target) {
                GW::Hook::RemoveHook(*hook.target);
            }
            *hook.target = nullptr;
        }
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
            "xxxxxxx????xxxxxxxxxxxxxxxxxxxxxxxx????xxxxxxxxx????xxxxx"
        );
        const auto ready_cipher = find("\x6A\x14\x8D\x73\x7C\xC7\x43\x60\x02\x00\x00\x00\x8B\xCE\xE8\x00\x00\x00\x00\x8B\x43\x64\x8D\xBB\x84\x01\x00\x00", "xxxxxxxxxxxxxxx????xxxxxxxxx");
        const auto receive = find(
            "\x55\x8B\xEC\x51\xA1\x00\x00\x00\x00\x33\xC5\x89\x45\xFC\x53\x56\x57\xE8\x00\x00\x00\x00\x8B\x5D\x10\x85\xC0\x74\x0E\x8D\x43\x34\x50\x6A\x00\xE8\x00\x00\x00\x00\x83\xC4\x08\x8B\x4D\x08\x83\x79\x34\x00\x74\x7A\x8B\x7D\x0C\x83\x79\x60\x02",
            "xxxxx????xxxxxxxxx????xxxxxxxxxxxxxx????xxxxxxxxxxxxxxxxxxx"
        );
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
            {"the transfer message's layout", message_table.size() > GAME_SMSG_TRANSFER_GAME_SERVER_INFO && message_table[GAME_SMSG_TRANSFER_GAME_SERVER_INFO] == transfer_fields},
        };
        for (const auto& [name, found] : checks) {
            if (!found) {
                return std::format("Playback not available: {} is not found in this client", name);
            }
        }
        game_connection_global = *reinterpret_cast<MsgConn***>(goodbye + 0x4B);
        net_context_global = *reinterpret_cast<void***>(net_callback + 0x50);
        PostNetEvent_Func = reinterpret_cast<PostNetEvent_pt>(post_net_event);
        GameConnect_Func = reinterpret_cast<GameConnect_pt>(game_connect);
        Goodbye_Func = reinterpret_cast<Goodbye_pt>(goodbye);
        InstanceAck_Func = reinterpret_cast<InstanceAck_pt>(instance_ack);
        MapLoaded_Func = reinterpret_cast<MapLoaded_pt>(map_loaded);
        WorldMapToggle_Func = reinterpret_cast<WorldMapToggle_pt>(world_map_toggle);
        MissionMapCreate_Func = reinterpret_cast<MissionMapCreate_pt>(mission_map_create);
        AgentAdvance_Func = reinterpret_cast<AgentAdvance_pt>(agent_advance);
        if (std::ranges::any_of(playback_hooks, [](const PlaybackHook& hook) {
                return *hook.target && GW::Hook::CreateHook(hook.target, hook.detour, hook.original) != 0;
            })) {
            DisarmPlayback();
            return "Playback not available: its hooks could not be made";
        }
        for (const auto& hook : playback_hooks) {
            if (*hook.target) {
                GW::Hook::EnableHooks(*hook.target);
            }
        }
        // s2c 172's handler passes {agent, u8} to the char client's observe call, which sets the viewpoint the camera follows (0x00811a20, x23-live, x25-live).
        const auto follow_missing = message_table.size() <= viewpoint_message_id || message_table[viewpoint_message_id] != viewpoint_fields ? "s2c 172's fields are not {agent, u8} in this client"
                                    : !observe_agent                                                                                        ? "the char client's observe call is not found in this client"
                                    : !game_viewpoint_handler                                                                               ? "s2c 172's handler was not read before the recorder's callbacks"
                                    : memcmp(reinterpret_cast<const void*>(game_viewpoint_handler), "\x55\x8B\xEC\x8B\x45\x08\xFF\x70\x08\xFF\x70\x04\xE8", 13) != 0
                                        ? "s2c 172's handler is not the viewpoint handler's code (or another module replaced it before the recorder read it)"
                                    : GW::Scanner::FunctionFromNearCall(game_viewpoint_handler + 12) != observe_agent ? "s2c 172's handler does not call the char client's observe call"
                                                                                                                      : "";
        viewpoint_available = !*follow_missing;
        playback_armed = true;
        return std::format(
            "Ready: Observe on one of your recordings plays it{}{}", viewpoint_available ? "" : std::format(" (following other agents is not available: {})", follow_missing),
            AgentAdvance_Func ? "" : " (pause and speeds run on the toolbox's clock: the agents' clock is not found in this client)"
        );
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
                            CloseReplayConnection(std::format("The game was still joining a map (mission flags {:#x}): the replay's load was not started", flags));
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
                    CloseReplayConnection(std::format("The game did not start the replay's connection (mission flags {:#x}, the join's map {}, a game connection {})", flags, JoinedMap(), *game_connection_global ? "open" : "none"));
                }
                return;
            }
            case Playback::Connecting: {
                const auto connection = *game_connection_global;
                if (connection && connection->mode == msgconn_encrypted) {
                    if (connection->socket) {
                        CloseReplayConnection("The game's connection is not the replay's: playback stopped");
                        return;
                    }
                    replay_connection = connection;
                    send_cipher = connection->receive_cipher;
                    LogPlayback("the replay's connection is ready: its receive cipher copied");
                    EnterStage(Playback::Acknowledging);
                    PostStage(ReplayWire::Stage::BeforeLoad);
                }
                else if (waited > connect_timeout) {
                    CloseReplayConnection("The replay's connection did not become ready");
                }
                return;
            }
            case Playback::Acknowledging:
            case Playback::Loading:
                // A stop asked during a load waits for the map: closing the connection then would leave the game with none, where stop observing brings it back.
                if (*game_connection_global != replay_connection) {
                    FinishPlayback("The replay's connection is gone");
                }
                else if (waited > load_timeout) {
                    CloseReplayConnection("The replay's map did not load");
                }
                return;
            case Playback::Playing: {
                const auto elapsed = std::chrono::duration<double, std::milli>(now - last_update).count();
                last_update = now;
                if (stop) {
                    Log::Info("Replay Recorder: playback stopped: back to the outpost");
                    Log::FlushFile();
                    StopObserving();
                    return;
                }
                if (const auto time_ms = seek_request.exchange(no_seek); time_ms != no_seek && Seek(time_ms)) {
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
                // An in-place seek's placement goes after the stream to its time.
                for (auto& post : std::exchange(placement, {})) {
                    send_cipher.Apply(post.bytes.data(), post.bytes.size());
                    PostNetEvent(net_event_data, replay_connection_id, post.bytes.data(), post.bytes.size());
                }
                if (next_post == plan.posts.size() && !playback_ended) {
                    if (auto next = plan.chapter + 1 < replay.chapters.size() ? ReplayWire::MakeSeekPlan(replay, replay.chapters[plan.chapter + 1].time_ms) : ReplayWire::Plan{}; next.error.empty() && next.chapter > plan.chapter) {
                        NoteSkipped(plan.chapter + 1, next.chapter);
                        LoadKeyframe(std::move(next));
                        return;
                    }
                    playback_ended = true;
                    paused_by_end = !playback_paused;
                    playback_paused = true;
                    played_ms = plan.duration_ms;
                    Log::Info("Replay Recorder: the replay has ended: paused on its last moment until a seek, Restart or Stop");
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
} // namespace ReplayRecorder

using namespace ReplayRecorder;

ReplayRecorderModule::PlaybackView ReplayRecorderModule::GetPlayback() const
{
    const auto stage = playback.load();
    PlaybackView view = {
        .active = stage != Playback::Off,
        .playing = stage == Playback::Playing,
        .ended = playback_ended,
        .paused = playback_paused,
        .can_follow = viewpoint_available,
        .speed = playback_speed,
        .position_ms = plan.start_ms + (stage == Playback::Playing ? static_cast<uint32_t>(played_ms) : plan.seek_ms),
        .length_ms = std::max(replay.header.duration_ms, replay.records.empty() ? 0 : replay.records.back().time_ms),
        .chapter = plan.chapter,
        .recorder_agent = plan.controlled_agent,
        .followed_agent = followed_agent,
        .name = TextUtils::WStringToString(playing_path.filename().wstring()),
        .note = playback_note,
    };
    for (size_t chapter = 0; chapter < replay.chapters.size(); chapter++) {
        const auto& opened = replay.chapters[chapter];
        view.chapters.push_back({opened.time_ms, chapter + 1 < replay.chapters.size() ? replay.chapters[chapter + 1].time_ms : view.length_ms, opened.map_id, opened.is_explorable != 0});
    }
    for (const auto& keyframe : replay.keyframes) {
        view.keyframes_ms.push_back(keyframe.time_ms);
    }
    return view;
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
    playback_stop_requested = true;
}

void ReplayRecorderModule::RestartPlayback()
{
    SeekPlayback(0);
}

void ReplayRecorderModule::SeekPlayback(const uint32_t time_ms)
{
    if (playback != Playback::Off) {
        seek_request = time_ms;
    }
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
