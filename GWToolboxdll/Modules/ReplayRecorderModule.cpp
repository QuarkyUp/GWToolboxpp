#include "stdafx.h"

#include <GWCA/Context/AgentContext.h>
#include <GWCA/Context/CharContext.h>
#include <GWCA/Context/PreGameContext.h>
#include <GWCA/GameEntities/Agent.h>
#include <GWCA/GameEntities/Camera.h>
#include <GWCA/GameEntities/Map.h>
#include <GWCA/Managers/AgentMgr.h>
// GWCA's CameraMgr.h gives a C export a Vec3f to return, which clang warns of in every file that includes it.
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wreturn-type-c-linkage"
#include <GWCA/Managers/CameraMgr.h>
#pragma clang diagnostic pop
#include <GWCA/Managers/MapMgr.h>
#include <GWCA/Managers/MemoryMgr.h>
#include <GWCA/Managers/StoCMgr.h>
#include <GWCA/Packets/StoC.h>
#include <GWCA/Utilities/Hooker.h>
#include <GWCA/Utilities/Scanner.h>
#include <GWToolbox.h>
#include <ImGuiAddons.h>
#include <Logger.h>
#include <Modules/ReplayRecorderInternal.h>
#include <Modules/ReplayRecorderModule.h>
#include <Modules/Resources.h>
#include <Utils/Replay/ReplayFile.h>
#include <Utils/Replay/ReplayMessages.h>
#include <Utils/Replay/ReplayState.h>
#include <Utils/StoCHandlers.h>
#include <Utils/TextUtils.h>
#include "stdafx.h"

namespace ReplayRecorder {
    namespace {
        constexpr auto idle_timeout = std::chrono::seconds(60);

        constexpr auto flush_interval = std::chrono::seconds(5);

        constexpr uint32_t keyframe_interval_ms = 30000;

        constexpr uint32_t heights_interval_ms = 250;

        constexpr uint32_t camera_interval_ms = 100;

        GW::HookEntry stoc_hook_entry;

        bool callbacks_registered = false;

        ReplayFile::Header header;

        Clock::time_point recording_start;

        Clock::time_point last_record_at;

        Clock::time_point last_flush_at;

        std::vector<uint8_t> body;

        std::vector<uint8_t> pending_chat;

        uint32_t pending_chat_time_ms = 0;

        bool has_pending_chat = false;

        std::optional<ReplayState::NameStripper> name_stripper;

        std::optional<ReplayState::InventoryFilter> inventory_filter;

        std::string status = "Not recording";

        bool record_requested = false;

        bool stopped_until_login = false;

        bool load_keyframe_due = false;

        bool load_finished = false;

        uint32_t next_keyframe_ms = 0;

        uint32_t keyframe_count = 0;

        uint32_t chapter_count = 0;

        uint64_t motions_written = 0;

        struct SampledWalk {
            ReplayFile::Motion motion;
            uint32_t stop_time = 0;
            float velocity_x = 0.f;
            float velocity_y = 0.f;

            bool operator==(const SampledWalk&) const = default;
        };

        std::map<uint32_t, SampledWalk> sampled_walks;

        uint32_t next_heights_ms = 0;

        uint32_t next_camera_ms = 0;

        ReplayFile::Camera sampled_camera;

        StartClip_pt StartClip_Func = nullptr;

        StartClip_pt StartClip_Ret = nullptr;

        struct StartedClip {
            uint32_t time_ms = 0;
            ReplayFile::Clip clip;
        };

        std::mutex started_clips_mutex;

        std::vector<StartedClip> started_clips;

        std::map<uint32_t, uint32_t> clip_starts;

        uint64_t clips_written = 0;

        // The decoded form of 270 to 277: the prop's tag or the gadget's agent, then the message's values (273's state and mask).
        struct PropStatePacket : PacketBase {
            uint32_t prop;
            uint32_t state;
            uint32_t mask;
        };

        std::map<std::array<uint32_t, 3>, std::vector<uint8_t>> prop_states;

        std::set<uint32_t> written_skills;

        std::set<uint32_t> written_effects;

        std::set<uint32_t> written_auras;

        std::set<uint32_t> written_maps;

        uint32_t ElapsedMs(const Clock::time_point since)
        {
            return static_cast<uint32_t>(std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - since).count());
        }

        std::string RecordingName()
        {
            return TextUtils::WStringToString(recording_path.filename().wstring());
        }

        bool PveMap(const GW::Constants::MapID map_id)
        {
            return GW::Map::GetMapInfo(map_id) && !GWToolbox::ShouldDisableToolbox(map_id);
        }

        void __fastcall OnStartClip(GW::AgentLiving* agent, void* edx, const uint32_t sequence, const uint32_t sequence_index, const float duration)
        {
            GW::Hook::EnterHook();
            StartClip_Ret(agent, edx, sequence, sequence_index, duration);
            {
                // The game starts clips inside its own message handlers and its advance: the recorder writes them at its next write, never from here.
                std::scoped_lock lock(started_clips_mutex);
                started_clips.push_back({ElapsedMs(recording_start), ClipOf(*agent)});
            }
            GW::Hook::LeaveHook();
        }

        void RemoveClipHook()
        {
            if (StartClip_Func) {
                GW::Hook::RemoveHook(StartClip_Func);
                StartClip_Func = nullptr;
            }
        }

        void FinishRecording()
        {
            if (!writer.IsOpen()) {
                return;
            }
            RemoveClipHook();
            has_pending_chat = false;
            inventory_filter.reset();
            header.duration_ms = ElapsedMs(recording_start);
            header.flags |= ReplayFile::Complete;
            const auto size = writer.Size();
            if (header.record_count == 0) {
                writer.Close(header);
                std::error_code error;
                std::filesystem::remove(recording_path, error);
                status = std::format("{} removed: no message was recorded", RecordingName());
                LogLine(Log::Log, status);
                return;
            }
            if (!writer.Close(header)) {
                status = std::format("Could not finish {}", RecordingName());
                LogLine(Log::Error, status);
                return;
            }
            status = std::format(
                "Saved {}: {} chapters, {} keyframes, {} messages, {} agents' motions, {} clips, {} KB, {} layout errors{}", RecordingName(), chapter_count, keyframe_count, header.record_count, motions_written, clips_written, size / 1024,
                header.layout_errors, header.flags & ReplayFile::SizeCapReached ? " (size cap reached)" : ""
            );
            LogLine(Log::Info, status);
        }

        void PackRecord(const PacketBase* packet, std::vector<uint8_t>& out)
        {
            const auto& fields = message_table[packet->header];
            if (!ReplayFile::PackMessage(
                    packet->header, fields.data(), static_cast<uint32_t>(fields.size()), reinterpret_cast<const uint8_t*>(packet), out, name_stripper ? name_stripper->For(static_cast<uint16_t>(packet->header)) : ReplayFile::TextFilter()
                )) {
                out = {static_cast<uint8_t>(packet->header), static_cast<uint8_t>(packet->header >> 8)};
                header.layout_errors++;
            }
        }

        std::vector<uint8_t> Encoded(const Message& message)
        {
            std::vector<uint8_t> encoded;
            Message stripped;
            if (!ReplayState::Encode(message_table, message, encoded) || !name_stripper) {
                return encoded;
            }
            // Texts made from memory pass the same names settings as the recorded ones, by the same decoding.
            if (!ReplayState::Decode(message_table, encoded, stripped, name_stripper->For(message.id)) || !ReplayState::Encode(message_table, stripped, encoded)) {
                encoded.clear();
            }
            return encoded;
        }

        bool WriteRecord(const uint32_t time_ms, const std::vector<uint8_t>& bytes)
        {
            const auto size_cap = static_cast<uint64_t>(settings.size_cap_mb) << 20;
            if (writer.Size() + bytes.size() + 6 > size_cap) {
                header.flags |= ReplayFile::SizeCapReached;
                FinishRecording();
                return false;
            }
            if (!writer.Append(time_ms, bytes.data(), bytes.size())) {
                FinishRecording();
                return false;
            }
            header.record_count++;
            last_record_at = Clock::now();
            return true;
        }

        bool WriteClips()
        {
            std::vector<StartedClip> started;
            {
                std::scoped_lock lock(started_clips_mutex);
                started.swap(started_clips);
            }
            for (const auto& [time_ms, clip] : started) {
                clip_starts[clip.agent] = time_ms;
                if (!writer.AppendClip(time_ms, clip)) {
                    FinishRecording();
                    return false;
                }
                clips_written++;
            }
            return true;
        }

        // A reader with Gw.dat and no Gw.exe draws a skill, an effect or an aura from its row: each is written once, before the first message that names it.
        bool WriteRowsNamed(const Message& message, const uint32_t time_ms)
        {
            using namespace GW::Packet::StoC::GenericValueID;
            constexpr size_t play_effect_field = 3;
            constexpr size_t projectile_effect_field = 4;
            constexpr size_t skillbar_skills_field = 1;
            std::vector<ReplayFile::SkillRow> skills;
            std::vector<ReplayFile::EffectRow> effects;
            std::vector<ReplayFile::AuraRow> auras;
            const auto effect = [&effects](const uint32_t effect_id) {
                if (const auto row = written_effects.insert(effect_id).second ? EffectRowOf(effect_id) : std::nullopt) {
                    effects.push_back(*row);
                }
            };
            const auto skill = [&skills, &effect](const uint32_t skill_id) {
                if (const auto row = written_skills.insert(skill_id).second ? SkillRowOf(skill_id) : std::nullopt) {
                    skills.push_back(*row);
                    std::ranges::for_each(row->visuals, effect);
                }
            };
            for (const auto& field : ReplayState::IdFields(message_table, message.id)) {
                if (field.kind == ReplayState::IdKind::Skill && field.field < message.fields.size()) {
                    skill(message.U(field.field));
                }
            }
            const auto notice = message.id == GAME_SMSG_AGENT_ATTR_UPDATE_INT || message.id == GAME_SMSG_AGENT_ATTR_UPDATE_INT_TARGET;
            if (notice && message.fields.size() > 2) {
                const auto value = message.U(message.fields.size() - 1);
                switch (message.U(0)) {
                    case skill_damage:
                    case attack_skill_finished:
                    case instant_skill_activated:
                    case attack_skill_stopped:
                    case attack_skill_activated:
                    case skill_finished:
                    case skill_stopped:
                    case skill_activated:
                        skill(value);
                        break;
                    case effect_on_target:
                    case effect_on_agent:
                        effect(value);
                        break;
                    case add_effect:
                    case remove_effect:
                        if (const auto row = written_auras.insert(value).second ? AuraRowOf(value) : std::nullopt) {
                            auras.push_back(*row);
                        }
                        break;
                    default:
                        break;
                }
            }
            else if (message.id == GAME_SMSG_AGENT_ATTR_PLAY_EFFECT && message.fields.size() > play_effect_field) {
                effect(message.U(play_effect_field));
            }
            else if (message.id == GAME_SMSG_AGENT_PROJECTILE_LAUNCHED && message.fields.size() > projectile_effect_field) {
                effect(message.U(projectile_effect_field));
            }
            else if (message.id == GAME_SMSG_SKILLBAR_UPDATE && message.fields.size() > skillbar_skills_field) {
                std::ranges::for_each(message.fields[skillbar_skills_field].numbers, skill);
            }
            if (writer.AppendSkills(time_ms, skills) && writer.AppendEffects(time_ms, effects) && writer.AppendAuras(time_ms, auras)) {
                return true;
            }
            FinishRecording();
            return false;
        }

        void WriteKeyframe(const ReplayFile::KeyframeKind kind)
        {
            const auto started = Clock::now();
            const auto world = Snapshot();
            ReplayFile::Keyframe keyframe = {0, ElapsedMs(recording_start), kind, {}, {}, {}};
            uint64_t bytes = 0;
            size_t not_encoded = 0;
            for (const auto& message : world.Join()) {
                auto encoded = Encoded(message);
                if (encoded.empty()) {
                    not_encoded++;
                    continue;
                }
                if (!WriteRowsNamed(message, keyframe.time_ms)) {
                    return;
                }
                bytes += encoded.size() + 6;
                keyframe.records.push_back({keyframe.time_ms, std::move(encoded)});
            }
            if (!WriteClips()) {
                return;
            }
            for (const auto& state : prop_states | std::views::values) {
                keyframe.props.push_back({keyframe.time_ms, state});
            }
            keyframe.bodies = Bodies(clip_starts);
            if (keyframe.records.empty() || !writer.AppendKeyframe(keyframe)) {
                LogLine(Log::Log, std::format("keyframe {} not written: the client's state could not be read", ReplayFile::KeyframeKindName(kind)));
                return;
            }
            keyframe_count++;
            next_keyframe_ms = keyframe.time_ms + keyframe_interval_ms;
            LogLine(
                Log::Log, std::format(
                              "keyframe {} ({}) at {} ms: {} messages, {} not encoded against the client's table, {} KB, {} bodies, {} props' states, read and written in {} ms", keyframe_count, ReplayFile::KeyframeKindName(kind), keyframe.time_ms,
                              keyframe.records.size(), not_encoded, bytes / 1024, keyframe.bodies.size(), keyframe.props.size(), ElapsedMs(started)
                          )
            );
        }

        void WriteMotions()
        {
            const auto context = GW::GetAgentContext();
            if (!context) {
                return;
            }
            // The server world changes only by the stream's messages: an agent whose walk (or, standing, its point) changed since its last sample is sampled again, after the records that changed it.
            const auto clock = ServerTime();
            const auto time_ms = ElapsedMs(recording_start);
            // A walking body's height is no straight line along its leg: it is sampled again at a period.
            const auto heights_due = time_ms >= next_heights_ms;
            std::vector<ReplayFile::Motion> changed;
            std::vector<ReplayFile::Height> heights;
            std::erase_if(sampled_walks, [&context](const auto& sampled) {
                return sampled.first >= context->agent_movement.size() || !context->agent_movement[sampled.first];
            });
            for (const auto agent : context->agent_movement) {
                if (!agent) {
                    continue;
                }
                const auto& walking = reinterpret_cast<const WalkingAgent&>(*agent);
                const auto drawn = GW::Agents::GetAgentByID(walking.agent_id);
                if (heights_due && walking.stop_time && drawn) {
                    heights.push_back({walking.agent_id, drawn->z});
                }
                SampledWalk sample = {MotionOf(walking, clock), walking.stop_time, walking.velocity_x, walking.velocity_y};
                if (sample.motion.walking) {
                    sample.motion.x = sample.motion.y = 0.f;
                    sample.motion.plane = 0;
                }
                auto& last = sampled_walks[walking.agent_id];
                if (last == sample) {
                    continue;
                }
                last = sample;
                auto& motion = changed.emplace_back(MotionOf(walking, clock));
                if (drawn) {
                    motion.drawn = true;
                    motion.z = drawn->z;
                    motion.rotation = drawn->rotation_angle;
                }
            }
            if (heights_due) {
                next_heights_ms = time_ms + heights_interval_ms;
            }
            if (!writer.AppendMotions(time_ms, clock, changed) || !writer.AppendHeights(time_ms, heights)) {
                FinishRecording();
                return;
            }
            motions_written += changed.size();
        }

        void WriteChapter(const uint32_t map_id, const bool explorable)
        {
            const auto map_info = GW::Map::GetMapInfo(static_cast<GW::Constants::MapID>(map_id));
            const auto time_ms = ElapsedMs(recording_start);
            writer.AppendChapter({0, time_ms, map_id, explorable, map_info ? static_cast<uint32_t>(map_info->type) : 0});
            if (map_info && written_maps.insert(map_id).second) {
                writer.AppendMaps(
                    time_ms, {{map_id, static_cast<uint32_t>(map_info->campaign), static_cast<uint32_t>(map_info->continent), static_cast<uint32_t>(map_info->region), static_cast<uint32_t>(map_info->type), map_info->flags, map_info->name_id,
                               map_info->description_id}}
                );
            }
            // Agents and props are a map's own: what the recorder holds of the map left says nothing of the next.
            clip_starts.clear();
            prop_states.clear();
            chapter_count++;
            chapter_map_file = 0;
            load_finished = false;
            load_keyframe_due = true;
        }

        bool OpenRecording(const uint32_t map_id, const bool explorable)
        {
            if (message_table.empty()) {
                status = "Not recording: the client's message table is not read yet";
                return false;
            }
            const auto folder = RecordingsFolder();
            std::wstring error;
            if (!Resources::EnsureFolderExists(folder, error)) {
                status = TextUtils::WStringToString(error);
                LogLine(Log::Error, status);
                return false;
            }
            recording_path = folder / std::format(L"{}_map{}.gwrec", TextUtils::StringToWString(TextUtils::FilenameTimestamp()), map_id);
            const auto char_context = GW::GetCharContext();
            const auto map_info = GW::Map::GetMapInfo(static_cast<GW::Constants::MapID>(map_id));
            header = {};
            header.client_build = GW::MemoryMgr::GetGWVersion();
            header.table_digest = message_table_digest;
            header.start_time_unix_ms = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count();
            header.map_id = map_id;
            header.map_region_type = map_info ? static_cast<uint32_t>(map_info->type) : 0;
            header.is_explorable = explorable;
            header.district = char_context ? char_context->district_number : 0;
            header.region = static_cast<uint32_t>(GW::Map::GetRegion());
            header.language = static_cast<uint32_t>(GW::Map::GetLanguage());
            header.flags = (settings.strip_names_and_chat ? ReplayFile::NamesStripped | ReplayFile::ChatDropped : 0) | (settings.strip_own_names ? ReplayFile::OwnNamesStripped : 0);
            strncpy(header.toolbox_version, GWTOOLBOXDLL_VERSION GWTOOLBOXDLL_VERSION_BETA, sizeof(header.toolbox_version) - 1);
            if (!AgentNotifyParams_Func) {
                if (const auto equipped_item_getter = GW::Scanner::Find("\x55\x8B\xEC\x83\xEC\x08\x8D\x45\xF8\x50\x8D\x45\xFC\x50\xFF\x75\x08\xE8\x00\x00\x00\x00\x83\xC4\x0C\x85\xC0\x75", "xxxxxxxxxxxxxxxxxx????xxxxxx", 0)) {
                    AgentNotifyParams_Func = reinterpret_cast<AgentNotifyParams_pt>(GW::Scanner::FunctionFromNearCall(equipped_item_getter + 0x11));
                }
                else {
                    LogLine(Log::Log, "the char client's equipped item lookup is not found in this client: keyframes leave out what agents wear");
                }
            }
            if (!writer.Open(recording_path, header, message_table)) {
                status = std::format("Could not create {}", RecordingName());
                LogLine(Log::Error, status);
                return false;
            }
            name_stripper.reset();
            if (settings.strip_names_and_chat || settings.strip_own_names) {
                name_stripper.emplace(settings.strip_names_and_chat, settings.strip_own_names, char_context ? std::wstring(char_context->player_name) : std::wstring());
            }
            inventory_filter.emplace(message_table);
            has_pending_chat = false;
            post_process = {};
            keyframe_count = 0;
            chapter_count = 0;
            motions_written = 0;
            clips_written = 0;
            sampled_walks.clear();
            started_clips.clear();
            written_skills.clear();
            written_effects.clear();
            written_auras.clear();
            written_maps.clear();
            next_heights_ms = next_camera_ms = 0;
            sampled_camera = {};
            recording_start = last_record_at = last_flush_at = Clock::now();
            WriteChapter(map_id, explorable);
            FindConstTables();
            writer.AppendTexts(0, TextFilesOf());
            StartClip_Func = reinterpret_cast<StartClip_pt>(GW::Scanner::Find("\x55\x8B\xEC\x83\xEC\x18\xA1\x00\x00\x00\x00\x33\xC5\x89\x45\xFC\x56\x8B\xF1\x57\x8B\x7D\x0C\x8B\x86\x5C\x01\x00\x00", "xxxxxxx????xxxxxxxxxxxxxxxxxx", 0));
            if (StartClip_Func && GW::Hook::CreateHook(&StartClip_Func, OnStartClip, &StartClip_Ret) == 0) {
                GW::Hook::EnableHooks(StartClip_Func);
            }
            else {
                StartClip_Func = nullptr;
                LogLine(Log::Log, "the start of a body's clip is not found in this client: recordings hold each keyframe's clips, none between");
            }
            status = std::format("Recording {}", RecordingName());
            LogLine(Log::Log, status);
            return true;
        }

        void StartFromMemory()
        {
            const auto map_id = GW::Map::GetMapID();
            if (!PveMap(map_id) || GW::Map::GetIsObserving()) {
                status = "Not recording: PvP maps and observed matches are never recorded";
                record_requested = false;
                return;
            }
            if (!OpenRecording(static_cast<uint32_t>(map_id), GW::Map::GetInstanceType() == GW::Constants::InstanceType::Explorable)) {
                record_requested = false;
                return;
            }
            load_finished = true;
            load_keyframe_due = false;
            WriteKeyframe(ReplayFile::KeyframeKind::Start);
        }

        bool WriteMessage(const uint32_t time_ms, const std::vector<uint8_t>& bytes)
        {
            Message message;
            if (!inventory_filter || !ReplayState::Decode(message_table, bytes, message)) {
                return WriteRecord(time_ms, bytes);
            }
            std::vector<Message> shown_before;
            std::vector<uint32_t> unknown_items;
            if (!inventory_filter->Keep(message, shown_before, unknown_items)) {
                return true;
            }
            for (const auto item_id : unknown_items) {
                std::ranges::move(ItemDetails(item_id), std::back_inserter(shown_before));
            }
            if (!WriteRowsNamed(message, time_ms)) {
                return false;
            }
            for (const auto& details : shown_before) {
                if (const auto encoded = Encoded(details); !encoded.empty() && !WriteRecord(time_ms, encoded)) {
                    return false;
                }
            }
            return WriteRecord(time_ms, bytes);
        }

        void OnServerMessage(GW::HookStatus*, PacketBase* packet, void*)
        {
            if (emulating_packet || playing_replay) {
                return;
            }
            std::scoped_lock lock(recording_mutex);
            if (packet->header >= message_table.size()) {
                return;
            }
            const auto transfer = packet->header == GAME_SMSG_TRANSFER_GAME_SERVER_INFO ? static_cast<const GW::Packet::StoC::GameSrvTransfer*>(packet) : nullptr;
            if (!writer.IsOpen()) {
                const auto char_context = GW::GetCharContext();
                // At login the game joins its first map with no 422: the join's map is in the mission context (MissionClient::GameJoin, +0x198).
                if (settings.record_from_login && !stopped_until_login && packet->header == GAME_SMSG_INSTANCE_LOAD_HEAD && char_context && PveMap(char_context->map_id)) {
                    OpenRecording(static_cast<uint32_t>(char_context->map_id), char_context->is_explorable != 0);
                }
                if (!writer.IsOpen()) {
                    return;
                }
            }
            if (packet->header == GAME_SMSG_INSTANCE_LOAD_INFO && static_cast<const GW::Packet::StoC::InstanceLoadInfo*>(packet)->is_observer) {
                FinishRecording();
                status = "Stopped: an observed match is never recorded";
                return;
            }
            if (!WriteClips()) {
                return;
            }
            // A map's after-load keyframe waits for the load's last message: an outpost's 446, 366, 398 and 404 come about 0.5 s after its 31.
            if (packet->header == GAME_SMSG_INSTANCE_LOAD_FINISH) {
                load_finished = true;
            }
            if (packet->header == GAME_SMSG_INSTANCE_LOAD_SPAWN_POINT) {
                chapter_map_file = static_cast<const GW::Packet::StoC::InstanceLoadFile*>(packet)->map_fileID;
            }
            if (packet->header == GAME_SMSG_POST_PROCESS) {
                post_process = *static_cast<const GW::Packet::StoC::PostProcess*>(packet);
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
                    if (!WriteMessage(pending_chat_time_ms, pending_chat)) {
                        return;
                    }
                }
            }
            if (transfer) {
                if (!PveMap(static_cast<GW::Constants::MapID>(transfer->map_id))) {
                    FinishRecording();
                    status = std::format("Stopped before map {}: PvP maps are never recorded", transfer->map_id);
                    return;
                }
                // The transfer carries the next game server's address and key: it opens the next chapter, never recorded itself.
                WriteChapter(transfer->map_id, transfer->is_explorable != 0);
                return;
            }
            PackRecord(packet, body);
            // A keyframe taken later in the map holds what its props and gadgets were last told (270 to 277, but a gadget's name): no memory of the client's is read for it.
            if (packet->header >= GAME_SMSG_MANIPULATE_MAP_OBJECT && packet->header <= GadgetState && packet->header != GadgetName && body.size() > 2) {
                const auto state = static_cast<const PropStatePacket*>(packet);
                prop_states[{packet->header, state->prop, packet->header == GAME_SMSG_MANIPULATE_MAP_OBJECT2 ? state->mask : 0}] = body;
            }
            if (packet->header == GAME_SMSG_AGENT_DESPAWNED) {
                std::erase_if(prop_states, [agent_id = static_cast<const GW::Packet::StoC::AgentRemove*>(packet)->agent_id](const auto& state) {
                    return state.first[0] >= GadgetColor && state.first[1] == agent_id;
                });
            }
            WriteMessage(time_ms, body);
        }

        void ReadMessageTable()
        {
            const auto table = GetStoCHandlerArray();
            if (!message_table.empty() || !(table && table->size())) {
                return;
            }
            for (const auto& handler : *table) {
                message_table.emplace_back(handler.fields, handler.fields + handler.field_count);
            }
            message_table_digest = ReplayFile::TableDigest(message_table);
            if (table->size() > viewpoint_message_id) {
                // GWCA puts its dispatcher in a header's handler_func once a callback is registered on it (PacketLoggerWindow.cpp's InitStoC): the game's own is read first.
                game_viewpoint_handler = reinterpret_cast<uintptr_t>((*table)[viewpoint_message_id].handler_func);
            }
        }

        void RegisterCallbacks()
        {
            if (callbacks_registered || message_table.empty()) {
                return;
            }
            // After the game's handler and the toolbox's own post callbacks (0x8000): a message is recorded as the game took it in the end.
            for (uint32_t message_id = 0; message_id < message_table.size(); message_id++) {
                if (!GW::StoC::RegisterPacketCallback(&stoc_hook_entry, message_id, OnServerMessage, nullptr, 0x9000)) {
                    GW::StoC::RemoveCallbacks(&stoc_hook_entry);
                    return;
                }
            }
            callbacks_registered = true;
        }

        void RemoveCallbacks()
        {
            GW::StoC::RemoveCallbacks(&stoc_hook_entry);
            callbacks_registered = false;
        }

        void UpdateRecording()
        {
            const auto in_game = !GW::GetPreGameContext() && GW::GetCharContext();
            if (!in_game) {
                stopped_until_login = false;
                record_requested = false;
            }
            const auto wanted = record_requested || (settings.record_from_login && !stopped_until_login);
            if (wanted && !message_table.empty()) {
                RegisterCallbacks();
            }
            else if (!writer.IsOpen() && callbacks_registered) {
                RemoveCallbacks();
            }
            const auto loaded = in_game && GW::Map::GetIsMapLoaded() && GW::Map::GetInstanceType() != GW::Constants::InstanceType::Loading && GW::Agents::GetControlledCharacter();
            if (!writer.IsOpen()) {
                if (wanted && loaded && callbacks_registered) {
                    StartFromMemory();
                }
                return;
            }
            const auto now = Clock::now();
            if (!in_game || now - last_record_at > idle_timeout || !wanted) {
                FinishRecording();
                return;
            }
            if (loaded && load_keyframe_due && load_finished) {
                load_keyframe_due = false;
                WriteKeyframe(ReplayFile::KeyframeKind::AfterLoad);
            }
            else if (loaded && !load_keyframe_due && ElapsedMs(recording_start) >= next_keyframe_ms) {
                WriteKeyframe(ReplayFile::KeyframeKind::Periodic);
            }
            if (writer.IsOpen() && WriteClips() && loaded) {
                WriteMotions();
            }
            const auto time_ms = ElapsedMs(recording_start);
            if (const auto camera = writer.IsOpen() && loaded && time_ms >= next_camera_ms ? GW::CameraMgr::GetCamera() : nullptr) {
                const ReplayFile::Camera sample = {
                    {camera->position.x, camera->position.y, camera->position.z}, {camera->look_at_target.x, camera->look_at_target.y, camera->look_at_target.z}, camera->yaw, camera->pitch, camera->distance, camera->field_of_view,
                };
                if (sample != sampled_camera) {
                    sampled_camera = sample;
                    next_camera_ms = time_ms + camera_interval_ms;
                    if (!writer.AppendCamera(time_ms, sample)) {
                        FinishRecording();
                    }
                }
            }
            if (writer.IsOpen() && now - last_flush_at > flush_interval) {
                writer.Flush();
                last_flush_at = now;
            }
        }
    } // namespace

    void LogLine(void (*log)(const char* format, ...), const std::string& text)
    {
        log("Replay Recorder: %s", text.c_str());
        Log::FlushFile();
    }

    std::filesystem::path RecordingsFolder()
    {
        return settings.folder.empty() ? Resources::GetSettingsFolderPath() / L"replays" : std::filesystem::path(TextUtils::StringToWString(settings.folder));
    }
} // namespace ReplayRecorder

using namespace ReplayRecorder;

ReplayRecorderModule::~ReplayRecorderModule()
{
    // A game ending without its window's close never runs SignalTerminate (DllMain's Terminate only asks the stopped game loop): the file's end is written at the module's unload, without the log or the game.
    if (writer.IsOpen()) {
        RemoveClipHook();
        header.duration_ms = ElapsedMs(recording_start);
        header.flags |= ReplayFile::Complete;
        writer.Close(header);
    }
}

void ReplayRecorderModule::Initialize()
{
    ToolboxModule::Initialize();
    SettingsRegistry::Register(this, settings);
    std::scoped_lock lock(recording_mutex);
    ReadMessageTable();
}

void ReplayRecorderModule::SignalTerminate()
{
    playback_stop_requested = true;
    GW::StoC::RemoveCallbacks(&observe_hook_entry);
    DropListedRows();
    std::scoped_lock lock(recording_mutex);
    RemoveCallbacks();
    FinishRecording();
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
        LogLine(Log::Log, playback_status);
    }
    else if (!play && playback == Playback::Off && (playback_armed || !playback_status.empty())) {
        DisarmPlayback();
        playback_status.clear();
    }
    std::scoped_lock lock(recording_mutex);
    ReadMessageTable();
    if (playback == Playback::Off) {
        UpdateRecording();
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
    if (writer.IsOpen()) {
        if (ImGui::Button("Stop recording")) {
            record_requested = false;
            stopped_until_login = settings.record_from_login;
            FinishRecording();
        }
        ImGui::SameLine();
        ImGui::Text("Recording %s: %u chapters, %u keyframes, %u messages, %llu KB", RecordingName().c_str(), chapter_count, keyframe_count, header.record_count, writer.Size() / 1024);
    }
    else {
        ImGui::BeginDisabled(record_requested);
        if (ImGui::Button("Record")) {
            record_requested = true;
            status = "Waiting for the map to load";
        }
        ImGui::EndDisabled();
        ImGui::SameLine();
        ImGui::TextUnformatted(status.c_str());
    }
    ImGui::ShowHelp(
        "Records what the game server sends from now on, in one file across map changes, until Stop or logging off. Its start, each map's load and every 30 seconds keep a keyframe: the moment as the game holds it, which a replay can start from."
    );
    ImGui::Checkbox("Start recording when entering the game", &settings.record_from_login);
    ImGui::Checkbox("Keep other players' names and chat out of recordings", &settings.strip_names_and_chat);
    ImGui::ShowHelp("Other players' names become \"Player 1\", \"Player 2\"...; player chat and speech bubbles are left out.");
    ImGui::Checkbox("Keep my own names out of recordings", &settings.strip_own_names);
    ImGui::ShowHelp("Your character's and account's names become \"Recorder\", your mercenary heroes' names \"Mercenary 1\"... With either setting on, guild names become \"Guild 1\"... and guild tags \"G1\"...");
    ImGui::TextDisabled("Recordings never hold your bags, storage or gold: only what each character wears or holds.");
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
    ImGui::TextDisabled("PvP maps and observed matches are never recorded.");
    ImGui::Checkbox("List recordings in the Observe window", &settings.list_in_observe_window);
    ImGui::ShowHelp(
        "Each recording made on this client build appears once in the Observe window (B, in an outpost) under \"Custom Event (0)\", with its date, maps and length; it plays from its first explorable map's first keyframe and on across its maps (outposts are skipped: not played yet). Choosing one is answered here; nothing is sent to the server."
    );
    if (!observe_status.empty()) {
        ImGui::TextDisabled("%s", observe_status.c_str());
    }
    ImGui::Checkbox("Play recordings from the Observe window", &settings.play_from_observe_window);
    ImGui::ShowHelp(
        "Observe on one of your recordings plays it in the game's observer mode, fed by the toolbox: nothing of the replay goes to the server. The Replay window shows while it plays: its bar goes anywhere in the recording, and its Stop, or stop observing (B, then Yes), brings you back to your outpost. Needs the list above."
    );
    if (!playback_status.empty()) {
        ImGui::TextDisabled("%s", playback_status.c_str());
    }
    if (playback != Playback::Off) {
        ImGui::TextDisabled("A replay is playing: its controls are in the Replay window.");
    }
}
