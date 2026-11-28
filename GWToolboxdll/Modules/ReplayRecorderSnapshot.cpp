#include "stdafx.h"

#include <GWCA/Context/AgentContext.h>
#include <GWCA/Context/CharContext.h>
#include <GWCA/Context/GadgetContext.h>
#include <GWCA/Context/GameContext.h>
#include <GWCA/Context/GuildContext.h>
#include <GWCA/Context/WorldContext.h>
#include <GWCA/GameEntities/Agent.h>
#include <GWCA/GameEntities/Guild.h>
#include <GWCA/GameEntities/Hero.h>
#include <GWCA/GameEntities/Item.h>
#include <GWCA/GameEntities/Map.h>
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
#include <GWCA/Managers/PartyMgr.h>
#include <GWCA/Managers/SkillbarMgr.h>
#include <GWCA/Managers/UIMgr.h>
#include <GWCA/Utilities/Scanner.h>
#include <Logger.h>
#include <Modules/ReplayRecorderInternal.h>
#include <Modules/ReplayRecorderModule.h>
#include <Utils/Replay/ReplayFile.h>
#include <Utils/Replay/ReplayMessages.h>
#include <Utils/Replay/ReplayState.h>
#include <Windows/Pathfinding/PathfindingWindow.h>
#include "stdafx.h"

namespace ReplayRecorder {
    namespace {
        // GWCA's AgentInfo: what the char client's notices 0, 14 and 15 write (CharCliOnNotifyDword) and the agent view dresses an agent with (CCharAgent::SetCompositeModel).
        struct AgentLook {
            uint32_t appearance;
            uint32_t composites[5];
            uint32_t colours[5];
            uint32_t h002C[2];
            wchar_t* name_enc;
        };
        static_assert(sizeof(AgentLook) == sizeof(GW::AgentInfo));

        // GWCA's GadgetInfo: a gadget class as 263's handler keeps it; a gadget's agent draws its model file (File.cpp:367 when 0).
        struct GadgetClass {
            uint32_t model_file;
            uint32_t flags;
            uint32_t scale;
            wchar_t* name_enc;
        };
        static_assert(sizeof(GadgetClass) == sizeof(GW::GadgetInfo));

        using ConstRow_pt = const void*(__cdecl*)(uint32_t index);

        struct ConstTable {
            ConstRow_pt row = nullptr;
            uint32_t rows = 0;
        };

        bool const_tables_sought = false;

        ConstTable effect_table;

        ConstTable aura_table;

        ConstTable text_file_table;

        const wchar_t* packed_characters = nullptr;

        // A const table's accessor asserts its index below the table's size before it gives the row: the size is that compare's operand, and the accessor is never called at or past it.
        ConstTable FindConstTable(const char* file, const char* assertion)
        {
            constexpr uint8_t index_to_esi[] = {0x55, 0x8B, 0xEC, 0x56, 0x8B, 0x75, 0x08};
            constexpr uint8_t compare_esi_dword = 0x81;
            constexpr uint8_t compare_esi_byte = 0x83;
            constexpr uint8_t esi = 0xFE;
            const auto accessor = GW::Scanner::ToFunctionStart(GW::Scanner::FindAssertion(file, assertion, 0, 0));
            const auto compare = reinterpret_cast<const uint8_t*>(accessor) + sizeof(index_to_esi);
            if (!accessor || memcmp(compare - sizeof(index_to_esi), index_to_esi, sizeof(index_to_esi)) != 0 || compare[1] != esi) {
                return {};
            }
            if (compare[0] == compare_esi_dword) {
                return {reinterpret_cast<ConstRow_pt>(accessor), *reinterpret_cast<const uint32_t*>(compare + 2)};
            }
            return compare[0] == compare_esi_byte ? ConstTable{reinterpret_cast<ConstRow_pt>(accessor), compare[2]} : ConstTable{};
        }

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

        // GWCA's CharContext is the mission context: 380's handler keeps its bytes at +0x54 (GWCA's h0054), 430's handler its rows in MissionCommon::CTeamPartyTable.
        struct MissionContext {
            uint8_t h0000[0x398];
            TeamTable teams;
        };
        static_assert(offsetof(MissionContext, teams) == 0x398 && GWCA_OFFSETOF(GW::CharContext, h0054) == 0x54);
        static_assert(GWCA_OFFSETOF(GW::WorldContext, h053C) == 0x53C);

        // The fields of GWCA's HeroInfo it leaves unnamed, as CHeroMgr::OnHeroDataAdd fills them from 115 and 116.
        struct HeroData {
            uint8_t h0000[0x1C];
            uint32_t first_value;
            uint32_t skill_count;
            uint32_t skills[8];
            uint32_t flags;
            uint32_t h0048;
            uint32_t armour[5];
            uint32_t dyes[5];
            wchar_t h0074[20];
        };
        static_assert(sizeof(HeroData) == sizeof(GW::HeroInfo) && offsetof(HeroData, armour) == GWCA_OFFSETOF(GW::HeroInfo, chestpiece_dye_tint) && offsetof(HeroData, dyes) == GWCA_OFFSETOF(GW::HeroInfo, chestpiece_dye));

        // A guild entry past GWCA's Guild: 289's sixth field.
        struct GuildEntry {
            uint8_t h0000[0xAC];
            uint32_t general_value;
        };
        static_assert(sizeof(GW::Guild) == offsetof(GuildEntry, general_value));

        // GWCA's AgentContext: the server world's clock, which s2c 31 sets and 30 advances, before the shown world's (instance_timer) that follows it.
        struct AgentClocks {
            uint8_t h0000[0x148];
            uint32_t server_time;
            uint8_t h014C[0x60];
            uint32_t shown_time;
        };
        static_assert(offsetof(AgentClocks, shown_time) == GWCA_OFFSETOF(GW::AgentContext, instance_timer));

        uint32_t TeamToken(const GW::AgentLiving* living, const GW::WorldContext* world)
        {
            // The tokens every load at hand gives: the players' team ('play', the world's own), foes ('mon1'), the rest ('nonc').
            constexpr uint32_t foes = 0x6D6F6E31;
            constexpr uint32_t others = 0x6E6F6E63;
            return living->allegiance == GW::Constants::Allegiance::Ally_NonAttackable || living->IsPlayer() ? world->playerTeamToken : living->allegiance == GW::Constants::Allegiance::Enemy ? foes : others;
        }
        void Apply(ReplayState::World& world, const Message& message)
        {
            if (!message.fields.empty() || message_table[message.id].size() <= 1) {
                world.Apply(message);
            }
        }

        void ApplyAll(ReplayState::World& world, const std::vector<Message>& messages)
        {
            for (const auto& message : messages) {
                Apply(world, message);
            }
        }

        Message Make(const uint16_t id, std::initializer_list<uint32_t> values)
        {
            auto message = ReplayState::Blank(message_table, id);
            size_t field = 0;
            for (const auto value : values) {
                message.Set(field++, value);
            }
            return message;
        }

        bool IsCharacter(const GW::WorldContext* world_context, const uint32_t agent_id)
        {
            return std::ranges::any_of(world_context->party_profession_states, [agent_id](const GW::ProfessionState& state) {
                return state.agent_id == agent_id;
            });
        }

        void SnapshotLoad(ReplayState::World& world, GW::CharContext* char_context, GW::WorldContext* world_context, GW::AgentLiving* controlled, const bool explorable)
        {
            const auto map_id = GW::Map::GetMapID();

            const auto& mission = reinterpret_cast<const MissionContext&>(*char_context);
            // 380's handler keeps its fields 0 to 3 at the mission context's +0x5c, +0x60, +0x54 and +0x58.
            Apply(world, Make(GAME_SMSG_INSTANCE_LOAD_HEAD, {char_context->h0054[2] & 0xFF, char_context->h0054[3] & 0xFF, char_context->h0054[0] & 0xFF, char_context->h0054[1] & 0xFF}));
            Apply(world, ReplayState::Blank(message_table, MissionConfigEnd));
            auto player_name = ReplayState::Blank(message_table, GAME_SMSG_INSTANCE_LOAD_PLAYER_NAME);
            if (!player_name.fields.empty()) {
                player_name.fields[0].text = char_context->player_name;
            }
            Apply(world, player_name);
            constexpr uint32_t instance_flag_0x20 = 0x20;
            Apply(
                world, Make(
                           GAME_SMSG_INSTANCE_LOAD_INFO, {char_context->player_number, static_cast<uint32_t>(char_context->current_map_id), static_cast<uint32_t>(char_context->current_map_type), char_context->district_number,
                                                          static_cast<uint32_t>(char_context->language), 0, (char_context->player_flags & instance_flag_0x20) != 0}
                       )
            );
            for (const auto& quest : world_context->quest_log) {
                constexpr uint32_t log_state_client_bit = 1;
                auto general = Make(GAME_SMSG_QUEST_GENERAL_INFO, {static_cast<uint32_t>(quest.quest_id), quest.log_state & ~log_state_client_bit});
                if (general.fields.size() > 5) {
                    general.fields[2].text = quest.location ? quest.location : L"";
                    general.fields[3].text = quest.name ? quest.name : L"";
                    general.fields[4].text = quest.npc ? quest.npc : L"";
                    general.Set(5, static_cast<uint32_t>(quest.map_from));
                }
                Apply(world, general);
                if (!std::isfinite(quest.marker.x) || !std::isfinite(quest.marker.y)) {
                    continue;
                }
                auto marker = Make(GAME_SMSG_QUEST_ADD_MARKER, {static_cast<uint32_t>(quest.quest_id)});
                if (marker.fields.size() > 3) {
                    marker.SetF(1, quest.marker.x, 0);
                    marker.SetF(1, quest.marker.y, 1);
                    marker.Set(2, quest.marker.zplane);
                    marker.Set(3, static_cast<uint32_t>(quest.map_to));
                }
                Apply(world, marker);
            }
            Apply(world, Make(GAME_SMSG_PLAYER_ATTR_MAX_KURZICK, {world_context->max_kurzick}));
            Apply(world, Make(GAME_SMSG_PLAYER_ATTR_MAX_LUXON, {world_context->max_luxon}));
            Apply(world, Make(GAME_SMSG_PLAYER_ATTR_MAX_BALTHAZAR, {world_context->max_balth}));
            Apply(world, Make(GAME_SMSG_PLAYER_ATTR_MAX_IMPERIAL, {world_context->max_imperial}));
            Apply(world, Make(CharContextValue112, {world_context->is_hard_mode_unlocked}));
            if (world_context->accountInfo) {
                const auto& account = *world_context->accountInfo;
                auto name = Make(CharAccountNameData, {});
                if (name.fields.size() > 6) {
                    name.fields[0].text = account.account_name ? account.account_name : L"";
                    const uint32_t values[] = {account.wins, account.losses, account.rating, account.qualifier_points, account.rank, account.tournament_reward_points};
                    for (size_t i = 0; i < std::size(values); i++) {
                        name.Set(i + 1, values[i]);
                    }
                }
                Apply(world, name);
            }
            Apply(world, Make(CharToClient153, {static_cast<uint32_t>(map_id), 0}));
            constexpr uint32_t manifest_size = 898;
            for (const auto phase : {0u, 1u}) {
                Apply(world, Make(MissionManifestPhase, {phase}));
            }
            Apply(world, Make(MissionManifestComplete, {2, manifest_size, 0}));
            for (const auto phase : {0u, 1u}) {
                Apply(world, Make(MissionManifestPhase, {phase}));
            }
            Apply(world, Make(MissionManifestComplete, {0, static_cast<uint32_t>(map_id), 0}));
            // 406's handler hands its map file to the map's load, which asserts it non-zero (File.cpp:367); AreaInfo::file_id is 0 for most PvE maps.
            auto load_map = Make(GAME_SMSG_INSTANCE_LOAD_SPAWN_POINT, {chapter_map_file ? chapter_map_file : PathfindingWindow::GetMapFileId(map_id)});
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
            Apply(world, load_map);
            Apply(world, Make(GAME_SMSG_INSTANCE_LOADED, {world_context->playerTeamToken}));
            const auto& teams = mission.teams;
            for (uint32_t party = 0; teams.by_party && party < teams.by_party_count; party++) {
                const auto row = teams.by_party[party];
                if (!row) {
                    continue;
                }
                auto team = Make(MissionTeam, {row->party, row->team, row->value, row->colour, row->territory});
                if (team.fields.size() > 5) {
                    team.fields[5].text.assign(row->name, wcsnlen(row->name, std::size(row->name)));
                    Apply(world, team);
                }
            }
            // Of the loads at hand only the explorable ones in hard mode send 91.
            if (const auto value_91 = world_context->h053C[0]; explorable && value_91) {
                Apply(world, Make(CharToClient91, {value_91}));
            }
            Apply(world, Make(GAME_SMSG_POST_PROCESS, {post_process.level, post_process.tint}));
            Apply(world, Make(MissionToClient404, {0}));
        }

        void SnapshotCharacters(ReplayState::World& world, GW::WorldContext* world_context)
        {
            for (const auto& state : world_context->party_profession_states) {
                const auto living = GW::Agents::GetAgentByID(state.agent_id) ? GW::Agents::GetAgentByID(state.agent_id)->GetAsAgentLiving() : nullptr;
                Apply(world, Make(CharAttributeState, {state.agent_id, 0, 0}));
                Apply(world, Make(GAME_SMSG_PLAYER_UPDATE_PROFESSION, {state.agent_id, static_cast<uint32_t>(state.primary), static_cast<uint32_t>(state.secondary), 0}));
                if (state.unlocked_professions) {
                    Apply(world, Make(GAME_SMSG_PLAYER_UNLOCKED_PROFESSION, {state.agent_id, state.unlocked_professions}));
                }
                if (const auto skillbar = GW::SkillbarMgr::GetSkillbar(state.agent_id); skillbar && message_table[GAME_SMSG_SKILLBAR_UPDATE].size() > 3) {
                    auto skills = Make(GAME_SMSG_SKILLBAR_UPDATE, {state.agent_id});
                    for (const auto& skill : skillbar->skills) {
                        skills.fields[1].numbers.push_back(static_cast<uint32_t>(skill.skill_id));
                        skills.fields[2].numbers.push_back(0);
                    }
                    skills.Set(3, 1);
                    Apply(world, skills);
                }
                if (living) {
                    Apply(world, Make(GAME_SMSG_AGENT_ATTR_UPDATE_INT, {MaxEnergy, state.agent_id, living->max_energy}));
                    Apply(world, Make(GAME_SMSG_AGENT_ATTR_UPDATE_INT, {MaxHealth, state.agent_id, living->max_hp}));
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
                    auto words = Make(CharAttributes, {state.agent_id});
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
                    Apply(world, words);
                }
            }
            for (const auto& info : world_context->hero_info) {
                const auto& hero = reinterpret_cast<const HeroData&>(info);
                const auto flag = hero.flags & 1;
                if (info.hero_id >= GW::Constants::HeroID::Merc1 && info.hero_id <= GW::Constants::HeroID::Merc8) {
                    auto mercenary = Make(
                        GAME_SMSG_MERCENARY_INFO,
                        {static_cast<uint32_t>(info.hero_id), info.level, static_cast<uint32_t>(info.primary), static_cast<uint32_t>(info.secondary), info.hero_file_id, info.model_file_id, hero.first_value, flag, info.appearance_bitmap}
                    );
                    constexpr size_t armor_pieces = std::extent_v<decltype(HeroData::armour)>;
                    if (mercenary.fields.size() > 9 + 2 * armor_pieces) {
                        for (size_t piece = 0; piece < armor_pieces; piece++) {
                            mercenary.Set(9 + piece, hero.armour[piece]);
                            mercenary.Set(9 + armor_pieces + piece, hero.dyes[piece]);
                        }
                        // OnHeroDataAdd copies the name only with an appearance: an empty slot's is stale memory, its 116 an empty name.
                        if (info.appearance_bitmap) {
                            mercenary.fields[9 + 2 * armor_pieces].text.assign(info.name, wcsnlen(info.name, std::size(info.name)));
                        }
                    }
                    Apply(world, mercenary);
                    continue;
                }
                auto data = Make(CharHeroDataAdd, {static_cast<uint32_t>(info.hero_id), info.level, static_cast<uint32_t>(info.primary), static_cast<uint32_t>(info.secondary), info.hero_file_id, info.model_file_id});
                if (data.fields.size() > 8) {
                    data.fields[6].numbers.assign(hero.skills, hero.skills + std::min<size_t>(hero.skill_count, std::size(hero.skills)));
                    data.Set(7, hero.first_value);
                    data.Set(8, flag);
                }
                Apply(world, data);
            }
            if (const auto inventory = GW::Items::GetInventory()) {
                Apply(world, Make(GAME_SMSG_ITEM_STREAM_CREATE, {inventory->inventory_id(), 0}));
                Apply(world, Make(GAME_SMSG_ITEM_SET_ACTIVE_WEAPON_SET, {inventory->inventory_id(), inventory->active_weapon_set}));
            }
            for (const auto& flag : world_context->hero_flags) {
                Apply(world, Make(GAME_SMSG_ITEM_STREAM_CREATE, {flag.inventory_id, 1}));
                Apply(world, Make(CharHeroActivate, {flag.hero_id, flag.agent_id, flag.inventory_id, static_cast<uint32_t>(flag.hero_behavior)}));
                Apply(world, Make(GAME_SMSG_HERO_SKILL_STATUS_BITMAP, {flag.agent_id, 0}));
            }
            if (world_context->unlocked_character_skills.size()) {
                auto unlocked = ReplayState::Blank(message_table, GAME_SMSG_SKILLS_UNLOCKED);
                if (!unlocked.fields.empty()) {
                    unlocked.fields[0].numbers.assign(world_context->unlocked_character_skills.begin(), world_context->unlocked_character_skills.end());
                }
                Apply(world, unlocked);
            }
            Apply(
                world, Make(
                           GAME_SMSG_PLAYER_ATTR_SET, {world_context->experience, world_context->current_kurzick, world_context->total_earned_kurzick, world_context->current_luxon, world_context->total_earned_luxon, world_context->current_imperial,
                                                       world_context->total_earned_imperial, 0, 0, world_context->level, world_context->morale, world_context->current_balth, world_context->total_earned_balth, world_context->current_skill_points,
                                                       world_context->total_earned_skill_points}
                       )
            );
        }

        void SnapshotAgents(ReplayState::World& world, GW::WorldContext* world_context, GW::AgentArray* agents)
        {
            // The server world's clock: the positions below are its agents', which the stream's orders and 30s go on from.
            const auto clock = ServerTime();
            const auto agent_context = GW::GetAgentContext();
            Apply(world, Make(GAME_SMSG_AGENT_INSTANCE_TIMER, {clock}));
            // A mercenary's look comes as the server sends it before the agent: notices 0, 14 and 15 (159 for slot 0, 160 for the others, none for an empty one).
            const auto dress = [&](const uint32_t agent_id) {
                if (agent_id >= world_context->agent_infos.size()) {
                    return;
                }
                const auto& look = reinterpret_cast<const AgentLook&>(world_context->agent_infos[agent_id]);
                if (!look.appearance) {
                    return;
                }
                Apply(world, Make(GAME_SMSG_AGENT_ATTR_UPDATE_INT, {LookAppearance, agent_id, look.appearance}));
                Apply(world, Make(GAME_SMSG_AGENT_ATTR_UPDATE_INT, {LookComposite, agent_id, look.composites[0]}));
                Apply(world, Make(GAME_SMSG_AGENT_ATTR_UPDATE_INT, {LookColour, agent_id, look.colours[0]}));
                for (uint32_t slot = 1; slot < std::size(look.composites); slot++) {
                    if (look.composites[slot]) {
                        Apply(world, Make(GAME_SMSG_AGENT_ATTR_UPDATE_INT_TARGET, {LookComposite, agent_id, slot, look.composites[slot]}));
                        Apply(world, Make(GAME_SMSG_AGENT_ATTR_UPDATE_INT_TARGET, {LookColour, agent_id, slot, look.colours[slot]}));
                    }
                }
            };
            std::set<uint32_t> livings;
            for (const auto agent : *agents) {
                if (!agent) {
                    continue;
                }
                auto create = ReplayState::Blank(message_table, GAME_SMSG_AGENT_SPAWNED);
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
                const auto walking = agent_context && agent->agent_id < agent_context->agent_movement.size() ? reinterpret_cast<const WalkingAgent*>(agent_context->agent_movement[agent->agent_id]) : nullptr;
                const auto motion = walking ? MotionOf(*walking, clock) : ReplayFile::Motion{.x = agent->pos.x, .y = agent->pos.y, .plane = agent->pos.zplane, .max_speed = living ? default_speed : 0.f, .move_speed = 1.f};
                create.SetF(4, motion.x, 0);
                create.SetF(4, motion.y, 1);
                create.Set(5, motion.plane);
                create.SetF(6, agent->rotation_cos, 0);
                create.SetF(6, agent->rotation_sin, 1);
                create.Set(7, 1);
                create.SetF(8, motion.max_speed);
                create.SetF(9, motion.move_speed);
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
                        auto update = Make(GAME_SMSG_AGENT_UPDATE_NPC_NAME, {agent->agent_id});
                        if (update.fields.size() > 1) {
                            update.fields[1].text = name;
                        }
                        Apply(world, update);
                    }
                }
                if (living) {
                    livings.insert(agent->agent_id);
                    dress(agent->agent_id);
                    if (static_cast<uint32_t>(living->primary) || static_cast<uint32_t>(living->secondary)) {
                        Apply(world, Make(GAME_SMSG_AGENT_UPDATE_PROFESSION, {agent->agent_id, static_cast<uint32_t>(living->primary), static_cast<uint32_t>(living->secondary)}));
                    }
                    Apply(world, Make(GAME_SMSG_AGENT_ATTR_UPDATE_INT, {Level, agent->agent_id, living->level}));
                    Apply(world, Make(GAME_SMSG_AGENT_INITIAL_EFFECTS, {agent->agent_id, living->effects}));
                }
                if (item) {
                    ApplyAll(world, ItemDetails(item->item_id));
                    if (item->owner) {
                        // All 13 309s at hand carry 600.0.
                        constexpr float assignment_seconds = 600.f;
                        Apply(world, Make(GAME_SMSG_ITEM_UPDATE_OWNER, {item->item_id, item->owner, std::bit_cast<uint32_t>(assignment_seconds)}));
                    }
                }
                Apply(world, create);
                if (!living) {
                    continue;
                }
                // A walk in progress goes on from the keyframe by its order, as 41 and 42 start one (CAgent::UpdateMovement).
                if (motion.walking) {
                    auto order = ReplayState::Blank(message_table, motion.target_agent ? GAME_SMSG_AGENT_UPDATE_DESTINATION : GAME_SMSG_AGENT_MOVE_TO_POINT);
                    if (order.fields.size() > 3) {
                        order.Set(0, agent->agent_id);
                        order.SetF(1, motion.target_x, 0);
                        order.SetF(1, motion.target_y, 1);
                        order.Set(2, motion.target_plane);
                        order.Set(3, motion.plane);
                        order.Set(4, motion.target_agent);
                        Apply(world, order);
                    }
                }
                // The agent view keeps 53's interval and modifier at +0xEC and +0xF0, and asserts the interval before an attack's warmup.
                if (living->weapon_attack_speed != 0.f && living->attack_speed_modifier != 0.f) {
                    Apply(world, Make(CharAttackInterval, {agent->agent_id, std::bit_cast<uint32_t>(living->weapon_attack_speed), std::bit_cast<uint32_t>(living->attack_speed_modifier)}));
                }
                Apply(world, Make(GAME_SMSG_AGENT_ATTR_UPDATE_FLOAT, {Health, agent->agent_id, std::bit_cast<uint32_t>(living->hp)}));
                if (living->hp_pips) {
                    Apply(world, Make(GAME_SMSG_AGENT_ATTR_UPDATE_FLOAT, {HealthRegen, agent->agent_id, std::bit_cast<uint32_t>(living->hp_pips)}));
                }
                if (IsCharacter(world_context, agent->agent_id)) {
                    Apply(world, Make(GAME_SMSG_AGENT_ATTR_UPDATE_FLOAT, {Energy, agent->agent_id, std::bit_cast<uint32_t>(living->energy)}));
                    Apply(world, Make(GAME_SMSG_AGENT_ATTR_UPDATE_FLOAT, {EnergyRegen, agent->agent_id, std::bit_cast<uint32_t>(living->energy_regen)}));
                }
                uint32_t definition = 0;
                const CharacterData* character = nullptr;
                constexpr uint32_t character_definition = 1;
                if (!AgentNotifyParams_Func || !AgentNotifyParams_Func(agent->agent_id, &definition, &character) || definition != character_definition || !character) {
                    continue;
                }
                const auto& worn = character->worn;
                if (living->IsPlayer()) {
                    auto equipment = Make(CharEquipment, {agent->agent_id});
                    for (size_t slot = 0; slot + 1 < equipment.fields.size(); slot++) {
                        ApplyAll(world, ItemDetails(worn[slot]));
                        equipment.Set(slot + 1, worn[slot]);
                    }
                    Apply(world, equipment);
                }
                else {
                    ApplyAll(world, ItemDetails(worn[0]));
                    ApplyAll(world, ItemDetails(worn[1]));
                    Apply(world, Make(GAME_SMSG_NPC_UPDATE_WEAPONS, {agent->agent_id, worn[0], worn[1]}));
                }
            }
            // The outpost's load sends the party's mercenary heroes' looks, which have no agent there.
            for (uint32_t agent_id = 0; agent_id < world_context->agent_infos.size(); agent_id++) {
                if (!livings.contains(agent_id)) {
                    dress(agent_id);
                }
            }
        }

        void SnapshotNpcsAndGadgets(ReplayState::World& world, GW::WorldContext* world_context)
        {
            for (uint32_t npc_id = 0; npc_id < world_context->npcs.size(); npc_id++) {
                const auto& npc = world_context->npcs[npc_id];
                if (!npc.model_file_id) {
                    continue;
                }
                auto properties = Make(GAME_SMSG_NPC_UPDATE_PROPERTIES, {npc_id, npc.model_file_id, npc.skin_file_id, std::bit_cast<uint32_t>(npc.visual_adjustment), npc.appearance, npc.npc_flags, static_cast<uint32_t>(npc.primary), npc.default_level});
                if (properties.fields.size() > 8) {
                    properties.fields[8].text = npc.name_enc ? npc.name_enc : L"";
                }
                Apply(world, properties);
                auto model = Make(GAME_SMSG_NPC_UPDATE_MODEL, {npc_id});
                if (model.fields.size() > 1 && npc.model_files) {
                    model.fields[1].numbers.assign(npc.model_files, npc.model_files + npc.files_count);
                }
                Apply(world, model);
            }
            // The table keeps every class defined since the game started: a gadget the stream makes later finds its class in the keyframe.
            if (const auto game_context = GW::GetGameContext(); game_context && game_context->gadget) {
                const auto& classes = game_context->gadget->GadgetInfo;
                for (uint32_t index = 0; index < classes.size(); index++) {
                    const auto& gadget_class = reinterpret_cast<const GadgetClass&>(classes[index]);
                    if (!gadget_class.model_file) {
                        continue;
                    }
                    auto define = Make(GadgetClassDefine, {index, gadget_class.model_file, gadget_class.flags, gadget_class.scale & 0xFF});
                    if (define.fields.size() > 4) {
                        define.fields[4].text = gadget_class.name_enc ? gadget_class.name_enc : L"";
                    }
                    Apply(world, define);
                }
            }
        }

        void SnapshotPlayersAndTitles(ReplayState::World& world, GW::WorldContext* world_context)
        {
            for (const auto& player : world_context->players) {
                if (!player.agent_id) {
                    continue;
                }
                // Every load at hand fills 89's sixth field with 0xCCCCCCCC, the server's unset bytes.
                constexpr uint32_t unset_bytes = 0xCCCCCCCC;
                auto row = Make(GAME_SMSG_AGENT_CREATE_PLAYER, {player.player_number, player.agent_id, player.appearance_bitmap, static_cast<uint32_t>(player.secondary), 0, unset_bytes});
                if (row.fields.size() > 6) {
                    row.fields[6].text = player.name ? player.name : L"";
                }
                Apply(world, row);
                Apply(world, Make(GAME_SMSG_UPDATE_AGENT_PARTYSIZE, {player.player_number, player.party_size}));
                Apply(world, Make(CharPlayerLink, {player.player_number, player.party_leader_player_number}));
                Apply(world, Make(GAME_SMSG_TITLE_RANK_DISPLAY, {player.player_number, player.active_title_tier}));
            }
            constexpr size_t morale_rows_at_most = 64;
            constexpr uint32_t lowest_morale = 40;
            constexpr uint32_t highest_morale = 110;
            size_t morale_rows = 0;
            for (const auto& morale : world_context->morale_table) {
                if (++morale_rows > morale_rows_at_most) {
                    break;
                }
                if (IsCharacter(world_context, morale.agent_id) && morale.morale >= lowest_morale && morale.morale <= highest_morale) {
                    Apply(world, Make(CharAgentMorale, {morale.agent_id, morale.morale}));
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
                auto track = Make(
                    GAME_SMSG_TITLE_TRACK_INFO,
                    {index, title.props, title.current_points, title.current_title_tier_index, title.points_needed_current_rank, title.h0010, title.next_title_tier_index, title.points_needed_next_rank, title.max_title_rank, title.max_title_tier_index}
                );
                if (track.fields.size() > 11) {
                    track.fields[10].text = title.points_desc ? title.points_desc : L"";
                    track.fields[11].text = title.h0028 ? title.h0028 : L"";
                }
                Apply(world, track);
            }
            for (const auto index : title_tiers) {
                if (index >= world_context->title_tiers.size()) {
                    continue;
                }
                const auto& tier = world_context->title_tiers[index];
                auto rank = Make(GAME_SMSG_TITLE_RANK_DATA, {index, tier.props, tier.tier_number});
                if (rank.fields.size() > 3 && tier.tier_name_enc) {
                    rank.fields[3].text = tier.tier_name_enc;
                }
                Apply(world, rank);
            }
        }

        void SnapshotGuilds(ReplayState::World& world)
        {
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
                    auto info = ReplayState::Blank(message_table, own ? GAME_SMSG_GUILD_GENERAL_INFO : GAME_SMSG_GUILD_ALLIANCE_INFO);
                    if (info.fields.size() < (own ? 19u : 15u)) {
                        continue;
                    }
                    // The last field of both is 0 in all 318 at hand.
                    const auto& cape = guild->cape;
                    std::vector<uint32_t> values = {guild->features};
                    if (own) {
                        values.push_back(reinterpret_cast<const GuildEntry*>(guild)->general_value);
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
                    Apply(world, info);
                }
                if (guild_context->player_guild_index) {
                    auto assign = Make(GAME_SMSG_GUILD_PLAYER_ROLE, {guild_context->player_guild_index});
                    if (assign.fields.size() > 2) {
                        set_key(assign, guild_context->player_gh_key);
                        assign.Set(2, guild_context->player_guild_rank);
                        Apply(world, assign);
                    }
                }
            }
        }

        void SnapshotEffects(ReplayState::World& world)
        {
            if (const auto party_effects = GW::Effects::GetPartyEffectsArray()) {
                for (const auto& effects : *party_effects) {
                    for (const auto& effect : effects.effects) {
                        // An effect with no duration came as an upkeep (65): no 66 in any recording at hand has a duration of 0.
                        if (effect.duration) {
                            Apply(world, Make(GAME_SMSG_EFFECT_APPLIED, {effects.agent_id, static_cast<uint32_t>(effect.skill_id), effect.attribute_level, effect.effect_id, std::bit_cast<uint32_t>(effect.GetTimeRemaining() / 1000.f)}));
                        }
                        else if (!effect.agent_id) {
                            Apply(world, Make(GAME_SMSG_EFFECT_UPKEEP_APPLIED, {effects.agent_id, 0, static_cast<uint32_t>(effect.skill_id), effect.attribute_level, effect.effect_id}));
                        }
                    }
                    for (const auto& buff : effects.buffs) {
                        // GWCA lists an upkeep under its caster; 63 and 65 name its target first.
                        const auto target = buff.target_agent_id ? buff.target_agent_id : effects.agent_id;
                        for (const auto id : std::initializer_list<uint16_t>{GAME_SMSG_EFFECT_UPKEEP_ADDED, GAME_SMSG_EFFECT_UPKEEP_APPLIED}) {
                            Apply(world, Make(id, {target, effects.agent_id, static_cast<uint32_t>(buff.skill_id), buff.h0004, buff.buff_id}));
                        }
                    }
                }
            }
        }

        void SnapshotParty(ReplayState::World& world, GW::CharContext* char_context, GW::AgentLiving* controlled, const bool explorable)
        {
            Apply(world, Make(GAME_SMSG_AGENT_SET_PLAYER, {controlled->agent_id, 3}));
            if (const auto party = GW::PartyMgr::GetPartyInfo()) {
                Apply(world, Make(GAME_SMSG_PARTY_CREATE, {party->party_id}));
                for (const auto& member : party->players) {
                    Apply(world, Make(GAME_SMSG_PARTY_PLAYER_ADD, {party->party_id, member.login_number, 1}));
                }
                for (const auto& hero : party->heroes) {
                    Apply(world, Make(GAME_SMSG_PARTY_HERO_ADD, {party->party_id, hero.owner_player_id, hero.agent_id, static_cast<uint32_t>(hero.hero_id), hero.level}));
                }
                Apply(world, Make(GAME_SMSG_PARTY_MEMBER_STREAM_END, {party->party_id}));
                Apply(world, Make(PartyAssign, {party->party_id, party->players.size() ? party->players[0].login_number : 0}));
                Apply(world, Make(GAME_SMSG_PARTY_SET_DIFFICULTY, {GW::PartyMgr::GetIsPartyInHardMode()}));
            }
            constexpr uint32_t access_rights_mask = 0xD;
            Apply(world, Make(MissionAccessRights, {char_context->player_flags & access_rights_mask}));
            Apply(world, Make(PartyToClient446, {0}));
            if (explorable) {
                Apply(world, ReplayState::Blank(message_table, GAME_SMSG_MISSION_STREAM_START));
            }
            Apply(world, ReplayState::Blank(message_table, GAME_SMSG_INSTANCE_LOAD_FINISH));
        }

    } // namespace

    uint32_t ServerTime()
    {
        const auto context = GW::GetAgentContext();
        return context ? reinterpret_cast<const AgentClocks&>(*context).server_time : GW::Map::GetInstanceTime();
    }

    ReplayFile::Motion MotionOf(const WalkingAgent& walking, const uint32_t clock)
    {
        constexpr uint32_t facing_mask = 0xF;
        ReplayFile::Motion motion = {
            .agent = walking.agent_id,
            .max_speed = walking.max_speed,
            .move_speed = walking.move_speed,
            .facing = walking.facing & facing_mask,
            .walking = walking.stop_time != 0,
        };
        // CAgent's position at a time: its point moved by its velocity since the point's time, its leg's end once the leg's stop time is past.
        if (motion.walking && static_cast<int32_t>(clock - walking.stop_time) >= 0) {
            motion.x = walking.leg.x;
            motion.y = walking.leg.y;
            motion.plane = walking.leg.plane;
        }
        else {
            const auto seconds = static_cast<float>(static_cast<int32_t>(clock - walking.point_time)) / 1000.f;
            motion.x = walking.point.x + walking.velocity_x * seconds;
            motion.y = walking.point.y + walking.velocity_y * seconds;
            motion.plane = walking.point.plane;
        }
        if (motion.walking) {
            motion.leg_x = walking.leg.x;
            motion.leg_y = walking.leg.y;
            motion.leg_plane = walking.leg.plane;
            motion.target_x = walking.target.x;
            motion.target_y = walking.target.y;
            motion.target_plane = walking.target.plane;
            motion.target_agent = walking.target_agent;
        }
        return motion;
    }

    ReplayFile::Clip ClipOf(const GW::AgentLiving& agent)
    {
        return {
            .agent = agent.agent_id,
            .animation = reinterpret_cast<const AnimatedAgent&>(agent).animation,
            .sequence = agent.animation_code,
            .sequence_index = agent.animation_id,
            .length = agent.animation_speed,
            .scale = agent.animation_type,
            .model_state = agent.model_state,
            .rotation = agent.rotation_angle,
        };
    }

    std::vector<ReplayFile::Body> Bodies(const std::map<uint32_t, uint32_t>& clip_starts)
    {
        std::vector<ReplayFile::Body> bodies;
        const auto agents = GW::Agents::GetAgentArray();
        if (!agents) {
            return bodies;
        }
        for (const auto agent : *agents) {
            const auto living = agent ? agent->GetAsAgentLiving() : nullptr;
            if (!living) {
                continue;
            }
            const auto started = clip_starts.find(living->agent_id);
            auto& body = bodies.emplace_back(ClipOf(*living), started == clip_starts.end() ? ReplayFile::unknown_time : started->second, living->z);
            for (const auto& effect : living->visible_effects) {
                body.effects.push_back({effect.unk, static_cast<uint32_t>(effect.id), effect.has_ended});
            }
        }
        return bodies;
    }

    void FindConstTables()
    {
        if (const_tables_sought) {
            return;
        }
        const_tables_sought = true;
        effect_table = FindConstTable("ConstEffect.cpp", "index < arrsize(s_effect)");
        aura_table = FindConstTable("ConstAura.cpp", "index < arrsize(s_aura)");
        text_file_table = FindConstTable("ConstText.cpp", "index < arrsize(s_fileId)");
        // The text decoder's read of a packed string's small values (TextDecode.cpp): the 32 characters they stand for.
        if (const auto characters = GW::Scanner::Find("\x83\xFA\x20\x73\x00\x0F\xB7\x0C\x55\x00\x00\x00\x00\xEB", "xxxx?xxxx????x", 9)) {
            packed_characters = *reinterpret_cast<const wchar_t**>(characters);
        }
        for (const auto& [found, what] : {std::pair{effect_table.rows != 0, "effects' rows"}, {aura_table.rows != 0, "auras' rows"}, {text_file_table.rows != 0, "string files"}, {packed_characters != nullptr, "packed text's characters"}}) {
            if (!found) {
                LogLine(Log::Log, std::format("the client's table is not found in this client: recordings leave out the {}", what));
            }
        }
    }

    std::optional<ReplayFile::SkillRow> SkillRowOf(const uint32_t skill_id)
    {
        const auto skill = skill_id ? GW::SkillbarMgr::GetSkillConstantData(static_cast<GW::Constants::SkillID>(skill_id)) : nullptr;
        if (!skill) {
            return std::nullopt;
        }
        return ReplayFile::SkillRow{
            .skill = skill_id,
            .type = static_cast<uint32_t>(skill->type),
            .special = skill->special,
            .profession = static_cast<uint32_t>(skill->profession),
            .attribute = static_cast<uint32_t>(skill->attribute),
            .name = skill->name,
            .description = skill->description,
            .icon_files = {skill->icon_file_id, skill->icon_file_id_2, skill->icon_file_id_hi_res},
            .visuals = {skill->caster_overhead_animation_id, skill->caster_body_animation_id, skill->target_body_animation_id, skill->target_overhead_animation_id, skill->projectile_animation_1_id, skill->projectile_animation_2_id},
            .activation = skill->activation,
            .aftercast = skill->aftercast,
            .recharge = skill->recharge,
        };
    }

    std::optional<ReplayFile::EffectRow> EffectRowOf(const uint32_t effect_id)
    {
        if (effect_id >= effect_table.rows) {
            return std::nullopt;
        }
        const auto& row = *static_cast<const EffectTableRow*>(effect_table.row(effect_id));
        return ReplayFile::EffectRow{effect_id, row.model_file, row.territory_model_file, row.h000C};
    }

    std::optional<ReplayFile::AuraRow> AuraRowOf(const uint32_t aura_id)
    {
        if (aura_id >= aura_table.rows) {
            return std::nullopt;
        }
        const auto& row = *static_cast<const AuraTableRow*>(aura_table.row(aura_id));
        return ReplayFile::AuraRow{aura_id, row.model_file, row.h0008};
    }

    ReplayFile::TextFiles TextFilesOf()
    {
        constexpr size_t packed_character_count = 32;
        ReplayFile::TextFiles texts = {.language = static_cast<uint32_t>(GW::UI::GetTextLanguage())};
        if (packed_characters) {
            texts.characters.assign(packed_characters, packed_character_count);
        }
        for (uint32_t language = 0; language < text_file_table.rows; language++) {
            ReplayFile::LanguageFiles files = {language, {}};
            for (const auto name : static_cast<const LanguageFileNames*>(text_file_table.row(language))->names) {
                // A file's name is its id in two characters (FileConvertPathToId): id = w0 - 0xff00ff + w1 * 0xff00.
                files.files.push_back(name && name[0] && name[1] ? static_cast<uint32_t>(name[0]) + static_cast<uint32_t>(name[1]) * 0xff00 - 0xff00ff : 0);
            }
            if (std::ranges::any_of(files.files, std::identity())) {
                texts.languages.push_back(std::move(files));
            }
        }
        return texts;
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

    ReplayState::World Snapshot()
    {
        ReplayState::World world(message_table);
        const auto char_context = GW::GetCharContext();
        const auto world_context = GW::GetWorldContext();
        const auto controlled = GW::Agents::GetControlledCharacter();
        const auto agents = GW::Agents::GetAgentArray();
        if (!char_context || !world_context || !controlled || !agents) {
            return world;
        }
        const auto explorable = GW::Map::GetInstanceType() == GW::Constants::InstanceType::Explorable;
        SnapshotLoad(world, char_context, world_context, controlled, explorable);
        SnapshotCharacters(world, world_context);
        SnapshotAgents(world, world_context, agents);
        SnapshotNpcsAndGadgets(world, world_context);
        SnapshotPlayersAndTitles(world, world_context);
        SnapshotGuilds(world);
        SnapshotEffects(world);
        SnapshotParty(world, char_context, controlled, explorable);
        return world;
    }
} // namespace ReplayRecorder
