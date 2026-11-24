#include <Utils/ReplayState.h>

#include <algorithm>
#include <bit>
#include <cmath>
#include <format>
#include <initializer_list>
#include <iterator>
#include <ranges>

namespace ReplayState {
    namespace {
        enum MessageId : uint16_t {
            AgentAdvance = 30,
            AgentConnect = 31,
            AgentCreate = 32,
            AgentDestroy = 33,
            AgentReporter = 34,
            AgentUpdateClass = 36,
            AgentUpdateMaxSpeed = 39,
            AgentUpdateMoveAbort = 40,
            AgentUpdateMovePath = 41,
            AgentUpdateMoveTarget = 42,
            AgentUpdateMoveType = 43,
            AgentUpdatePoint = 44,
            AgentUpdateRotateAbort = 45,
            AgentUpdateRotation = 46,
            AgentUpdateTeamToken = 47,
            AccountCurrency = 15,
            CharAccountNameData = 48,
            CharAttackInterval = 53,
            CharAttributeState = 55,
            CharAttribPointsAvail = 56,
            CharAttribPointsTotal = 57,
            CharAttributes = 58,
            CharAgentUpdateAttribute = 59,
            CharPlayerRowFlags = 60,
            CharAgentAllyDestroy = 62,
            CharEffectUpkeepAdded = 63,
            CharEffectUpkeepRemoved = 64,
            CharEffectUpkeepApplied = 65,
            CharEffectApplied = 66,
            CharEffectRemoved = 68,
            CharAgentDisplayCape = 72,
            CharQuestAdd = 73,
            CharQuestDescription = 76,
            CharQuestEarned = 77,
            CharQuestGeneralInfo = 80,
            CharQuestUpdateMarker = 81,
            CharQuestRemove = 82,
            CharQuestAddMarker = 83,
            CharQuestUpdateName = 84,
            CharNpcProperties = 86,
            CharNpcModel = 87,
            CharPlayerCreate = 89,
            CharAgentDestroyPlayer = 90,
            CharToClient91 = 91,
            CharHeroSkillStatusBitmap = 101,
            CharPostProcess = 107,
            CharHeroActivate = 114,
            CharHeroDataAdd = 115,
            CharMercenaryInfo = 116,
            CharNpcUpdateWeapons = 109,
            CharEquipment = 110,
            CharEquipUpdate = 111,
            CharContextValue112 = 112,
            CharToClient124 = 124,
            CharToClient125 = 125,
            CharMapInit = 138,
            CharMapInitStart = 139,
            CharPointTable141 = 141,
            CharToClient153 = 153,
            CharAgentUpdateScale = 154,
            CharAgentUpdateNpcName = 155,
            CharAgentMorale = 156,
            CharNotifyDword = 159,
            CharAgentAttrUpdateFloat = 162,
            CharAgentAttrUpdateFloatTarget = 163,
            CharAgentUpdateProfession = 166,
            CharAgentAllyAdd = 170,
            CharUpdateAgentModel = 174,
            CharUpdateAgentPartysize = 176,
            CharPlayerLink = 177,
            CharPlayerUnlockedProfession = 182,
            CharProfessions = 183,
            CharPrefs = 181,
            CharMissionStreamStart = 186,
            CharSkillbar = 218,
            CharUnlockedSkills = 219,
            CharSkillUpdateSkillCount2 = 221,
            CharPlayerAttrSet = 233,
            CharPlayerAttrMaxKurzick = 234,
            CharPlayerAttrMaxLuxon = 235,
            CharPlayerAttrMaxBalthazar = 236,
            CharPlayerAttrMaxImperial = 237,
            CharDisplay = 239,
            CharAgentInitialEffects = 240,
            CharAgentUpdateEffects = 241,
            CharPlayerTeam = 242,
            CharTitleRankData = 243,
            CharTitleRankDisplay = 244,
            CharTitleTrackInfo = 246,
            GadgetClassDefine = 263,
            PropAnimate = 270,
            PropLayers = 273,
            GadgetState = 277,
            GuildAssign = 280,
            GuildToClient284 = 284,
            GuildAllianceInfo = 288,
            GuildGeneralInfo = 289,
            GuildToClient302 = 302,
            ItemAssign = 309,
            ItemCount = 313,
            ItemCustomize = 314,
            ItemInventoryAdd = 318,
            ItemInventoryAddBag = 319,
            ItemInventoryAddGold = 320,
            ItemInventoryAddGoldAccount = 321,
            ItemInventoryCreate = 324,
            ItemInventoryEquipSet = 327,
            ItemInventoryEquipSetActivate = 328,
            ItemInventoryMove = 331,
            ItemInventoryRemove = 333,
            ItemInventoryRemoveGold = 335,
            ItemInventorySwap = 338,
            ItemProfession = 346,
            ItemReportDetailLow = 350,
            ItemReportDetailAll = 353,
            ItemReportDetailAllReset = 354,
            ItemToClient360 = 360,
            MissionAccessRights = 366,
            MissionInstanceLoadHead = 380,
            MissionInstanceLoadPlayerName = 381,
            MissionConfigEnd = 390,
            MissionInstanceLoadFinish = 398,
            MissionToClient404 = 404,
            MissionLoadMap = 406,
            MissionManifestChunk = 407,
            MissionManifestComplete = 408,
            MissionManifestPhase = 409,
            MissionInstanceInfo = 410,
            MissionReadyForMapSpawn = 428,
            MissionTeam = 430,
            PartyAssign = 435,
            PartyToClient446 = 446,
            PartySetDifficulty = 447,
            PartyHeroAdd = 451,
            PartyMemberAdd = 460,
            PartyMembershipBegin = 467,
            PartyMembershipEnd = 468,
            PartyLock = 474,
        };

        enum CreateField : size_t {
            CreateAgent = 0,
            CreateClass = 1,
            CreateDef = 2,
            CreatePosition = 4,
            CreatePlane = 5,
            CreateSpeed = 8,
            CreateMoveSpeed = 9,
            CreateTeam = 11,
        };

        enum Notice : uint32_t {
            CharacterNotice0 = 0,
            CharacterNotice14 = 14,
            CharacterNotice15 = 15,
            AuraAdded = 6,
            AuraRemoved = 7,
            Marker = 11,
            ModelsStopped = 12,
            Guild = 30,
            Level = 36,
            MaxEnergy = 41,
            MaxHealth = 42,
            Unlurk = 66,
        };

        enum AgentValue : uint32_t {
            Energy = 33,
            Health = 34,
            EnergyRegen = 43,
            HealthRegen = 44,
            EnergySpent = 62,
            Hit = 16,
            CriticalHit = 17,
            Damage = 18,
            IgnoringArmour = 55,
        };

        constexpr uint16_t bag_content_ids[] = {0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 15, 131, 132, 133, 134, 195, 196, 197, 202, 203, 204, 224, 318, 319, 320, 321, 322, 323, 326, 327, 329, 331, 332, 333, 334, 335, 336, 337, 338, 339, 341, 342};
        constexpr uint16_t transient_ids[] = {12, 13, 35, 52, 61, 69, 70, 71, 93, 94, 96, 97, 160, 161, 164, 165, 226, 227, 228, 345};
        constexpr uint32_t class_kind_mask = 0xF0000000;
        constexpr uint32_t npc_class = 0x20000000;
        constexpr uint32_t gadget_def = 2;
        constexpr uint32_t item_def = 4;
        constexpr size_t observer_field = 5;
        constexpr size_t equipment_slots = 9;
        constexpr uint16_t quest_ids[] = {CharQuestAdd, CharQuestGeneralInfo, CharQuestUpdateName, CharQuestDescription, CharQuestAddMarker, CharQuestUpdateMarker, CharQuestEarned};
        constexpr uint16_t item_ids[] = {ItemReportDetailAll, ItemReportDetailAllReset, ItemReportDetailLow, ItemProfession, ItemCustomize, ItemAssign};
        constexpr uint16_t attribute_state_users[] = {CharAttribPointsAvail, CharAttribPointsTotal, CharAttributes, CharAgentUpdateAttribute, CharProfessions};
        constexpr size_t skillbar_create_field = 3;
        constexpr size_t item_quantity_field = 10;
        constexpr IdField named_id_fields[] = {
            {60, 0, IdKind::Player},     {73, 0, IdKind::Quest},   {76, 0, IdKind::Quest},      {77, 0, IdKind::Quest},      {80, 0, IdKind::Quest},      {81, 0, IdKind::Quest},      {82, 0, IdKind::Quest},      {83, 0, IdKind::Quest},
            {84, 0, IdKind::Quest},      {66, 1, IdKind::Skill},   {86, 0, IdKind::Npc},        {87, 0, IdKind::Npc},        {89, 0, IdKind::Player},     {90, 0, IdKind::Player},     {109, 1, IdKind::Item},      {109, 2, IdKind::Item},
            {110, 1, IdKind::Item},      {110, 2, IdKind::Item},   {110, 3, IdKind::Item},      {110, 4, IdKind::Item},      {110, 5, IdKind::Item},      {110, 6, IdKind::Item},      {110, 7, IdKind::Item},      {110, 8, IdKind::Item},
            {110, 9, IdKind::Item},      {111, 2, IdKind::Item},   {114, 0, IdKind::Hero},      {114, 2, IdKind::Inventory}, {115, 0, IdKind::Hero},      {116, 0, IdKind::Hero},      {174, 1, IdKind::Npc},       {176, 0, IdKind::Player},
            {177, 0, IdKind::Player},    {177, 1, IdKind::Player}, {209, 1, IdKind::Skill},     {210, 1, IdKind::Skill},     {217, 2, IdKind::Skill},     {220, 0, IdKind::Skill},     {226, 1, IdKind::Skill},     {227, 1, IdKind::Skill},
            {228, 1, IdKind::Skill},     {229, 1, IdKind::Skill},  {230, 1, IdKind::Skill},     {270, 0, IdKind::Prop},      {271, 0, IdKind::Prop},      {272, 0, IdKind::Prop},      {273, 0, IdKind::Prop},      {280, 0, IdKind::Guild},
            {288, 0, IdKind::Guild},     {289, 0, IdKind::Guild},  {309, 0, IdKind::Item},      {313, 0, IdKind::Item},      {314, 0, IdKind::Item},      {318, 0, IdKind::Inventory}, {318, 1, IdKind::Item},      {318, 2, IdKind::Bag},
            {319, 0, IdKind::Inventory}, {319, 3, IdKind::Bag},    {320, 0, IdKind::Inventory}, {324, 0, IdKind::Inventory}, {327, 0, IdKind::Inventory}, {327, 2, IdKind::Item},      {327, 3, IdKind::Item},      {328, 0, IdKind::Inventory},
            {331, 0, IdKind::Inventory}, {331, 1, IdKind::Item},   {331, 2, IdKind::Bag},       {333, 0, IdKind::Inventory}, {333, 1, IdKind::Item},      {335, 0, IdKind::Inventory}, {338, 0, IdKind::Inventory}, {338, 1, IdKind::Item},
            {338, 2, IdKind::Item},      {345, 0, IdKind::Item},   {346, 0, IdKind::Item},      {350, 0, IdKind::Item},      {353, 0, IdKind::Item},      {354, 0, IdKind::Item},      {410, 0, IdKind::Player},    {435, 0, IdKind::Party},
            {460, 0, IdKind::Party},     {460, 1, IdKind::Player}, {465, 0, IdKind::Party},     {465, 1, IdKind::Player},    {467, 0, IdKind::Party},     {468, 0, IdKind::Party},
        };
        constexpr IdField id_bringers[] = {
            {32, 0, IdKind::Agent},  {55, 0, IdKind::Agent},  {89, 1, IdKind::Agent},  {154, 0, IdKind::Agent},  {155, 0, IdKind::Agent},  {159, 1, IdKind::Agent},  {160, 2, IdKind::Agent},     {167, 0, IdKind::Agent},
            {240, 0, IdKind::Agent}, {241, 0, IdKind::Agent}, {350, 0, IdKind::Item},  {353, 0, IdKind::Item},   {354, 0, IdKind::Item},   {319, 3, IdKind::Bag},    {324, 0, IdKind::Inventory}, {288, 0, IdKind::Guild},
            {289, 0, IdKind::Guild}, {467, 0, IdKind::Party}, {89, 0, IdKind::Player}, {176, 0, IdKind::Player}, {177, 0, IdKind::Player}, {410, 0, IdKind::Player}, {73, 0, IdKind::Quest},      {80, 0, IdKind::Quest},
            {116, 0, IdKind::Hero},  {115, 0, IdKind::Hero},  {86, 0, IdKind::Npc},    {273, 0, IdKind::Prop},   {66, 1, IdKind::Skill},   {226, 1, IdKind::Skill},  {228, 1, IdKind::Skill},
        };
        constexpr uint16_t row_ids[] = {CharAgentUpdateScale, CharAgentUpdateNpcName, CharNotifyDword, CharAgentUpdateProfession, CharAgentAllyAdd, CharAgentInitialEffects, CharAgentUpdateEffects, CharAgentMorale, CharAgentAttrUpdateFloat};
        constexpr uint16_t character_rows[] = {CharAgentUpdateScale, CharAgentUpdateNpcName, CharNotifyDword, CharAgentUpdateProfession, CharAgentUpdateEffects, CharAgentMorale, CharAgentAttrUpdateFloat};

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
            return Contains(std::initializer_list<uint32_t>{CharacterNotice0, CharacterNotice14, CharacterNotice15, Marker, ModelsStopped, Guild, Level, MaxEnergy, MaxHealth, Unlurk}, kind);
        }

        bool IsRow(const Message& message)
        {
            return Contains(row_ids, message.id) && (message.id != CharNotifyDword || IsStateNotice(message.U(0)));
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
                case CharAgentAllyAdd:
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
                    const auto type = descriptors[i] & 0xF;
                    const auto sub = descriptors[i] >> 4 & 0xF;
                    const auto size = descriptors[i] >> 8;
                    switch (type) {
                        case 0:
                        case 1:
                            Numbers(1, 4);
                            break;
                        case 2:
                            Numbers(2, 4);
                            break;
                        case 3:
                            Numbers(3, 4);
                            break;
                        case 4:
                        case 8:
                            if (size != 1 && size != 2 && size != 4) {
                                return false;
                            }
                            Numbers(1, size);
                            break;
                        case 5:
                        case 9:
                            Numbers(size, 1);
                            break;
                        case 6:
                        case 10:
                            break;
                        case 7: {
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
                        case 11: {
                            const auto items = Take(2);
                            if (sub > 2 || items > size) {
                                return false;
                            }
                            Numbers(items, size_t{1} << sub);
                            break;
                        }
                        case 12: {
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
                    const auto type = descriptors[i] & 0xF;
                    const auto sub = descriptors[i] >> 4 & 0xF;
                    const auto size = descriptors[i] >> 8;
                    switch (type) {
                        case 0:
                        case 1:
                            if (!Numbers(1, 4)) {
                                return false;
                            }
                            break;
                        case 2:
                            if (!Numbers(2, 4)) {
                                return false;
                            }
                            break;
                        case 3:
                            if (!Numbers(3, 4)) {
                                return false;
                            }
                            break;
                        case 4:
                        case 8:
                            if ((size != 1 && size != 2 && size != 4) || !Numbers(1, size)) {
                                return false;
                            }
                            break;
                        case 5:
                        case 9:
                            if (!Numbers(size, 1)) {
                                return false;
                            }
                            break;
                        case 6:
                        case 10:
                            break;
                        case 7: {
                            const auto field = Next();
                            if (!field || field->text.size() >= size || !Put(static_cast<uint32_t>(field->text.size()), 2)) {
                                return false;
                            }
                            for (const auto c : field->text) {
                                Put(static_cast<uint16_t>(c), 2);
                            }
                            break;
                        }
                        case 11: {
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
                        case 12: {
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
            const auto type = descriptors[i] & 0xF;
            const auto size = descriptors[i] >> 8;
            switch (type) {
                case 0:
                case 1:
                case 4:
                case 8:
                    message.fields.push_back({{0}, {}});
                    break;
                case 2:
                    message.fields.push_back({{0, 0}, {}});
                    break;
                case 3:
                    message.fields.push_back({{0, 0, 0}, {}});
                    break;
                case 5:
                case 9:
                    message.fields.push_back({std::vector<uint32_t>(size, 0), {}});
                    break;
                case 7:
                case 11:
                    message.fields.emplace_back();
                    break;
                case 12:
                    message.fields.push_back({{0}, {}});
                    return message;
                default:
                    break;
            }
        }
        return message;
    }

    const char* IdKindName(const IdKind kind)
    {
        switch (kind) {
            case IdKind::Agent:
                return "agent";
            case IdKind::Item:
                return "item";
            case IdKind::Bag:
                return "bag";
            case IdKind::Inventory:
                return "inventory";
            case IdKind::Guild:
                return "guild";
            case IdKind::Party:
                return "party";
            case IdKind::Player:
                return "player";
            case IdKind::Quest:
                return "quest";
            case IdKind::Hero:
                return "hero";
            case IdKind::Npc:
                return "npc";
            case IdKind::Prop:
                return "prop";
            case IdKind::Skill:
                return "skill";
        }
        return "?";
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
                const auto type = descriptors[i] & 0xF;
                if (type == 6 || type == 10) {
                    continue;
                }
                if (type == 12) {
                    break;
                }
                if (type == 0 && (descriptors[i] >> 4 & 0xF) == 1) {
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

    bool BringsIdsIn(const IdField& field)
    {
        return std::ranges::find(id_bringers, field) != std::ranges::end(id_bringers);
    }

    bool OrderChecked(const IdKind kind)
    {
        // Skill ids are the game's own data: what comes first in a load is a skill's use (228, then 227 to 230), not its making.
        return kind != IdKind::Skill;
    }

    std::string Describe(const OrderFailure& failure)
    {
        return std::format("record {} (message {}): its {} {} (field {}) comes before any message that brings it in", failure.record, failure.field.message, IdKindName(failure.field.kind), failure.id, failure.field.field);
    }

    std::vector<std::pair<IdField, uint32_t>> Order::Unbrought(const ReplayFile::MessageTable& table, const Message& message) const
    {
        std::vector<std::pair<IdField, uint32_t>> unbrought;
        std::set<std::pair<IdKind, uint32_t>> own;
        for (const auto& field : IdFields(table, message.id)) {
            if (field.field >= message.fields.size()) {
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
        return unbrought;
    }

    void Order::Bring(const ReplayFile::MessageTable& table, const Message& message)
    {
        for (const auto& field : IdFields(table, message.id)) {
            if (field.field < message.fields.size() && BringsIdsIn(field)) {
                for (const auto id : message.fields[field.field].numbers) {
                    if (id) {
                        brought.insert({field.kind, id});
                    }
                }
            }
        }
    }

    std::vector<OrderFailure> CheckOrder(const ReplayFile::MessageTable& table, const std::vector<Message>& messages)
    {
        std::vector<OrderFailure> failures;
        Order order;
        for (size_t i = 0; i < messages.size(); i++) {
            for (const auto& [field, id] : order.Unbrought(table, messages[i])) {
                failures.push_back({i, field, id});
            }
            order.Bring(table, messages[i]);
        }
        return failures;
    }

    std::vector<std::string> CheckClientNeeds(const std::vector<Message>& messages)
    {
        std::vector<std::string> failures;
        std::set<uint32_t> attribute_states;
        std::set<uint32_t> hero_data;
        std::set<uint32_t> active_heroes;
        const auto fail = [&failures](const size_t index, const uint16_t id, const std::string_view what) {
            failures.push_back(std::format("record {} (message {}): {}", index, id, what));
        };
        for (size_t i = 0; i < messages.size(); i++) {
            const auto& message = messages[i];
            const auto agent_id = message.U(0);
            if (message.id == CharAttributeState && !attribute_states.insert(agent_id).second) {
                fail(i, message.id, std::format("a second attribute state for agent {}: the client asserts !attribState", agent_id));
            }
            else if (Contains(attribute_state_users, message.id) && !attribute_states.contains(agent_id)) {
                fail(i, message.id, std::format("agent {} has no attribute state (55) yet: the client asserts attribState", agent_id));
            }
            else if (message.id == CharHeroDataAdd || message.id == CharMercenaryInfo) {
                hero_data.insert(message.U(0));
            }
            else if (message.id == CharHeroActivate) {
                if (!hero_data.contains(message.U(0))) {
                    fail(i, message.id, std::format("hero {} activated before its data (115, or 116 for a mercenary): the client asserts charHeroData", message.U(0)));
                }
                active_heroes.insert(message.U(1));
            }
            else if (message.id == AgentReporter && agent_id) {
                auto walked = active_heroes;
                walked.insert(agent_id);
                for (const auto walked_agent : walked) {
                    if (!attribute_states.contains(walked_agent)) {
                        fail(i, message.id, std::format("34 makes agent {} the controlled one, and agent {} has no attribute state (55): the skill list asserts attribState (ChCliAttrib.cpp:146)", agent_id, walked_agent));
                    }
                }
            }
        }
        return failures;
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

    bool InventoryFilter::Keep(const Message& message, std::vector<Message>& shown_before, std::vector<uint32_t>& unknown_items)
    {
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
            case CharMapInit:
                if (!map_init.empty()) {
                    map_init.push_back(message);
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
            items[message.U(0)][message.id] = message;
            return;
        }
        if (const auto key = InstanceKey(message); !key.empty()) {
            instance[key] = message;
            return;
        }
        const auto agent_field = AgentField(message.id);
        if (agent_field < 0 || Contains(transient_ids, message.id)) {
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
                return;
            case AgentUpdateMovePath:
            case AgentUpdateMoveTarget:
                agent.move = message;
                return;
            case AgentUpdatePoint:
                agent.create.Set(CreatePosition, message.U(1, 0), 0);
                agent.create.Set(CreatePosition, message.U(1, 1), 1);
                agent.create.Set(CreatePlane, message.U(2));
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
                agent.slots.erase({CharAgentAllyAdd});
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
            if (agent.move.id) {
                const auto max_speed = agent.slots.find({AgentUpdateMaxSpeed});
                const auto move_type = agent.slots.find({AgentUpdateMoveType});
                const auto speed = max_speed != agent.slots.end() ? max_speed->second.F(1) : agent.create.F(CreateSpeed);
                const auto modifier = move_type != agent.slots.end() ? move_type->second.F(1) : agent.create.F(CreateMoveSpeed);
                const auto step = speed * modifier * seconds;
                const auto x = agent.create.F(CreatePosition, 0);
                const auto y = agent.create.F(CreatePosition, 1);
                const auto dx = agent.move.F(1, 0) - x;
                const auto dy = agent.move.F(1, 1) - y;
                const auto distance = std::sqrt(dx * dx + dy * dy);
                if (step < distance) {
                    agent.create.SetF(CreatePosition, x + dx / distance * step, 0);
                    agent.create.SetF(CreatePosition, y + dy / distance * step, 1);
                }
                else {
                    agent.create.Set(CreatePosition, agent.move.U(1, 0), 0);
                    agent.create.Set(CreatePosition, agent.move.U(1, 1), 1);
                    agent.create.Set(CreatePlane, agent.move.U(2));
                    agent.move = {};
                }
            }
            ++it;
        }
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
        for (const auto id : {ItemInventoryCreate, ItemInventoryEquipSetActivate}) {
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
                if (!IsRow(message) || brings_agent(message) != bringers) {
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
        for (const auto id : {CharProfessions, CharPlayerUnlockedProfession, CharSkillbar}) {
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
        for (const auto id : {CharPlayerAttrMaxKurzick, CharPlayerAttrMaxLuxon, CharPlayerAttrMaxBalthazar, CharPlayerAttrMaxImperial, CharContextValue112, CharAccountNameData, CharToClient153}) {
            emit_instance(id);
        }
        emit_entry(2);
        emit_entry(1);
        emit_entry(3);
        for (const auto id : {MissionTeam, CharToClient91, CharPlayerTeam, CharToClient124, CharToClient125}) {
            emit_instance(id);
        }
        JoinCharacter(controlled, out, emitted);
        emit_instance(CharHeroDataAdd);
        emit_instance(CharMercenaryInfo);
        out.insert(out.end(), map_init.begin(), map_init.end());
        for (const auto id : {CharPrefs, CharUnlockedSkills, CharSkillUpdateSkillCount2, CharPlayerAttrSet, CharDisplay}) {
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
        for (const auto id :
             {CharUpdateAgentPartysize, CharPlayerLink, CharTitleRankDisplay, GuildAllianceInfo, GuildGeneralInfo, GuildAssign, GuildToClient284, GuildToClient302, CharPostProcess, GadgetClassDefine, CharNpcProperties, CharNpcModel, PropLayers,
              PropAnimate}) {
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
        for (const auto id : {PartyMemberAdd, PartyHeroAdd, PartyMembershipEnd, PartyAssign, PartySetDifficulty, PartyToClient446, PartyLock}) {
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

    std::vector<ReplayFile::Record> JoinRecords(const ReplayFile::MessageTable& table, const World& world, const uint32_t time_ms)
    {
        std::vector<ReplayFile::Record> records;
        for (const auto& message : world.Join()) {
            ReplayFile::Record record{time_ms, {}};
            if (!Encode(table, message, record.body)) {
                record.body = {static_cast<uint8_t>(message.id), static_cast<uint8_t>(message.id >> 8)};
            }
            records.push_back(std::move(record));
        }
        return records;
    }
} // namespace ReplayState
