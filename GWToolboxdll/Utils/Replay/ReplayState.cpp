#include <Utils/Replay/ReplayMessages.h>
#include <Utils/Replay/ReplayState.h>

#include <algorithm>
#include <bit>
#include <cmath>
#include <format>
#include <initializer_list>
#include <iterator>
#include <limits>
#include <optional>
#include <ranges>
#include <span>

namespace ReplayState {
    namespace {
        using namespace ReplayMessages;
        using ReplayFile::StoCFieldType;


        enum CreateField : size_t {
            CreateAgent = 0,
            CreateClass = 1,
            CreateDef = 2,
            CreatePosition = 4,
            CreatePlane = 5,
            CreateSpeed = 8,
            CreateMoveSpeed = 9,
            CreateTeam = 11,
            CreateStopTime = 13,
            CreatePointTime = 15,
            CreateVelocity = 17,
            CreateTarget = 18,
            CreateTargetPlane = 19,
            CreateTargetAgent = 20,
            CreateSegment = 21,
            CreateSegmentPlane = 22,
        };

        enum MoveField : size_t {
            MovePoint = 1,
            MovePlane = 2,
            MoveCurrentPlane = 3,
            MoveTargetAgent = 4,
        };
        constexpr uint32_t living_def = 1;



        constexpr uint16_t bag_content_ids[] = {
            TradeRequest,
            TradeTerminate,
            TradeChangeOffer,
            TradeReceiveOffer,
            TradeAddItem,
            TradeAcknowledge,
            TradeAccept,
            TradeToClient7,
            TradeOfferedCount,
            TradeToClient9,
            AccountCurrency,
            CharWindowOpen,
            CharWindowAddItems,
            CharWindowItemsEnd,
            CharWindowItemStreamEnd,
            CharWindowMerchant,
            CharWindowOwner,
            CharWindowCollect,
            CharWindowPriced,
            CharWindowSkills,
            CharTransactionDone,
            CharWindowAddList,
            ItemInventoryAdd,
            ItemInventoryAddBag,
            ItemInventoryAddGold,
            ItemInventoryAddGoldAccount,
            ItemInventoryClean,
            ItemInventoryCompact,
            ItemInventoryEquipBag,
            ItemInventoryEquipSet,
            ItemInventoryGrowBag,
            ItemInventoryMove,
            ItemInventoryMoveBag,
            ItemInventoryRemove,
            ItemInventoryRemoveBag,
            ItemInventoryRemoveGold,
            ItemInventoryRemoveGoldAccount,
            ItemInventorySetBundleItem,
            ItemInventorySwap,
            ItemInventoryUnequipBag,
            ItemMarketData,
            ItemMarketDataRemove
        };
        constexpr uint16_t transient_ids[] = {
            NetPing,
            NetPingReply,
            AgentSyncCheck,
            CharAgentPinged,
            CharToClient61,
            CharToClient69,
            CharScreenShake,
            CharToClient71,
            CharChatMessageCore,
            CharChatMessageServer,
            CharChatMessageGlobal,
            CharChatMessageLocal,
            CharAgentAttrUpdateIntTarget,
            CharAgentAttrPlayEffect,
            CharAgentProjectileLaunched,
            CharSpeechBubble,
            CharSkillInterupted,
            CharSkillActivated,
            CharSkillActivate,
            ItemPickup
        };
        constexpr uint32_t class_kind_mask = 0xF0000000;
        constexpr uint32_t npc_class = 0x20000000;
        constexpr uint32_t gadget_def = 2;
        constexpr uint32_t item_def = 4;
        constexpr size_t observer_field = 5;
        constexpr size_t equipment_slots = 9;
        constexpr uint32_t look_slots = 5;
        constexpr uint16_t quest_ids[] = {CharQuestAdd, CharQuestGeneralInfo, CharQuestUpdateName, CharQuestDescription, CharQuestAddMarker, CharQuestUpdateMarker, CharQuestEarned};
        constexpr uint16_t item_ids[] = {ItemReportDetailAll, ItemReportDetailAllReset, ItemReportDetailLow, ItemProfession, ItemCustomize, ItemAssign};
        constexpr size_t skillbar_create_field = 3;
        constexpr size_t item_model_file_field = 1;
        constexpr size_t item_quantity_field = 10;
        constexpr size_t item_name_field = 11;
        constexpr size_t item_mods_field = 12;
        constexpr uint32_t item_code_terminator = 0xC0000000;
        constexpr uint32_t no_formula = 0x5E5;
        constexpr uint32_t any_profession = 11;
        constexpr size_t title_points_field = 2;
        constexpr IdField named_id_fields[] = {
            {CharPlayerRowFlags, 0, IdKind::Player},
            {CharQuestAdd, 0, IdKind::Quest},
            {CharQuestDescription, 0, IdKind::Quest},
            {CharQuestEarned, 0, IdKind::Quest},
            {CharQuestGeneralInfo, 0, IdKind::Quest},
            {CharQuestUpdateMarker, 0, IdKind::Quest},
            {CharQuestRemove, 0, IdKind::Quest},
            {CharQuestAddMarker, 0, IdKind::Quest},
            {CharQuestUpdateName, 0, IdKind::Quest},
            {CharEffectApplied, 1, IdKind::Skill},
            {CharNpcProperties, 0, IdKind::Npc},
            {CharNpcModel, 0, IdKind::Npc},
            {CharPlayerCreate, 0, IdKind::Player},
            {CharAgentDestroyPlayer, 0, IdKind::Player},
            {CharNpcUpdateWeapons, 1, IdKind::Item},
            {CharNpcUpdateWeapons, 2, IdKind::Item},
            {CharEquipment, 1, IdKind::Item},
            {CharEquipment, 2, IdKind::Item},
            {CharEquipment, 3, IdKind::Item},
            {CharEquipment, 4, IdKind::Item},
            {CharEquipment, 5, IdKind::Item},
            {CharEquipment, 6, IdKind::Item},
            {CharEquipment, 7, IdKind::Item},
            {CharEquipment, 8, IdKind::Item},
            {CharEquipment, 9, IdKind::Item},
            {CharEquipUpdate, 2, IdKind::Item},
            {CharHeroActivate, 0, IdKind::Hero},
            {CharHeroActivate, 2, IdKind::Inventory},
            {CharHeroDataAdd, 0, IdKind::Hero},
            {CharMercenaryInfo, 0, IdKind::Hero},
            {CharUpdateAgentModel, 1, IdKind::Npc},
            {CharUpdateAgentPartysize, 0, IdKind::Player},
            {CharPlayerLink, 0, IdKind::Player},
            {CharPlayerLink, 1, IdKind::Player},
            {CharSkillAdrenalineSet, 1, IdKind::Skill},
            {CharSkillAdrenalineSpend, 1, IdKind::Skill},
            {CharSkillbarUpdateSkill, 2, IdKind::Skill},
            {CharSkillUpdateSkillAdded, 0, IdKind::Skill},
            {CharSkillInterupted, 1, IdKind::Skill},
            {CharSkillActivated, 1, IdKind::Skill},
            {CharSkillActivate, 1, IdKind::Skill},
            {CharSkillRecharge, 1, IdKind::Skill},
            {CharSkillRecharged, 1, IdKind::Skill},
            {PropAnimate, 0, IdKind::Prop},
            {PropColor, 0, IdKind::Prop},
            {PropTeam, 0, IdKind::Prop},
            {PropLayers, 0, IdKind::Prop},
            {GuildAssign, 0, IdKind::Guild},
            {GuildAllianceInfo, 0, IdKind::Guild},
            {GuildGeneralInfo, 0, IdKind::Guild},
            {ItemAssign, 0, IdKind::Item},
            {ItemCount, 0, IdKind::Item},
            {ItemCustomize, 0, IdKind::Item},
            {ItemInventoryAdd, 0, IdKind::Inventory},
            {ItemInventoryAdd, 1, IdKind::Item},
            {ItemInventoryAdd, 2, IdKind::Bag},
            {ItemInventoryAddBag, 0, IdKind::Inventory},
            {ItemInventoryAddBag, 3, IdKind::Bag},
            {ItemInventoryAddGold, 0, IdKind::Inventory},
            {ItemInventoryCreate, 0, IdKind::Inventory},
            {ItemInventoryEquipSet, 0, IdKind::Inventory},
            {ItemInventoryEquipSet, 2, IdKind::Item},
            {ItemInventoryEquipSet, 3, IdKind::Item},
            {ItemInventoryEquipSetActivate, 0, IdKind::Inventory},
            {ItemInventoryMove, 0, IdKind::Inventory},
            {ItemInventoryMove, 1, IdKind::Item},
            {ItemInventoryMove, 2, IdKind::Bag},
            {ItemInventoryRemove, 0, IdKind::Inventory},
            {ItemInventoryRemove, 1, IdKind::Item},
            {ItemInventoryRemoveGold, 0, IdKind::Inventory},
            {ItemInventorySwap, 0, IdKind::Inventory},
            {ItemInventorySwap, 1, IdKind::Item},
            {ItemInventorySwap, 2, IdKind::Item},
            {ItemPickup, 0, IdKind::Item},
            {ItemProfession, 0, IdKind::Item},
            {ItemReportDetailLow, 0, IdKind::Item},
            {ItemReportDetailAll, 0, IdKind::Item},
            {ItemReportDetailAllReset, 0, IdKind::Item},
            {MissionInstanceInfo, 0, IdKind::Player},
            {PartyAssign, 0, IdKind::Party},
            {PartyMemberAdd, 0, IdKind::Party},
            {PartyMemberAdd, 1, IdKind::Player},
            {PartyMemberRemove, 0, IdKind::Party},
            {PartyMemberRemove, 1, IdKind::Player},
            {PartyMembershipBegin, 0, IdKind::Party},
            {PartyMembershipEnd, 0, IdKind::Party},
        };
        constexpr IdField id_bringers[] = {
            {AgentCreate, 0, IdKind::Agent},
            {CharAttributeState, 0, IdKind::Agent},
            {CharPlayerCreate, 1, IdKind::Agent},
            {CharAgentUpdateScale, 0, IdKind::Agent},
            {CharAgentUpdateNpcName, 0, IdKind::Agent},
            {CharNotifyDword, 1, IdKind::Agent},
            {CharAgentAttrUpdateIntTarget, 2, IdKind::Agent},
            {CharProjectileLanded, 0, IdKind::Agent},
            {CharAgentInitialEffects, 0, IdKind::Agent},
            {CharAgentUpdateEffects, 0, IdKind::Agent},
            {ItemReportDetailLow, 0, IdKind::Item},
            {ItemReportDetailAll, 0, IdKind::Item},
            {ItemReportDetailAllReset, 0, IdKind::Item},
            {ItemInventoryAddBag, 3, IdKind::Bag},
            {ItemInventoryCreate, 0, IdKind::Inventory},
            {GuildAllianceInfo, 0, IdKind::Guild},
            {GuildGeneralInfo, 0, IdKind::Guild},
            {PartyMembershipBegin, 0, IdKind::Party},
            {CharPlayerCreate, 0, IdKind::Player},
            {CharUpdateAgentPartysize, 0, IdKind::Player},
            {CharPlayerLink, 0, IdKind::Player},
            {MissionInstanceInfo, 0, IdKind::Player},
            {CharQuestAdd, 0, IdKind::Quest},
            {CharQuestGeneralInfo, 0, IdKind::Quest},
            {CharMercenaryInfo, 0, IdKind::Hero},
            {CharHeroDataAdd, 0, IdKind::Hero},
            {CharNpcProperties, 0, IdKind::Npc},
            {PropLayers, 0, IdKind::Prop},
            {CharEffectApplied, 1, IdKind::Skill},
            {CharSkillInterupted, 1, IdKind::Skill},
            {CharSkillActivate, 1, IdKind::Skill},
        };
        constexpr uint16_t row_ids[] = {CharAgentUpdateScale,    CharAgentUpdateNpcName, CharNotifyDword, CharAgentAttrUpdateIntTarget, CharAgentUpdateProfession, CharAgentCreateNpc,
                                        CharAgentInitialEffects, CharAgentUpdateEffects, CharAgentMorale, CharAgentAttrUpdateFloat};
        constexpr uint16_t character_rows[] = {CharAgentUpdateScale, CharAgentUpdateNpcName, CharNotifyDword, CharAgentAttrUpdateIntTarget, CharAgentUpdateProfession, CharAgentUpdateEffects, CharAgentMorale, CharAgentAttrUpdateFloat};

        template <typename List>
        bool Contains(const List& list, const uint32_t value)
        {
            return std::ranges::find(list, value) != std::ranges::end(list);
        }

        bool IsAgentClient(const uint16_t id)
        {
            return id >= AgentAdvance && id <= AgentUpdateTeamToken;
        }

        bool IsStateNotice(const uint32_t kind)
        {
            return Contains(std::initializer_list<uint32_t>{LookAppearance, LookComposite, LookColour, Marker, ModelsStopped, Guild, Level, MaxEnergy, MaxHealth, Unlurk}, kind);
        }

        // 160's kinds 14 and 15 dress armour slot f2 of an agent (CharCliOnNotifyDword's composite and colour overrides): that f2 is no agent.
        bool IsLookNotice(const Message& message)
        {
            return message.id == CharAgentAttrUpdateIntTarget && (message.U(0) == LookComposite || message.U(0) == LookColour);
        }

        bool IsRow(const Message& message)
        {
            return Contains(row_ids, message.id) && (message.id != CharNotifyDword || IsStateNotice(message.U(0))) && (message.id != CharAgentAttrUpdateIntTarget || IsLookNotice(message));
        }

        bool IsUpkeep(const uint32_t id)
        {
            return id == CharEffectUpkeepAdded || id == CharEffectUpkeepApplied;
        }

        int AgentField(const uint16_t id)
        {
            if (IsAgentClient(id)) {
                return id == AgentAdvance || id == AgentConnect || id == AgentReporter ? -1 : 0;
            }
            switch (id) {
                case CharNotifyDword:
                case CharAgentAttrUpdateIntTarget:
                case CharAgentAttrUpdateFloat:
                case CharAgentAttrUpdateFloatTarget:
                    return 1;
                case CharAttackInterval:
                case CharAgentAllyDestroy:
                case CharEffectUpkeepAdded:
                case CharEffectUpkeepRemoved:
                case CharEffectUpkeepApplied:
                case CharEffectApplied:
                case CharEffectRemoved:
                case CharAgentDisplayCape:
                case CharNpcUpdateWeapons:
                case CharEquipment:
                case CharEquipUpdate:
                case CharAgentUpdateScale:
                case CharAgentUpdateNpcName:
                case CharAgentMorale:
                case CharAgentUpdateProfession:
                case CharAgentCreateNpc:
                case CharUpdateAgentModel:
                case CharAgentInitialEffects:
                case CharAgentUpdateEffects:
                case GadgetState:
                    return 0;
                default:
                    return -1;
            }
        }

        std::vector<uint32_t> InstanceKey(const Message& message)
        {
            switch (message.id) {
                case CharPlayerTeam:
                case CharPostProcess:
                case CharContextValue112:
                case CharAccountNameData:
                case CharToClient91:
                case CharToClient124:
                case CharToClient125:
                case CharToClient153:
                case CharPrefs:
                case CharUnlockedSkills:
                case CharSkillUpdateSkillCount2:
                case CharPlayerAttrSet:
                case CharPlayerAttrMaxKurzick:
                case CharPlayerAttrMaxLuxon:
                case CharPlayerAttrMaxBalthazar:
                case CharPlayerAttrMaxImperial:
                case CharDisplay:
                case GuildToClient284:
                case GuildToClient302:
                case ItemToClient360:
                case MissionAccessRights:
                case MissionToClient404:
                case PartyAssign:
                case PartyToClient446:
                case PartySetDifficulty:
                case PartyLock:
                    return {message.id};
                case MissionTeam:
                case GuildAssign:
                case GuildAllianceInfo:
                case GuildGeneralInfo:
                case CharNpcProperties:
                case CharNpcModel:
                case GadgetClassDefine:
                case PropAnimate:
                case PropLayers:
                case CharUpdateAgentPartysize:
                case CharPlayerLink:
                case CharPointTable141:
                case CharQuestAdd:
                case CharQuestDescription:
                case CharQuestEarned:
                case CharQuestGeneralInfo:
                case CharQuestUpdateMarker:
                case CharQuestAddMarker:
                case CharQuestUpdateName:
                case PartyMembershipBegin:
                case PartyMembershipEnd:
                case ItemInventoryCreate:
                case ItemInventoryEquipSetActivate:
                case CharHeroSkillStatusBitmap:
                case CharHeroActivate:
                case CharHeroDataAdd:
                case CharMercenaryInfo:
                case CharTitleRankData:
                case CharTitleRankDisplay:
                case CharTitleTrackInfo:
                    return {message.id, message.U(0)};
                case PartyMemberAdd:
                    return {message.id, message.U(0), message.U(1)};
                case PartyHeroAdd:
                    return {message.id, message.U(0), message.U(1), message.U(2)};
                default:
                    return {};
            }
        }

        int EntrySection(const uint16_t id)
        {
            switch (id) {
                case MissionInstanceLoadHead:
                case MissionConfigEnd:
                case MissionInstanceLoadPlayerName:
                case MissionInstanceInfo:
                    return 0;
                case MissionManifestPhase:
                case MissionManifestChunk:
                case MissionManifestComplete:
                    return 1;
                case MissionReadyForMapSpawn:
                    return 2;
                case MissionLoadMap:
                    return 3;
                case CharMissionStreamStart:
                case MissionInstanceLoadFinish:
                    return 4;
                default:
                    return -1;
            }
        }

        struct Decoder {
            const uint8_t* body;
            size_t body_size;
            Message& out;
            const ReplayFile::TextFilter& filter;
            size_t offset = 2;
            bool ok = true;

            uint32_t Take(const size_t size)
            {
                if (offset + size > body_size) {
                    ok = false;
                    return 0;
                }
                uint32_t value = 0;
                for (size_t i = 0; i < size; i++) {
                    value |= static_cast<uint32_t>(body[offset + i]) << (8 * i);
                }
                offset += size;
                return value;
            }

            void Numbers(const size_t count, const size_t size)
            {
                auto& field = out.fields.emplace_back();
                for (size_t i = 0; i < count && ok; i++) {
                    field.numbers.push_back(Take(size));
                }
            }

            bool Walk(const uint32_t* descriptors, const size_t count)
            {
                for (size_t i = 0; i < count && ok; i++) {
                    const auto type = static_cast<StoCFieldType>(descriptors[i] & 0xF);
                    const auto sub = descriptors[i] >> 4 & 0xF;
                    const auto size = descriptors[i] >> 8;
                    switch (type) {
                        case StoCFieldType::Id:
                        case StoCFieldType::Float:
                            Numbers(1, 4);
                            break;
                        case StoCFieldType::Vect2:
                            Numbers(2, 4);
                            break;
                        case StoCFieldType::Vect3:
                            Numbers(3, 4);
                            break;
                        case StoCFieldType::Int:
                        case StoCFieldType::IntB:
                            if (size != 1 && size != 2 && size != 4) {
                                return false;
                            }
                            Numbers(1, size);
                            break;
                        case StoCFieldType::Blob:
                        case StoCFieldType::BlobB:
                            Numbers(size, 1);
                            break;
                        case StoCFieldType::ArrayData:
                        case StoCFieldType::ArrayDataB:
                            break;
                        case StoCFieldType::String16: {
                            const auto length = Take(2);
                            if (length >= size) {
                                return false;
                            }
                            auto& field = out.fields.emplace_back();
                            for (uint32_t c = 0; c < length && ok; c++) {
                                field.text.push_back(static_cast<wchar_t>(Take(2)));
                            }
                            if (filter && ok && !field.text.empty()) {
                                filter(field.text, size);
                                field.text.resize(std::min<size_t>(field.text.size(), size - 1));
                            }
                            break;
                        }
                        case StoCFieldType::ArrayCount: {
                            const auto items = Take(2);
                            if (sub > 2 || items > size) {
                                return false;
                            }
                            Numbers(items, size_t{1} << sub);
                            break;
                        }
                        case StoCFieldType::NestedStruct: {
                            const auto repetitions = Take(1);
                            if (repetitions > size) {
                                return false;
                            }
                            out.fields.push_back({{repetitions}, {}});
                            for (uint32_t r = 0; r < repetitions && ok; r++) {
                                if (!Walk(descriptors + i + 1, count - i - 1)) {
                                    return false;
                                }
                            }
                            return ok;
                        }
                        default:
                            return false;
                    }
                }
                return ok;
            }
        };

        struct Encoder {
            const Message& message;
            std::vector<uint8_t>& out;
            size_t next = 0;

            const Field* Next() { return next < message.fields.size() ? &message.fields[next++] : nullptr; }

            bool Put(const uint32_t value, const size_t size)
            {
                if (size < 4 && value >> (8 * size)) {
                    return false;
                }
                for (size_t i = 0; i < size; i++) {
                    out.push_back(static_cast<uint8_t>(value >> (8 * i)));
                }
                return true;
            }

            bool Numbers(const size_t count, const size_t size)
            {
                const auto field = Next();
                if (!field || field->numbers.size() != count) {
                    return false;
                }
                return std::ranges::all_of(field->numbers, [&](const uint32_t value) {
                    return Put(value, size);
                });
            }

            bool Walk(const uint32_t* descriptors, const size_t count)
            {
                for (size_t i = 0; i < count; i++) {
                    const auto type = static_cast<StoCFieldType>(descriptors[i] & 0xF);
                    const auto sub = descriptors[i] >> 4 & 0xF;
                    const auto size = descriptors[i] >> 8;
                    switch (type) {
                        case StoCFieldType::Id:
                        case StoCFieldType::Float:
                            if (!Numbers(1, 4)) {
                                return false;
                            }
                            break;
                        case StoCFieldType::Vect2:
                            if (!Numbers(2, 4)) {
                                return false;
                            }
                            break;
                        case StoCFieldType::Vect3:
                            if (!Numbers(3, 4)) {
                                return false;
                            }
                            break;
                        case StoCFieldType::Int:
                        case StoCFieldType::IntB:
                            if ((size != 1 && size != 2 && size != 4) || !Numbers(1, size)) {
                                return false;
                            }
                            break;
                        case StoCFieldType::Blob:
                        case StoCFieldType::BlobB:
                            if (!Numbers(size, 1)) {
                                return false;
                            }
                            break;
                        case StoCFieldType::ArrayData:
                        case StoCFieldType::ArrayDataB:
                            break;
                        case StoCFieldType::String16: {
                            const auto field = Next();
                            if (!field || field->text.size() >= size || !Put(static_cast<uint32_t>(field->text.size()), 2)) {
                                return false;
                            }
                            for (const auto c : field->text) {
                                Put(static_cast<uint16_t>(c), 2);
                            }
                            break;
                        }
                        case StoCFieldType::ArrayCount: {
                            const auto field = Next();
                            if (!field || sub > 2 || field->numbers.size() > size || !Put(static_cast<uint32_t>(field->numbers.size()), 2)) {
                                return false;
                            }
                            for (const auto value : field->numbers) {
                                if (!Put(value, size_t{1} << sub)) {
                                    return false;
                                }
                            }
                            break;
                        }
                        case StoCFieldType::NestedStruct: {
                            const auto field = Next();
                            if (!field || field->numbers.size() != 1 || field->numbers[0] > size || !Put(field->numbers[0], 1)) {
                                return false;
                            }
                            for (uint32_t r = 0; r < field->numbers[0]; r++) {
                                if (!Walk(descriptors + i + 1, count - i - 1)) {
                                    return false;
                                }
                            }
                            return true;
                        }
                        default:
                            return false;
                    }
                }
                return true;
            }
        };

        float Clamped(const float value)
        {
            return std::clamp(value, 0.f, 1.f);
        }

        Message Make(const uint16_t id, std::initializer_list<uint32_t> values)
        {
            Message message{id, {}};
            for (const auto value : values) {
                message.fields.push_back({{value}, {}});
            }
            return message;
        }
        bool BringsIdsIn(const IdField& field)
        {
            return std::ranges::find(id_bringers, field) != std::ranges::end(id_bringers);
        }
        bool OrderChecked(const IdKind kind)
        {
            // Skill ids are the game's own data: what comes first in a load is a skill's use (228, then 227 to 230), not its making.
            return kind != IdKind::Skill;
        }
        std::vector<uint32_t> ShownItems(const Message& message)
        {
            std::vector<uint32_t> shown;
            const auto add = [&shown, &message](const size_t field) {
                if (message.U(field)) {
                    shown.push_back(message.U(field));
                }
            };
            switch (message.id) {
                case AgentCreate:
                    if (message.U(CreateDef) == item_def) {
                        add(CreateClass);
                    }
                    break;
                case CharEquipment:
                    for (size_t slot = 1; slot <= equipment_slots; slot++) {
                        add(slot);
                    }
                    break;
                case CharEquipUpdate:
                    add(2);
                    break;
                case CharNpcUpdateWeapons:
                    add(1);
                    add(2);
                    break;
                default:
                    break;
            }
            return shown;
        }

    } // namespace

    uint32_t Message::U(const size_t field, const size_t item) const
    {
        return field < fields.size() && item < fields[field].numbers.size() ? fields[field].numbers[item] : 0;
    }

    float Message::F(const size_t field, const size_t item) const
    {
        return std::bit_cast<float>(U(field, item));
    }

    void Message::Set(const size_t field, const uint32_t value, const size_t item)
    {
        if (field < fields.size() && item < fields[field].numbers.size()) {
            fields[field].numbers[item] = value;
        }
    }

    void Message::SetF(const size_t field, const float value, const size_t item)
    {
        Set(field, std::bit_cast<uint32_t>(value), item);
    }

    size_t DecodePrefix(const ReplayFile::MessageTable& table, const uint8_t* bytes, const size_t size, Message& out, const ReplayFile::TextFilter& filter)
    {
        out = {};
        if (size < 2) {
            return 0;
        }
        out.id = static_cast<uint16_t>(bytes[0] | bytes[1] << 8);
        if (out.id >= table.size() || table[out.id].empty()) {
            return 0;
        }
        Decoder decoder{bytes, std::min(size, ReplayFile::max_body_size), out, filter};
        const auto& descriptors = table[out.id];
        return decoder.Walk(descriptors.data() + 1, descriptors.size() - 1) ? decoder.offset : 0;
    }

    bool Decode(const ReplayFile::MessageTable& table, const std::vector<uint8_t>& body, Message& out, const ReplayFile::TextFilter& filter)
    {
        return !body.empty() && DecodePrefix(table, body.data(), body.size(), out, filter) == body.size();
    }

    bool Encode(const ReplayFile::MessageTable& table, const Message& message, std::vector<uint8_t>& out)
    {
        out.clear();
        if (message.id >= table.size() || table[message.id].empty()) {
            return false;
        }
        Encoder encoder{message, out};
        encoder.Put(message.id, 2);
        const auto& descriptors = table[message.id];
        return encoder.Walk(descriptors.data() + 1, descriptors.size() - 1) && encoder.next == message.fields.size() && out.size() <= ReplayFile::max_body_size;
    }

    Message Blank(const ReplayFile::MessageTable& table, const uint16_t message_id)
    {
        Message message{message_id, {}};
        if (message_id >= table.size() || table[message_id].empty()) {
            return message;
        }
        const auto& descriptors = table[message_id];
        for (size_t i = 1; i < descriptors.size(); i++) {
            const auto type = static_cast<StoCFieldType>(descriptors[i] & 0xF);
            const auto size = descriptors[i] >> 8;
            switch (type) {
                case StoCFieldType::Id:
                case StoCFieldType::Float:
                case StoCFieldType::Int:
                case StoCFieldType::IntB:
                    message.fields.push_back({{0}, {}});
                    break;
                case StoCFieldType::Vect2:
                    message.fields.push_back({{0, 0}, {}});
                    break;
                case StoCFieldType::Vect3:
                    message.fields.push_back({{0, 0, 0}, {}});
                    break;
                case StoCFieldType::Blob:
                case StoCFieldType::BlobB:
                    message.fields.push_back({std::vector<uint32_t>(size, 0), {}});
                    break;
                case StoCFieldType::String16:
                case StoCFieldType::ArrayCount:
                    message.fields.emplace_back();
                    break;
                case StoCFieldType::NestedStruct:
                    message.fields.push_back({{0}, {}});
                    return message;
                default:
                    break;
            }
        }
        return message;
    }

    std::vector<Message> ItemReport(const ReplayFile::MessageTable& table, const ClientItem& item)
    {
        auto details = Blank(table, ItemReportDetailAll);
        if (details.fields.size() <= item_mods_field) {
            return {};
        }
        // CItemTable::AddDetailLow keeps 353's formula less one (0x5E5 for none), AddDetailHigh its mods and the item code's terminator.
        const uint32_t values[] = {item.item_id, item.model_file, item.type, item.dye_tint, item.dye_colours, item.formula == no_formula ? 0 : item.formula + 1, item.multiplier, item.interaction, item.value, item.model_id, item.quantity};
        for (size_t i = 0; i < std::size(values); i++) {
            details.Set(i, values[i]);
        }
        details.fields[item_name_field].text = item.name;
        const auto code = !item.code.empty() && item.code.back() == item_code_terminator ? std::span(item.code).first(item.code.size() - 1) : std::span(item.code);
        details.Set(item_mods_field, static_cast<uint32_t>(code.size()));
        for (const auto mod : code) {
            details.fields.push_back({{mod}, {}});
        }
        std::vector<Message> messages = {details};
        if (auto profession = Blank(table, ItemProfession); item.profession != any_profession && profession.fields.size() > 1) {
            profession.Set(0, item.item_id);
            profession.Set(1, item.profession);
            messages.push_back(profession);
        }
        if (auto customize = Blank(table, ItemCustomize); !item.customizer.empty() && customize.fields.size() > 1) {
            customize.Set(0, item.item_id);
            customize.fields[1].text = item.customizer;
            messages.push_back(customize);
        }
        return messages;
    }

    std::vector<IdField> IdFields(const ReplayFile::MessageTable& table, const uint16_t message_id)
    {
        std::vector<IdField> fields;
        const auto agent = [&fields, message_id](const size_t field) {
            fields.push_back({message_id, static_cast<uint8_t>(field), IdKind::Agent});
        };
        if (IsAgentClient(message_id)) {
            if (message_id != AgentAdvance && message_id != AgentConnect) {
                agent(0);
            }
            if (message_id == AgentUpdateMoveTarget) {
                agent(4);
            }
            if (message_id == AgentCreate) {
                agent(20);
            }
        }
        else if (message_id == GadgetState) {
            agent(0);
        }
        else if (message_id != GadgetClassDefine && message_id < table.size() && !table[message_id].empty()) {
            const auto& descriptors = table[message_id];
            size_t field = 0;
            for (size_t i = 1; i < descriptors.size(); i++) {
                const auto type = static_cast<StoCFieldType>(descriptors[i] & 0xF);
                if (type == StoCFieldType::ArrayData || type == StoCFieldType::ArrayDataB) {
                    continue;
                }
                if (type == StoCFieldType::NestedStruct) {
                    break;
                }
                if (type == StoCFieldType::Id && (descriptors[i] >> 4 & 0xF) == 1) {
                    agent(field);
                }
                field++;
            }
        }
        for (const auto& named : named_id_fields) {
            if (named.message == message_id) {
                fields.push_back(named);
            }
        }
        return fields;
    }

    std::vector<std::pair<IdField, uint32_t>> Order::Unbrought(const ReplayFile::MessageTable& table, const Message& message) const
    {
        std::vector<std::pair<IdField, uint32_t>> unbrought;
        std::set<std::pair<IdKind, uint32_t>> own;
        for (const auto& field : IdFields(table, message.id)) {
            if (field.field >= message.fields.size() || (IsLookNotice(message) && field.field == 2)) {
                continue;
            }
            const auto brings = BringsIdsIn(field);
            for (const auto id : message.fields[field.field].numbers) {
                if (!id) {
                    continue;
                }
                if (brings) {
                    own.insert({field.kind, id});
                }
                else if (OrderChecked(field.kind) && !brought.contains({field.kind, id}) && !own.contains({field.kind, id})) {
                    unbrought.emplace_back(field, id);
                }
            }
        }
        // A ground item's 32 names its item by its class: the stream sends the item's details first.
        if (const auto item = message.U(CreateClass); message.id == AgentCreate && message.U(CreateDef) == item_def && item && !brought.contains({IdKind::Item, item})) {
            unbrought.emplace_back(IdField{AgentCreate, static_cast<uint8_t>(CreateClass), IdKind::Item}, item);
        }
        return unbrought;
    }

    void Order::Bring(const ReplayFile::MessageTable& table, const Message& message)
    {
        for (const auto& field : IdFields(table, message.id)) {
            if (field.field < message.fields.size() && BringsIdsIn(field) && !(IsLookNotice(message) && field.field == 2)) {
                for (const auto id : message.fields[field.field].numbers) {
                    if (id) {
                        brought.insert({field.kind, id});
                    }
                }
            }
        }
    }

    std::string FieldAssert(const Message& message)
    {
        constexpr uint32_t highest_agent_def = 8;
        constexpr size_t create_radius = 10;
        constexpr size_t create_team_radius = 12;
        constexpr uint32_t quest_description_filled = 1;
        constexpr size_t most_npc_model_files = 8;
        constexpr uint32_t highest_weapon_set = 3;
        constexpr uint32_t highest_manifest_type = 3;
        constexpr uint32_t highest_manifest_phase = 1;
        const auto agent_invalid_position = std::numeric_limits<float>::infinity();
        switch (message.id) {
            case AgentCreate:
                if (message.U(CreateDef) > highest_agent_def) {
                    return std::format("f2, the agent's def ({}), is past the agent context's definitions (AgAgent.cpp:180)", message.U(CreateDef));
                }
                if (message.F(CreatePosition, 0) == agent_invalid_position && message.F(CreatePosition, 1) == agent_invalid_position) {
                    return "f4, the position, is AGENT_INVALID_POSITION (AgMsg.cpp:252)";
                }
                return message.F(create_team_radius) > message.F(create_radius) ? "f12, the team radius, is past f10, the bounding radius (AgAgent.cpp:156)" : "";
            case CharAttackInterval:
                if (message.F(1) == 0.f) {
                    return "f1, the attack interval, is 0: CCharAgent::SetAttackInterval asserts base (AvChar.cpp:7207)";
                }
                return message.F(2) == 0.f ? "f2, the attack modifier, is 0: CCharAgent::SetAttackInterval asserts modifier (AvChar.cpp:7208)" : "";
            case CharAgentAttrUpdateIntTarget:
                return IsLookNotice(message) && message.U(2) >= look_slots ? std::format("f2, the armour slot ({}), is past the agent's five (ChCliApi.cpp:{})", message.U(2), message.U(0) == LookComposite ? 2156 : 2167) : "";
            case CharQuestGeneralInfo:
                return message.U(1) & quest_description_filled ? "f1, the quest's state, holds the bit the client sets once it has the description (ChCliApi.cpp:702)" : "";
            case CharNpcModel:
                return message.fields.size() > 1 && message.fields[1].numbers.size() > most_npc_model_files ? std::format("f1, the NPC's model files, holds {}, past MONSTER_COMPOSITE_MAX_DWORDS (CharMsg.cpp:4438)", message.fields[1].numbers.size()) : "";
            case ItemInventoryEquipSetActivate:
                return message.U(1) > highest_weapon_set ? std::format("f1, the weapon set ({}), is past ITEM_PLAYER_EQUIP_SETS (ItCliInv.cpp:329)", message.U(1)) : "";
            case ItemReportDetailAll:
            case ItemReportDetailAllReset: {
                if (!message.U(item_model_file_field)) {
                    return "f1, the model file, is 0 (ItCliApi.cpp:2505)";
                }
                const auto mods = message.U(item_mods_field);
                return mods && message.U(item_mods_field + mods) == item_code_terminator ? "f12, the mods, end with the item code's terminator (ItemCode.h:516)" : "";
            }
            case MissionLoadMap:
                return message.U(0) ? "" : "f0, the map file, is 0, which the game's map load asserts on (File.cpp:367)";
            case MissionManifestComplete:
                return message.U(0) > highest_manifest_type ? std::format("f0, the manifest's type ({}), is past MANIFEST_TYPES (MsCliMan.cpp:487)", message.U(0)) : "";
            case MissionManifestPhase:
                return message.U(0) > highest_manifest_phase ? std::format("f0, the manifest's phase ({}), is not one (MsCliMan.cpp:472)", message.U(0)) : "";
            default:
                return "";
        }
    }

    std::string ClientAgents::Breaks(const Message& message)
    {
        const auto agent_id = message.U(0);
        switch (message.id) {
            case GadgetClassDefine:
                gadget_classes.insert(message.U(0));
                return "";
            case AgentCreate:
                attack_intervals.erase(agent_id);
                if (!created.insert(agent_id).second) {
                    return std::format("agent {} created again before its 33: the client asserts !array[agent] (AgAgent.cpp:162)", agent_id);
                }
                // The gadget agent's constructor draws its class's model file, which only 263 sets (GadgetCliGetClass, then FileConvertIdToPath).
                return message.U(CreateDef) == gadget_def && !gadget_classes.contains(message.U(CreateClass)) ? std::format("agent {} is a gadget of class {} that no 263 defined: its model file is 0 (File.cpp:367)", agent_id, message.U(CreateClass)) : "";
            case AgentDestroy:
                created.erase(agent_id);
                attack_intervals.erase(agent_id);
                return "";
            case AgentUpdateDirection:
            case AgentUpdateMaxSpeed:
            case AgentUpdateMoveAbort:
            case AgentUpdateMovePath:
            case AgentUpdateMoveTarget:
            case AgentUpdateMoveType:
            case AgentUpdatePoint: {
                // Each IAgent::OnMsgSrvUpdate* asserts its agent in the server's world.
                constexpr std::pair<uint16_t, uint32_t> assert_lines[] = {
                    {AgentUpdateDirection, 412}, {AgentUpdateMaxSpeed, 462}, {AgentUpdateMoveAbort, 486}, {AgentUpdateMovePath, 513}, {AgentUpdateMoveTarget, 535}, {AgentUpdateMoveType, 554}, {AgentUpdatePoint, 579},
                };
                const auto line = std::ranges::find(assert_lines, message.id, &std::pair<uint16_t, uint32_t>::first)->second;
                return created.contains(agent_id) ? "" : std::format("agent {} has no 32 before its message {}: its handler asserts the agent (AgMsg.cpp:{})", agent_id, message.id, line);
            }
            case CharAttackInterval:
                if (!created.contains(agent_id)) {
                    return std::format("agent {} has no 32 before its attack interval: AvCharSetAttackInterval asserts its agent view (AvApi.cpp:750)", agent_id);
                }
                attack_intervals.insert(agent_id);
                return "";
            case CharAgentAttrUpdateIntTarget: {
                const auto kind = message.U(0);
                const auto attacker = message.U(1);
                if (!created.contains(attacker) || attack_intervals.contains(attacker)) {
                    return "";
                }
                // Only 53 sets an agent view's attack interval: CCharAgent's constructor leaves it 0.
                if (kind == AttackStarted) {
                    return std::format("agent {} starts an attack with no attack interval (53) since its 32: ProcessActionAttackWarmup asserts m_attackInterval (AvChar.cpp:4791)", attacker);
                }
                return kind == AttackSkillActivated ? std::format("agent {} starts an attack skill with no attack interval (53) since its 32: its swing's length is that interval", attacker) : "";
            }
            default:
                return "";
        }
    }

    std::vector<uint32_t> Identity(const Message& message)
    {
        if (auto key = InstanceKey(message); !key.empty()) {
            return key;
        }
        const auto id = message.id;
        if (Contains(item_ids, id)) {
            return {id, message.U(0)};
        }
        switch (id) {
            case CharPlayerCreate:
            case CharPlayerRowFlags:
            case CharAttributeState:
            case CharAttributes:
            case CharPlayerUnlockedProfession:
            case CharProfessions:
            case CharSkillbar:
                return {id, message.U(0)};
            case CharAgentUpdateAttribute:
                return {id, message.U(0), message.U(1)};
            default:
                break;
        }
        const auto agent_field = AgentField(id);
        if (agent_field < 0) {
            return {id};
        }
        const auto agent = message.U(agent_field);
        switch (id) {
            case CharNotifyDword:
                return message.U(0) == AuraAdded || message.U(0) == AuraRemoved ? std::vector<uint32_t>{id, agent, message.U(0), message.U(2)} : std::vector<uint32_t>{id, agent, message.U(0)};
            case CharAgentAttrUpdateIntTarget:
                return IsLookNotice(message) ? std::vector<uint32_t>{id, agent, message.U(0), message.U(2)} : std::vector<uint32_t>{id, agent};
            case CharAgentAttrUpdateFloat:
            case CharAgentAttrUpdateFloatTarget:
                return {id, agent, message.U(0)};
            case CharEffectApplied:
            case CharEffectUpkeepAdded:
            case CharEffectUpkeepApplied:
                return {id, agent, message.U(3)};
            case CharEffectRemoved:
            case CharEffectUpkeepRemoved:
                return {id, agent, message.U(1)};
            default:
                return {id, agent};
        }
    }

    bool IsBagContent(const Message& message)
    {
        return Contains(bag_content_ids, message.id);
    }

    namespace {
        constexpr uint32_t tag_capacity = 6;
        constexpr uint32_t guild_name_min_capacity = 32;
        constexpr uint16_t guild_messages[] = {GuildTownAllianceObject, GuildAllianceInfo, GuildGeneralInfo, GuildInviteReceived, MissionAutoTournPartyAdd, MissionToClient431};
        constexpr uint16_t guild_tag_messages[] = {CharChatMessageGlobal, GuildTownAllianceObject, GuildAllianceInfo, GuildGeneralInfo, GuildInviteReceived, MissionAutoTournPartyAdd, MissionToClient431};
    } // namespace

    NameStripper::NameStripper(const bool other_players, const bool own_names, std::wstring own_name) : other_players(other_players), own_names(own_names)
    {
        if (!own_name.empty()) {
            recorder_names.insert(std::move(own_name));
        }
    }

    std::wstring NameStripper::Label(const uint16_t message_id, const std::wstring_view name, const uint32_t capacity)
    {
        const auto numbered = [name](std::unordered_map<std::wstring, std::wstring>& labels, const std::wstring_view prefix) {
            auto& label = labels[std::wstring(name)];
            if (label.empty()) {
                label = std::format(L"{}{}", prefix, labels.size());
            }
            return label;
        };
        if (std::ranges::contains(guild_tag_messages, message_id) && capacity == tag_capacity) {
            return numbered(tags, L"G");
        }
        if (std::ranges::contains(guild_messages, message_id) && capacity >= guild_name_min_capacity) {
            return numbered(guilds, L"Guild ");
        }
        if (message_id == CharAccountNameData || message_id == CharHeroAccountName) {
            account_names.emplace(name);
        }
        if (message_id == MissionInstanceLoadPlayerName) {
            recorder_names.emplace(name);
        }
        if (account_names.contains(name)) {
            return own_names ? ReplayFile::own_name_placeholder : other_players ? numbered(players, L"Player ") : std::wstring(name);
        }
        if (message_id == CharMercenaryInfo) {
            return own_names ? numbered(mercenaries, L"Mercenary ") : other_players ? numbered(players, L"Player ") : std::wstring(name);
        }
        if (recorder_names.contains(name)) {
            return own_names ? ReplayFile::own_name_placeholder : std::wstring(name);
        }
        return other_players ? numbered(players, L"Player ") : std::wstring(name);
    }

    void NameStripper::Apply(const uint16_t message_id, std::wstring& text, const uint32_t capacity)
    {
        if (ReplayFile::IsPlainText(text)) {
            text = Label(message_id, text, capacity);
            return;
        }
        std::wstring stripped;
        size_t position = 0;
        for (auto start = text.find(ReplayFile::literal_start); start != std::wstring::npos; start = text.find(ReplayFile::literal_start, position)) {
            const auto end = text.find(ReplayFile::literal_end, start + 1);
            if (end == std::wstring::npos) {
                break;
            }
            stripped.append(text, position, start + 1 - position);
            const auto literal = std::wstring_view(text).substr(start + 1, end - start - 1);
            stripped += ReplayFile::IsPlainText(literal) ? Label(message_id, literal, 0) : std::wstring(literal);
            position = end;
        }
        stripped.append(text, position);
        text = std::move(stripped);
    }

    ReplayFile::TextFilter NameStripper::For(const uint16_t message_id)
    {
        return [this, message_id](std::wstring& text, const uint32_t capacity) {
            Apply(message_id, text, capacity);
        };
    }

    bool InventoryFilter::Keep(const Message& message, std::vector<Message>& shown_before, std::vector<uint32_t>& unknown_items)
    {
        if (message.id == MissionInstanceLoadHead) {
            // Item ids are the instance's: a new map's items are not those shown in the one before.
            shown.clear();
            held.clear();
        }
        if (IsBagContent(message)) {
            return false;
        }
        if (Contains(item_ids, message.id) || message.id == ItemCount) {
            if (shown.contains(message.U(0))) {
                return true;
            }
            auto& kept = held[message.U(0)];
            if (message.id != ItemCount) {
                kept[message.id] = message;
            }
            else if (kept.contains(ItemReportDetailAll)) {
                kept[ItemReportDetailAll].Set(item_quantity_field, message.U(1));
            }
            return false;
        }
        for (const auto item : ShownItems(message)) {
            if (!shown.insert(item).second) {
                continue;
            }
            const auto found = held.find(item);
            if (found == held.end() || found->second.empty()) {
                unknown_items.push_back(item);
                continue;
            }
            for (const auto id : item_ids) {
                if (const auto detail = found->second.find(id); detail != found->second.end()) {
                    shown_before.push_back(detail->second);
                }
            }
            held.erase(found);
        }
        for (const auto& field : IdFields(*table, message.id)) {
            if (field.kind != IdKind::Item || field.field >= message.fields.size()) {
                continue;
            }
            if (std::ranges::any_of(message.fields[field.field].numbers, [this](const uint32_t item) {
                    return item && !shown.contains(item);
                })) {
                return false;
            }
        }
        return true;
    }

    bool InventoryFilter::Holds(const Message& message) const
    {
        return (Contains(item_ids, message.id) || message.id == ItemCount) && !shown.contains(message.U(0));
    }

    size_t InventoryFilter::Held() const
    {
        size_t count = 0;
        for (const auto& details : held | std::views::values) {
            count += details.size();
        }
        return count;
    }

    bool World::Apply(const ReplayFile::Record& record)
    {
        Message message;
        if (!Decode(*table, record.body, message)) {
            layout_errors++;
            return false;
        }
        Apply(message);
        return true;
    }

    void World::Apply(const Message& message)
    {
        if ((message.id == MissionInstanceLoadHead || message.id == MissionInstanceInfo) && connected) {
            const auto errors = layout_errors;
            *this = World(*table);
            layout_errors = errors;
        }
        switch (message.id) {
            case AgentAdvance:
                Tick(message.U(0));
                return;
            case AgentConnect:
                clock = message.U(0);
                connected = true;
                return;
            case AgentCreate: {
                auto& agent = agents[message.U(0)];
                if (agent.create.id) {
                    agent = {};
                }
                agent.create = message;
                // An agent made mid-walk (CAgent's constructor with a stop time) is kept as the client walks it: where it is now, then its order, as a keyframe writes it.
                if (const auto point_time = message.U(CreatePointTime); message.U(CreateStopTime) && std::isfinite(message.F(CreateTarget, 0))) {
                    agent.move = Blank(*table, message.U(CreateTargetAgent) ? AgentUpdateMoveTarget : AgentUpdateMovePath);
                    agent.move.Set(0, message.U(0));
                    agent.move.Set(MovePoint, message.U(CreateTarget, 0), 0);
                    agent.move.Set(MovePoint, message.U(CreateTarget, 1), 1);
                    agent.move.Set(MovePlane, message.U(CreateTargetPlane));
                    agent.move.Set(MoveCurrentPlane, message.U(CreatePlane));
                    agent.move.Set(MoveTargetAgent, message.U(CreateTargetAgent));
                    if (message.U(CreateSegment, 0) != message.U(CreateTarget, 0) || message.U(CreateSegment, 1) != message.U(CreateTarget, 1)) {
                        agent.leg = Leg{message.F(CreateSegment, 0), message.F(CreateSegment, 1), message.U(CreateSegmentPlane)};
                    }
                    agent.pace = std::hypot(message.F(CreateVelocity, 0), message.F(CreateVelocity, 1));
                    const auto infinity = std::numeric_limits<float>::infinity();
                    for (const auto field : {CreateStopTime, CreatePointTime, CreateTargetPlane, CreateTargetAgent, CreateSegmentPlane}) {
                        agent.create.Set(field, 0);
                    }
                    for (const size_t item : {0, 1}) {
                        agent.create.SetF(CreateVelocity, 0.f, item);
                        agent.create.SetF(CreateTarget, infinity, item);
                        agent.create.SetF(CreateSegment, infinity, item);
                    }
                    if (clock > point_time) {
                        Walk(agent, clock - point_time);
                    }
                }
                return;
            }
            case AgentDestroy:
                agents.erase(message.U(0));
                return;
            case AgentReporter:
                reporter = message;
                return;
            case MissionInstanceInfo: {
                auto stored = message;
                stored.Set(observer_field, 0);
                entry.push_back(stored);
                return;
            }
            case CharPlayerCreate:
                players[message.U(0)] = message;
                return;
            case CharAgentDestroyPlayer:
                players.erase(message.U(0));
                player_flags.erase(message.U(0));
                return;
            case CharPlayerRowFlags: {
                auto& flags = player_flags[message.U(0)];
                flags.value = (flags.value & ~message.U(2)) | (message.U(1) & message.U(2));
                flags.mask |= message.U(2);
                return;
            }
            case CharQuestRemove:
                for (const auto id : quest_ids) {
                    instance.erase({id, message.U(0)});
                }
                return;
            case CharMapInitStart:
                map_init = {message};
                return;
            case PartyLock:
                // With f0 0 the party table's lock is cleared (PyCliParty.cpp): a load's own state, nothing kept.
                if (!message.U(0)) {
                    instance.erase({message.id});
                    return;
                }
                break;
            case CharMapInit:
                if (!map_init.empty()) {
                    map_init.push_back(message);
                }
                return;
            case CharTitleUpdate:
                if (const auto found = instance.find({CharTitleTrackInfo, message.U(0)}); found != instance.end()) {
                    found->second.Set(title_points_field, message.U(1));
                }
                return;
            case ItemCount:
                if (const auto found = items.find(message.U(0)); found != items.end() && found->second.contains(ItemReportDetailAll)) {
                    found->second[ItemReportDetailAll].Set(item_quantity_field, message.U(1));
                }
                return;
            case CharAttributeState:
                characters[message.U(0)].attribute_state = message;
                return;
            case CharAttribPointsAvail:
            case CharAttribPointsTotal:
                if (const auto found = characters.find(message.U(0)); found != characters.end() && found->second.attribute_state.id) {
                    found->second.attribute_state.Set(message.id == CharAttribPointsAvail ? 1 : 2, message.U(1));
                }
                return;
            case CharAttributes: {
                auto& character = characters[message.U(0)];
                character.latest[message.id] = message;
                character.attribute_updates.clear();
                return;
            }
            case CharAgentUpdateAttribute:
                characters[message.U(0)].attribute_updates[message.U(1)] = message;
                return;
            case CharPlayerUnlockedProfession:
            case CharProfessions:
                characters[message.U(0)].latest[message.id] = message;
                return;
            case CharSkillbar: {
                auto& skillbar = characters[message.U(0)].latest[message.id];
                skillbar = message;
                skillbar.Set(skillbar_create_field, 1);
                return;
            }
            default:
                break;
        }
        if (EntrySection(message.id) >= 0) {
            entry.push_back(message);
            return;
        }
        if (Contains(item_ids, message.id)) {
            auto& item = items[message.U(0)];
            // 354 frees the item before reporting it as 353 does (ItemCliOnReportDetailAllReset); 353's AddDetailLow unassigns it and sets its profession to any (11).
            if (message.id == ItemReportDetailAllReset) {
                item.clear();
            }
            else if (message.id == ItemReportDetailAll) {
                for (const auto replaced : std::initializer_list<uint16_t>{ItemReportDetailAllReset, ItemProfession, ItemAssign}) {
                    item.erase(replaced);
                }
            }
            item[message.id] = message;
            return;
        }
        if (const auto key = InstanceKey(message); !key.empty()) {
            instance[key] = message;
            return;
        }
        const auto agent_field = AgentField(message.id);
        if (agent_field < 0 || (Contains(transient_ids, message.id) && !IsLookNotice(message))) {
            return;
        }
        auto found = agents.find(message.U(agent_field));
        if (found == agents.end() || !found->second.create.id) {
            if (!IsRow(message) && !IsUpkeep(message.id)) {
                return;
            }
            found = agents.try_emplace(message.U(agent_field)).first;
        }
        ApplyToAgent(found->second, message);
    }

    void World::ApplyToAgent(Agent& agent, const Message& message)
    {
        switch (message.id) {
            case AgentUpdateMoveAbort:
                agent.move = {};
                agent.leg.reset();
                return;
            case AgentUpdateMovePath:
            case AgentUpdateMoveTarget:
                agent.move = message;
                agent.leg.reset();
                agent.pace = Pace(agent);
                return;
            case AgentUpdateMaxSpeed:
                // CAgent::UpdateMaxSpeed starts the walk again from where the agent is, toward its order's point, at the new speed.
                agent.slots[{message.id}] = message;
                agent.leg.reset();
                agent.pace = Pace(agent);
                return;
            case AgentUpdatePoint:
                // CAgent::UpdatePoint: the agent stands at the point, its walk stopped (StopMovement).
                agent.create.Set(CreatePosition, message.U(1, 0), 0);
                agent.create.Set(CreatePosition, message.U(1, 1), 1);
                agent.create.Set(CreatePlane, message.U(2));
                agent.move = {};
                agent.leg.reset();
                return;
            case AgentUpdateRotateAbort:
                agent.slots.erase({AgentUpdateRotation});
                return;
            case CharEquipment:
                for (size_t slot = 0; slot < equipment_slots; slot++) {
                    agent.equipment[slot] = message.U(slot + 1);
                }
                agent.equipped = true;
                return;
            case CharEquipUpdate:
                if (message.U(1) < equipment_slots) {
                    agent.equipment[message.U(1)] = message.U(2);
                    agent.equipped = true;
                }
                return;
            case CharNpcUpdateWeapons:
                agent.slots[{message.id}] = message;
                return;
            case CharAgentInitialEffects:
            case CharAgentUpdateEffects:
                agent.slots[{CharAgentInitialEffects}] = message;
                return;
            case CharAgentAllyDestroy:
                agent.slots.erase({CharAgentCreateNpc});
                return;
            case CharEffectUpkeepAdded:
            case CharEffectUpkeepApplied:
                agent.slots[{message.id, message.U(3)}] = message;
                return;
            case CharEffectUpkeepRemoved:
                agent.slots.erase({CharEffectUpkeepAdded, message.U(1)});
                agent.slots.erase({CharEffectUpkeepApplied, message.U(1)});
                return;
            case CharEffectApplied: {
                const auto duration = message.F(4);
                agent.effects[message.U(3)] = {message, duration > 0.f ? clock + static_cast<uint32_t>(std::lround(duration * 1000.0)) : 0};
                return;
            }
            case CharEffectRemoved:
                agent.effects.erase(message.U(1));
                return;
            case CharNotifyDword: {
                const auto kind = message.U(0);
                if (kind == AuraAdded) {
                    agent.slots[{message.id, AuraAdded, message.U(2)}] = message;
                }
                else if (kind == AuraRemoved) {
                    agent.slots.erase({message.id, AuraAdded, message.U(2)});
                }
                else if (IsStateNotice(kind)) {
                    agent.slots[{message.id, kind}] = message;
                }
                return;
            }
            case CharAgentAttrUpdateIntTarget:
                agent.slots[{message.id, message.U(0), message.U(2)}] = message;
                return;
            case CharAgentAttrUpdateFloat:
                switch (message.U(0)) {
                    case Energy:
                        agent.energy = message.F(2);
                        break;
                    case Health:
                        agent.health = message.F(2);
                        break;
                    case EnergyRegen:
                    case HealthRegen:
                        agent.slots[{message.id, message.U(0)}] = message;
                        break;
                    case EnergySpent:
                        agent.energy = Clamped(agent.energy + message.F(2));
                        break;
                    default:
                        break;
                }
                return;
            case CharAgentAttrUpdateFloatTarget:
                if (Contains(std::initializer_list<uint32_t>{Hit, CriticalHit, Damage, IgnoringArmour}, message.U(0))) {
                    agent.health = Clamped(agent.health + message.F(3));
                }
                return;
            default:
                agent.slots[{message.id}] = message;
                return;
        }
    }

    void World::Tick(const uint32_t milliseconds)
    {
        clock += milliseconds;
        const auto seconds = static_cast<float>(milliseconds) / 1000.f;
        for (auto it = agents.begin(); it != agents.end();) {
            auto& agent = it->second;
            if (!agent.create.id) {
                ++it;
                continue;
            }
            std::erase_if(agent.effects, [this](const auto& effect) {
                return effect.second.expiry && clock >= effect.second.expiry;
            });
            if (const auto regen = agent.slots.find({CharAgentAttrUpdateFloat, HealthRegen}); regen != agent.slots.end()) {
                agent.health = Clamped(agent.health + regen->second.F(2) * seconds);
            }
            if (const auto regen = agent.slots.find({CharAgentAttrUpdateFloat, EnergyRegen}); regen != agent.slots.end()) {
                agent.energy = Clamped(agent.energy + regen->second.F(2) * seconds);
            }
            Walk(agent, milliseconds);
            ++it;
        }
    }

    float World::Pace(const Agent& agent)
    {
        // BeginMovementSegment's velocity: the max speed (32's or 39's) times the move speed (32's or 43's), kept until the next leg.
        const auto max_speed = agent.slots.find({AgentUpdateMaxSpeed});
        const auto move_type = agent.slots.find({AgentUpdateMoveType});
        return (max_speed != agent.slots.end() ? max_speed->second.F(1) : agent.create.F(CreateSpeed)) * (move_type != agent.slots.end() ? move_type->second.F(1) : agent.create.F(CreateMoveSpeed));
    }

    void World::Walk(Agent& agent, const uint32_t milliseconds)
    {
        // The client walks a leg in a straight line (CAgent's position at a time), then from a detour's end on to its order's point (OnTimeStopMovement).
        auto left = agent.pace * static_cast<float>(milliseconds) / 1000.f;
        while (agent.move.id && left > 0.f) {
            const auto x = agent.create.F(CreatePosition, 0);
            const auto y = agent.create.F(CreatePosition, 1);
            const auto to_x = agent.leg ? agent.leg->x : agent.move.F(MovePoint, 0);
            const auto to_y = agent.leg ? agent.leg->y : agent.move.F(MovePoint, 1);
            const auto distance = std::hypot(to_x - x, to_y - y);
            if (left < distance) {
                agent.create.SetF(CreatePosition, x + (to_x - x) / distance * left, 0);
                agent.create.SetF(CreatePosition, y + (to_y - y) / distance * left, 1);
                return;
            }
            left -= distance;
            agent.create.SetF(CreatePosition, to_x, 0);
            agent.create.SetF(CreatePosition, to_y, 1);
            agent.create.Set(CreatePlane, agent.leg ? agent.leg->plane : agent.move.U(MovePlane));
            if (agent.leg) {
                agent.leg.reset();
            }
            else {
                agent.move = {};
            }
        }
    }

    void World::Apply(const ReplayFile::Motion& motion)
    {
        const auto found = agents.find(motion.agent);
        if (found == agents.end() || !found->second.create.id) {
            return;
        }
        auto& agent = found->second;
        agent.create.SetF(CreatePosition, motion.x, 0);
        agent.create.SetF(CreatePosition, motion.y, 1);
        agent.create.Set(CreatePlane, motion.plane);
        agent.create.SetF(CreateSpeed, motion.max_speed);
        agent.create.SetF(CreateMoveSpeed, motion.move_speed);
        agent.slots.erase({AgentUpdateMaxSpeed});
        agent.slots.erase({AgentUpdateMoveType});
        agent.move = {};
        agent.leg.reset();
        if (!motion.walking) {
            return;
        }
        agent.move = Blank(*table, motion.target_agent ? AgentUpdateMoveTarget : AgentUpdateMovePath);
        agent.move.Set(0, motion.agent);
        agent.move.SetF(MovePoint, motion.target_x, 0);
        agent.move.SetF(MovePoint, motion.target_y, 1);
        agent.move.Set(MovePlane, motion.target_plane);
        agent.move.Set(MoveCurrentPlane, motion.plane);
        agent.move.Set(MoveTargetAgent, motion.target_agent);
        if (motion.leg_x != motion.target_x || motion.leg_y != motion.target_y) {
            agent.leg = Leg{motion.leg_x, motion.leg_y, motion.leg_plane};
        }
        agent.pace = motion.max_speed * motion.move_speed;
    }

    std::vector<Message> World::Placement() const
    {
        // s2c 44 stands an agent at its point in both of the client's worlds and drops its tracked walk; its order then starts the walk again from there.
        std::vector<Message> out;
        for (const auto& [agent_id, agent] : agents) {
            if (!agent.create.id || agent.create.U(CreateDef) != living_def) {
                continue;
            }
            auto point = Blank(*table, AgentUpdatePoint);
            point.Set(0, agent_id);
            point.Set(1, agent.create.U(CreatePosition, 0), 0);
            point.Set(1, agent.create.U(CreatePosition, 1), 1);
            point.Set(2, agent.create.U(CreatePlane));
            out.push_back(std::move(point));
            if (agent.move.id) {
                out.push_back(agent.move);
                out.back().Set(MoveCurrentPlane, agent.create.U(CreatePlane));
                if (agent.move.id == AgentUpdateMoveTarget && !Alive(agent.move.U(MoveTargetAgent))) {
                    out.back().Set(MoveTargetAgent, 0);
                }
            }
        }
        return out;
    }

    bool World::Alive(const uint32_t agent_id) const
    {
        const auto found = agents.find(agent_id);
        return agent_id && found != agents.end() && found->second.create.id;
    }

    uint32_t World::ReportedAgent() const
    {
        return reporter.id && Alive(reporter.U(0)) ? reporter.U(0) : 0;
    }

    uint32_t World::ControlledAgent() const
    {
        const auto reported = ReportedAgent();
        const auto found = characters.find(reported);
        return found != characters.end() && found->second.attribute_state.id ? reported : 0;
    }

    uint32_t World::LeftOutAgent() const
    {
        return ControlledAgent() ? 0 : ReportedAgent();
    }

    std::set<uint32_t> World::ShownItems() const
    {
        std::set<uint32_t> shown;
        const auto left_out = LeftOutAgent();
        for (const auto& [agent_id, agent] : agents) {
            if (!agent.create.id || agent_id == left_out) {
                continue;
            }
            if (agent.create.U(CreateDef) == item_def) {
                shown.insert(agent.create.U(CreateClass));
            }
            if (agent.equipped) {
                shown.insert(agent.equipment.begin(), agent.equipment.end());
            }
            if (const auto weapons = agent.slots.find({CharNpcUpdateWeapons}); weapons != agent.slots.end()) {
                shown.insert({weapons->second.U(1), weapons->second.U(2)});
            }
        }
        shown.erase(0);
        return shown;
    }

    void World::JoinItem(const uint32_t item_id, std::vector<Message>& out, std::set<Key>& emitted) const
    {
        const auto found = items.find(item_id);
        if (!item_id || found == items.end() || !emitted.insert({ItemReportDetailAll, item_id}).second) {
            return;
        }
        for (const auto id : item_ids) {
            if (const auto message = found->second.find(id); message != found->second.end()) {
                out.push_back(message->second);
            }
        }
    }

    void World::JoinInventory(const uint32_t inventory_id, std::vector<Message>& out, std::set<Key>& emitted) const
    {
        for (const auto id : std::initializer_list<uint16_t>{ItemInventoryCreate, ItemInventoryEquipSetActivate}) {
            if (const auto found = instance.find({id, inventory_id}); found != instance.end() && emitted.insert(found->first).second) {
                out.push_back(found->second);
            }
        }
    }

    void World::JoinRows(const uint32_t agent_id, const Agent& agent, const bool before_connect, std::vector<Message>& out, std::set<Key>& emitted) const
    {
        const auto& create = agent.create;
        const auto define = [&](const Key& key) {
            if (const auto found = instance.find(key); found != instance.end() && emitted.insert(key).second) {
                out.push_back(found->second);
            }
        };
        const auto define_npc = [&](const uint32_t npc) {
            define({CharNpcProperties, npc});
            define({CharNpcModel, npc});
        };
        if (create.U(CreateDef) == gadget_def) {
            define({GadgetClassDefine, create.U(CreateClass)});
        }
        if ((create.U(CreateClass) & class_kind_mask) == npc_class) {
            define_npc(create.U(CreateClass) & ~class_kind_mask);
        }
        if (const auto model = agent.slots.find({CharUpdateAgentModel}); model != agent.slots.end()) {
            define_npc(model->second.U(1));
        }
        if (create.U(CreateDef) == item_def) {
            JoinItem(create.U(CreateClass), out, emitted);
        }
        const auto brings_agent = [](const Message& message) {
            return BringsIdsIn({message.id, static_cast<uint8_t>(AgentField(message.id)), IdKind::Agent});
        };
        const auto brought = before_connect || agent.create.id || std::ranges::any_of(agent.slots | std::views::values, [&](const Message& message) {
                                 return IsRow(message) && brings_agent(message);
                             });
        for (const auto bringers : {true, false}) {
            uint32_t row = 0;
            for (const auto& message : agent.slots | std::views::values) {
                row++;
                if (!IsRow(message) || (brings_agent(message) || IsLookNotice(message)) != bringers) {
                    continue;
                }
                const auto now = before_connect ? Contains(character_rows, message.id) : bringers || (brought && !agent.create.id);
                if (now && emitted.insert({AgentCreate, agent_id, row}).second) {
                    out.push_back(message);
                }
            }
        }
    }

    void World::JoinAgent(const uint32_t agent_id, const Agent& agent, std::vector<Message>& out, std::set<Key>& emitted) const
    {
        JoinRows(agent_id, agent, false, out, emitted);
        if (!agent.create.id) {
            return;
        }
        const auto& create = agent.create;
        out.push_back(create);
        if (const auto weapons = agent.slots.find({CharNpcUpdateWeapons}); weapons != agent.slots.end()) {
            JoinItem(weapons->second.U(1), out, emitted);
            JoinItem(weapons->second.U(2), out, emitted);
        }
        if (agent.equipped) {
            auto equipment = Make(CharEquipment, {agent_id});
            for (const auto item : agent.equipment) {
                JoinItem(item, out, emitted);
                equipment.fields.push_back({{item}, {}});
            }
            out.push_back(equipment);
        }
        uint32_t row = 0;
        for (const auto& [key, message] : agent.slots) {
            row++;
            if ((!IsRow(message) && !IsUpkeep(key[0])) || (IsRow(message) && emitted.insert({AgentCreate, agent_id, row}).second)) {
                out.push_back(message);
            }
        }
        if (agent.health != 1.f) {
            out.push_back(Make(CharAgentAttrUpdateFloat, {Health, agent_id, std::bit_cast<uint32_t>(agent.health)}));
        }
        if (agent.energy != 1.f) {
            out.push_back(Make(CharAgentAttrUpdateFloat, {Energy, agent_id, std::bit_cast<uint32_t>(agent.energy)}));
        }
        for (const auto& effect : agent.effects | std::views::values) {
            auto applied = effect.applied;
            if (effect.expiry) {
                applied.SetF(4, static_cast<float>(effect.expiry - clock) / 1000.f);
            }
            out.push_back(applied);
        }
    }

    void World::JoinCharacter(const uint32_t agent_id, std::vector<Message>& out, std::set<Key>& emitted) const
    {
        const auto found = characters.find(agent_id);
        if (found == characters.end() || !found->second.attribute_state.id || !emitted.insert({CharAttributeState, agent_id}).second) {
            return;
        }
        const auto& character = found->second;
        out.push_back(character.attribute_state);
        for (const auto id : std::initializer_list<uint16_t>{CharProfessions, CharPlayerUnlockedProfession, CharSkillbar}) {
            if (const auto latest = character.latest.find(id); latest != character.latest.end()) {
                out.push_back(latest->second);
            }
        }
        if (const auto status = instance.find({CharHeroSkillStatusBitmap, agent_id}); status != instance.end() && emitted.insert(status->first).second) {
            out.push_back(status->second);
        }
        if (const auto agent = agents.find(agent_id); agent != agents.end()) {
            JoinRows(agent_id, agent->second, true, out, emitted);
        }
        if (const auto attributes = character.latest.find(CharAttributes); attributes != character.latest.end()) {
            out.push_back(attributes->second);
        }
        for (const auto& update : character.attribute_updates | std::views::values) {
            out.push_back(update);
        }
    }

    std::vector<Message> World::Join() const
    {
        std::vector<Message> out;
        std::set<Key> emitted;
        const auto reported = ReportedAgent();
        const auto controlled = ControlledAgent();
        const auto left_out = LeftOutAgent();
        const auto alive = [this, left_out](const uint32_t agent_id) {
            return Alive(agent_id) && agent_id != left_out;
        };
        const auto has_character = [this](const uint32_t agent_id) {
            const auto found = characters.find(agent_id);
            return found != characters.end() && found->second.attribute_state.id;
        };
        const auto emit_entry = [&](const int section) {
            for (const auto& message : entry) {
                if (EntrySection(message.id) != section) {
                    continue;
                }
                out.push_back(message);
            }
        };
        const auto emit_instance = [&](const uint16_t id) {
            for (auto found = instance.lower_bound({id}); found != instance.end() && found->first[0] == id; ++found) {
                if (emitted.insert(found->first).second) {
                    out.push_back(found->second);
                }
            }
        };
        const auto emit_party_member = [&](const uint16_t id, const uint32_t party) {
            for (auto found = instance.lower_bound({id, party}); found != instance.end() && found->first[0] == id && found->first[1] == party; ++found) {
                if (emitted.insert(found->first).second) {
                    out.push_back(found->second);
                }
            }
        };
        const auto emit_flags = [&](const uint32_t player) {
            if (const auto found = player_flags.find(player); found != player_flags.end() && emitted.insert({CharPlayerRowFlags, player}).second) {
                out.push_back(Make(CharPlayerRowFlags, {player, found->second.value, found->second.mask}));
            }
        };
        const auto emit_upkeeps = [&](const bool characters_only) {
            for (const auto& [agent_id, agent] : agents) {
                if (!alive(agent_id) && !(characters_only && has_character(agent_id))) {
                    continue;
                }
                for (const auto& [key, message] : agent.slots) {
                    const auto target = message.U(1);
                    if (!IsUpkeep(key[0]) || (characters_only && !(has_character(agent_id) && (!target || has_character(target)))) || !emitted.insert({key[0], agent_id, key[1]}).second) {
                        continue;
                    }
                    out.push_back(message);
                    if (!characters_only && !alive(target)) {
                        out.back().Set(1, 0);
                    }
                }
            }
        };
        std::set<uint32_t> hero_inventories;
        std::vector<std::pair<uint32_t, uint32_t>> heroes;
        for (auto found = instance.lower_bound({CharHeroActivate}); found != instance.end() && found->first[0] == CharHeroActivate; ++found) {
            hero_inventories.insert(found->second.U(2));
            heroes.emplace_back(found->second.U(1), found->first[1]);
        }
        std::ranges::sort(heroes);

        emit_entry(0);
        for (auto found = instance.lower_bound({ItemInventoryCreate}); found != instance.end() && found->first[0] == ItemInventoryCreate; ++found) {
            if (!hero_inventories.contains(found->first[1])) {
                JoinInventory(found->first[1], out, emitted);
            }
        }
        std::set<uint32_t> quests;
        for (const auto& key : instance | std::views::keys) {
            if (Contains(quest_ids, key[0])) {
                quests.insert(key[1]);
            }
        }
        for (const auto quest : quests) {
            for (const auto id : quest_ids) {
                if (const auto found = instance.find({id, quest}); found != instance.end() && emitted.insert(found->first).second) {
                    out.push_back(found->second);
                }
            }
        }
        for (const auto id : std::initializer_list<uint16_t>{CharPlayerAttrMaxKurzick, CharPlayerAttrMaxLuxon, CharPlayerAttrMaxBalthazar, CharPlayerAttrMaxImperial, CharContextValue112, CharAccountNameData, CharToClient153}) {
            emit_instance(id);
        }
        emit_entry(2);
        emit_entry(1);
        emit_entry(3);
        for (const auto id : std::initializer_list<uint16_t>{MissionTeam, CharToClient91, CharPlayerTeam, CharToClient124, CharToClient125}) {
            emit_instance(id);
        }
        JoinCharacter(controlled, out, emitted);
        emit_instance(CharHeroDataAdd);
        emit_instance(CharMercenaryInfo);
        out.insert(out.end(), map_init.begin(), map_init.end());
        for (const auto id : std::initializer_list<uint16_t>{CharPrefs, CharUnlockedSkills, CharSkillUpdateSkillCount2, CharPlayerAttrSet, CharDisplay}) {
            emit_instance(id);
        }
        for (const auto& entry_message : entry) {
            if (entry_message.id == MissionInstanceInfo) {
                emit_flags(entry_message.U(0));
            }
        }
        emit_instance(CharTitleRankData);
        emit_instance(CharTitleTrackInfo);
        emit_instance(CharPointTable141);
        for (const auto& [agent_id, hero] : heroes) {
            if (const auto inventory = instance.find({CharHeroActivate, hero}); inventory != instance.end()) {
                JoinInventory(inventory->second.U(2), out, emitted);
            }
            JoinCharacter(agent_id, out, emitted);
            if (const auto activate = instance.find({CharHeroActivate, hero}); activate != instance.end() && emitted.insert(activate->first).second) {
                out.push_back(activate->second);
            }
        }
        for (const auto agent_id : characters | std::views::keys) {
            if (agent_id != left_out) {
                JoinCharacter(agent_id, out, emitted);
            }
        }
        emit_upkeeps(true);

        if (connected) {
            out.push_back(Make(AgentConnect, {clock}));
            for (const auto item_id : ShownItems()) {
                JoinItem(item_id, out, emitted);
            }
            const auto row_of = [this](const uint32_t agent_id) -> const Message* {
                for (const auto& row : players | std::views::values) {
                    if (row.U(1) == agent_id) {
                        return &row;
                    }
                }
                return nullptr;
            };
            if (controlled) {
                if (const auto row = row_of(controlled)) {
                    out.push_back(*row);
                }
                JoinAgent(controlled, agents.at(controlled), out, emitted);
            }
            for (const auto& [agent_id, agent] : agents) {
                if (agent.create.id && agent_id != reported && !row_of(agent_id)) {
                    JoinAgent(agent_id, agent, out, emitted);
                }
            }
            if (controlled) {
                out.push_back(reporter);
            }
            for (const auto& row : players | std::views::values) {
                const auto agent_id = row.U(1);
                if (reported && agent_id == reported) {
                    continue;
                }
                out.push_back(row);
                if (alive(agent_id)) {
                    JoinAgent(agent_id, agents.at(agent_id), out, emitted);
                }
            }
            for (const auto& [agent_id, agent] : agents) {
                if (!agent.create.id && agent_id != left_out) {
                    JoinRows(agent_id, agent, false, out, emitted);
                }
            }
            emit_upkeeps(false);
            for (const auto& [agent_id, agent] : agents) {
                if (alive(agent_id) && agent.move.id) {
                    out.push_back(agent.move);
                    if (agent.move.id == AgentUpdateMoveTarget && !alive(agent.move.U(4))) {
                        out.back().Set(4, 0);
                    }
                }
            }
        }

        for (const auto player : player_flags | std::views::keys) {
            emit_flags(player);
        }
        for (const auto id : std::initializer_list<uint16_t>{
                 CharUpdateAgentPartysize, CharPlayerLink, CharTitleRankDisplay, GuildAllianceInfo, GuildGeneralInfo, GuildAssign, GuildToClient284, GuildToClient302, CharPostProcess, GadgetClassDefine, CharNpcProperties, CharNpcModel, PropLayers,
                 PropAnimate
             }) {
            emit_instance(id);
        }
        for (auto found = instance.lower_bound({PartyMembershipBegin}); found != instance.end() && found->first[0] == PartyMembershipBegin; ++found) {
            const auto party = found->first[1];
            if (emitted.insert(found->first).second) {
                out.push_back(found->second);
            }
            emit_party_member(PartyMemberAdd, party);
            emit_party_member(PartyHeroAdd, party);
            emit_party_member(PartyMembershipEnd, party);
        }
        for (const auto id : std::initializer_list<uint16_t>{PartyMemberAdd, PartyHeroAdd, PartyMembershipEnd, PartyAssign, PartySetDifficulty, PartyToClient446, PartyLock}) {
            emit_instance(id);
        }
        emit_entry(4);
        for (const auto& [key, message] : instance) {
            if (emitted.insert(key).second) {
                out.push_back(message);
            }
        }
        return out;
    }

} // namespace ReplayState
