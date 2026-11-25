#include "stdafx.h"

#include <GWCA/Context/CharContext.h>
#include <GWCA/Context/GuildContext.h>
#include <GWCA/Context/ItemContext.h>
#include <GWCA/Context/PreGameContext.h>
#include <GWCA/Context/WorldContext.h>

#include <GWCA/Constants/UIMessages.h>

#include <GWCA/GameEntities/Agent.h>
#include <GWCA/GameEntities/Guild.h>
#include <GWCA/GameEntities/Hero.h>
#include <GWCA/GameEntities/Item.h>
#include <GWCA/GameEntities/Map.h>
#include <GWCA/GameEntities/Match.h>
#include <GWCA/GameEntities/NPC.h>
#include <GWCA/GameEntities/Party.h>
#include <GWCA/GameEntities/Player.h>
#include <GWCA/GameEntities/Quest.h>
#include <GWCA/GameEntities/Skill.h>
#include <GWCA/GameEntities/Title.h>

#include <GWCA/Managers/AgentMgr.h>
#include <GWCA/Managers/EffectMgr.h>
#include <GWCA/Managers/ItemMgr.h>
#include <GWCA/Managers/MapMgr.h>
#include <GWCA/Managers/MemoryMgr.h>
#include <GWCA/Managers/PartyMgr.h>
#include <GWCA/Managers/SkillbarMgr.h>
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
#include <Utils/ReplayState.h>
#include <Utils/ReplayWire.h>
#include <Utils/StoCHandlers.h>
#include <Utils/TextUtils.h>
#include <Windows/Pathfinding/PathfindingWindow.h>

namespace {
    using Clock = std::chrono::steady_clock;
    using GW::Packet::StoC::PacketBase;
    using ReplayState::Message;

    constexpr auto idle_timeout = std::chrono::seconds(60);
    constexpr auto flush_interval = std::chrono::seconds(5);
    constexpr uint32_t keyframe_interval_ms = 30000;

    enum MessageId : uint16_t {
        AgentConnect = 31,
        AgentCreate = 32,
        AgentReporter = 34,
        CharAccountNameData = 48,
        CharAttackInterval = 53,
        CharAttributeState = 55,
        CharAttributes = 58,
        CharEffectUpkeepAdded = 63,
        CharEffectUpkeepApplied = 65,
        CharEffectApplied = 66,
        CharQuestGeneralInfo = 80,
        CharQuestAddMarker = 83,
        CharNpcProperties = 86,
        CharNpcModel = 87,
        CharPlayerCreate = 89,
        CharToClient91 = 91,
        CharHeroSkillStatusBitmap = 101,
        CharPostProcess = 107,
        CharNpcUpdateWeapons = 109,
        CharEquipment = 110,
        CharContextValue112 = 112,
        CharHeroActivate = 114,
        CharHeroDataAdd = 115,
        CharMercenaryInfo = 116,
        CharToClient153 = 153,
        CharAgentUpdateNpcName = 155,
        CharAgentMorale = 156,
        CharNotifyDword = 159,
        CharAgentAttrUpdateFloat = 162,
        CharAgentUpdateProfession = 166,
        CharUpdateAgentPartysize = 176,
        CharPlayerLink = 177,
        CharPlayerUnlockedProfession = 182,
        CharProfessions = 183,
        CharMissionStreamStart = 186,
        CharSkillbar = 218,
        CharUnlockedSkills = 219,
        CharPlayerAttrSet = 233,
        CharPlayerAttrMaxKurzick = 234,
        CharPlayerAttrMaxLuxon = 235,
        CharPlayerAttrMaxBalthazar = 236,
        CharPlayerAttrMaxImperial = 237,
        CharAgentInitialEffects = 240,
        CharPlayerTeam = 242,
        CharTitleRankData = 243,
        CharTitleRankDisplay = 244,
        CharTitleTrackInfo = 246,
        GuildAssign = 280,
        GuildAllianceInfo = 288,
        GuildGeneralInfo = 289,
        ItemAssign = 309,
        ItemInventoryCreate = 324,
        ItemInventoryEquipSetActivate = 328,
        MissionAccessRights = 366,
        MissionInstanceLoadHead = 380,
        MissionInstanceLoadPlayerName = 381,
        MissionConfigEnd = 390,
        MissionInstanceLoadFinish = 398,
        MissionToClient404 = 404,
        MissionLoadMap = 406,
        MissionManifestComplete = 408,
        MissionManifestPhase = 409,
        MissionTeam = 430,
        PartyAssign = 435,
        PartyToClient446 = 446,
        PartySetDifficulty = 447,
        PartyHeroAdd = 451,
        PartyMemberAdd = 460,
        PartyMembershipBegin = 467,
        PartyMembershipEnd = 468,
    };

    enum Notice : uint32_t {
        Level = 36,
        MaxEnergy = 41,
        MaxHealth = 42,
    };
    constexpr uint32_t energy_value = 33;
    constexpr uint32_t health_value = 34;
    constexpr uint32_t energy_regen_value = 43;
    constexpr uint32_t health_regen_value = 44;

    ReplayRecorderModule::Settings settings;

    GW::HookEntry stoc_hook_entry;
    bool callbacks_registered = false;
    ReplayFile::MessageTable message_table;
    uint64_t message_table_digest = 0;
    constexpr uint32_t viewpoint_message_id = 172;
    uintptr_t game_viewpoint_handler = 0;

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
    std::optional<ReplayState::InventoryFilter> inventory_filter;
    std::string status = "Not recording";
    bool record_requested = false;
    bool stopped_until_login = false;
    bool load_keyframe_due = false;
    bool load_finished = false;
    uint32_t next_keyframe_ms = 0;
    uint32_t keyframe_count = 0;
    uint32_t chapter_count = 0;
    uint32_t chapter_map_file = 0;
    GW::Packet::StoC::PostProcess post_process{};

    using AgentNotifyParams_pt = int(__cdecl*)(uint32_t agent_id, uint32_t* definition, uint8_t** params);
    AgentNotifyParams_pt AgentNotifyParams_Func = nullptr;

    using ObserveGame_pt = void(__cdecl*)(uint32_t game_key);
    ObserveGame_pt ObserveGame_Func = nullptr;
    ObserveGame_pt ObserveGame_Ret = nullptr;
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

    bool PveMap(const GW::Constants::MapID map_id)
    {
        return GW::Map::GetMapInfo(map_id) && !GWToolbox::ShouldDisableToolbox(map_id);
    }

    void FinishRecording()
    {
        if (!writer.IsOpen()) {
            return;
        }
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
            Log::Log("Replay Recorder: %s", status.c_str());
            return;
        }
        if (!writer.Close(header)) {
            status = std::format("Could not finish {}", RecordingName());
            Log::Error("Replay Recorder: %s", status.c_str());
            return;
        }
        status = std::format(
            "Saved {}: {} chapters, {} keyframes, {} messages, {} KB, {} layout errors{}", RecordingName(), chapter_count, keyframe_count, header.record_count, size / 1024, header.layout_errors,
            header.flags & ReplayFile::SizeCapReached ? " (size cap reached)" : ""
        );
        Log::Info("Replay Recorder: %s", status.c_str());
    }

    void PackRecord(const PacketBase* packet, std::vector<uint8_t>& out)
    {
        const auto& fields = message_table[packet->header];
        if (!ReplayFile::PackMessage(packet->header, fields.data(), static_cast<uint32_t>(fields.size()), reinterpret_cast<const uint8_t*>(packet), out, name_stripper ? name_stripper->For(static_cast<uint16_t>(packet->header)) : ReplayFile::TextFilter())) {
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

    std::vector<Message> ItemDetails(const uint32_t item_id)
    {
        const auto item = GW::Items::GetItemById(item_id);
        if (!item) {
            return {};
        }
        // CItemTable::AddDetailLow keeps 353's value as a dword at +0x24 (GWCA's value and h0026) and its multiplier at +0x4a.
        ReplayState::ClientItem client_item = {
            .item_id = item->item_id,
            .model_file = item->model_file_id,
            .type = static_cast<uint32_t>(item->type),
            .dye_tint = item->dye.dye_tint,
            .dye_colours = static_cast<uint32_t>(item->dye.dye1) | static_cast<uint32_t>(item->dye.dye2) << 4 | static_cast<uint32_t>(item->dye.dye3) << 8 | static_cast<uint32_t>(item->dye.dye4) << 12,
            .formula = item->item_formula,
            .multiplier = item->is_material_salvageable,
            .interaction = item->interaction,
            .value = item->value | static_cast<uint32_t>(item->h0026) << 16,
            .model_id = item->model_id,
            .quantity = item->quantity,
            .name = item->name_enc ? item->name_enc : L"",
            .profession = item->profession,
            .customizer = item->customized ? item->customized : L"",
        };
        if (item->mod_struct) {
            std::ranges::transform(std::span(item->mod_struct, item->mod_struct_size), std::back_inserter(client_item.code), &GW::ItemModifier::mod);
        }
        return ReplayState::ItemReport(message_table, client_item);
    }

    uint32_t TeamToken(const GW::AgentLiving* living, const GW::WorldContext* world)
    {
        // The tokens every load at hand gives: the players' team ('play', the world's own), foes ('mon1'), the rest ('nonc').
        constexpr uint32_t foes = 0x6D6F6E31;
        constexpr uint32_t others = 0x6E6F6E63;
        return living->allegiance == GW::Constants::Allegiance::Ally_NonAttackable || living->IsPlayer() ? world->playerTeamToken : living->allegiance == GW::Constants::Allegiance::Enemy ? foes : others;
    }

    ReplayState::World Snapshot()
    {
        ReplayState::World world(message_table);
        const auto apply = [&world](const Message& message) {
            if (!message.fields.empty() || message_table[message.id].size() <= 1) {
                world.Apply(message);
            }
        };
        const auto apply_all = [&apply](const std::vector<Message>& messages) {
            std::ranges::for_each(messages, apply);
        };
        const auto make = [](const uint16_t id, std::initializer_list<uint32_t> values) {
            auto message = ReplayState::Blank(message_table, id);
            size_t field = 0;
            for (const auto value : values) {
                message.Set(field++, value);
            }
            return message;
        };
        const auto char_context = GW::GetCharContext();
        const auto world_context = GW::GetWorldContext();
        const auto controlled = GW::Agents::GetControlledCharacter();
        const auto agents = GW::Agents::GetAgentArray();
        if (!char_context || !world_context || !controlled || !agents) {
            return world;
        }
        const auto map_id = GW::Map::GetMapID();

        // 380's handler keeps its fields at +0x5c, +0x60, +0x54 and +0x58 of the mission context.
        apply(make(MissionInstanceLoadHead, {char_context->h0054[2] & 0xFF, char_context->h0054[3] & 0xFF, char_context->h0054[0] & 0xFF, char_context->h0054[1] & 0xFF}));
        apply(ReplayState::Blank(message_table, MissionConfigEnd));
        auto player_name = ReplayState::Blank(message_table, MissionInstanceLoadPlayerName);
        if (!player_name.fields.empty()) {
            player_name.fields[0].text = char_context->player_name;
        }
        apply(player_name);
        constexpr uint32_t instance_flag_0x20 = 0x20;
        apply(make(
            GAME_SMSG_INSTANCE_LOAD_INFO, {char_context->player_number, static_cast<uint32_t>(char_context->current_map_id), static_cast<uint32_t>(char_context->current_map_type), char_context->district_number,
                                           static_cast<uint32_t>(char_context->language), 0, (char_context->player_flags & instance_flag_0x20) != 0}
        ));
        for (const auto& quest : world_context->quest_log) {
            constexpr uint32_t log_state_client_bit = 1;
            auto general = make(CharQuestGeneralInfo, {static_cast<uint32_t>(quest.quest_id), quest.log_state & ~log_state_client_bit});
            if (general.fields.size() > 5) {
                general.fields[2].text = quest.location ? quest.location : L"";
                general.fields[3].text = quest.name ? quest.name : L"";
                general.fields[4].text = quest.npc ? quest.npc : L"";
                general.Set(5, static_cast<uint32_t>(quest.map_from));
            }
            apply(general);
            if (!std::isfinite(quest.marker.x) || !std::isfinite(quest.marker.y)) {
                continue;
            }
            auto marker = make(CharQuestAddMarker, {static_cast<uint32_t>(quest.quest_id)});
            if (marker.fields.size() > 3) {
                marker.SetF(1, quest.marker.x, 0);
                marker.SetF(1, quest.marker.y, 1);
                marker.Set(2, quest.marker.zplane);
                marker.Set(3, static_cast<uint32_t>(quest.map_to));
            }
            apply(marker);
        }
        apply(make(CharPlayerAttrMaxKurzick, {world_context->max_kurzick}));
        apply(make(CharPlayerAttrMaxLuxon, {world_context->max_luxon}));
        apply(make(CharPlayerAttrMaxBalthazar, {world_context->max_balth}));
        apply(make(CharPlayerAttrMaxImperial, {world_context->max_imperial}));
        apply(make(CharContextValue112, {world_context->is_hard_mode_unlocked}));
        if (world_context->accountInfo) {
            const auto& account = *world_context->accountInfo;
            auto name = make(CharAccountNameData, {});
            if (name.fields.size() > 6) {
                name.fields[0].text = account.account_name ? account.account_name : L"";
                const uint32_t values[] = {account.wins, account.losses, account.rating, account.qualifier_points, account.rank, account.tournament_reward_points};
                for (size_t i = 0; i < std::size(values); i++) {
                    name.Set(i + 1, values[i]);
                }
            }
            apply(name);
        }
        apply(make(CharToClient153, {static_cast<uint32_t>(map_id), 0}));
        constexpr uint32_t manifest_size = 898;
        for (const auto phase : {0u, 1u}) {
            apply(make(MissionManifestPhase, {phase}));
        }
        apply(make(MissionManifestComplete, {2, manifest_size, 0}));
        for (const auto phase : {0u, 1u}) {
            apply(make(MissionManifestPhase, {phase}));
        }
        apply(make(MissionManifestComplete, {0, static_cast<uint32_t>(map_id), 0}));
        // 406's handler (0x0084d640) hands its map file to the map's load, which asserts it non-zero (File.cpp:367); AreaInfo::file_id is 0 for most PvE maps.
        auto load_map = make(MissionLoadMap, {chapter_map_file ? chapter_map_file : PathfindingWindow::GetMapFileId(map_id)});
        if (load_map.fields.size() > 5 && load_map.fields[5].numbers.size() == sizeof(uint64_t)) {
            constexpr uint32_t facing_steps = 255;
            constexpr auto turn = 2 * std::numbers::pi_v<float>;
            load_map.SetF(1, controlled->pos.x, 0);
            load_map.SetF(1, controlled->pos.y, 1);
            load_map.Set(2, controlled->pos.zplane);
            load_map.Set(3, static_cast<uint32_t>(std::lround(std::fmod(controlled->rotation_angle + turn, turn) * facing_steps / turn)) % facing_steps);
            // The server's clock as a FILETIME: its handler keeps it as an offset from the client's (+0x378).
            constexpr uint64_t unix_epoch_as_filetime = 116444736000000000;
            const auto server_time = unix_epoch_as_filetime + std::chrono::duration_cast<std::chrono::duration<uint64_t, std::ratio<1, 10000000>>>(std::chrono::system_clock::now().time_since_epoch()).count();
            for (size_t i = 0; i < sizeof(uint64_t); i++) {
                load_map.Set(5, static_cast<uint8_t>(server_time >> (8 * i)), i);
            }
        }
        apply(load_map);
        apply(make(CharPlayerTeam, {world_context->playerTeamToken}));
        struct TeamRow {
            uint32_t team;
            uint32_t party;
            uint32_t value;
            uint32_t colour;
            uint32_t territory;
            wchar_t name[20];
        };
        struct TeamTable {
            uint32_t count;
            const TeamRow* const* by_party;
            uint32_t capacity;
            uint32_t by_party_count;
        };
        // 430's handler keeps its rows by party id in MissionCommon::CTeamPartyTable, at +0x398 of the mission context (GWCA's CharContext).
        const auto teams = reinterpret_cast<const TeamTable*>(reinterpret_cast<const uint8_t*>(char_context) + 0x398);
        for (uint32_t party = 0; teams->by_party && party < teams->by_party_count; party++) {
            const auto row = teams->by_party[party];
            if (!row) {
                continue;
            }
            auto team = make(MissionTeam, {row->party, row->team, row->value, row->colour, row->territory});
            if (team.fields.size() > 5) {
                team.fields[5].text.assign(row->name, wcsnlen(row->name, std::size(row->name)));
                apply(team);
            }
        }
        const auto explorable = GW::Map::GetInstanceType() == GW::Constants::InstanceType::Explorable;
        // 91's handler keeps its value at the world context's +0x53c; of the loads at hand only the explorable ones in hard mode send it.
        if (explorable && world_context->h053C[0]) {
            apply(make(CharToClient91, {world_context->h053C[0]}));
        }
        apply(make(CharPostProcess, {post_process.level, post_process.tint}));
        apply(make(MissionToClient404, {0}));

        const auto is_character = [world_context](const uint32_t agent_id) {
            return std::ranges::any_of(world_context->party_profession_states, [agent_id](const GW::ProfessionState& state) {
                return state.agent_id == agent_id;
            });
        };
        for (const auto& state : world_context->party_profession_states) {
            const auto living = GW::Agents::GetAgentByID(state.agent_id) ? GW::Agents::GetAgentByID(state.agent_id)->GetAsAgentLiving() : nullptr;
            apply(make(CharAttributeState, {state.agent_id, 0, 0}));
            apply(make(CharProfessions, {state.agent_id, static_cast<uint32_t>(state.primary), static_cast<uint32_t>(state.secondary), 0}));
            if (state.unlocked_professions) {
                apply(make(CharPlayerUnlockedProfession, {state.agent_id, state.unlocked_professions}));
            }
            if (const auto skillbar = GW::SkillbarMgr::GetSkillbar(state.agent_id); skillbar && message_table[CharSkillbar].size() > 3) {
                auto skills = make(CharSkillbar, {state.agent_id});
                for (const auto& skill : skillbar->skills) {
                    skills.fields[1].numbers.push_back(static_cast<uint32_t>(skill.skill_id));
                    skills.fields[2].numbers.push_back(0);
                }
                skills.Set(3, 1);
                apply(skills);
            }
            if (living) {
                apply(make(CharNotifyDword, {MaxEnergy, state.agent_id, living->max_energy}));
                apply(make(CharNotifyDword, {MaxHealth, state.agent_id, living->max_hp}));
            }
            const auto attributes = std::ranges::find(world_context->attributes, state.agent_id, &GW::PartyAttribute::agent_id);
            if (attributes != world_context->attributes.end() && message_table[CharAttributes].size() > 2) {
                std::vector<const GW::Attribute*> raised;
                // Past the attributes' own slots (ids 0 to 44) the array holds entries no 58 at hand sends.
                for (size_t index = 0; index < std::size(attributes->attribute); index++) {
                    if (const auto& attribute = attributes->attribute[index]; attribute.level && static_cast<size_t>(attribute.id) == index) {
                        raised.push_back(&attribute);
                    }
                }
                auto words = make(CharAttributes, {state.agent_id});
                auto& numbers = words.fields[1].numbers;
                for (const auto attribute : raised) {
                    numbers.push_back(static_cast<uint32_t>(attribute->id));
                }
                for (const auto attribute : raised) {
                    numbers.push_back(attribute->level_base);
                }
                for (const auto attribute : raised) {
                    numbers.push_back(attribute->level);
                }
                apply(words);
            }
        }
        for (const auto& hero : world_context->hero_info) {
            const auto primary = static_cast<uint32_t>(hero.primary);
            const auto secondary = static_cast<uint32_t>(hero.secondary);
            const auto flag = hero.h0044 & 1;
            // CHeroMgr::OnHeroDataAdd keeps 115's and 116's fields in this entry: 115's skills from +0x24, 116's appearance, armor and dyes from +0x48.
            if (hero.hero_id >= GW::Constants::HeroID::Merc1 && hero.hero_id <= GW::Constants::HeroID::Merc8) {
                auto mercenary = make(CharMercenaryInfo, {hero.hero_id, hero.level, primary, secondary, hero.hero_file_id, hero.model_file_id, hero.h001C, flag, hero.appearance_bitmap});
                constexpr size_t armor_pieces = 5;
                if (mercenary.fields.size() > 9 + 2 * armor_pieces) {
                    for (size_t piece = 0; piece < armor_pieces; piece++) {
                        mercenary.Set(9 + piece, reinterpret_cast<const uint32_t*>(&hero.chestpiece_dye_tint)[piece]);
                        mercenary.Set(9 + armor_pieces + piece, reinterpret_cast<const uint32_t*>(hero.chestpiece_dye)[piece]);
                    }
                    mercenary.fields[9 + 2 * armor_pieces].text.assign(hero.name, wcsnlen(hero.name, std::size(hero.name)));
                }
                apply(mercenary);
                continue;
            }
            auto data = make(CharHeroDataAdd, {hero.hero_id, hero.level, primary, secondary, hero.hero_file_id, hero.model_file_id});
            if (data.fields.size() > 8) {
                constexpr uint32_t skills_at_most = 8;
                data.fields[6].numbers.assign(&hero.h0024, &hero.h0024 + std::min(hero.h0020, skills_at_most));
                data.Set(7, hero.h001C);
                data.Set(8, flag);
            }
            apply(data);
        }
        if (const auto inventory = GW::Items::GetInventory()) {
            apply(make(ItemInventoryCreate, {inventory->inventory_id(), 0}));
            apply(make(ItemInventoryEquipSetActivate, {inventory->inventory_id(), inventory->active_weapon_set}));
        }
        for (const auto& flag : world_context->hero_flags) {
            apply(make(ItemInventoryCreate, {flag.inventory_id, 1}));
            apply(make(CharHeroActivate, {flag.hero_id, flag.agent_id, flag.inventory_id, static_cast<uint32_t>(flag.hero_behavior)}));
            apply(make(CharHeroSkillStatusBitmap, {flag.agent_id, 0}));
        }
        if (world_context->unlocked_character_skills.size()) {
            auto unlocked = ReplayState::Blank(message_table, CharUnlockedSkills);
            if (!unlocked.fields.empty()) {
                unlocked.fields[0].numbers.assign(world_context->unlocked_character_skills.begin(), world_context->unlocked_character_skills.end());
            }
            apply(unlocked);
        }
        apply(make(
            CharPlayerAttrSet,
            {world_context->experience, world_context->current_kurzick, world_context->total_earned_kurzick, world_context->current_luxon, world_context->total_earned_luxon, world_context->current_imperial, world_context->total_earned_imperial, 0, 0,
             world_context->level, world_context->morale, world_context->current_balth, world_context->total_earned_balth, world_context->current_skill_points, world_context->total_earned_skill_points}
        ));

        apply(make(AgentConnect, {GW::Map::GetInstanceTime()}));
        for (const auto agent : *agents) {
            if (!agent) {
                continue;
            }
            auto create = ReplayState::Blank(message_table, AgentCreate);
            if (create.fields.size() < 23) {
                break;
            }
            const auto living = agent->GetAsAgentLiving();
            const auto item = agent->GetAsAgentItem();
            if (!living && !item) {
                continue;
            }
            constexpr uint32_t npc_class = 0x20000000;
            constexpr uint32_t player_class = 0x30000000;
            constexpr uint32_t living_def = 1;
            constexpr uint32_t item_def = 4;
            constexpr uint32_t npc_flags = 9;
            constexpr uint32_t player_flags = 5;
            constexpr float living_radius = 12.f;
            constexpr float default_speed = 288.f;
            const auto infinity = std::numeric_limits<float>::infinity();
            const auto agent_class = living ? (living->IsPlayer() ? player_class | living->login_number : npc_class | living->player_number) : item->item_id;
            const uint32_t values[] = {agent->agent_id, agent_class, living ? living_def : item_def, living ? (living->IsPlayer() ? player_flags : npc_flags) : 0};
            for (size_t i = 0; i < std::size(values); i++) {
                create.Set(i, values[i]);
            }
            create.SetF(4, agent->pos.x, 0);
            create.SetF(4, agent->pos.y, 1);
            create.Set(5, agent->pos.zplane);
            create.SetF(6, agent->rotation_cos, 0);
            create.SetF(6, agent->rotation_sin, 1);
            create.Set(7, 1);
            create.SetF(8, living ? default_speed : 0.f);
            create.SetF(9, 1.f);
            create.SetF(10, living ? living_radius : 0.f);
            create.Set(11, living ? TeamToken(living, world_context) : 0);
            create.SetF(18, infinity, 0);
            create.SetF(18, infinity, 1);
            create.SetF(21, infinity, 0);
            create.SetF(21, infinity, 1);
            if (living && !living->IsPlayer() && living->player_number < world_context->npcs.size()) {
                const auto name = GW::Agents::GetAgentEncName(agent);
                const auto npc_name = world_context->npcs[living->player_number].name_enc;
                if (name && !(npc_name && wcscmp(name, npc_name) == 0)) {
                    auto update = make(CharAgentUpdateNpcName, {agent->agent_id});
                    if (update.fields.size() > 1) {
                        update.fields[1].text = name;
                    }
                    apply(update);
                }
            }
            if (living) {
                if (static_cast<uint32_t>(living->primary) || static_cast<uint32_t>(living->secondary)) {
                    apply(make(CharAgentUpdateProfession, {agent->agent_id, static_cast<uint32_t>(living->primary), static_cast<uint32_t>(living->secondary)}));
                }
                apply(make(CharNotifyDword, {Level, agent->agent_id, living->level}));
                apply(make(CharAgentInitialEffects, {agent->agent_id, living->effects}));
            }
            if (item) {
                apply_all(ItemDetails(item->item_id));
                if (item->owner) {
                    // All 13 309s at hand carry 600.0.
                    constexpr float assignment_seconds = 600.f;
                    apply(make(ItemAssign, {item->item_id, item->owner, std::bit_cast<uint32_t>(assignment_seconds)}));
                }
            }
            apply(create);
            if (!living) {
                continue;
            }
            // The agent view keeps 53's interval and modifier at +0xEC and +0xF0, and asserts the interval before an attack's warmup.
            if (living->weapon_attack_speed != 0.f && living->attack_speed_modifier != 0.f) {
                apply(make(CharAttackInterval, {agent->agent_id, std::bit_cast<uint32_t>(living->weapon_attack_speed), std::bit_cast<uint32_t>(living->attack_speed_modifier)}));
            }
            apply(make(CharAgentAttrUpdateFloat, {health_value, agent->agent_id, std::bit_cast<uint32_t>(living->hp)}));
            if (living->hp_pips) {
                apply(make(CharAgentAttrUpdateFloat, {health_regen_value, agent->agent_id, std::bit_cast<uint32_t>(living->hp_pips)}));
            }
            if (is_character(agent->agent_id)) {
                apply(make(CharAgentAttrUpdateFloat, {energy_value, agent->agent_id, std::bit_cast<uint32_t>(living->energy)}));
                apply(make(CharAgentAttrUpdateFloat, {energy_regen_value, agent->agent_id, std::bit_cast<uint32_t>(living->energy_regen)}));
            }
            uint32_t definition = 0;
            uint8_t* character = nullptr;
            constexpr uint32_t character_definition = 1;
            if (!AgentNotifyParams_Func || !AgentNotifyParams_Func(agent->agent_id, &definition, &character) || definition != character_definition || !character) {
                continue;
            }
            // The char client keeps what an agent wears at +0x24, in s2c 110's slot order (CharCliOnEquipUpdate, CharCliAgentGetEquippedItemId).
            const auto worn = reinterpret_cast<const uint32_t*>(character + 0x24);
            if (living->IsPlayer()) {
                auto equipment = make(CharEquipment, {agent->agent_id});
                for (size_t slot = 0; slot + 1 < equipment.fields.size(); slot++) {
                    apply_all(ItemDetails(worn[slot]));
                    equipment.Set(slot + 1, worn[slot]);
                }
                apply(equipment);
            }
            else {
                apply_all(ItemDetails(worn[0]));
                apply_all(ItemDetails(worn[1]));
                apply(make(CharNpcUpdateWeapons, {agent->agent_id, worn[0], worn[1]}));
            }
        }
        for (uint32_t npc_id = 0; npc_id < world_context->npcs.size(); npc_id++) {
            const auto& npc = world_context->npcs[npc_id];
            if (!npc.model_file_id) {
                continue;
            }
            auto properties = make(CharNpcProperties, {npc_id, npc.model_file_id, npc.skin_file_id, std::bit_cast<uint32_t>(npc.visual_adjustment), npc.appearance, npc.npc_flags, static_cast<uint32_t>(npc.primary), npc.default_level});
            if (properties.fields.size() > 8) {
                properties.fields[8].text = npc.name_enc ? npc.name_enc : L"";
            }
            apply(properties);
            auto model = make(CharNpcModel, {npc_id});
            if (model.fields.size() > 1 && npc.model_files) {
                model.fields[1].numbers.assign(npc.model_files, npc.model_files + npc.files_count);
            }
            apply(model);
        }
        for (const auto& player : world_context->players) {
            if (!player.agent_id) {
                continue;
            }
            // Every load at hand fills 89's sixth field with 0xCCCCCCCC, the server's unset bytes.
            constexpr uint32_t unset_bytes = 0xCCCCCCCC;
            auto row = make(CharPlayerCreate, {player.player_number, player.agent_id, player.appearance_bitmap, static_cast<uint32_t>(player.secondary), 0, unset_bytes});
            if (row.fields.size() > 6) {
                row.fields[6].text = player.name ? player.name : L"";
            }
            apply(row);
            apply(make(CharUpdateAgentPartysize, {player.player_number, player.party_size}));
            apply(make(CharPlayerLink, {player.player_number, player.party_leader_player_number}));
            apply(make(CharTitleRankDisplay, {player.player_number, player.active_title_tier}));
        }
        constexpr size_t morale_rows_at_most = 64;
        constexpr uint32_t lowest_morale = 40;
        constexpr uint32_t highest_morale = 110;
        size_t morale_rows = 0;
        for (const auto& morale : world_context->morale_table) {
            if (++morale_rows > morale_rows_at_most) {
                break;
            }
            if (is_character(morale.agent_id) && morale.morale >= lowest_morale && morale.morale <= highest_morale) {
                apply(make(CharAgentMorale, {morale.agent_id, morale.morale}));
            }
        }
        // The server sends a title only with a tier, and the tiers its titles are at and reach next.
        std::set<uint32_t> title_tiers;
        for (uint32_t index = 0; index < world_context->titles.size(); index++) {
            const auto& title = world_context->titles[index];
            if (!title.next_title_tier_index) {
                continue;
            }
            title_tiers.insert({title.current_title_tier_index, title.next_title_tier_index});
            auto track = make(
                CharTitleTrackInfo,
                {index, title.props, title.current_points, title.current_title_tier_index, title.points_needed_current_rank, title.h0010, title.next_title_tier_index, title.points_needed_next_rank, title.max_title_rank, title.max_title_tier_index}
            );
            if (track.fields.size() > 11) {
                track.fields[10].text = title.points_desc ? title.points_desc : L"";
                track.fields[11].text = title.h0028 ? title.h0028 : L"";
            }
            apply(track);
        }
        for (const auto index : title_tiers) {
            if (index >= world_context->title_tiers.size()) {
                continue;
            }
            const auto& tier = world_context->title_tiers[index];
            auto rank = make(CharTitleRankData, {index, tier.props, tier.tier_number});
            if (rank.fields.size() > 3 && tier.tier_name_enc) {
                rank.fields[3].text = tier.tier_name_enc;
            }
            apply(rank);
        }
        if (const auto guild_context = GW::GetGuildContext()) {
            const auto set_key = [](Message& message, const GW::GHKey& key) {
                for (size_t i = 0; i < sizeof(key); i++) {
                    message.Set(1, reinterpret_cast<const uint8_t*>(&key)[i], i);
                }
            };
            for (const auto guild : guild_context->guilds) {
                if (!guild) {
                    continue;
                }
                const auto own = guild->index == guild_context->player_guild_index;
                auto info = ReplayState::Blank(message_table, own ? GuildGeneralInfo : GuildAllianceInfo);
                if (info.fields.size() < (own ? 19u : 15u)) {
                    continue;
                }
                // 289 keeps its sixth field at +0xac past GWCA's Guild (k09); the last field of both is 0 in all 318 at hand.
                const auto& cape = guild->cape;
                std::vector<uint32_t> values = {guild->features};
                if (own) {
                    values.push_back(*reinterpret_cast<const uint32_t*>(reinterpret_cast<const uint8_t*>(guild) + 0xAC));
                }
                values.insert(values.end(), {cape.cape_bg_color, cape.cape_detail_color, cape.cape_emblem_color, cape.cape_shape, cape.cape_detail, cape.cape_emblem, cape.cape_trim, guild->faction, guild->faction_point});
                if (own) {
                    values.insert(values.end(), {guild->qualifier_point, guild->rating, guild->rank});
                }
                values.push_back(0);
                info.Set(0, guild->index);
                set_key(info, guild->key);
                info.fields[2].text.assign(guild->name, wcsnlen(guild->name, std::size(guild->name)));
                info.fields[3].text.assign(guild->tag, wcsnlen(guild->tag, ReplayFile::guild_tag_length));
                for (size_t i = 0; i < values.size(); i++) {
                    info.Set(4 + i, values[i]);
                }
                apply(info);
            }
            if (guild_context->player_guild_index) {
                auto assign = make(GuildAssign, {guild_context->player_guild_index});
                if (assign.fields.size() > 2) {
                    set_key(assign, guild_context->player_gh_key);
                    assign.Set(2, guild_context->player_guild_rank);
                    apply(assign);
                }
            }
        }
        if (const auto party_effects = GW::Effects::GetPartyEffectsArray()) {
            for (const auto& effects : *party_effects) {
                for (const auto& effect : effects.effects) {
                    // An effect with no duration came as an upkeep (65): no 66 in any recording at hand has a duration of 0.
                    if (effect.duration) {
                        apply(make(CharEffectApplied, {effects.agent_id, static_cast<uint32_t>(effect.skill_id), effect.attribute_level, effect.effect_id, std::bit_cast<uint32_t>(effect.GetTimeRemaining() / 1000.f)}));
                    }
                    else if (!effect.agent_id) {
                        apply(make(CharEffectUpkeepApplied, {effects.agent_id, 0, static_cast<uint32_t>(effect.skill_id), effect.attribute_level, effect.effect_id}));
                    }
                }
                for (const auto& buff : effects.buffs) {
                    // GWCA lists an upkeep under its caster; 63 and 65 name its target first.
                    const auto target = buff.target_agent_id ? buff.target_agent_id : effects.agent_id;
                    for (const auto id : {CharEffectUpkeepAdded, CharEffectUpkeepApplied}) {
                        apply(make(id, {target, effects.agent_id, static_cast<uint32_t>(buff.skill_id), buff.h0004, buff.buff_id}));
                    }
                }
            }
        }
        apply(make(AgentReporter, {controlled->agent_id, 3}));
        if (const auto party = GW::PartyMgr::GetPartyInfo()) {
            apply(make(PartyMembershipBegin, {party->party_id}));
            for (const auto& member : party->players) {
                apply(make(PartyMemberAdd, {party->party_id, member.login_number, 1}));
            }
            for (const auto& hero : party->heroes) {
                apply(make(PartyHeroAdd, {party->party_id, hero.owner_player_id, hero.agent_id, static_cast<uint32_t>(hero.hero_id), hero.level}));
            }
            apply(make(PartyMembershipEnd, {party->party_id}));
            apply(make(PartyAssign, {party->party_id, party->players.size() ? party->players[0].login_number : 0}));
            apply(make(PartySetDifficulty, {GW::PartyMgr::GetIsPartyInHardMode()}));
        }
        constexpr uint32_t access_rights_mask = 0xD;
        apply(make(MissionAccessRights, {char_context->player_flags & access_rights_mask}));
        apply(make(PartyToClient446, {0}));
        if (explorable) {
            apply(ReplayState::Blank(message_table, CharMissionStreamStart));
        }
        apply(ReplayState::Blank(message_table, MissionInstanceLoadFinish));
        return world;
    }

    void WriteKeyframe(const ReplayFile::KeyframeKind kind)
    {
        const auto started = Clock::now();
        const auto world = Snapshot();
        const auto time_ms = ElapsedMs(recording_start);
        std::vector<ReplayFile::Record> records;
        uint64_t bytes = 0;
        size_t not_encoded = 0;
        for (const auto& message : world.Join()) {
            auto encoded = Encoded(message);
            if (encoded.empty()) {
                not_encoded++;
                continue;
            }
            bytes += encoded.size() + 6;
            records.push_back({time_ms, std::move(encoded)});
        }
        if (records.empty() || !writer.AppendKeyframe(time_ms, kind, records)) {
            Log::Log("Replay Recorder: keyframe %s not written: the client's state could not be read", ReplayFile::KeyframeKindName(kind));
            return;
        }
        keyframe_count++;
        next_keyframe_ms = time_ms + keyframe_interval_ms;
        Log::Log(
            "Replay Recorder: keyframe %u (%s) at %u ms: %zu messages, %zu not encoded against the client's table, %llu KB, read and written in %u ms", keyframe_count, ReplayFile::KeyframeKindName(kind), time_ms, records.size(), not_encoded, bytes / 1024,
            ElapsedMs(started)
        );
    }

    void WriteChapter(const uint32_t map_id, const bool explorable)
    {
        const auto map_info = GW::Map::GetMapInfo(static_cast<GW::Constants::MapID>(map_id));
        writer.AppendChapter({0, ElapsedMs(recording_start), map_id, explorable, map_info ? static_cast<uint32_t>(map_info->type) : 0});
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
            Log::Error("Replay Recorder: %s", status.c_str());
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
                Log::Log("Replay Recorder: the char client's equipped item lookup is not found in this client: keyframes leave out what agents wear");
            }
        }
        if (!writer.Open(recording_path, header, message_table)) {
            status = std::format("Could not create {}", RecordingName());
            Log::Error("Replay Recorder: %s", status.c_str());
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
        recording_start = last_record_at = last_flush_at = Clock::now();
        WriteChapter(map_id, explorable);
        status = std::format("Recording {}", RecordingName());
        Log::Log("Replay Recorder: %s", status.c_str());
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
        for (const auto& details : shown_before) {
            if (const auto encoded = Encoded(details); !encoded.empty() && !WriteRecord(time_ms, encoded)) {
                return false;
            }
        }
        return WriteRecord(time_ms, bytes);
    }

    void OnServerMessage(GW::HookStatus*, PacketBase* packet, void*)
    {
        if (emulating_rows || playing_replay) {
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
            if (settings.record_from_login && !stopped_until_login && packet->header == MissionInstanceLoadHead && char_context && PveMap(char_context->map_id)) {
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
        // A map's after-load keyframe waits for the load's last message: an outpost's 446, 366, 398 and 404 come about 0.5 s after its 31.
        if (packet->header == MissionInstanceLoadFinish) {
            load_finished = true;
        }
        if (packet->header == MissionLoadMap) {
            chapter_map_file = static_cast<const GW::Packet::StoC::InstanceLoadFile*>(packet)->map_fileID;
        }
        if (packet->header == CharPostProcess) {
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
        if (writer.IsOpen() && now - last_flush_at > flush_interval) {
            writer.Flush();
            last_flush_at = now;
        }
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
        for (const auto hooked :
             {reinterpret_cast<void*>(GameConnect_Func), reinterpret_cast<void*>(Goodbye_Func), reinterpret_cast<void*>(InstanceAck_Func), reinterpret_cast<void*>(MapLoaded_Func), reinterpret_cast<void*>(WorldMapToggle_Func),
              reinterpret_cast<void*>(MissionMapCreate_Func), reinterpret_cast<void*>(AgentAdvance_Func)}) {
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
        if (GW::Hook::CreateHook(&GameConnect_Func, OnGameConnect, &GameConnect_Ret) != 0 || GW::Hook::CreateHook(&Goodbye_Func, OnGoodbye, &Goodbye_Ret) != 0 || GW::Hook::CreateHook(&InstanceAck_Func, OnInstanceAck, &InstanceAck_Ret) != 0 ||
            GW::Hook::CreateHook(&MapLoaded_Func, OnMapLoaded, &MapLoaded_Ret) != 0 || GW::Hook::CreateHook(&WorldMapToggle_Func, OnWorldMapToggle, &WorldMapToggle_Ret) != 0 ||
            GW::Hook::CreateHook(&MissionMapCreate_Func, OnMissionMapCreate, &MissionMapCreate_Ret) != 0 || (AgentAdvance_Func && GW::Hook::CreateHook(&AgentAdvance_Func, OnAgentAdvance, &AgentAdvance_Ret) != 0)) {
            DisarmPlayback();
            return "Playback not available: its hooks could not be made";
        }
        for (const auto hooked :
             {reinterpret_cast<void*>(GameConnect_Func), reinterpret_cast<void*>(Goodbye_Func), reinterpret_cast<void*>(InstanceAck_Func), reinterpret_cast<void*>(MapLoaded_Func), reinterpret_cast<void*>(WorldMapToggle_Func),
              reinterpret_cast<void*>(MissionMapCreate_Func), reinterpret_cast<void*>(AgentAdvance_Func)}) {
            if (hooked) {
                GW::Hook::EnableHooks(hooked);
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

    void StartPlayback(const ReplayObserve::Row& row)
    {
        ReplayFile::Contents contents;
        std::string error;
        auto made = !ReplayFile::Read(row.path, contents, error)                                                             ? ReplayWire::Plan{.error = error}
                    : !row.keyframe || *row.keyframe >= contents.keyframes.size() || row.chapter >= contents.chapters.size() ? ReplayWire::Plan{.error = "its keyframe is not in the file"}
                                                                                                                             : ReplayWire::MakeKeyframePlan(contents, contents.keyframes[*row.keyframe]);
        const auto name = std::format("{} (map {}, from {} ms)", TextUtils::WStringToString(row.path.filename().wstring()), row.header.map_id, made.start_ms);
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
        const auto char_context = GW::GetCharContext();
        transfer.game_type = char_context ? char_context->token1 : 0;
        const auto joined_map = JoinedMap();
        transfer.territory = contents.header.region;
        transfer.map_id = contents.chapters[row.chapter].map_id;
        transfer.map_type = contents.chapters[row.chapter].is_explorable;
        transfer.key = RandomBytes()();
        transfer_due = true;
        playback_status = std::format("Playing {}", name);
        std::string changes;
        for (const auto& change : plan.changes) {
            changes += change.kind == ReplayWire::ChangeKind::LeftOut     ? std::format("; left out: {} records from record {} ({})", change.count, change.record, change.why)
                       : change.kind == ReplayWire::ChangeKind::BroughtIn ? std::format("; brought in: {} records before record {} ({})", change.count, change.record, change.why)
                                                                          : std::format("; message {}: 410's observer byte set ({})", change.record, change.why);
        }
        LogPlayback(
            std::format(
                "{}: {} posts, {} keyframe messages, the recording from record {} ({} ms) to record {}, its stream timed from {} ms, {} ms long, controlled agent {}{}", playback_status, plan.posts.size(), plan.keyframe_records, plan.first_record,
                plan.start_ms, plan.end_record, plan.entry_ms, plan.duration_ms, plan.controlled_agent, changes
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
                    FinishPlayback(std::format("The game did not start the replay's connection (mission flags {:#x}, the join's map {}, a game connection {})", flags, JoinedMap(), *game_connection_global ? "open" : "none"));
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
} // namespace

void ReplayRecorderModule::Initialize()
{
    ToolboxModule::Initialize();
    SettingsRegistry::Register(this, settings);
    std::scoped_lock lock(recording_mutex);
    ReadMessageTable();
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
    GW::StoC::RemoveCallbacks(&observe_hook_entry);
    DropListedRows();
    std::scoped_lock lock(recording_mutex);
    RemoveCallbacks();
    FinishRecording();
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
    const auto settled = GW::Map::GetInstanceType() == GW::Constants::InstanceType::Outpost && GW::Map::GetIsMapLoaded() && !GW::Map::GetIsObserving() && !(MissionFlags() & (join_pending | transfer_kept));
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
        "Each recording made on this client build appears once in the Observe window (B, in an outpost) under \"Custom Event (0)\", with its date, maps and length; it plays from its first explorable map's first keyframe (outposts are not played yet). Choosing one is answered here; nothing is sent to the server."
    );
    if (!observe_status.empty()) {
        ImGui::TextDisabled("%s", observe_status.c_str());
    }
    ImGui::Checkbox("Play recordings from the Observe window", &settings.play_from_observe_window);
    ImGui::ShowHelp(
        "Observe on one of your recordings plays it in the game's observer mode, fed by the toolbox: nothing of the replay goes to the server. The Replay window shows while it plays; its Stop, or stop observing (B, then Yes), brings you back to your outpost. Needs the list above."
    );
    if (!playback_status.empty()) {
        ImGui::TextDisabled("%s", playback_status.c_str());
    }
    if (playback != Playback::Off) {
        ImGui::TextDisabled("A replay is playing: its controls are in the Replay window.");
    }
}
