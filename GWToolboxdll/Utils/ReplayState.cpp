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
            CharAttackInterval = 53,
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
            CharPostProcess = 107,
            CharNpcUpdateWeapons = 109,
            CharEquipment = 110,
            CharEquipUpdate = 111,
            CharContextValue112 = 112,
            CharPointTable141 = 141,
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
            CharMissionStreamStart = 186,
            CharAgentInitialEffects = 240,
            CharAgentUpdateEffects = 241,
            CharPlayerTeam = 242,
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
            ItemCustomize = 314,
            ItemInventoryAdd = 318,
            ItemInventoryEquipSet = 327,
            ItemProfession = 346,
            ItemReportDetailLow = 350,
            ItemReportDetailAll = 353,
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

        constexpr uint16_t state_ids[] = {
            30, 31, 32, 33, 34, 36, 37, 38, 39, 40, 41, 42, 43, 44, 45, 46, 47, 53, 60, 62, 63, 64, 65, 66, 68, 72, 73, 76, 77, 80, 81, 82, 83,
            84, 86, 87, 89, 90, 107, 109, 110, 111, 112, 141, 154, 155, 156, 159, 162, 163, 166, 170, 174, 176, 177, 186, 240, 241, 242, 263,
            270, 273, 277, 280, 284, 288, 289, 302, 309, 314, 346, 350, 353, 366, 380, 381, 390, 398, 404, 406, 407, 408, 409, 410, 428, 430,
            435, 446, 447, 451, 460, 467, 468, 474,
        };
        constexpr uint16_t transient_ids[] = {12, 13, 35, 52, 61, 69, 70, 71, 93, 94, 96, 97, 160, 161, 164, 165, 226, 227, 228, 345};
        constexpr uint16_t private_ids[] = {
            15, 48, 49, 50, 51, 54, 55, 56, 57, 58, 59, 75, 78, 92, 101, 115, 116, 124, 125, 126, 128, 129, 131, 132, 138, 139, 140, 181, 182,
            183, 195, 196, 197, 202, 203, 204, 218, 219, 221, 224, 229, 230, 233, 234, 235, 236, 237, 238, 239, 243, 244, 245, 246, 313, 318,
            319, 320, 321, 324, 327, 328, 331, 333, 335, 337, 479, 480, 481,
        };

        constexpr uint32_t class_kind_mask = 0xF0000000;
        constexpr uint32_t npc_class = 0x20000000;
        constexpr uint32_t gadget_def = 2;
        constexpr uint32_t item_def = 4;
        constexpr size_t observer_field = 5;
        constexpr size_t equipment_slots = 9;
        constexpr uint16_t quest_ids[] = {CharQuestAdd, CharQuestGeneralInfo, CharQuestUpdateName, CharQuestDescription, CharQuestAddMarker, CharQuestUpdateMarker, CharQuestEarned};
        constexpr uint16_t item_ids[] = {ItemReportDetailAll, ItemReportDetailLow, ItemProfession, ItemCustomize, ItemAssign};
        constexpr uint16_t row_ids[] = {CharAgentUpdateScale, CharAgentUpdateNpcName, CharNotifyDword, CharAgentUpdateProfession, CharAgentAllyAdd, CharAgentInitialEffects, CharAgentUpdateEffects};

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
            return Contains(std::initializer_list<uint32_t>{Marker, ModelsStopped, Guild, Level, MaxEnergy, MaxHealth, Unlurk}, kind);
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
                case GuildToClient284:
                case GuildToClient302:
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
            const std::vector<uint8_t>& body;
            Message& out;
            size_t offset = 2;
            bool ok = true;

            uint32_t Take(const size_t size)
            {
                if (offset + size > body.size()) {
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

            const Field* Next()
            {
                return next < message.fields.size() ? &message.fields[next++] : nullptr;
            }

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

        void Describe(std::string& out, const Message& message, const bool full, const size_t skipped_field = SIZE_MAX)
        {
            out += std::format("{}", message.id);
            for (size_t i = 0; i < message.fields.size(); i++) {
                if (i == skipped_field) {
                    out += " -";
                    continue;
                }
                const auto& field = message.fields[i];
                out += " ";
                if (!full) {
                    out += field.text.empty() ? std::format("n{}", field.numbers.size()) : std::format("t{}", field.text.size());
                    continue;
                }
                for (size_t j = 0; j < field.numbers.size(); j++) {
                    out += std::format("{}{:x}", j ? "," : "", field.numbers[j]);
                }
                if (!field.text.empty() || field.numbers.empty()) {
                    out += "'";
                    for (const auto c : field.text) {
                        out += std::format("{:04x}", static_cast<uint16_t>(c));
                    }
                }
            }
            out += "\n";
        }
    }

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

    bool Decode(const ReplayFile::MessageTable& table, const std::vector<uint8_t>& body, Message& out)
    {
        out = {};
        if (body.size() < 2) {
            return false;
        }
        out.id = static_cast<uint16_t>(body[0] | body[1] << 8);
        if (out.id >= table.size() || table[out.id].empty()) {
            return false;
        }
        Decoder decoder{body, out};
        const auto& descriptors = table[out.id];
        return decoder.Walk(descriptors.data() + 1, descriptors.size() - 1) && decoder.offset == body.size();
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

    std::vector<uint32_t> NamedAgents(const ReplayFile::MessageTable& table, const Message& message)
    {
        std::vector<uint32_t> named;
        const auto add = [&named](const uint32_t agent) {
            if (agent) {
                named.push_back(agent);
            }
        };
        if (IsAgentClient(message.id)) {
            if (message.id != AgentAdvance && message.id != AgentConnect) {
                add(message.U(0));
            }
            if (message.id == AgentUpdateMoveTarget) {
                add(message.U(4));
            }
            if (message.id == AgentCreate) {
                add(message.U(20));
            }
            return named;
        }
        if (message.id == GadgetState) {
            add(message.U(0));
            return named;
        }
        if (message.id == GadgetClassDefine || message.id >= table.size()) {
            return named;
        }
        const auto& descriptors = table[message.id];
        size_t field = 0;
        for (size_t i = 1; i < descriptors.size() && field < message.fields.size(); i++) {
            const auto type = descriptors[i] & 0xF;
            if (type == 6 || type == 10) {
                continue;
            }
            if (type == 12) {
                break;
            }
            if (type == 0 && (descriptors[i] >> 4 & 0xF) == 1) {
                add(message.U(field));
            }
            field++;
        }
        return named;
    }

    Kind KindOf(const uint16_t message_id)
    {
        if (Contains(state_ids, message_id)) {
            return Kind::State;
        }
        if (Contains(transient_ids, message_id)) {
            return Kind::Transient;
        }
        if (Contains(private_ids, message_id)) {
            return Kind::Private;
        }
        return Kind::Unknown;
    }

    const char* KindName(const Kind kind)
    {
        switch (kind) {
            case Kind::State:
                return "state";
            case Kind::Transient:
                return "transient";
            case Kind::Private:
                return "private";
            default:
                return "unknown";
        }
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
                if (message.U(CreateDef) == item_def) {
                    shown_items.insert(message.U(CreateClass));
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
                return;
            case CharPlayerRowFlags: {
                auto& flags = player_flags[message.U(0)];
                flags = (flags & ~message.U(2)) | message.U(1);
                return;
            }
            case CharQuestRemove:
                for (const auto id : quest_ids) {
                    instance.erase({id, message.U(0)});
                }
                return;
            case ItemInventoryEquipSet:
                shown_items.insert({message.U(2), message.U(3)});
                return;
            case ItemInventoryAdd:
                inventory_items.insert(message.U(1));
                return;
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
        if (agent_field < 0 || KindOf(message.id) != Kind::State) {
            return;
        }
        auto found = agents.find(message.U(agent_field));
        if (found == agents.end() || !found->second.create.id) {
            if (!IsRow(message)) {
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
                    shown_items.insert(message.U(slot + 1));
                }
                agent.equipped = true;
                return;
            case CharEquipUpdate:
                if (message.U(1) < equipment_slots) {
                    agent.equipment[message.U(1)] = message.U(2);
                    agent.equipped = true;
                    shown_items.insert(message.U(2));
                }
                return;
            case CharNpcUpdateWeapons:
                agent.slots[{message.id}] = message;
                shown_items.insert({message.U(1), message.U(2)});
                return;
            case CharAgentInitialEffects:
            case CharAgentUpdateEffects:
                agent.slots[{CharAgentInitialEffects}] = Message{CharAgentInitialEffects, message.fields};
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
                it = agents.erase(it);
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

    std::set<uint32_t> World::ShownItems() const
    {
        std::set<uint32_t> shown;
        for (const auto& agent : agents | std::views::values) {
            if (!agent.create.id) {
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

    void World::JoinAgent(const Agent& agent, std::vector<Message>& out, std::set<Key>& emitted) const
    {
        const auto& create = agent.create;
        const auto define = [&](const Key& key) {
            if (const auto found = instance.find(key); found != instance.end() && emitted.insert(key).second) {
                out.push_back(found->second);
            }
        };
        const auto define_npc = [&](const uint32_t agent_class) {
            if ((agent_class & class_kind_mask) == npc_class) {
                define({CharNpcProperties, agent_class & ~class_kind_mask});
                define({CharNpcModel, agent_class & ~class_kind_mask});
            }
        };
        if (create.U(CreateDef) == gadget_def) {
            define({GadgetClassDefine, create.U(CreateClass)});
        }
        define_npc(create.U(CreateClass));
        if (const auto model = agent.slots.find({CharUpdateAgentModel}); model != agent.slots.end()) {
            define({CharNpcProperties, model->second.U(1)});
            define({CharNpcModel, model->second.U(1)});
        }
        if (create.U(CreateDef) == item_def) {
            JoinItem(create.U(CreateClass), out, emitted);
        }
        for (const auto& [key, message] : agent.slots) {
            if (IsRow(message)) {
                out.push_back(message);
            }
        }
        out.push_back(create);
        if (const auto weapons = agent.slots.find({CharNpcUpdateWeapons}); weapons != agent.slots.end()) {
            JoinItem(weapons->second.U(1), out, emitted);
            JoinItem(weapons->second.U(2), out, emitted);
        }
        if (agent.equipped) {
            auto equipment = Make(CharEquipment, {create.U(CreateAgent)});
            for (const auto item : agent.equipment) {
                JoinItem(item, out, emitted);
                equipment.fields.push_back({{item}, {}});
            }
            out.push_back(equipment);
        }
        for (const auto& [key, message] : agent.slots) {
            if (!IsRow(message) && !IsUpkeep(key[0])) {
                out.push_back(message);
            }
        }
        if (agent.health != 1.f) {
            out.push_back(Make(CharAgentAttrUpdateFloat, {Health, create.U(CreateAgent), std::bit_cast<uint32_t>(agent.health)}));
        }
        if (agent.energy != 1.f) {
            out.push_back(Make(CharAgentAttrUpdateFloat, {Energy, create.U(CreateAgent), std::bit_cast<uint32_t>(agent.energy)}));
        }
        for (const auto& effect : agent.effects | std::views::values) {
            auto applied = effect.applied;
            if (effect.expiry) {
                applied.SetF(4, static_cast<float>(effect.expiry - clock) / 1000.f);
            }
            out.push_back(applied);
        }
    }

    std::vector<Message> World::Join() const
    {
        std::vector<Message> out;
        std::set<Key> emitted;
        const auto alive = [this](const uint32_t agent_id) {
            const auto found = agents.find(agent_id);
            return agent_id && found != agents.end() && found->second.create.id;
        };
        const auto emit_entry = [&](const int section) {
            for (const auto& message : entry) {
                if (EntrySection(message.id) != section) {
                    continue;
                }
                out.push_back(message);
                if (message.id == MissionInstanceInfo) {
                    out.back().Set(observer_field, 1);
                }
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

        emit_entry(0);
        emit_entry(1);
        emit_entry(2);
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
        emit_entry(3);
        emit_instance(MissionTeam);
        emit_instance(CharPlayerTeam);

        if (connected) {
            out.push_back(Make(AgentConnect, {clock}));
            for (const auto item_id : items | std::views::keys) {
                if (shown_items.contains(item_id) || !inventory_items.contains(item_id)) {
                    JoinItem(item_id, out, emitted);
                }
            }
            const auto controlled = reporter.id && alive(reporter.U(0)) ? reporter.U(0) : 0;
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
                JoinAgent(agents.at(controlled), out, emitted);
            }
            for (const auto& [agent_id, agent] : agents) {
                if (agent.create.id && agent_id != controlled && !row_of(agent_id)) {
                    JoinAgent(agent, out, emitted);
                }
            }
            if (controlled) {
                out.push_back(reporter);
            }
            for (const auto& row : players | std::views::values) {
                const auto agent_id = row.U(1);
                if (controlled && agent_id == controlled) {
                    continue;
                }
                out.push_back(row);
                if (alive(agent_id)) {
                    JoinAgent(agents.at(agent_id), out, emitted);
                }
            }
            for (const auto& agent : agents | std::views::values) {
                if (!agent.create.id) {
                    continue;
                }
                for (const auto& [key, message] : agent.slots) {
                    if (IsUpkeep(key[0])) {
                        out.push_back(message);
                        if (!alive(message.U(1))) {
                            out.back().Set(1, 0);
                        }
                    }
                }
                if (agent.move.id) {
                    out.push_back(agent.move);
                    if (agent.move.id == AgentUpdateMoveTarget && !alive(agent.move.U(4))) {
                        out.back().Set(4, 0);
                    }
                }
            }
        }

        for (const auto& [player, flags] : player_flags) {
            out.push_back(Make(CharPlayerRowFlags, {player, flags, ~flags}));
        }
        for (const auto id : {CharUpdateAgentPartysize, CharPlayerLink, CharPointTable141, GuildAssign, GuildAllianceInfo, GuildGeneralInfo, GuildToClient284,
                              GuildToClient302, CharPostProcess, CharContextValue112, GadgetClassDefine, CharNpcProperties, CharNpcModel, PropAnimate, PropLayers}) {
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

    std::string World::Dump(const bool full) const
    {
        const auto alive = [this](const uint32_t agent_id) {
            const auto found = agents.find(agent_id);
            return agent_id && found != agents.end() && found->second.create.id ? agent_id : 0;
        };
        std::string out = std::format("clock {} connected {}\n", full ? clock : 0, connected);
        for (auto section = 0; section <= 4; section++) {
            for (const auto& message : entry) {
                if (EntrySection(message.id) == section) {
                    out += "entry ";
                    Describe(out, message, full);
                }
            }
        }
        for (const auto& message : instance | std::views::values) {
            out += "instance ";
            Describe(out, message, full);
        }
        for (const auto& [player, flags] : player_flags) {
            out += full ? std::format("player flags {} {:x}\n", player, flags) : std::format("player flags {}\n", player);
        }
        for (const auto& row : players | std::views::values) {
            out += "player ";
            Describe(out, row, full);
        }
        if (reporter.id && alive(reporter.U(0))) {
            out += "reporter ";
            Describe(out, reporter, full);
        }
        for (const auto& [agent_id, agent] : agents) {
            if (!agent.create.id) {
                continue;
            }
            out += std::format("agent {}\n create ", agent_id);
            Describe(out, agent.create, full, full ? SIZE_MAX : CreatePosition);
            if (agent.move.id) {
                auto move = agent.move;
                if (move.id == AgentUpdateMoveTarget) {
                    move.Set(4, alive(move.U(4)));
                }
                out += " move ";
                Describe(out, move, full);
            }
            if (full) {
                out += std::format(" health {:x} energy {:x}\n", std::bit_cast<uint32_t>(agent.health), std::bit_cast<uint32_t>(agent.energy));
            }
            if (agent.equipped) {
                out += " equipment";
                for (const auto item : agent.equipment) {
                    out += std::format(" {}", item);
                }
                out += "\n";
            }
            for (const auto& [key, message] : agent.slots) {
                auto slot = message;
                if (IsUpkeep(key[0])) {
                    slot.Set(1, alive(slot.U(1)));
                }
                out += " slot ";
                Describe(out, slot, full);
            }
            for (const auto& [effect_id, effect] : agent.effects) {
                out += full ? std::format(" effect {} until {} ", effect_id, effect.expiry) : std::format(" effect {} ", effect_id);
                Describe(out, effect.applied, full, 4);
            }
        }
        for (const auto item_id : ShownItems()) {
            out += std::format("item {}\n", item_id);
            if (const auto found = items.find(item_id); found != items.end()) {
                for (const auto& message : found->second | std::views::values) {
                    out += " ";
                    Describe(out, message, full);
                }
            }
        }
        return out;
    }

    std::vector<Keyframe> Keyframes(const ReplayFile::Contents& contents, const uint32_t interval_ms)
    {
        std::vector<Keyframe> keyframes;
        World world(contents.table);
        auto next = interval_ms;
        for (size_t i = 0; i < contents.records.size(); i++) {
            const auto& record = contents.records[i];
            if (interval_ms && record.time_ms >= next && record.MessageId() == AgentAdvance) {
                keyframes.push_back({i, record.time_ms, world});
                next = (record.time_ms / interval_ms + 1) * interval_ms;
            }
            world.Apply(record);
        }
        return keyframes;
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

    std::vector<ReplayFile::Record> Seek(const ReplayFile::Contents& contents, const std::vector<Keyframe>& keyframes, const uint32_t time_ms)
    {
        const auto after = std::ranges::upper_bound(keyframes, time_ms, {}, &Keyframe::time_ms);
        std::vector<ReplayFile::Record> records;
        size_t start = 0;
        if (after != keyframes.begin()) {
            const auto& keyframe = *std::prev(after);
            records = JoinRecords(contents.table, keyframe.world, keyframe.time_ms);
            start = keyframe.record_index;
        }
        for (auto i = start; i < contents.records.size() && contents.records[i].time_ms <= time_ms; i++) {
            records.push_back(contents.records[i]);
        }
        return records;
    }

    JoinCheck CheckJoin(const ReplayFile::Contents& contents)
    {
        JoinCheck check;
        const auto fail = [&check](const size_t index, const uint16_t id, const std::string_view what) {
            check.failures.push_back(std::format("record {} (message {}): {}", index, id, what));
        };
        std::vector<Message> messages(contents.records.size());
        size_t connect_index = SIZE_MAX;
        check.join_records = contents.records.size();
        for (size_t i = 0; i < contents.records.size(); i++) {
            std::vector<uint8_t> encoded;
            if (!Decode(contents.table, contents.records[i].body, messages[i])) {
                fail(i, contents.records[i].MessageId(), "does not decode against the file's table");
            }
            else if (!Encode(contents.table, messages[i], encoded) || encoded != contents.records[i].body) {
                fail(i, messages[i].id, "does not encode back to its bytes");
            }
            if (messages[i].id == AgentConnect && connect_index == SIZE_MAX) {
                connect_index = i;
            }
            if (messages[i].id == AgentAdvance && connect_index != SIZE_MAX && check.join_records == contents.records.size()) {
                check.join_records = i;
            }
        }
        bool instance_info = false;
        bool manifest_phase = false;
        std::map<uint32_t, uint32_t> alive_teams;
        std::set<uint32_t> player_agents;
        uint32_t player_team = 0;
        bool player_team_known = false;
        for (size_t i = 0; i < messages.size(); i++) {
            const auto& message = messages[i];
            const auto id = message.id;
            if (id == MissionInstanceInfo) {
                instance_info = true;
            }
            if (id == MissionManifestPhase) {
                manifest_phase = true;
            }
            if (id == MissionManifestComplete && !instance_info) {
                fail(i, id, "408 before 410");
            }
            if (id == MissionManifestChunk && !manifest_phase) {
                fail(i, id, "407 before 409");
            }
            if (IsAgentClient(id) && id != AgentAdvance && id != AgentConnect && i < connect_index) {
                fail(i, id, "an agent message before 31");
            }
            if (id == CharPlayerTeam) {
                player_team = message.U(0);
                player_team_known = true;
            }
            for (const auto agent_id : NamedAgents(contents.table, message)) {
                if (alive_teams.contains(agent_id) || (id == AgentCreate && agent_id == message.U(CreateAgent))) {
                    continue;
                }
                const auto next_create = std::find_if(messages.begin() + static_cast<std::ptrdiff_t>(i) + 1, messages.end(), [](const Message& m) {
                    return m.id == AgentCreate;
                });
                const auto own_row = (IsRow(message) || id == CharPlayerCreate) && next_create != messages.end() && next_create->U(CreateAgent) == agent_id;
                if (own_row) {
                    continue;
                }
                if (id == CharPlayerCreate) {
                    check.player_rows_without_agent++;
                }
                else if (i < check.join_records || IsAgentClient(id)) {
                    fail(i, id, std::format("names agent {} before its creation", agent_id));
                }
                else {
                    check.stream_rows_before_creation++;
                }
            }
            switch (id) {
                case AgentCreate:
                    alive_teams[message.U(CreateAgent)] = message.U(CreateTeam);
                    break;
                case AgentDestroy:
                    alive_teams.erase(message.U(0));
                    break;
                case AgentUpdateTeamToken:
                    if (alive_teams.contains(message.U(0))) {
                        alive_teams[message.U(0)] = message.U(1);
                    }
                    break;
                case CharPlayerCreate:
                    player_agents.insert(message.U(1));
                    break;
                case AgentReporter: {
                    const auto agent_id = message.U(0);
                    if (!agent_id) {
                        fail(i, id, "a reporter for agent 0 clears the agents");
                    }
                    else if (!player_agents.contains(agent_id) || !alive_teams.contains(agent_id)) {
                        fail(i, id, "the reporter's agent was not created as a player first");
                    }
                    else if (!player_team_known) {
                        fail(i, id, "34 before 242");
                    }
                    else if (alive_teams[agent_id] != player_team) {
                        fail(i, id, "the reporter's agent's team token is not 242's");
                    }
                    break;
                }
                default:
                    break;
            }
        }
        return check;
    }
}
