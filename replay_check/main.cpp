#include <Utils/Replay/ReplayFile.h>
#include <Utils/Replay/ReplayMessages.h>
#include <Utils/Replay/ReplayObserve.h>
#include <Utils/Replay/ReplayState.h>
#include <Utils/Replay/ReplayWire.h>

#include <algorithm>
#include <bit>
#include <chrono>
#include <cstring>
#include <format>
#include <functional>
#include <iterator>
#include <limits>
#include <map>
#include <numeric>
#include <optional>
#include <random>
#include <ranges>
#include <set>
#include <string>
#include <tuple>
#include <vector>

namespace {
    using namespace ReplayMessages;

    constexpr uint16_t attribute_state_users[] = {CharAttribPointsAvail, CharAttribPointsTotal, CharAttributes, GAME_SMSG_AGENT_UPDATE_ATTRIBUTE, GAME_SMSG_PLAYER_UPDATE_PROFESSION};
    constexpr uint16_t agent_row_ids[] = {GAME_SMSG_AGENT_UPDATE_SCALE,           GAME_SMSG_AGENT_UPDATE_NPC_NAME,   GAME_SMSG_AGENT_ATTR_UPDATE_INT,
                                          GAME_SMSG_AGENT_ATTR_UPDATE_INT_TARGET, GAME_SMSG_AGENT_UPDATE_PROFESSION, GAME_SMSG_AGENT_CREATE_NPC,
                                          GAME_SMSG_AGENT_INITIAL_EFFECTS,        GAME_SMSG_AGENT_UPDATE_EFFECTS,    CharAgentMorale,
                                          GAME_SMSG_AGENT_ATTR_UPDATE_FLOAT};
    constexpr std::wstring_view numbered_placeholders[] = {L"Player ", L"Mercenary ", L"Guild ", L"G"};

    struct OrderFailure {
        size_t record = 0;
        ReplayState::IdField field;
        uint32_t id = 0;
    };

    const char* IdKindName(const ReplayState::IdKind kind)
    {
        switch (kind) {
            case ReplayState::IdKind::Agent:
                return "agent";
            case ReplayState::IdKind::Item:
                return "item";
            case ReplayState::IdKind::Bag:
                return "bag";
            case ReplayState::IdKind::Inventory:
                return "inventory";
            case ReplayState::IdKind::Guild:
                return "guild";
            case ReplayState::IdKind::Party:
                return "party";
            case ReplayState::IdKind::Player:
                return "player";
            case ReplayState::IdKind::Quest:
                return "quest";
            case ReplayState::IdKind::Hero:
                return "hero";
            case ReplayState::IdKind::Npc:
                return "npc";
            case ReplayState::IdKind::Prop:
                return "prop";
            case ReplayState::IdKind::Skill:
                return "skill";
        }
        return "?";
    }

    std::string Describe(const OrderFailure& failure)
    {
        return std::format("record {} (message {}): its {} {} (field {}) comes before any message that brings it in", failure.record, failure.field.message, IdKindName(failure.field.kind), failure.id, failure.field.field);
    }

    std::vector<OrderFailure> CheckOrder(const ReplayFile::MessageTable& table, const std::vector<ReplayState::Message>& messages)
    {
        std::vector<OrderFailure> failures;
        ReplayState::Order order;
        for (size_t i = 0; i < messages.size(); i++) {
            for (const auto& [field, id] : order.Unbrought(table, messages[i])) {
                failures.push_back({i, field, id});
            }
            order.Bring(table, messages[i]);
        }
        return failures;
    }

    std::vector<std::string> CheckClientNeeds(const std::vector<ReplayState::Message>& messages)
    {
        std::vector<std::string> failures;
        std::set<uint32_t> attribute_states;
        std::set<uint32_t> hero_data;
        std::set<uint32_t> active_heroes;
        ReplayState::ClientAgents agents;
        std::set<uint32_t> inventories;
        std::set<uint32_t> team_rows;
        std::optional<uint32_t> party_building;
        std::optional<uint32_t> controlled;
        bool camera_lost = false;
        bool account_name = false;
        const auto fail = [&failures](const size_t index, const uint16_t id, const std::string_view what) {
            failures.push_back(std::format("record {} (message {}): {}", index, id, what));
        };
        for (size_t i = 0; i < messages.size(); i++) {
            const auto& message = messages[i];
            const auto agent_id = message.U(0);
            if (const auto broken = ReplayState::FieldAssert(message); !broken.empty()) {
                fail(i, message.id, broken);
            }
            if (const auto broken = agents.Breaks(message); !broken.empty()) {
                fail(i, message.id, broken);
            }
            // The 33 of the agent the camera follows (34's) frees GmCam's controller, which only the next 34 makes again (AvApi's 0x10000008, then 0x10000022).
            camera_lost |= message.id == GAME_SMSG_AGENT_DESPAWNED && agent_id == controlled;
            if ((message.id == GAME_SMSG_EFFECT_REMOVED || message.id == GAME_SMSG_EFFECT_UPKEEP_REMOVED) && !agents.Created().contains(agent_id)) {
                fail(i, message.id, std::format("a removal for agent {}, which the client does not hold: its UI message reaches listeners of an agent gone (0x10000057 crashed GmCam)", agent_id));
            }
            else if (camera_lost && agent_id == controlled && (message.id == GAME_SMSG_EFFECT_APPLIED || message.id == GAME_SMSG_EFFECT_UPKEEP_APPLIED || message.id == GAME_SMSG_EFFECT_REMOVED)) {
                fail(i, message.id, std::format("a buff of controlled agent {} after its 33 and before a 34: GmCam's buff add (0x10000055) and remove (0x10000057) call its freed controller", agent_id));
            }
            if (message.id == CharAccountNameData && std::exchange(account_name, true)) {
                fail(i, message.id, "a second account name: the client asserts !context->acctNameData (ChCliApi.cpp:385)");
            }
            else if (message.id == GAME_SMSG_ITEM_STREAM_CREATE && !inventories.insert(agent_id).second) {
                fail(i, message.id, std::format("inventory {} created again: the client asserts !inventory (ItCliApi.cpp:2010)", agent_id));
            }
            else if (message.id == MissionTeam && !team_rows.insert(agent_id).second) {
                fail(i, message.id, std::format("team row {} created again: the client asserts !m_tableByPartyId[partyId] (MsnTeam.cpp:105)", agent_id));
            }
            else if (message.id == GAME_SMSG_PARTY_CREATE && std::exchange(party_building, agent_id)) {
                fail(i, message.id, "a party begun while another is: the client asserts !m_partyBuilding (PyCliParty.cpp:1228)");
            }
            else if (message.id == GAME_SMSG_PARTY_MEMBER_STREAM_END && std::exchange(party_building, std::nullopt) != agent_id) {
                fail(i, message.id, std::format("party {} ended with no 467 of its own open: the client asserts m_partyBuilding and its party id (PyCliParty.cpp:1237, 1238)", agent_id));
            }
            if (message.id == CharAttributeState && !attribute_states.insert(agent_id).second) {
                fail(i, message.id, std::format("a second attribute state for agent {}: the client asserts !attribState", agent_id));
            }
            else if (std::ranges::contains(attribute_state_users, message.id) && !attribute_states.contains(agent_id)) {
                fail(i, message.id, std::format("agent {} has no attribute state (55) yet: the client asserts attribState", agent_id));
            }
            else if (message.id == CharHeroDataAdd || message.id == GAME_SMSG_MERCENARY_INFO) {
                hero_data.insert(message.U(0));
            }
            else if (message.id == CharHeroActivate) {
                if (!hero_data.contains(message.U(0))) {
                    fail(i, message.id, std::format("hero {} activated before its data (115, or 116 for a mercenary): the client asserts charHeroData", message.U(0)));
                }
                active_heroes.insert(message.U(1));
            }
            else if (message.id == GAME_SMSG_AGENT_SET_PLAYER && agent_id) {
                controlled = agent_id;
                camera_lost = false;
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

    std::vector<ReplayFile::Record> JoinRecords(const ReplayFile::MessageTable& table, const ReplayState::World& world, const uint32_t time_ms)
    {
        std::vector<ReplayFile::Record> records;
        for (const auto& message : world.Join()) {
            ReplayFile::Record record{time_ms, {}};
            if (!ReplayState::Encode(table, message, record.body)) {
                record.body = {static_cast<uint8_t>(message.id), static_cast<uint8_t>(message.id >> 8)};
            }
            records.push_back(std::move(record));
        }
        return records;
    }
    std::vector<std::vector<uint8_t>> Split(const ReplayFile::MessageTable& table, const std::vector<uint8_t>& stream, std::string& error)
    {
        std::vector<std::vector<uint8_t>> bodies;
        size_t offset = 0;
        while (offset < stream.size()) {
            ReplayState::Message message;
            const auto used = ReplayState::DecodePrefix(table, stream.data() + offset, stream.size() - offset, message);
            if (!used) {
                error = std::format("the stream does not decode at byte {} (message {})", offset, message.id);
                return bodies;
            }
            bodies.emplace_back(stream.begin() + static_cast<std::ptrdiff_t>(offset), stream.begin() + static_cast<std::ptrdiff_t>(offset + used));
            offset += used;
        }
        return bodies;
    }
    bool IsPlaceholder(const std::wstring_view text)
    {
        return text == ReplayFile::own_name_placeholder || std::ranges::any_of(numbered_placeholders, [text](const std::wstring_view prefix) {
                   return text.size() > prefix.size() && text.starts_with(prefix) && std::ranges::all_of(text.substr(prefix.size()), [](const wchar_t c) {
                              return c >= L'0' && c <= L'9';
                          });
               });
    }

    void InitRc4(ReplayWire::Rc4& rc4, const uint8_t* key, const size_t size)
    {
        for (uint32_t k = 0; k < 256; k++) {
            rc4.s[k] = static_cast<uint8_t>(k);
        }
        uint8_t mixed = 0;
        for (uint32_t k = 0; k < 256; k++) {
            mixed = static_cast<uint8_t>(mixed + rc4.s[k] + key[k % size]);
            std::swap(rc4.s[k], rc4.s[mixed]);
        }
        rc4.i = 0;
        rc4.j = 0;
    }


    int failures = 0;

    void Check(const bool condition, const char* what)
    {
        if (!condition) {
            printf("FAILED: %s\n", what);
            failures++;
        }
    }

    void Put(std::vector<uint8_t>& out, const uint32_t value, const size_t size)
    {
        for (size_t i = 0; i < size; i++) {
            out.push_back(static_cast<uint8_t>(value >> (8 * i)));
        }
    }

    void PutText(std::vector<uint8_t>& out, const std::wstring& text)
    {
        Put(out, static_cast<uint32_t>(text.size()), 2);
        for (const auto c : text) {
            Put(out, c, 2);
        }
    }

    constexpr uint32_t Descriptor(const uint32_t type, const uint32_t sub, const uint32_t size)
    {
        return size << 8 | sub << 4 | type;
    }

    struct DecodedGroup {
        uint32_t flag;
        wchar_t name[4];
    };

    struct DecodedMessage {
        uint32_t header;
        uint32_t byte_value;
        uint32_t word_value;
        uint32_t dword_value;
        uint32_t agent_id;
        float speed;
        float position[2];
        float vector[3];
        uint8_t blob[3];
        wchar_t text[5];
        uint32_t after_text;
        uint32_t byte_count;
        uint8_t bytes[4];
        uint32_t after_bytes;
        uint32_t word_count;
        uint16_t words[3];
        uint32_t dword_count;
        uint32_t dwords[2];
        uint32_t group_count;
        DecodedGroup groups[3];
    };

    const std::vector<uint32_t> test_fields = {
        Descriptor(4, 0, 2),  Descriptor(4, 0, 1), Descriptor(4, 0, 2), Descriptor(4, 0, 4),  Descriptor(0, 1, 0), Descriptor(1, 0, 0),  Descriptor(2, 0, 0), Descriptor(3, 0, 0),  Descriptor(5, 0, 3), Descriptor(7, 1, 5), Descriptor(4, 0, 4),
        Descriptor(11, 0, 4), Descriptor(6, 0, 0), Descriptor(4, 0, 4), Descriptor(11, 1, 3), Descriptor(6, 0, 0), Descriptor(11, 2, 2), Descriptor(6, 0, 0), Descriptor(12, 0, 3), Descriptor(4, 0, 1), Descriptor(7, 1, 4),
    };

    DecodedMessage TestMessage()
    {
        DecodedMessage decoded{};
        decoded.header = 77;
        decoded.byte_value = 0xAB;
        decoded.word_value = 0xBEEF;
        decoded.dword_value = 0xDEADBEEF;
        decoded.agent_id = 1234;
        decoded.speed = 1.5f;
        decoded.position[0] = -2.0f;
        decoded.position[1] = 3.25f;
        decoded.vector[0] = 1.0f;
        decoded.vector[1] = 2.0f;
        decoded.vector[2] = 3.0f;
        memcpy(decoded.blob, "\x01\x02\x03", 3);
        wcscpy(decoded.text, L"Ann");
        decoded.after_text = 0x01020304;
        decoded.byte_count = 3;
        memcpy(decoded.bytes, "\x0A\x0B\x0C\xFF", 4);
        decoded.after_bytes = 0x05060708;
        decoded.word_count = 2;
        decoded.words[0] = 0x1111;
        decoded.words[1] = 0x2222;
        decoded.words[2] = 0xFFFF;
        decoded.dword_count = 1;
        decoded.dwords[0] = 0x33333333;
        decoded.dwords[1] = 0xFFFFFFFF;
        decoded.group_count = 2;
        decoded.groups[0] = {7, L"Bo"};
        decoded.groups[1] = {8, {0x107, L'C', 0x1, 0}};
        return decoded;
    }

    std::vector<uint8_t> ExpectedWire(const DecodedMessage& decoded, const std::wstring& text, const std::wstring& group0, const std::wstring& group1)
    {
        std::vector<uint8_t> out;
        const auto put_float = [&](const float value) {
            uint32_t bits;
            memcpy(&bits, &value, sizeof(bits));
            Put(out, bits, 4);
        };
        Put(out, decoded.header, 2);
        Put(out, decoded.byte_value, 1);
        Put(out, decoded.word_value, 2);
        Put(out, decoded.dword_value, 4);
        Put(out, decoded.agent_id, 4);
        put_float(decoded.speed);
        put_float(decoded.position[0]);
        put_float(decoded.position[1]);
        for (const auto value : decoded.vector) {
            put_float(value);
        }
        out.insert(out.end(), decoded.blob, decoded.blob + 3);
        PutText(out, text);
        Put(out, decoded.after_text, 4);
        Put(out, decoded.byte_count, 2);
        out.insert(out.end(), decoded.bytes, decoded.bytes + decoded.byte_count);
        Put(out, decoded.after_bytes, 4);
        Put(out, decoded.word_count, 2);
        Put(out, decoded.words[0], 2);
        Put(out, decoded.words[1], 2);
        Put(out, decoded.dword_count, 2);
        Put(out, decoded.dwords[0], 4);
        Put(out, decoded.group_count, 1);
        Put(out, decoded.groups[0].flag, 1);
        PutText(out, group0);
        Put(out, decoded.groups[1].flag, 1);
        PutText(out, group1);
        return out;
    }

    void TestPacking()
    {
        auto decoded = TestMessage();
        const auto packed = [&](const ReplayFile::TextFilter& filter, std::vector<uint8_t>& out) {
            return ReplayFile::PackMessage(decoded.header, test_fields.data(), static_cast<uint32_t>(test_fields.size()), reinterpret_cast<const uint8_t*>(&decoded), out, filter);
        };
        std::vector<uint8_t> out;
        Check(packed({}, out), "a message of every field type packs");
        Check(out == ExpectedWire(decoded, L"Ann", L"Bo", {0x107, L'C', 0x1}), "the packed message is its wire form");

        const ReplayFile::TextFilter rename = [](std::wstring& text, uint32_t) {
            if (text == L"Ann") {
                text = L"Pl 1";
            }
        };
        Check(packed(rename, out), "a filtered message packs");
        Check(out == ExpectedWire(decoded, L"Pl 1", L"Bo", {0x107, L'C', 0x1}), "the filter replaces the text it is given");

        const ReplayFile::TextFilter too_long = [](std::wstring& text, uint32_t) {
            text = L"much longer than the field";
        };
        Check(packed(too_long, out), "a filter's text longer than its field packs");
        Check(out == ExpectedWire(decoded, L"much", L"muc", L"muc"), "a filter's text is cut to its field");

        decoded.byte_count = 5;
        Check(!packed({}, out), "an array longer than its field is refused");
        decoded = TestMessage();
        wmemset(decoded.text, L'x', 5);
        Check(!packed({}, out), "a text without its terminator is refused");
        decoded = TestMessage();
        decoded.group_count = 4;
        Check(!packed({}, out), "more repetitions than the field holds are refused");

        const uint32_t unknown_type[] = {Descriptor(4, 0, 2), Descriptor(13, 0, 0)};
        Check(!ReplayFile::PackMessage(1, unknown_type, 2, reinterpret_cast<const uint8_t*>(&decoded), out, {}), "an unknown field type is refused");
        const uint32_t header_only[] = {Descriptor(4, 0, 2)};
        Check(ReplayFile::PackMessage(300, header_only, 1, reinterpret_cast<const uint8_t*>(&decoded), out, {}) && out == std::vector<uint8_t>{0x2C, 0x01}, "a message of no field is its id");
    }

    ReplayFile::MessageTable SyntheticTable(std::mt19937& random)
    {
        ReplayFile::MessageTable table(480);
        for (auto& descriptors : table) {
            descriptors.resize(1 + random() % 12);
            for (auto& descriptor : descriptors) {
                descriptor = random();
            }
        }
        return table;
    }

    std::vector<ReplayFile::Record> SyntheticRecords(std::mt19937& random, const size_t count, const uint16_t message_count)
    {
        std::vector<ReplayFile::Record> records(count);
        uint32_t time_ms = 0;
        for (auto& record : records) {
            time_ms += random() % 50;
            record.time_ms = time_ms;
            const auto message_id = static_cast<uint16_t>(random() % 7 ? random() % 40 : random() % message_count);
            record.body.resize(2 + random() % (random() % 100 ? 60 : 3000));
            record.body[0] = static_cast<uint8_t>(message_id);
            record.body[1] = static_cast<uint8_t>(message_id >> 8);
            for (size_t i = 2; i < record.body.size(); i++) {
                record.body[i] = static_cast<uint8_t>(random());
            }
        }
        return records;
    }

    ReplayFile::Header SyntheticHeader(const ReplayFile::MessageTable& table)
    {
        ReplayFile::Header header;
        header.client_build = 39112;
        header.table_digest = ReplayFile::TableDigest(table);
        header.start_time_unix_ms = 1795000000000;
        header.map_id = 148;
        header.map_region_type = 2;
        header.is_explorable = 1;
        header.district = 1;
        header.region = 2;
        header.language = 3;
        header.flags = ReplayFile::NamesStripped | ReplayFile::ChatDropped;
        strcpy(header.toolbox_version, "self-test");
        return header;
    }

    bool WriteRecording(const std::filesystem::path& path, const ReplayFile::Header& header, const ReplayFile::MessageTable& table, const std::vector<ReplayFile::Record>& records, const bool close)
    {
        ReplayFile::Writer writer;
        if (!writer.Open(path, header, table) || !writer.AppendChapter({0, 0, header.map_id, header.is_explorable, header.map_region_type})) {
            return false;
        }
        for (const auto& record : records) {
            if (!writer.Append(record.time_ms, record.body.data(), record.body.size())) {
                return false;
            }
        }
        return close ? writer.Close(header) : writer.Flush();
    }

    using ReplayState::Field;

    Field N(const uint32_t value)
    {
        return {{value}, {}};
    }

    Field Floats(std::initializer_list<float> values)
    {
        Field field;
        for (const auto value : values) {
            field.numbers.push_back(std::bit_cast<uint32_t>(value));
        }
        return field;
    }

    Field Bytes(const size_t count, const uint8_t value)
    {
        return {std::vector<uint32_t>(count, value), {}};
    }

    Field Text(const std::wstring& text)
    {
        return {{}, text};
    }

    const std::map<uint16_t, std::vector<uint32_t>> world_layouts = {
        {12, {}},
        {30, {0x404}},
        {31, {0x404}},
        {32, {0x404, 0x404, 0x104, 0x104, 0x12, 0x204, 0x2, 0x104, 0x21, 0x1, 0x404, 0x404, 0x404, 0x404, 0x404, 0x404, 0x404, 0x2, 0x12, 0x204, 0x404, 0x12, 0x204}},
        {33, {0x404}},
        {34, {0x404, 0x404}},
        {39, {0x404, 0x21}},
        {40, {0x404}},
        {41, {0x404, 0x12, 0x204, 0x204}},
        {42, {0x404, 0x12, 0x204, 0x204, 0x404}},
        {43, {0x404, 0x1, 0x104}},
        {44, {0x404, 0x12, 0x204}},
        {46, {0x404, 0x404, 0x404}},
        {47, {0x404, 0x404}},
        {53, {0x10, 0x404, 0x404}},
        {55, {0x10, 0x104, 0x104}},
        {56, {0x10, 0x104}},
        {57, {0x10, 0x104}},
        {58, {0x10, 0x302b, 0x6}},
        {59, {0x10, 0x104, 0x104, 0x104}},
        {62, {0x10}},
        {63, {0x10, 0x10, 0x204, 0x404, 0x404}},
        {64, {0x10, 0x404}},
        {65, {0x10, 0x10, 0x204, 0x404, 0x404}},
        {66, {0x10, 0x204, 0x404, 0x404, 0x404}},
        {68, {0x10, 0x404}},
        {89, {0x404, 0x10, 0x404, 0x104, 0x404, 0x404, 0x2017}},
        {90, {0x404}},
        {93, {0x7a17}},
        {110, {0x10, 0x404, 0x404, 0x404, 0x404, 0x404, 0x404, 0x404, 0x404, 0x404}},
        {111, {0x10, 0x404, 0x404}},
        {114, {0x204, 0x10, 0x404, 0x404}},
        {115, {0x204, 0x104, 0x104, 0x104, 0x404, 0x404, 0x82b, 0x6, 0x104, 0x104}},
        {154, {0x10, 0x404}},
        {155, {0x10, 0x2017}},
        {159, {0x404, 0x10, 0x404}},
        {160, {0x404, 0x10, 0x10, 0x404}},
        {162, {0x404, 0x10, 0x404}},
        {163, {0x404, 0x10, 0x10, 0x404}},
        {170, {0x10, 0x404, 0x404}},
        {182, {0x10, 0x404}},
        {183, {0x10, 0x104, 0x104, 0x104}},
        {218, {0x10, 0x82b, 0x6, 0x82b, 0x6, 0x104}},
        {240, {0x10, 0x404}},
        {241, {0x10, 0x404}},
        {242, {0x404}},
        {263, {0x10, 0x404, 0x404, 0x104, 0x2017}},
        {280, {0x204, 0x1005, 0x104}},
        {289, {0x204, 0x1005, 0x2017, 0x104}},
        {277, {0x404, 0x404}},
        {314, {0x404, 0x2017}},
        {318, {0x204, 0x404, 0x204, 0x104}},
        {319, {0x204, 0x104, 0x104, 0x204, 0x104, 0x404}},
        {324, {0x204, 0x104}},
        {327, {0x204, 0x104, 0x404, 0x404}},
        {328, {0x204, 0x104}},
        {346, {0x404, 0x104}},
        {353, {0x404, 0x404, 0x104, 0x104, 0x204, 0x204, 0x104, 0x404, 0x404, 0x404, 0x404, 0x4017, 0x400c, 0x404}},
        {380, {0x104, 0x104, 0x104, 0x104}},
        {381, {0x1417}},
        {390, {}},
        {406, {0x404, 0x12, 0x204, 0x104, 0x104, 0x805}},
        {407, {0x4000b, 0x6}},
        {408, {0x104, 0x204, 0x404}},
        {409, {0x104}},
        {410, {0x50, 0x204, 0x104, 0x404, 0x104, 0x104, 0x104}},
        {428, {0x404}},
        {430, {0x204, 0x404, 0x104, 0x104, 0x104, 0x1417}},
        {435, {0x204, 0x104}},
        {460, {0x204, 0x204, 0x104}},
        {467, {0x204}},
        {468, {0x204}},
    };

    struct WorldWriter {
        const ReplayFile::MessageTable& table;
        std::vector<ReplayFile::Record> records;
        uint32_t time_ms = 0;

        void operator()(const uint16_t message_id, std::vector<Field> fields)
        {
            ReplayFile::Record record{time_ms, {}};
            Check(ReplayState::Encode(table, {message_id, std::move(fields)}, record.body), std::format("the synthetic message {} encodes", message_id).c_str());
            records.push_back(std::move(record));
        }
    };

    std::vector<ReplayFile::Record> SyntheticWorld(const ReplayFile::MessageTable& table, std::mt19937& random)
    {
        constexpr uint32_t players_team = 0x706C6179;
        constexpr uint32_t foes_team = 0x6D6F6E73;
        constexpr uint32_t recorder = 10;
        constexpr uint32_t hero = 3;
        constexpr uint32_t hero_agent = 13;
        WorldWriter write{table, {}};
        const auto unit = [&random] {
            return static_cast<float>(random() % 2001) / 1000.f - 1.f;
        };
        const auto point = [&] {
            return Floats({unit() * 3000.f, unit() * 3000.f});
        };
        const auto create = [&](const uint32_t agent, const uint32_t agent_class, const uint32_t def, const uint32_t team) {
            const auto infinity = std::numeric_limits<float>::infinity();
            write(32, {N(agent),
                       N(agent_class),
                       N(def),
                       N(1),
                       point(),
                       N(0),
                       Floats({1.f, 0.f}),
                       N(1),
                       Floats({288.f}),
                       Floats({1.f}),
                       N(0),
                       N(team),
                       N(0),
                       N(0),
                       N(0),
                       N(0),
                       N(0),
                       Floats({0.f, 0.f}),
                       Floats({infinity, infinity}),
                       N(0),
                       N(0),
                       Floats({infinity, infinity}),
                       N(0)});
        };

        write(30, {N(100)});
        write(380, {N(1), N(2), N(3), N(4)});
        write(390, {});
        write(381, {Text(L"Recorder")});
        write(410, {N(1), N(148), N(1), N(1), N(0), N(0), N(0)});
        write(324, {N(1), N(0)});
        write(328, {N(1), N(0)});
        write(319, {N(1), N(2), N(0), N(1), N(9), N(0)});
        for (uint32_t item = 100; item < 105; item++) {
            write(353, {N(item), N(item * 7), N(2), N(0), N(0), N(0), N(1), N(0), N(10), N(item), N(1), Text(L"\x108\x107Player 3\x1"), N(1), N(0x2400)});
            write(318, {N(1), N(item), N(1), N(item - 100)});
        }
        write(319, {N(1), N(1), N(1), N(2), N(20), N(104)});
        write(324, {N(2), N(0)});
        write(328, {N(2), N(1)});
        write(319, {N(2), N(2), N(0), N(3), N(9), N(0)});
        write(327, {N(1), N(0), N(103), N(104)});
        write(409, {N(1)});
        write(407, {Bytes(40, 7)});
        write(409, {N(0)});
        write(407, {Bytes(12, 9)});
        write(408, {N(1), N(2), N(3)});
        write(428, {N(7)});
        write(406, {N(0x1234), point(), N(0), N(1), N(0), Bytes(8, 1)});
        write(430, {N(1), N(players_team), N(0), N(1), N(2), Text(L"Recorder")});
        write(242, {N(players_team)});
        write(55, {N(recorder), N(5), N(30)});
        write(183, {N(recorder), N(1), N(2), N(0)});
        write(182, {N(recorder), N(6)});
        write(218, {N(recorder), {{1, 2, 3}, {}}, {{0, 0, 0}, {}}, N(1)});
        write(115, {N(hero), N(1), N(3), N(0), N(20), N(0), {{4, 5, 6}, {}}, N(0), N(0)});
        write(57, {N(recorder), N(35)});
        write(58, {N(recorder), {{17, 18, 5, 4, 6, 4}, {}}});
        write(55, {N(hero_agent), N(0), N(20)});
        write(58, {N(hero_agent), {{1, 12, 12}, {}}});
        write(114, {N(hero), N(hero_agent), N(0), N(1)});
        write.time_ms = 2000;
        write(31, {N(5000)});
        write(89, {N(1), N(recorder), N(0x1234), N(2), N(0), N(0), Text(L"Recorder")});
        write(240, {N(recorder), N(0)});
        create(recorder, 0x30000001, 1, players_team);
        write(110, {N(recorder), N(100), N(101), N(102), N(0), N(0), N(0), N(0), N(0), N(0)});
        write(39, {N(recorder), Floats({288.f})});
        write(34, {N(recorder), N(3)});
        std::vector<uint32_t> others;
        const auto spawn = [&](const uint32_t agent) {
            write(154, {N(agent), N(100 + agent)});
            write(155, {N(agent), Text(L"\x8101\x2345")});
            write(159, {N(66), N(agent), N(0)});
            write(240, {N(agent), N(0)});
            if (agent % 5 == 0) {
                write(170, {N(agent), N(1), N(2)});
            }
            create(agent, 0x20000000 | (50 + agent % 4), 1, agent % 3 ? foes_team : players_team);
            others.push_back(agent);
        };
        for (uint32_t agent = 20; agent < 32; agent++) {
            spawn(agent);
        }
        write(263, {N(5), N(0x4444), N(0), N(100), Text(L"\x8102")});
        create(40, 5, 2, 0);
        write(277, {N(40), N(1)});
        write(353, {N(200), N(1400), N(11), N(0), N(0), N(0), N(1), N(0), N(10), N(200), N(3), Text(L"\x8103"), N(0)});
        create(41, 200, 4, 0);
        write(89, {N(2), N(12), N(0x99), N(3), N(0), N(0), Text(L"Player 1")});
        create(12, 0x30000002, 1, players_team);
        others.push_back(12);
        create(hero_agent, 0x20000060, 1, players_team);
        write(467, {N(1)});
        write(460, {N(1), N(1), N(0)});
        write(460, {N(1), N(2), N(0)});
        write(468, {N(1)});
        write(435, {N(1), N(0)});
        write(289, {N(1), Bytes(16, 3), Text(L"Guild 1"), N(1)});
        write(280, {N(1), Bytes(16, 3), N(3)});

        uint32_t next_agent = 60;
        uint32_t next_effect = 1000;
        std::vector<std::pair<uint32_t, uint32_t>> effects;
        std::vector<std::pair<uint32_t, uint32_t>> upkeeps;
        constexpr uint32_t fight_start = 50;
        constexpr uint32_t attack_every = 7;
        for (uint32_t tick = 0; tick < 600; tick++) {
            write.time_ms += 100;
            write(30, {N(100)});
            if (tick == fight_start) {
                write(53, {N(recorder), N(std::bit_cast<uint32_t>(1.33f)), N(std::bit_cast<uint32_t>(1.f))});
            }
            if (tick > fight_start && tick % attack_every == 0 && !others.empty()) {
                write(160, {N(tick % 3 ? 4 : 50), N(recorder), N(others[tick % others.size()]), N(0)});
            }
            const auto events = random() % 6;
            for (uint32_t e = 0; e < events && !others.empty(); e++) {
                const auto agent = random() % 4 == 0 ? recorder : others[random() % others.size()];
                const auto other = others[random() % others.size()];
                switch (random() % 22) {
                    case 0:
                    case 1:
                    case 2:
                        write(41, {N(agent), point(), N(0), N(0)});
                        break;
                    case 3:
                        write(42, {N(agent), point(), N(0), N(0), N(other)});
                        break;
                    case 4:
                        write(40, {N(agent)});
                        break;
                    case 5:
                        write(43, {N(agent), Floats({random() % 2 ? 0.5f : 1.f}), N(1)});
                        break;
                    case 6:
                        write(39, {N(agent), Floats({static_cast<float>(200 + random() % 150)})});
                        break;
                    case 7:
                        write(44, {N(agent), point(), N(0)});
                        break;
                    case 8:
                        write(46, {N(agent), N(std::bit_cast<uint32_t>(unit() * 3.f)), N(std::bit_cast<uint32_t>(2.f))});
                        break;
                    case 9:
                        write(66, {N(agent), N(random() % 3000), N(1), N(next_effect), N(std::bit_cast<uint32_t>(static_cast<float>(1 + random() % 15) + 0.25f))});
                        effects.emplace_back(agent, next_effect++);
                        break;
                    case 10:
                        if (!effects.empty()) {
                            const auto index = random() % effects.size();
                            write(68, {N(effects[index].first), N(effects[index].second)});
                            effects.erase(effects.begin() + index);
                        }
                        break;
                    case 11:
                        write(65, {N(agent), N(other), N(random() % 3000), N(next_effect), N(0)});
                        upkeeps.emplace_back(agent, next_effect++);
                        break;
                    case 12:
                        if (!upkeeps.empty()) {
                            const auto index = random() % upkeeps.size();
                            write(64, {N(upkeeps[index].first), N(upkeeps[index].second)});
                            upkeeps.erase(upkeeps.begin() + index);
                        }
                        break;
                    case 13:
                        write(162, {N(44), N(agent), N(std::bit_cast<uint32_t>(unit() * 0.02f))});
                        break;
                    case 14:
                        write(163, {N(random() % 2 ? 16 : 55), N(agent), N(other), N(std::bit_cast<uint32_t>(unit() * 0.3f))});
                        break;
                    case 15:
                        write(159, {N(random() % 2 ? 36 : 22), N(agent), N(random() % 20)});
                        break;
                    case 16:
                        write(159, {N(random() % 2 ? 6 : 7), N(agent), N(random() % 4)});
                        break;
                    case 17:
                        write(111, {N(recorder), N(random() % 2), N(100 + random() % 5)});
                        break;
                    case 18:
                        if (agent != recorder && agent != 12) {
                            write(33, {N(agent)});
                            std::erase(others, agent);
                            // An agent's buffs go with its 33: the server removes none of them after it.
                            for (auto* held : {&effects, &upkeeps}) {
                                std::erase_if(*held, [agent](const std::pair<uint32_t, uint32_t>& buff) {
                                    return buff.first == agent;
                                });
                            }
                            spawn(random() % 2 ? agent : next_agent++);
                        }
                        break;
                    case 19:
                        write(241, {N(agent), N(random() % 2 ? 0x10 : 0)});
                        break;
                    case 20:
                        write(93, {Text(L"\x8104")});
                        write(12, {});
                        write(218, {N(recorder), {{1, 2, 3}, {}}, {{0, 0, 0}, {}}, N(0)});
                        write(59, {N(recorder), N(17), N(5), N(7)});
                        write(56, {N(recorder), N(random() % 4)});
                        write(59, {N(hero_agent), N(1), N(12), N(13)});
                        break;
                    default:
                        if (agent != recorder) {
                            write(47, {N(agent), N(random() % 2 ? foes_team : players_team)});
                        }
                        break;
                }
            }
        }
        return write.records;
    }

    uint64_t NowUnixMs()
    {
        return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count();
    }

    struct RowsProof {
        size_t rows = 0;
        size_t layout_errors = 0;
        size_t disallowed_types = 0;
        size_t keys_out_of_range = 0;
        size_t duplicate_keys = 0;
    };

    RowsProof ProveRows(const ReplayFile::MessageTable& table, const ReplayObserve::Scan& scan, const bool verbose)
    {
        RowsProof proof;
        std::set<uint32_t> keys;
        if (verbose) {
            printf("%10s %6s %5s %9s %10s %9s  %s\n", "key", "map", "type", "category", "age (min)", "length", "check against the client's 414 table");
        }
        for (const auto& row : scan.rows) {
            const auto& message = row.message;
            const auto error = ReplayObserve::CheckRow(table, message);
            proof.rows++;
            proof.layout_errors += !error.empty();
            const auto category = ReplayObserve::Category(message.type, message.title);
            proof.disallowed_types += !ReplayObserve::IsAllowedType(message.type) || category == 9;
            proof.keys_out_of_range += !ReplayObserve::IsOurKey(message.key);
            proof.duplicate_keys += !keys.insert(message.key).second;
            if (verbose) {
                printf("%#10x %6u %5u %9u %10u %9ls  %s\n", message.key, message.map_id, message.type, category, message.age_minutes, ReplayObserve::Duration(row.header.duration_ms).c_str(), error.empty() ? "valid" : error.c_str());
            }
        }
        if (verbose) {
            for (const auto& [path, reason] : scan.left_out) {
                printf("left out %ls: %s\n", path.filename().c_str(), reason.c_str());
            }
        }
        return proof;
    }

    int Rows(const std::filesystem::path& folder, const uint32_t client_build)
    {
        std::filesystem::path reference_path;
        ReplayFile::Header reference;
        std::error_code error_code;
        for (const auto& entry : std::filesystem::directory_iterator(folder, error_code)) {
            ReplayFile::Header header;
            std::string error;
            if (entry.path().extension() == L".gwrec" && ReplayFile::ReadHeader(entry.path(), header, error) && (header.flags & ReplayFile::Complete) && (!client_build || header.client_build == client_build) &&
                (reference_path.empty() || header.start_time_unix_ms > reference.start_time_unix_ms)) {
                reference_path = entry.path();
                reference = header;
            }
        }
        if (reference_path.empty()) {
            printf("%ls: no complete recording%s to take the client's message table from\n", folder.c_str(), client_build ? std::format(" of client build {}", client_build).c_str() : "");
            return 1;
        }
        ReplayFile::Contents contents;
        std::string error;
        if (!ReplayFile::Read(reference_path, contents, error)) {
            printf("%ls: %s\n", reference_path.c_str(), error.c_str());
            return 1;
        }
        const auto& fields = contents.table.size() > ReplayObserve::row_message_id ? contents.table[ReplayObserve::row_message_id] : std::vector<uint32_t>();
        std::string descriptors;
        for (const auto descriptor : fields) {
            descriptors += std::format(" {:#x}", descriptor);
        }
        printf("client build %u; the 414 table from %ls:%s\n\n", contents.header.client_build, reference_path.filename().c_str(), descriptors.c_str());
        const auto scan = ReplayObserve::ScanFolder(folder, contents.header.client_build, contents.header.table_digest, {}, NowUnixMs(), {});
        const auto proof = ProveRows(contents.table, scan, true);
        const auto passed = proof.rows && !proof.layout_errors && !proof.disallowed_types && !proof.keys_out_of_range && !proof.duplicate_keys;
        printf(
            "\nrows %zu, left out %zu; layout errors %zu, types not allowed %zu, keys outside %#x to %#x %zu, keys not unique %zu: %s\n", proof.rows, scan.left_out.size(), proof.layout_errors, proof.disallowed_types, ReplayObserve::first_key,
            ReplayObserve::first_key + ReplayObserve::key_span - 1, proof.keys_out_of_range, proof.duplicate_keys, passed ? "passed" : "FAILED"
        );
        return passed ? 0 : 1;
    }

    std::vector<ReplayState::Message> Fed(const ReplayFile::Contents& contents, const ReplayWire::Plan& plan, const uint32_t until_ms = UINT32_MAX)
    {
        std::vector<ReplayState::Message> fed;
        for (const auto& post : plan.posts) {
            if (post.stage == ReplayWire::Stage::Stream && post.time_ms > until_ms) {
                break;
            }
            std::string error;
            for (const auto& body : Split(contents.table, post.bytes, error)) {
                ReplayState::Decode(contents.table, body, fed.emplace_back());
            }
        }
        return fed;
    }

    size_t JoinMessages(const ReplayWire::Plan& plan)
    {
        return std::accumulate(plan.posts.begin(), plan.posts.end(), size_t{0}, [](const size_t sum, const ReplayWire::Post& post) {
            return sum + (post.stage == ReplayWire::Stage::Stream ? 0 : post.records);
        });
    }

    std::string OrderSummary(const std::vector<OrderFailure>& failures, const size_t join_messages)
    {
        std::map<std::tuple<bool, uint16_t, ReplayState::IdKind>, size_t> counts;
        for (const auto& failure : failures) {
            counts[{failure.record >= join_messages, failure.field.message, failure.field.kind}]++;
        }
        std::string summary;
        for (const auto& [key, count] : counts) {
            const auto& [streamed, message_id, kind] = key;
            summary += std::format("{}{} {} {} x{}", summary.empty() ? "" : ", ", streamed ? "stream" : "join", message_id, IdKindName(kind), count);
        }
        return summary.empty() ? "none" : summary;
    }

    std::vector<ReplayState::Message> Decoded(const ReplayFile::MessageTable& table, const std::vector<ReplayFile::Record>& records)
    {
        std::vector<ReplayState::Message> messages(records.size());
        for (size_t i = 0; i < records.size(); i++) {
            ReplayState::Decode(table, records[i].body, messages[i]);
        }
        return messages;
    }

    struct Load {
        size_t head = 0;
        size_t end = 0;
    };

    std::optional<Load> RealLoad(const ReplayFile::Contents& contents, const size_t chapter)
    {
        const auto& records = contents.records;
        const auto begin = contents.chapters[chapter].first_record;
        const auto end = contents.ChapterEnd(chapter);
        const auto index_of = [&](const uint16_t id, const size_t from) {
            auto i = from;
            while (i < end && records[i].MessageId() != id) {
                i++;
            }
            return i;
        };
        const auto load = index_of(406, begin);
        const auto connect = index_of(31, load);
        if (connect >= end) {
            return std::nullopt;
        }
        Load found{index_of(380, begin) < load ? index_of(380, begin) : begin, connect};
        while (found.end < end && records[found.end].time_ms <= records[connect].time_ms) {
            found.end++;
        }
        return found;
    }

    ReplayFile::Keyframe ModelKeyframe(const ReplayFile::Contents& contents, const size_t chapter, const uint32_t time_ms)
    {
        ReplayState::World world(contents.table);
        auto i = contents.chapters[chapter].first_record;
        const auto end = contents.ChapterEnd(chapter);
        for (; i < end && (contents.records[i].time_ms < time_ms || contents.records[i].MessageId() != GAME_SMSG_AGENT_MOVEMENT_TICK); i++) {
            world.Apply(contents.records[i]);
        }
        if (i == end && i > contents.chapters[chapter].first_record) {
            i--;
        }
        return {i, contents.records[std::min(i, contents.records.size() - 1)].time_ms, ReplayFile::KeyframeKind::Periodic, JoinRecords(contents.table, world, contents.records[std::min(i, contents.records.size() - 1)].time_ms)};
    }

    struct Starts {
        std::vector<std::string> names;
        std::vector<ReplayWire::Plan> plans;
        size_t file_keyframes = 0;

        [[nodiscard]] bool Refused(const size_t start) const
        {
            if (!plans[start].error.empty()) {
                printf("%s: refused, the toolbox does not play it: %s%s\n", names[start].c_str(), plans[start].error.c_str(), start < file_keyframes ? " (--compare tells why)" : "");
            }
            return !plans[start].error.empty();
        }
    };

    Starts PlayableStarts(const ReplayFile::Contents& contents)
    {
        Starts starts;
        for (size_t k = 0; k < contents.keyframes.size(); k++) {
            const auto& keyframe = contents.keyframes[k];
            starts.names.push_back(std::format("keyframe {} ({}) at {} ms", k + 1, ReplayFile::KeyframeKindName(keyframe.kind), keyframe.time_ms));
            starts.plans.push_back(ReplayWire::MakeKeyframePlan(contents, keyframe));
        }
        starts.file_keyframes = starts.plans.size();
        for (size_t chapter = 0; chapter < contents.chapters.size(); chapter++) {
            if (const auto load = RealLoad(contents, chapter)) {
                const auto keyframe = ModelKeyframe(contents, chapter, contents.records[load->end - 1].time_ms);
                starts.names.push_back(std::format("chapter {}: the model's keyframe after its load, at {} ms", chapter + 1, keyframe.time_ms));
                starts.plans.push_back(ReplayWire::MakeKeyframePlan(contents, keyframe));
            }
        }
        return starts;
    }

    std::vector<std::pair<size_t, std::string>> StreamNeeds(const ReplayFile::Contents& contents, const ReplayFile::Keyframe& keyframe)
    {
        constexpr uint16_t load_ids[] = {GAME_SMSG_INSTANCE_LOAD_HEAD, GAME_SMSG_INSTANCE_LOAD_SPAWN_POINT, GAME_SMSG_INSTANCE_LOAD_INFO};
        std::vector<std::pair<size_t, std::string>> failures;
        ReplayState::Order order;
        ReplayState::ClientAgents agents;
        for (const auto& message : Decoded(contents.table, keyframe.records)) {
            order.Bring(contents.table, message);
            agents.Breaks(message);
        }
        const auto chapter = static_cast<size_t>(std::ranges::upper_bound(contents.chapters, keyframe.record_index, {}, &ReplayFile::Chapter::first_record) - contents.chapters.begin() - 1);
        for (auto i = keyframe.record_index; i < contents.ChapterEnd(chapter) && !std::ranges::contains(load_ids, contents.records[i].MessageId()); i++) {
            ReplayState::Message message;
            if (!ReplayState::Decode(contents.table, contents.records[i].body, message)) {
                continue;
            }
            // The plan leaves out a record naming an id the start has not brought in, but brings an item's details before it.
            if (std::ranges::any_of(order.Unbrought(contents.table, message), [](const std::pair<ReplayState::IdField, uint32_t>& field) {
                    return field.first.kind != ReplayState::IdKind::Item;
                })) {
                continue;
            }
            order.Bring(contents.table, message);
            if (const auto broken = agents.Breaks(message); !broken.empty()) {
                failures.emplace_back(i, std::format("(message {}): {}", message.id, broken));
            }
        }
        return failures;
    }

    struct Leaks {
        size_t bag_content = 0;
        size_t unshown_items = 0;
        size_t held_details = 0;

        [[nodiscard]] size_t Total() const { return bag_content + unshown_items + held_details; }
    };

    Leaks CountLeaks(const ReplayFile::MessageTable& table, const std::vector<ReplayFile::Record>& records)
    {
        Leaks leaks;
        ReplayState::InventoryFilter filter(table);
        for (const auto& message : Decoded(table, records)) {
            std::vector<ReplayState::Message> shown_before;
            std::vector<uint32_t> unknown_items;
            constexpr uint16_t instance_load_head = GAME_SMSG_INSTANCE_LOAD_HEAD;
            if (message.id == instance_load_head) {
                leaks.held_details += filter.Held();
            }
            if (ReplayState::IsBagContent(message)) {
                leaks.bag_content++;
            }
            else if (!filter.Keep(message, shown_before, unknown_items) && !filter.Holds(message)) {
                leaks.unshown_items++;
            }
        }
        leaks.held_details += filter.Held();
        return leaks;
    }

    ReplayFile::Contents Filtered(const ReplayFile::Contents& contents)
    {
        auto filtered = contents;
        filtered.records.clear();
        filtered.keyframes.clear();
        ReplayState::InventoryFilter filter(contents.table);
        auto chapter = filtered.chapters.begin();
        for (size_t i = 0; i < contents.records.size(); i++) {
            for (; chapter != filtered.chapters.end() && chapter->first_record == i; ++chapter) {
                chapter->first_record = filtered.records.size();
            }
            ReplayState::Message message;
            std::vector<ReplayState::Message> shown_before;
            std::vector<uint32_t> unknown_items;
            if (!ReplayState::Decode(contents.table, contents.records[i].body, message) || !filter.Keep(message, shown_before, unknown_items)) {
                continue;
            }
            for (const auto& detail : shown_before) {
                ReplayFile::Record record{contents.records[i].time_ms, {}};
                ReplayState::Encode(contents.table, detail, record.body);
                filtered.records.push_back(std::move(record));
            }
            filtered.records.push_back(contents.records[i]);
        }
        filtered.header.record_count = static_cast<uint32_t>(filtered.records.size());
        return filtered;
    }

    struct Difference {
        size_t expected = 0;
        size_t actual = 0;
        size_t equal = 0;
        size_t repeated = 0;
        size_t missing = 0;
        size_t extra = 0;
        std::map<size_t, size_t> fields;

        [[nodiscard]] bool Differs() const { return missing || extra || !fields.empty(); }
    };

    std::map<uint32_t, Difference> Compare(const std::vector<ReplayState::Message>& expected, const std::vector<ReplayState::Message>& actual)
    {
        std::map<std::vector<uint32_t>, std::pair<std::vector<const ReplayState::Message*>, std::vector<const ReplayState::Message*>>> paired;
        for (const auto& message : expected) {
            paired[ReplayState::Identity(message)].first.push_back(&message);
        }
        for (const auto& message : actual) {
            paired[ReplayState::Identity(message)].second.push_back(&message);
        }
        std::map<uint32_t, Difference> differences;
        for (const auto& [identity, sides] : paired) {
            // One-time notices (159 kind 22) and a look's notices (159 kinds 0, 14, 15; 160 kinds 14, 15) are compared apart from the other notices.
            constexpr uint32_t notice_message = 159;
            constexpr uint32_t slot_notice_message = 160;
            const auto split = identity.size() > 2 && ((identity[0] == notice_message && std::ranges::contains(std::initializer_list<uint32_t>{0, 14, 15, 22}, identity[2])) || (identity[0] == slot_notice_message && identity.size() > 3));
            auto& difference = differences[split ? identity[0] | (identity[2] + 1) << 16 : identity[0]];
            const auto& [left, right] = sides;
            difference.expected += left.size();
            difference.actual += right.size();
            const auto paired_count = std::min(left.size(), right.size());
            if (!right.empty() && left.size() > right.size()) {
                difference.repeated += left.size() - right.size();
            }
            difference.missing += right.empty() ? left.size() : 0;
            difference.extra += right.size() > left.size() ? right.size() - left.size() : 0;
            for (size_t i = 1; i <= paired_count; i++) {
                const auto& expected_message = *left[left.size() - i];
                const auto& actual_message = *right[right.size() - i];
                if (expected_message == actual_message) {
                    difference.equal++;
                    continue;
                }
                for (size_t field = 0; field < std::max(expected_message.fields.size(), actual_message.fields.size()); field++) {
                    if (field >= expected_message.fields.size() || field >= actual_message.fields.size() || expected_message.fields[field] != actual_message.fields[field]) {
                        difference.fields[field]++;
                    }
                }
            }
        }
        return differences;
    }

    size_t CheckedWalks(std::vector<ReplayState::Message>& sent, std::vector<ReplayState::Message>& built)
    {
        // A 32 with a stop time makes its agent mid-walk (CAgent's constructor); the model writes it standing where it walked to on its leg, its order after it.
        size_t checked = 0;
        for (auto it = sent.begin(); it != sent.end();) {
            const auto agent_id = it->U(0);
            const auto standing = std::ranges::find_if(built, [agent_id](const ReplayState::Message& message) {
                return message.id == GAME_SMSG_AGENT_SPAWNED && message.U(0) == agent_id;
            });
            const auto order = std::ranges::find_if(built, [agent_id](const ReplayState::Message& message) {
                return (message.id == GAME_SMSG_AGENT_MOVE_TO_POINT || message.id == GAME_SMSG_AGENT_UPDATE_DESTINATION) && message.U(0) == agent_id;
            });
            if (it->id != GAME_SMSG_AGENT_SPAWNED || !it->U(13) || standing == built.end() || order == built.end() || standing->U(13) || order->U(1, 0) != it->U(18, 0) || order->U(1, 1) != it->U(18, 1)) {
                ++it;
                continue;
            }
            const auto along = (standing->F(4, 0) - it->F(4, 0)) * (it->F(21, 0) - it->F(4, 0)) + (standing->F(4, 1) - it->F(4, 1)) * (it->F(21, 1) - it->F(4, 1));
            const auto across = (standing->F(4, 0) - it->F(4, 0)) * (it->F(21, 1) - it->F(4, 1)) - (standing->F(4, 1) - it->F(4, 1)) * (it->F(21, 0) - it->F(4, 0));
            const auto leg = std::hypot(it->F(21, 0) - it->F(4, 0), it->F(21, 1) - it->F(4, 1));
            if (along < 0.f || along > leg * leg + 1.f || std::abs(across) > leg) {
                ++it;
                continue;
            }
            auto rest = *standing;
            for (const auto field : {4, 13, 15, 17, 18, 19, 20, 21, 22}) {
                rest.fields[field] = it->fields[field];
            }
            if (rest != *it) {
                ++it;
                continue;
            }
            built.erase(order);
            built.erase(std::ranges::find_if(built, [agent_id](const ReplayState::Message& message) {
                return message.id == GAME_SMSG_AGENT_SPAWNED && message.U(0) == agent_id;
            }));
            it = sent.erase(it);
            checked++;
        }
        return checked;
    }

    struct Reason {
        Reason(const char* why) : why(why), any_field(true) {}
        Reason(const char* why, std::initializer_list<size_t> fields, const bool extra_only = false) : why(why), any_field(false), extra_only(extra_only), fields(fields) {}

        const char* why;
        bool any_field;
        bool extra_only = false;
        std::vector<size_t> fields;
    };

    const std::map<uint32_t, Reason> listed_differences = {
        {12, "a ping: the connection's, not the instance's state"},
        {13, "a ping's answer: the connection's, not the instance's state"},
        {15, "account currencies: left out by the no-inventory rule"},
        {30, "the clock's advance: the join's 31 carries the clock"},
        {31, "the clock: the model's 31 carries it at the join's moment, advanced by the load's last 30s"},
        {55, "the attribute points: the model folds 56 and 57 into the attribute state"},
        {56, "the attribute points available: folded into the attribute state (55)"},
        {57, "the attribute points in all: folded into the attribute state (55)"},
        {46, "a rotation in progress: the model keeps an agent's last rotation, sent after its 32"},
        {60, "a player's row flags: the model keeps the flags and the bits ever set or cleared, one message per player"},
        {89, "a player removed by a 90 during the load: the model holds the players present at its end"},
        {90, "a player removed during the load: the model holds the players present at its end"},
        {93, "chat: transient"},
        {94, "chat: transient"},
        {159 | 23 << 16, "a notice of kind 22, once in all the loads at hand: a one-time event, not kept by the model"},
        {160, "integer value updates: transient, not kept by the model"},
        {241, "an agent's effects row: a 240 after it for the same agent replaces it, the model keeping the latest"},
        {314, "a bag's item: left out by the no-inventory rule"},
        {318, "bag contents: left out by the no-inventory rule"},
        {319, "bags: left out by the no-inventory rule"},
        {320, "gold: left out by the no-inventory rule"},
        {321, "storage gold: left out by the no-inventory rule"},
        {327, "weapon sets point at bag items: left out by the no-inventory rule (what an agent holds comes by 110 and 111)"},
        {346, "a bag's item: left out by the no-inventory rule"},
        {350, "a bag's item: left out by the no-inventory rule"},
        {353, "a bag's item: left out by the no-inventory rule"},
        {354, "a bag's item: left out by the no-inventory rule"},
        {113, "once, in an outpost's load: not kept by the model, so a keyframe built from it leaves it out; outposts are not played"},
        {479, "the party search list: an outpost's window, not kept by the model"},
        {480, "the party search list: an outpost's window, not kept by the model"},
        {481, "the party search list: an outpost's window, not kept by the model"},
    };

    const std::map<uint32_t, Reason> keyframe_differences = {
        {31, {"the clock: the model's 31 is the stream's clock at its last 30, the snapshot's the client's instance time", {0}}},
        {32,
         "gadgets (chests, doors, shrines) and agents of def 3 are left out: GWCA gives them as neither living nor items; an agent's places and speeds are the client's server world's (CAgent), where the model's places are the orders' estimates and its speeds the 32's before a 39 or 43"},
        {36, "an agent's class change since its 32: the snapshot writes the agent as the client holds it"},
        {37, "an agent's move since its 32: the snapshot writes it where it stands; it waits there until its next move"},
        {38, "an agent's move since its 32: the snapshot writes it where it stands; it waits there until its next move"},
        {39, "an agent's speed change: the keyframe's 32 carries the speed it set (CAgent's max speed)"},
        {41, "a walk in progress: the keyframe writes the client's order (CAgent's target), the model the stream's latest order to the agents its estimate has walking"},
        {42, "a walk in progress: the keyframe writes the client's order (CAgent's target and its agent), the model the stream's latest order to the agents its estimate has walking"},
        {43, "a move type's change: the keyframe's 32 carries the move speed it set (CAgent's move speed)"},
        {46, "a rotation in progress: the 32 carries the agent's facing now"},
        {55, "the attribute points: not in GWCA; the attribute window, which an observer does not have"},
        {59, "an attribute changed after the load: the 58 written from GWCA holds its level"},
        {60, "a player's row flags: not read; the party window's, which observers replace with ObservedPartiesHud"},
        {66, {"a periodic keyframe's effect durations are the client's time remaining", {4}}},
        {72, "a cape shown: not read; capes hidden until the stream's next 72"},
        {76, "quest descriptions: the quest log's text, which an observer does not have"},
        {81, "a quest's marker moved after the load: the 83 written holds the marker the client keeps"},
        {83, "a quest's marker: the stream adds it (83) and moves it (81), the snapshot adds it where the client keeps it"},
        {87, "an NPC the client holds that the stream has not defined since the load: defined, unused unless an agent's class names it"},
        {109, {"a hero's weapons after a swap: the model keeps the load's 109 and the swap's 111s, the snapshot the char client's slots now: the same weapons", {1, 2}}},
        {110, "a hero's weapons after a swap: the model keeps the swap's 111s as a 110, the snapshot writes the hero's 109 from the char client's slots"},
        {124, "not read: unknown purpose, no use of it seen in the observer's view"},
        {125, "not read: unknown purpose, no use of it seen in the observer's view"},
        {138, "the explored areas: not written; the world map stays closed in replays (OnWorldMapToggle)"},
        {139, "the explored areas: not written; the world map stays closed in replays (OnWorldMapToggle)"},
        {141, "the mission map's points: the mission map stays closed in replays (6b, OnMissionMapCreate)"},
        {154, "an agent's scale: not read; models at their default size"},
        {155, {"an agent's name: GWCA's encoded name, not written when it is its NPC's own (the agent shows the NPC's name); rows of agents not in the world (an outpost's heroes) are not written: outposts are not played", {1}}},
        {159, "notice 30 (guild) is not read; one-time notices are not kept"},
        {162, {"rows of agents not in the world (an outpost's heroes) are not written: outposts are not played; a periodic keyframe's values are the client's", {2}}},
        {166, "rows of agents not in the world (an outpost's heroes) are not written: outposts are not played"},
        {170, "an ally added after the load: not read; missing from the party's allies until the stream's next 170"},
        {181, "the recorder's interface preferences: not the viewer's"},
        {221, "the recorder's skill count: not read; the skills window's"},
        {233, "heroes' skill points: not in GWCA (0 written)"},
        {239, "the recorder's display settings: not the viewer's"},
        {240, "an agent's effect bits: the model keeps the stream's latest row (a 241), the snapshot writes them as a load does (240)"},
        {241, "an agent's effect bits: the model keeps the stream's latest row (a 241), the snapshot writes them as a load does (240)"},
        {263, {"classes of the maps before: the client's table keeps every class since the game started, the snapshot writes them all", {}, true}},
        {270, "props' animations: not in GWCA; props at their default state until the stream sends them"},
        {273, "props' layers: not in GWCA; props at their default state until the stream sends them"},
        {277, "gadgets' states: gadgets are left out (32)"},
        {284, "not read: no field, its purpose unknown"},
        {302, "not read: a guild value of unknown purpose"},
        {314, {"the item's customizer through the names settings: memory's text is the recorder's where the stream's 314 names another, a placeholder either way; the item's description, which observers have no window for", {1}}},
        {346, {"an item's profession, written with its details: missing with its 353 (an item no agent wears now)", {}}},
        {353, {"an item no agent wears now (a hero's other weapon set) is not written, the plan bringing its details from the stream when a record shows it again; an item described again (354) is written as a 353", {}}},
        {354, {"an item described again after the load (354): the snapshot writes the client's details as a 353", {}}},
        {360, {"a drop's fall from the agent it came from (360's handler hands both agents to the agent view's drop, f09): a keyframe's ground item lies where it fell", {}}},
        {406, {"the spawn, its plane and facing: the controlled agent's place and facing now; the server's time: the recorder's clock as a FILETIME", {1, 2, 3, 5}}},
        {407, "the manifest's chunks: absent from 12 of 18 loads at hand, which loaded"},
        {408, "the manifest's hash: 0, as in gwlab's loads, which loaded"},
        {409, "the manifest's phases: written as gwlab's loads have them, which loaded"},
        {428, "not read: gwlab's map 779 load had 0 and loaded"},
        {159 | 23 << 16, "a one-time notice: not kept"},
    };

    bool PrintComparison(const std::map<uint32_t, Difference>& differences, const std::map<uint32_t, Reason>& reasons)
    {
        size_t unlisted = 0;
        size_t equal = 0;
        size_t expected = 0;
        size_t repeated = 0;
        printf("%6s %8s %8s %8s %8s %8s %8s  %s\n", "id", "load", "built", "equal", "repeated", "missing", "extra", "fields differing; why");
        for (const auto& [id, difference] : differences) {
            expected += difference.expected;
            equal += difference.equal;
            repeated += difference.repeated;
            if (!difference.Differs()) {
                continue;
            }
            std::string fields;
            for (const auto& [field, count] : difference.fields) {
                fields += std::format(" f{} x{}", field, count);
            }
            const auto why = reasons.find(id);
            const auto listed = why != reasons.end() && !(why->second.extra_only && difference.missing) && std::ranges::all_of(difference.fields | std::views::keys, [&why](const size_t field) {
                                    return why->second.any_field || std::ranges::contains(why->second.fields, field);
                                });
            unlisted += !listed;
            printf(
                "%6s %8zu %8zu %8zu %8zu %8zu %8zu %s; %s\n", (id >> 16 ? std::format("{}/{}", id & 0xFFFF, (id >> 16) - 1) : std::format("{}", id)).c_str(), difference.expected, difference.actual, difference.equal, difference.repeated, difference.missing,
                difference.extra, fields.empty() ? " none" : fields.c_str(),
                listed                 ? why->second.why
                : why == reasons.end() ? "NOT LISTED"
                                       : "NOT LISTED: what its reason does not cover differs"
            );
        }
        printf("%zu of %zu messages equal, %zu repeats of a message the model keeps once (the latest, equal); %zu message ids differ with no reason listed\n", equal, expected, repeated, unlisted);
        return !unlisted;
    }

    struct NameCount {
        size_t texts = 0;
        size_t literals = 0;
        size_t not_placeholders = 0;
    };

    constexpr uint16_t own_name_messages[] = {CharAccountNameData, GAME_SMSG_HERO_ACCOUNT_NAME, GAME_SMSG_MERCENARY_INFO, GAME_SMSG_INSTANCE_LOAD_PLAYER_NAME};

    NameCount CountNames(const ReplayFile::MessageTable& table, const std::vector<ReplayFile::Record>& records, const uint32_t flags)
    {
        NameCount count;
        for (const auto& message : Decoded(table, records)) {
            const auto checked = (flags & ReplayFile::NamesStripped) || ((flags & ReplayFile::OwnNamesStripped) && std::ranges::contains(own_name_messages, message.id));
            for (const auto& field : message.fields) {
                const std::wstring_view text = field.text;
                if (ReplayFile::IsPlainText(text)) {
                    count.texts++;
                    count.not_placeholders += checked && !IsPlaceholder(text);
                    continue;
                }
                for (auto start = text.find(ReplayFile::literal_start); start != std::wstring_view::npos; start = text.find(ReplayFile::literal_start, start + 1)) {
                    const auto end = text.find(ReplayFile::literal_end, start + 1);
                    if (end == std::wstring_view::npos) {
                        break;
                    }
                    if (const auto literal = text.substr(start + 1, end - start - 1); ReplayFile::IsPlainText(literal)) {
                        count.literals++;
                        count.not_placeholders += checked && !IsPlaceholder(literal);
                    }
                    start = end;
                }
            }
        }
        return count;
    }

    int ProveCompare(const ReplayFile::Contents& contents)
    {
        printf("%zu chapters, %zu keyframes, %zu records\n", contents.chapters.size(), contents.keyframes.size(), contents.records.size());
        for (size_t c = 0; c < contents.chapters.size(); c++) {
            const auto& chapter = contents.chapters[c];
            printf("  chapter %zu: map %u, %s, from %u ms, records %zu to %zu\n", c + 1, chapter.map_id, chapter.is_explorable ? "explorable" : "outpost", chapter.time_ms, chapter.first_record, contents.ChapterEnd(c));
        }
        for (size_t k = 0; k < contents.keyframes.size(); k++) {
            const auto& keyframe = contents.keyframes[k];
            uint64_t bytes = 0;
            for (const auto& record : keyframe.records) {
                bytes += record.body.size() + 6;
            }
            printf("  keyframe %zu: %s, at %u ms before record %zu, %zu messages, %llu bytes\n", k + 1, ReplayFile::KeyframeKindName(keyframe.kind), keyframe.time_ms, keyframe.record_index, keyframe.records.size(), static_cast<unsigned long long>(bytes));
        }
        auto passed = true;
        for (size_t c = 0; c < contents.chapters.size(); c++) {
            const auto load = RealLoad(contents, c);
            if (!load) {
                continue;
            }
            const std::vector<ReplayFile::Record> records(contents.records.begin() + static_cast<std::ptrdiff_t>(load->head), contents.records.begin() + static_cast<std::ptrdiff_t>(load->end));
            ReplayState::World world(contents.table);
            for (const auto& record : records) {
                world.Apply(record);
            }
            auto built = world.Join();
            auto sent = Decoded(contents.table, records);
            const auto walks = CheckedWalks(sent, built);
            printf(
                "\nchapter %zu: its real load (records %zu to %zu) against the model's join built from it: %zu messages built, layout errors %u; %zu agents made mid-walk, written where they walked to with their order after them\n", c + 1, load->head,
                load->end, built.size(), world.LayoutErrors(), walks
            );
            passed &= PrintComparison(Compare(sent, built), listed_differences);
            passed &= !world.LayoutErrors();
        }
        for (size_t k = 0; k < contents.keyframes.size(); k++) {
            const auto& keyframe = contents.keyframes[k];
            const auto chapter = static_cast<size_t>(std::ranges::upper_bound(contents.chapters, keyframe.record_index, {}, &ReplayFile::Chapter::first_record) - contents.chapters.begin() - 1);
            const auto load = RealLoad(contents, chapter);
            if (keyframe.kind == ReplayFile::KeyframeKind::Start || !load || keyframe.record_index < load->end) {
                printf(
                    "\nkeyframe %zu (%s, at %u ms): %s\n", k + 1, ReplayFile::KeyframeKindName(keyframe.kind), keyframe.time_ms,
                    keyframe.kind == ReplayFile::KeyframeKind::Start ? "the recording's start: no stream before it to model" : "no load of its map before it in the stream to model"
                );
                continue;
            }
            ReplayState::World stream(contents.table);
            for (auto i = load->head; i < keyframe.record_index; i++) {
                stream.Apply(contents.records[i]);
            }
            auto model = stream.Join();
            // CItemTable::AddDetailLow keeps an item's model file without bit 31, its fetch at once: memory cannot give it back.
            for (auto& message : model) {
                if (message.id == GAME_SMSG_ITEM_GENERAL_INFO || message.id == GAME_SMSG_ITEM_REUSE_ID) {
                    message.Set(1, message.U(1) & 0x7FFFFFFF);
                }
            }
            // Memory holds an attribute's level after its 59s, where the model keeps the 58 and each 59 apart.
            const auto fold_attribute_updates = [](std::vector<ReplayState::Message>& messages) {
                std::map<uint32_t, std::vector<ReplayState::Message>> updates;
                for (const auto& message : messages) {
                    if (message.id == GAME_SMSG_AGENT_UPDATE_ATTRIBUTE) {
                        updates[message.U(0)].push_back(message);
                    }
                }
                for (auto& message : messages) {
                    if (message.id != CharAttributes || message.fields.size() < 2 || !updates.contains(message.U(0))) {
                        continue;
                    }
                    auto& words = message.fields[1].numbers;
                    const auto count = words.size() / 3;
                    std::map<uint32_t, std::pair<uint32_t, uint32_t>> levels;
                    for (size_t i = 0; i < count; i++) {
                        levels[words[i]] = {words[count + i], words[2 * count + i]};
                    }
                    for (const auto& update : updates[message.U(0)]) {
                        levels[update.U(1)] = {update.U(2), update.U(3)};
                    }
                    std::erase_if(levels, [](const auto& level) {
                        return !level.second.second;
                    });
                    words.clear();
                    std::ranges::copy(levels | std::views::keys, std::back_inserter(words));
                    std::ranges::transform(levels | std::views::values, std::back_inserter(words), &std::pair<uint32_t, uint32_t>::first);
                    std::ranges::transform(levels | std::views::values, std::back_inserter(words), &std::pair<uint32_t, uint32_t>::second);
                }
            };
            auto snapshot = Decoded(contents.table, keyframe.records);
            fold_attribute_updates(model);
            fold_attribute_updates(snapshot);
            printf("\nkeyframe %zu (%s, at %u ms): the snapshot from memory against the model of the stream up to it (records %zu to %zu)\n", k + 1, ReplayFile::KeyframeKindName(keyframe.kind), keyframe.time_ms, load->head, keyframe.record_index);
            passed &= PrintComparison(Compare(model, snapshot), keyframe_differences);
        }
        std::map<std::string, size_t> broken;
        for (size_t k = 0; k < contents.keyframes.size(); k++) {
            const auto& records = contents.keyframes[k].records;
            for (const auto& record : records) {
                if (ReplayState::Message message; !ReplayState::Decode(contents.table, record.body, message)) {
                    broken[std::format("keyframe {}: message {} does not decode against the file's table", k + 1, record.MessageId())]++;
                }
            }
            for (const auto& failure : CheckClientNeeds(Decoded(contents.table, records))) {
                broken[std::format("keyframe {}: {}", k + 1, failure.substr(failure.find('(')))]++;
            }
            std::map<std::string, std::pair<size_t, size_t>> unset;
            for (const auto& [record, failure] : StreamNeeds(contents, contents.keyframes[k])) {
                unset.try_emplace(failure, record, 0).first->second.second++;
            }
            for (const auto& [failure, first] : unset) {
                broken[std::format("keyframe {}, the stream it feeds, from record {}: {}", k + 1, first.first, failure)] = first.second;
            }
        }
        for (const auto& message : Decoded(contents.table, contents.records)) {
            if (const auto failure = ReplayState::FieldAssert(message); !failure.empty()) {
                broken[std::format("the stream: (message {}): {}", message.id, failure)]++;
            }
        }
        printf("\nthe game's asserts on the keyframes' messages, the stream each feeds and the stream's fields: %s\n", broken.empty() ? "none broken" : "broken");
        for (const auto& [what, count] : broken) {
            printf("  %zu x %s\n", count, what.c_str());
        }
        passed &= broken.empty();
        const auto stream_leaks = CountLeaks(contents.table, contents.records);
        Leaks keyframe_leaks;
        auto names = CountNames(contents.table, contents.records, contents.header.flags);
        NameCount keyframe_names;
        for (const auto& keyframe : contents.keyframes) {
            const auto leaks = CountLeaks(contents.table, keyframe.records);
            keyframe_leaks.bag_content += leaks.bag_content;
            keyframe_leaks.unshown_items += leaks.unshown_items;
            keyframe_leaks.held_details += leaks.held_details;
            const auto counted = CountNames(contents.table, keyframe.records, contents.header.flags);
            keyframe_names.texts += counted.texts;
            keyframe_names.literals += counted.literals;
            keyframe_names.not_placeholders += counted.not_placeholders;
        }
        const auto bags_passed = stream_leaks.Total() + keyframe_leaks.Total() == 0;
        printf(
            "\nthe no-inventory rule: the stream holds %zu bag messages, %zu messages naming an item no agent wears, holds or has on the ground, %zu details of such items; the keyframes %zu, %zu and %zu: %s\n", stream_leaks.bag_content,
            stream_leaks.unshown_items, stream_leaks.held_details, keyframe_leaks.bag_content, keyframe_leaks.unshown_items, keyframe_leaks.held_details, bags_passed ? "passed" : "FAILED"
        );
        const auto settings = contents.header.flags & ReplayFile::NamesStripped      ? "every plain text and name inside a game string"
                              : contents.header.flags & ReplayFile::OwnNamesStripped ? "the recorder's, account's and mercenaries' names (48, 49, 116, 381)"
                                                                                     : nullptr;
        const auto names_passed = names.not_placeholders + keyframe_names.not_placeholders == 0;
        printf(
            "the names: the stream holds %zu plain texts and %zu names inside game strings, the keyframes %zu and %zu; %s: %s\n", names.texts, names.literals, keyframe_names.texts, keyframe_names.literals,
            settings ? std::format("checked by the names settings ({}), {} not a placeholder", settings, names.not_placeholders + keyframe_names.not_placeholders).c_str() : "the names settings were off for this recording: not checked",
            names_passed ? "passed" : "FAILED"
        );
        printf("compare: %s\n", passed && bags_passed && names_passed ? "passed" : "FAILED");
        return passed && bags_passed && names_passed ? 0 : 1;
    }

    struct WireProof {
        size_t posts = 0;
        size_t records = 0;
        size_t bytes = 0;
        std::string error;
    };

    WireProof ProveWire(const ReplayFile::Contents& contents, const ReplayWire::Plan& plan, const uint32_t seed)
    {
        WireProof proof;
        if (!plan.error.empty()) {
            proof.error = plan.error;
            return proof;
        }
        std::mt19937 random(seed);
        uint8_t key[ReplayWire::ready_secret_size];
        for (auto& byte : key) {
            byte = static_cast<uint8_t>(random());
        }
        ReplayWire::Rc4 sender;
        InitRc4(sender, key, sizeof(key));
        auto receiver = sender;
        std::vector<uint8_t> plain;
        std::vector<uint8_t> received;
        size_t unchanged = 0;
        for (const auto& post : plan.posts) {
            if (post.bytes.size() > ReplayWire::max_post_size && post.records > 1) {
                proof.error = std::format("post {} holds {} records in {} bytes, over {}", proof.posts, post.records, post.bytes.size(), ReplayWire::max_post_size);
                return proof;
            }
            auto chunk = post.bytes;
            sender.Apply(chunk.data(), chunk.size());
            unchanged += chunk == post.bytes;
            receiver.Apply(chunk.data(), chunk.size());
            received.insert(received.end(), chunk.begin(), chunk.end());
            plain.insert(plain.end(), post.bytes.begin(), post.bytes.end());
            proof.posts++;
            proof.records += post.records;
        }
        proof.bytes = plain.size();
        if (unchanged) {
            proof.error = std::format("{} of {} posts left as they were by the cipher", unchanged, plan.posts.size());
            return proof;
        }
        if (received != plain) {
            proof.error = "the stream decrypted post by post is not the plain stream";
            return proof;
        }
        std::string error;
        const auto bodies = Split(contents.table, received, error);
        if (!error.empty() || bodies.size() != proof.records) {
            proof.error = error.empty() ? std::format("{} messages cut from the stream, {} posted", bodies.size(), proof.records) : error;
            return proof;
        }
        for (size_t i = 0; i < bodies.size(); i++) {
            ReplayState::Message message;
            std::vector<uint8_t> encoded;
            if (!ReplayState::Decode(contents.table, bodies[i], message) || !ReplayState::Encode(contents.table, message, encoded) || encoded != bodies[i]) {
                proof.error = std::format("message {} (id {}) does not decode and encode back to its bytes", i, message.id);
                return proof;
            }
        }
        return proof;
    }

    bool ProveFeed(const ReplayFile::Contents& contents, const ReplayWire::Plan& plan, const uint32_t until_ms, const bool verbose)
    {
        if (!plan.error.empty()) {
            if (verbose) {
                printf("feed: %s\nfeed: FAILED\n", plan.error.c_str());
            }
            return false;
        }
        size_t failures = 0;
        const auto fail = [&failures, verbose](const std::string& what) {
            if (verbose && failures < 20) {
                printf("  FAILED: %s\n", what.c_str());
            }
            failures++;
        };
        if (verbose) {
            printf(
                "a keyframe at %u ms of the recording (%zu messages), then the stream from record %zu to %zu, %u ms played from there; map type %u; controlled agent %u; changes %zu\n\n", plan.start_ms, plan.keyframe_records, plan.first_record,
                plan.end_record, plan.duration_ms, plan.map_type, plan.controlled_agent, plan.changes.size()
            );
        }
        if (verbose) {
            printf("%6s  %-38s %9s %8s %6s  %s\n", "post", "stage (posted when)", "time ms", "records", "bytes", "first message ids");
        }
        auto stage = ReplayWire::Stage::BeforeLoad;
        uint32_t time_ms = 0;
        size_t shown = 0;
        size_t shown_records = 0;
        size_t shown_bytes = 0;
        std::map<ReplayWire::Stage, size_t> per_stage;
        bool observer_info = false;
        for (size_t i = 0; i < plan.posts.size(); i++) {
            const auto& post = plan.posts[i];
            if (post.stage < stage) {
                fail(std::format("post {} goes back to an earlier stage", i));
            }
            if (post.stage != ReplayWire::Stage::Stream && post.time_ms) {
                fail(std::format("join post {} has a time", i));
            }
            if (post.time_ms < time_ms) {
                fail(std::format("post {} goes back in time", i));
            }
            if (post.bytes.size() > ReplayWire::max_post_size && post.records > 1) {
                fail(std::format("post {} holds {} records in {} bytes", i, post.records, post.bytes.size()));
            }
            std::string error;
            const auto bodies = Split(contents.table, post.bytes, error);
            if (!error.empty() || bodies.size() != post.records) {
                fail(std::format("post {} does not cut into its {} messages ({})", i, post.records, error));
            }
            for (const auto& body : bodies) {
                ReplayState::Message message;
                observer_info |= ReplayState::Decode(contents.table, body, message) && post.stage == ReplayWire::Stage::BeforeLoad && message.id == GAME_SMSG_INSTANCE_LOAD_INFO && message.U(5) == 1;
            }
            stage = post.stage;
            time_ms = post.time_ms;
            per_stage[post.stage] += post.records;
            if (post.time_ms > until_ms) {
                continue;
            }
            shown++;
            shown_records += post.records;
            shown_bytes += post.bytes.size();
            if (verbose) {
                std::string ids;
                for (size_t b = 0; b < bodies.size() && b < 6; b++) {
                    ids += std::format(" {}", bodies[b][0] | bodies[b][1] << 8);
                }
                printf("%6zu  %-38s %9u %8u %6zu %s%s\n", i, ReplayWire::StageName(post.stage), post.time_ms, post.records, post.bytes.size(), ids.c_str(), bodies.size() > 6 ? " ..." : "");
            }
        }
        for (const auto required : {ReplayWire::Stage::BeforeLoad, ReplayWire::Stage::Load, ReplayWire::Stage::Entry}) {
            if (!per_stage[required]) {
                fail(std::format("no records {}", ReplayWire::StageName(required)));
            }
        }
        if (!observer_info) {
            fail("no 410 with its observer byte set before 406");
        }
        const auto fed = Fed(contents, plan);
        for (const auto& need : CheckClientNeeds(fed)) {
            fail(std::format("what the client needs: {}", need));
        }
        const auto order = CheckOrder(contents.table, fed);
        for (const auto& failure : order) {
            fail(std::format("the order of real loads: {}", Describe(failure)));
        }
        if (verbose) {
            printf(
                "\nup to %u ms: %zu posts, %zu records, %zu bytes; the whole plan: %zu posts (%zu + %zu + %zu records in the load's stages, stream %zu records)\n", until_ms, shown, shown_records, shown_bytes, plan.posts.size(),
                per_stage[ReplayWire::Stage::BeforeLoad], per_stage[ReplayWire::Stage::Load], per_stage[ReplayWire::Stage::Entry], per_stage[ReplayWire::Stage::Stream]
            );
            printf("the order of real loads: %zu failures (%s)\n", order.size(), OrderSummary(order, JoinMessages(plan)).c_str());
            printf("feed: %s\n", failures ? "FAILED" : "passed");
        }
        return !failures;
    }

    std::string JoinTime(const uint32_t join_ms)
    {
        return join_ms == UINT32_MAX ? "the end" : std::format("{} ms", join_ms);
    }

    size_t LeftOutRecords(const ReplayWire::Plan& plan)
    {
        return std::accumulate(plan.changes.begin(), plan.changes.end(), size_t{0}, [](const size_t sum, const ReplayWire::Change& change) {
            return sum + (change.kind == ReplayWire::ChangeKind::LeftOut && change.why.starts_with("they name") ? change.count : 0);
        });
    }

    bool ProveOrder(const ReplayFile::Contents& contents)
    {
        auto passed = true;
        const auto starts = PlayableStarts(contents);
        for (size_t s = 0; s < starts.plans.size(); s++) {
            const auto& plan = starts.plans[s];
            if (starts.Refused(s)) {
                passed &= s < starts.file_keyframes;
                continue;
            }
            const auto fed = Fed(contents, plan);
            const auto order = CheckOrder(contents.table, fed);
            const auto needs = CheckClientNeeds(fed);
            printf(
                "%s: %zu messages fed; order failures %zu (%s); what the client needs, failures %zu; stream records left out naming an id the start does not hold %zu\n", starts.names[s].c_str(), fed.size(), order.size(),
                OrderSummary(order, JoinMessages(plan)).c_str(), needs.size(), LeftOutRecords(plan)
            );
            passed &= order.empty() && needs.empty();
        }
        for (size_t chapter = 0; chapter < contents.chapters.size(); chapter++) {
            if (!contents.chapters[chapter].is_explorable || !RealLoad(contents, chapter)) {
                continue;
            }
            for (const auto join_ms : {10000u, 30000u, 60000u, UINT32_MAX}) {
                const auto started = std::chrono::steady_clock::now();
                const auto keyframe = ModelKeyframe(contents, chapter, join_ms == UINT32_MAX ? join_ms : contents.chapters[chapter].time_ms + join_ms);
                const auto built_us = std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - started).count();
                const auto bytes = std::accumulate(keyframe.records.begin(), keyframe.records.end(), size_t{0}, [](const size_t sum, const ReplayFile::Record& record) {
                    return sum + record.body.size() + 6;
                });
                const auto plan = ReplayWire::MakeKeyframePlan(contents, keyframe);
                if (!plan.error.empty()) {
                    printf("chapter %zu, a model keyframe at %s: %s\n", chapter + 1, JoinTime(join_ms).c_str(), plan.error.c_str());
                    passed = false;
                    continue;
                }
                const auto fed = Fed(contents, plan);
                const auto order = CheckOrder(contents.table, fed);
                const auto needs = CheckClientNeeds(fed);
                printf(
                    "chapter %zu, a keyframe built by the model at %s (its 30 at %u ms): %zu keyframe messages, %zu bytes, the model fed and joined in %lld us, %zu streamed; order failures %zu (%s); what the client needs, failures %zu; stream records left out naming an id the keyframe does not hold %zu\n",
                    chapter + 1, JoinTime(join_ms).c_str(), plan.start_ms, plan.keyframe_records, bytes, static_cast<long long>(built_us), fed.size() - plan.keyframe_records, order.size(), OrderSummary(order, JoinMessages(plan)).c_str(), needs.size(),
                    LeftOutRecords(plan)
                );
                passed &= order.empty() && needs.empty();
            }
        }
        printf("order: %s\n", passed ? "passed" : "FAILED");
        return passed;
    }

    ReplayFile::Contents WithModelKeyframes(const ReplayFile::Contents& contents, const bool verbose)
    {
        constexpr uint32_t model_keyframe_interval_ms = 10000;
        auto with = contents;
        for (size_t chapter = 0; chapter < contents.chapters.size(); chapter++) {
            const auto load = RealLoad(contents, chapter);
            if (!contents.chapters[chapter].is_explorable || !load || std::ranges::any_of(contents.keyframes, [&](const ReplayFile::Keyframe& keyframe) {
                    return contents.ChapterOf(keyframe.record_index) == chapter;
                })) {
                continue;
            }
            const auto last_ms = contents.records[contents.ChapterEnd(chapter) - 1].time_ms;
            for (auto time_ms = contents.records[load->end - 1].time_ms; time_ms <= last_ms; time_ms += model_keyframe_interval_ms) {
                with.keyframes.push_back(ModelKeyframe(contents, chapter, time_ms));
            }
            if (verbose) {
                printf("chapter %zu holds no keyframe: the model's, after its load and every %u ms\n", chapter + 1, model_keyframe_interval_ms);
            }
        }
        std::ranges::stable_sort(with.keyframes, {}, &ReplayFile::Keyframe::record_index);
        const auto repeated = std::ranges::unique(with.keyframes, {}, &ReplayFile::Keyframe::record_index);
        with.keyframes.erase(repeated.begin(), repeated.end());
        return with;
    }

    std::vector<float> PlacementErrors(const ReplayFile::Contents& contents, const ReplayWire::Plan& from, const ReplayFile::Keyframe& later)
    {
        // A later keyframe's 32s hold the client's own places at its time: the placement there from an earlier keyframe against them.
        std::map<uint32_t, std::pair<float, float>> client;
        for (const auto& message : Decoded(contents.table, later.records)) {
            if (message.id == GAME_SMSG_AGENT_SPAWNED && message.U(2) == 1) {
                client[message.U(0)] = {message.F(4, 0), message.F(4, 1)};
            }
        }
        std::vector<float> errors;
        for (const auto& message : ReplayWire::Placement(contents, from, later.time_ms - from.start_ms)) {
            if (const auto found = client.find(message.U(0)); message.id == GAME_SMSG_AGENT_UPDATE_POSITION && found != client.end()) {
                errors.push_back(std::hypot(message.F(1, 0) - found->second.first, message.F(1, 1) - found->second.second));
            }
        }
        return errors;
    }

    std::string ErrorSummary(std::vector<float> errors)
    {
        if (errors.empty()) {
            return "no agent";
        }
        std::ranges::sort(errors);
        const auto mean = std::accumulate(errors.begin(), errors.end(), 0.0) / static_cast<double>(errors.size());
        const auto over = std::ranges::count_if(errors, [](const float error) {
            return error > 100.f;
        });
        return std::format("{} agents, mean {:.0f}, median {:.0f}, p90 {:.0f}, most {:.0f} units, {} over 100", errors.size(), mean, errors[errors.size() / 2], errors[errors.size() * 9 / 10], errors.back(), over);
    }

    std::map<uint32_t, std::pair<float, float>> Placed(const std::vector<ReplayState::Message>& placement)
    {
        std::map<uint32_t, std::pair<float, float>> placed;
        for (const auto& message : placement) {
            if (message.id == GAME_SMSG_AGENT_UPDATE_POSITION) {
                placed[message.U(0)] = {message.F(1, 0), message.F(1, 1)};
            }
        }
        return placed;
    }

    struct SeekProof {
        size_t seeks = 0;
        size_t refused = 0;
        size_t failed = 0;
        size_t in_place = 0;
        size_t rewinds = 0;
        size_t rewind_kept = 0;
        size_t rewind_props = 0;
        size_t most_records = 0;
        size_t most_bytes = 0;
        size_t placed = 0;
        size_t placed_apart = 0;
        std::vector<float> sampled_errors;
        std::vector<float> orders_errors;
    };

    SeekProof ProveSeek(const ReplayFile::Contents& contents, const bool verbose, const std::set<uint32_t>& asked_ms = {})
    {
        SeekProof proof;
        const auto fail = [&proof, verbose](const std::string& what) {
            if (verbose && proof.failed < 40) {
                printf("  FAILED: %s\n", what.c_str());
            }
            proof.failed++;
        };
        for (size_t chapter = 0; chapter < contents.chapters.size(); chapter++) {
            const auto& opened = contents.chapters[chapter];
            const auto end = contents.ChapterEnd(chapter);
            std::vector<size_t> keyframes;
            for (size_t k = 0; k < contents.keyframes.size(); k++) {
                if (contents.ChapterOf(contents.keyframes[k].record_index) == chapter) {
                    keyframes.push_back(k);
                }
            }
            if (!opened.is_explorable) {
                const auto plan = ReplayWire::MakeSeekPlan(contents, opened.time_ms);
                proof.seeks++;
                if (verbose) {
                    printf("chapter %zu (an outpost): a seek to its start %s\n", chapter + 1, plan.error.empty() ? std::format("plays chapter {} from its keyframe {}", plan.chapter + 1, plan.keyframe + 1).c_str() : plan.error.c_str());
                }
                if (plan.error.empty() && plan.chapter <= chapter) {
                    fail(std::format("a seek into outpost chapter {} plays chapter {}", chapter + 1, plan.chapter + 1));
                }
                continue;
            }
            if (keyframes.empty() || end == opened.first_record) {
                continue;
            }
            // The whole stream's state: the chapter from its load when the stream holds it, else from its first keyframe, then every record.
            ReplayState::World whole(contents.table);
            auto next_record = opened.first_record;
            if (!RealLoad(contents, chapter)) {
                for (const auto& record : contents.keyframes[keyframes.front()].records) {
                    whole.Apply(record);
                }
                next_record = contents.keyframes[keyframes.front()].record_index;
            }
            std::map<size_t, ReplayFile::Keyframe> model_keyframes;
            std::map<size_t, ReplayWire::Plan> playing;
            for (const auto k : keyframes) {
                if (auto plan = ReplayWire::MakeKeyframePlan(contents, contents.keyframes[k]); plan.error.empty() && plan.map_type == ReplayWire::game_map_type) {
                    plan.keyframe = k;
                    playing.emplace(k, std::move(plan));
                }
            }
            size_t in_place = 0;
            size_t in_place_differing = 0;
            auto orders_only = contents;
            orders_only.motions.clear();
            for (const auto& [k, from] : playing) {
                const auto later = std::ranges::find_if(playing, [&](const auto& entry) {
                    return entry.first > k;
                });
                if (later == playing.end()) {
                    continue;
                }
                const auto& truth = contents.keyframes[later->first];
                auto sampled = PlacementErrors(contents, from, truth);
                auto estimated = PlacementErrors(orders_only, from, truth);
                if (verbose) {
                    printf(
                        "chapter %zu, keyframe %zu's client places against the placement from keyframe %zu: with the file's %zu samples, %s; orders only, %s\n", chapter + 1, later->first + 1, k + 1, contents.motions.size(), ErrorSummary(sampled).c_str(),
                        ErrorSummary(estimated).c_str()
                    );
                }
                proof.sampled_errors.insert(proof.sampled_errors.end(), sampled.begin(), sampled.end());
                proof.orders_errors.insert(proof.orders_errors.end(), estimated.begin(), estimated.end());
            }
            auto at_keyframe = whole;
            auto keyframe_record = next_record;
            for (const auto k : keyframes) {
                const auto& keyframe = contents.keyframes[k];
                for (; keyframe_record < keyframe.record_index; keyframe_record++) {
                    at_keyframe.Apply(contents.records[keyframe_record]);
                }
                model_keyframes[k] = {keyframe.record_index, keyframe.time_ms, keyframe.kind, JoinRecords(contents.table, at_keyframe, keyframe.time_ms)};
            }
            // A time a chapter starts at is that chapter's, even when the map before it has a record at the same time.
            const auto last_ms = chapter + 1 < contents.chapters.size() ? std::min(contents.records[end - 1].time_ms, contents.chapters[chapter + 1].time_ms - 1) : contents.records[end - 1].time_ms;
            std::set<uint32_t> times = {opened.time_ms, last_ms};
            for (size_t i = 0; i < keyframes.size(); i++) {
                const auto from = contents.keyframes[keyframes[i]].time_ms;
                const auto to = std::max(from, i + 1 < keyframes.size() ? contents.keyframes[keyframes[i + 1]].time_ms : last_ms);
                times.insert({from, from + (to - from) / 3, from + (to - from) * 2 / 3, to > from ? to - 1 : to});
            }
            for (const auto time_ms : asked_ms) {
                if (time_ms >= opened.time_ms && time_ms <= last_ms) {
                    times.insert(time_ms);
                }
            }
            for (const auto time_ms : times) {
                proof.seeks++;
                const auto plan = ReplayWire::MakeSeekPlan(contents, time_ms);
                if (!plan.error.empty()) {
                    proof.refused++;
                    if (verbose) {
                        printf("chapter %zu, a seek to %u ms: refused, the toolbox says why: %s\n", chapter + 1, time_ms, plan.error.c_str());
                    }
                    continue;
                }
                const auto target_ms = plan.start_ms + plan.seek_ms;
                const auto later = std::ranges::find_if(keyframes, [&](const size_t k) {
                    return k > plan.keyframe && contents.keyframes[k].time_ms <= time_ms && ReplayWire::MakeKeyframePlan(contents, contents.keyframes[k]).error.empty();
                });
                if (plan.chapter != chapter || (target_ms != time_ms && time_ms >= plan.start_ms) || later != keyframes.end()) {
                    fail(std::format("a seek to {} ms plays chapter {} from keyframe {} at {} ms to {} ms", time_ms, plan.chapter + 1, plan.keyframe + 1, plan.start_ms, target_ms));
                    continue;
                }
                for (; next_record < end && contents.records[next_record].time_ms <= target_ms; next_record++) {
                    whole.Apply(contents.records[next_record]);
                }
                const auto fed = Fed(contents, plan, plan.seek_ms);
                const auto order = CheckOrder(contents.table, fed);
                const auto needs = CheckClientNeeds(fed);
                for (const auto& failure : order) {
                    fail(std::format("a seek to {} ms, the order of real loads: {}", time_ms, Describe(failure)));
                }
                for (const auto& need : needs) {
                    fail(std::format("a seek to {} ms, what the client needs: {}", time_ms, need));
                }
                const auto placement = ReplayWire::Placement(contents, plan, plan.seek_ms);
                proof.placed += plan.placed;
                if (plan.seek_ms && plan.placed != placement.size()) {
                    fail(std::format("a seek to {} ms posts {} placement messages, its placement holds {}", time_ms, plan.placed, placement.size()));
                }
                if (placement != ReplayWire::Placement(contents, plan, plan.seek_ms)) {
                    fail(std::format("a seek to {} ms: its placement is not the same twice", time_ms));
                }
                const auto& model_keyframe = model_keyframes.at(plan.keyframe);
                const auto model_plan = ReplayWire::MakeKeyframePlan(contents, model_keyframe);
                ReplayState::World sought(contents.table);
                for (const auto& message : Fed(contents, model_plan, plan.seek_ms)) {
                    sought.Apply(message);
                }
                ReplayState::World from_file(contents.table);
                for (const auto& message : fed) {
                    from_file.Apply(message);
                }
                const auto expected = whole.Join();
                const auto differences = Compare(expected, sought.Join());
                const auto differing = std::ranges::count_if(differences | std::views::values, &Difference::Differs);
                const auto file_differing = std::ranges::count_if(Compare(expected, from_file.Join()) | std::views::values, &Difference::Differs);
                if (verbose) {
                    printf(
                        "chapter %zu, a seek to %u ms: keyframe %zu at %u ms, then %zu stream messages at once; order failures %zu, what the client needs %zu; its state against the whole stream's at %u ms: built on the model's keyframe there, %td message ids differ; on the file's keyframe, %td (the keyframe's own, --compare's subject)\n",
                        chapter + 1, time_ms, plan.keyframe + 1, plan.start_ms, fed.size() - plan.keyframe_records, order.size(), needs.size(), target_ms, differing, file_differing
                    );
                }
                if (differing) {
                    if (verbose) {
                        PrintComparison(differences, {});
                    }
                    fail(std::format("a seek to {} ms: its state built on the model's keyframe is not the whole stream's", time_ms));
                }
                // Forward from an earlier keyframe played, the module posts its stream to the time with no load (ReplayWire::SeeksInPlace).
                for (const auto& [k, from] : playing) {
                    if (k == plan.keyframe || !ReplayWire::SeeksInPlace(from, 0, plan)) {
                        continue;
                    }
                    in_place++;
                    const auto played_ms = std::min(target_ms - from.start_ms, from.duration_ms);
                    const auto from_fed = Fed(contents, from, played_ms);
                    for (const auto& failure : CheckOrder(contents.table, from_fed)) {
                        fail(std::format("a seek to {} ms in place from keyframe {}, the order of real loads: {}", time_ms, k + 1, Describe(failure)));
                    }
                    for (const auto& need : CheckClientNeeds(from_fed)) {
                        fail(std::format("a seek to {} ms in place from keyframe {}, what the client needs: {}", time_ms, k + 1, need));
                    }
                    const auto posted = std::accumulate(from.posts.begin(), from.posts.end(), std::pair<size_t, size_t>{}, [&](std::pair<size_t, size_t> sum, const ReplayWire::Post& post) {
                        if (post.stage == ReplayWire::Stage::Stream && post.time_ms <= played_ms) {
                            sum.first += post.records;
                            sum.second += post.bytes.size();
                        }
                        return sum;
                    });
                    proof.most_records = std::max(proof.most_records, posted.first);
                    proof.most_bytes = std::max(proof.most_bytes, posted.second);
                    ReplayState::World in_place_state(contents.table);
                    for (const auto& message : Fed(contents, ReplayWire::MakeKeyframePlan(contents, model_keyframes.at(k)), played_ms)) {
                        in_place_state.Apply(message);
                    }
                    // The same time reached in place: the placement from the keyframe played is the load's for every agent the samples hold.
                    const auto loaded = Placed(placement);
                    for (const auto& [agent_id, place] : Placed(ReplayWire::Placement(contents, from, played_ms))) {
                        if (const auto found = loaded.find(agent_id); found != loaded.end() && std::hypot(found->second.first - place.first, found->second.second - place.second) > 1.f) {
                            proof.placed_apart++;
                            if (!contents.motions.empty()) {
                                fail(
                                    std::format(
                                        "a seek to {} ms in place from keyframe {}: agent {} is placed {:.0f} units from the load's placement", time_ms, k + 1, agent_id, std::hypot(found->second.first - place.first, found->second.second - place.second)
                                    )
                                );
                            }
                        }
                    }
                    const auto in_place_differences = Compare(expected, in_place_state.Join());
                    if (std::ranges::any_of(in_place_differences | std::views::values, &Difference::Differs)) {
                        in_place_differing++;
                        if (verbose) {
                            PrintComparison(in_place_differences, {});
                        }
                        fail(std::format("a seek to {} ms in place from keyframe {}: its state built on the model's keyframe is not the whole stream's", time_ms, k + 1));
                    }
                }
            }
            // Back from a later time of the chapter played, the module rewinds in place (ReplayWire::RewindsInPlace), Restart into the chapter played among them.
            std::map<uint32_t, std::pair<ReplayWire::Plan, std::vector<ReplayState::Message>>> loads;
            for (const auto time_ms : times) {
                if (auto sought = ReplayWire::MakeSeekPlan(contents, time_ms); sought.error.empty() && sought.chapter == chapter) {
                    ReplayState::World loaded(contents.table);
                    for (const auto& message : Fed(contents, sought, sought.seek_ms)) {
                        loaded.Apply(message);
                    }
                    loads.emplace(time_ms, std::pair{std::move(sought), loaded.Join()});
                }
            }
            size_t rewinds = 0;
            size_t rewinds_differing = 0;
            for (const auto& [k, from] : playing) {
                for (const auto later_ms : times) {
                    if (later_ms < from.start_ms) {
                        continue;
                    }
                    const auto played_ms = std::min(later_ms - from.start_ms, from.duration_ms);
                    const auto before = Fed(contents, from, played_ms);
                    ReplayState::ClientAgents client;
                    ReplayState::World played(contents.table);
                    for (const auto& message : before) {
                        client.Breaks(message);
                        played.Apply(message);
                    }
                    for (const auto& [time_ms, load] : loads) {
                        const auto& [sought, loaded] = load;
                        if (!ReplayWire::RewindsInPlace(from, played_ms, sought)) {
                            continue;
                        }
                        rewinds++;
                        const auto failed_before = proof.failed;
                        const auto what = std::format("a rewind to {} ms from keyframe {} played to {} ms", time_ms, k + 1, from.start_ms + played_ms);
                        const auto rewind = ReplayWire::Rewind(contents, from, played_ms, sought);
                        const auto connect = std::ranges::find(rewind, GAME_SMSG_AGENT_INSTANCE_TIMER, &ReplayState::Message::id);
                        std::set<uint32_t> destroyed;
                        constexpr uint16_t removals[] = {GAME_SMSG_QUEST_REMOVE, GAME_SMSG_EFFECT_REMOVED, GAME_SMSG_EFFECT_UPKEEP_REMOVED, GAME_SMSG_PARTY_LOCK};
                        for (auto message = rewind.begin(); message != connect; ++message) {
                            if (message->id == GAME_SMSG_AGENT_DESPAWNED) {
                                destroyed.insert(message->U(0));
                            }
                            else if (!std::ranges::contains(removals, message->id) || !destroyed.empty()) {
                                fail(std::format("{}: message {} comes before its 31, where only the removals then the 33s go", what, message->id));
                            }
                        }
                        if (connect == rewind.end()) {
                            fail(std::format("{}: no 31", what));
                        }
                        // The client asserts a 33's agent (AgMsg.cpp:316) and a 32's absence (AgAgent.cpp:162): a 33 for each agent it holds, none for another.
                        if (destroyed != client.Created()) {
                            fail(std::format("{}: 33s for {} agents, the client holds {}", what, destroyed.size(), client.Created().size()));
                        }
                        for (const auto& message : rewind) {
                            if (std::ranges::contains(std::initializer_list<uint16_t>{CharAccountNameData, MissionTeam, GAME_SMSG_ITEM_STREAM_CREATE}, message.id) ||
                                (message.id >= GAME_SMSG_INSTANCE_LOAD_SPAWN_POINT && message.id <= GAME_SMSG_INSTANCE_LOAD_INFO)) {
                                fail(std::format("{}: it feeds message {}, which a rewind leaves out", what, message.id));
                            }
                            if (message.id == GAME_SMSG_AGENT_SET_PLAYER && message.U(1) & 2) {
                                fail(std::format("{}: its 34 asks for a game view, which asserts while one is up (UiGame.cpp:497)", what));
                            }
                        }
                        if (rewind != ReplayWire::Rewind(contents, from, played_ms, sought)) {
                            fail(std::format("{}: not the same twice", what));
                        }
                        auto fed = before;
                        fed.insert(fed.end(), rewind.begin(), rewind.end());
                        const auto stream = Fed(contents, sought, sought.seek_ms);
                        fed.insert(fed.end(), stream.begin() + static_cast<std::ptrdiff_t>(JoinMessages(sought)), stream.end());
                        for (const auto& need : CheckClientNeeds(fed)) {
                            fail(std::format("{}, what the client needs: {}", what, need));
                        }
                        auto state = played;
                        for (auto message = fed.begin() + static_cast<std::ptrdiff_t>(before.size()); message != fed.end(); ++message) {
                            state.Apply(*message);
                        }
                        auto built = state.Join();
                        std::set<std::vector<uint32_t>> loaded_identities;
                        for (const auto& message : loaded) {
                            loaded_identities.insert(ReplayState::Identity(message));
                        }
                        std::set<uint32_t> created;
                        for (const auto& message : built) {
                            if (message.id == GAME_SMSG_AGENT_SPAWNED) {
                                created.insert(message.U(0));
                            }
                        }
                        // Only the teardown empties the NPC and gadget class tables and an agent's rows never made (its 33 asserts, AgMsg.cpp:316); 360 keeps nothing (f09).
                        const auto kept = std::erase_if(built, [&](const ReplayState::Message& message) {
                            const auto identity = ReplayState::Identity(message);
                            const auto defined = message.id == GAME_SMSG_NPC_UPDATE_PROPERTIES || message.id == GAME_SMSG_NPC_UPDATE_MODEL || message.id == GadgetClassDefine || message.id == ItemToClient360;
                            const auto row = std::ranges::contains(agent_row_ids, message.id) && identity.size() > 1 && !created.contains(identity[1]);
                            return (defined || row) && !loaded_identities.contains(identity);
                        });
                        // A prop's animation (270) and layers (273) have no way back to the map's default but its load: they stay until the stream sets them.
                        const auto props = std::erase_if(built, [&](const ReplayState::Message& message) {
                            return (message.id == GAME_SMSG_MANIPULATE_MAP_OBJECT || message.id == GAME_SMSG_MANIPULATE_MAP_OBJECT2) && !loaded_identities.contains(ReplayState::Identity(message));
                        });
                        proof.rewind_kept += kept;
                        proof.rewind_props += props;
                        // 34's bit 1 asks for a game view, not state; 406's spawn, plane, facing and server time (the instance's age) stay the played load's: 406 loads the map.
                        auto expected = loaded;
                        for (auto* side : {&expected, &built}) {
                            for (auto& message : *side) {
                                if (message.id == GAME_SMSG_AGENT_SET_PLAYER) {
                                    message.Set(1, message.U(1) & ~2u);
                                }
                                if (message.id == GAME_SMSG_INSTANCE_LOAD_SPAWN_POINT) {
                                    for (const auto field : {1, 2, 3, 5}) {
                                        message.fields[field] = {};
                                    }
                                }
                            }
                        }
                        const auto differences = Compare(expected, built);
                        if (std::ranges::any_of(differences | std::views::values, &Difference::Differs)) {
                            rewinds_differing++;
                            if (verbose && rewinds_differing < 4) {
                                PrintComparison(differences, {});
                            }
                            fail(std::format("{}: its state is not the one the same keyframe loaded gives", what));
                        }
                        if (verbose && asked_ms.contains(time_ms) && asked_ms.contains(later_ms)) {
                            printf("%s (asked): %zu messages, %zu failures: %s\n", what.c_str(), rewind.size(), proof.failed - failed_before, proof.failed == failed_before ? "passed" : "FAILED");
                        }
                    }
                }
                for (size_t other = 0; other < contents.chapters.size(); other++) {
                    if (const auto elsewhere = ReplayWire::MakeSeekPlan(contents, contents.chapters[other].time_ms);
                        other != chapter && elsewhere.error.empty() && elsewhere.chapter != chapter && (ReplayWire::SeeksInPlace(from, 0, elsewhere) || ReplayWire::RewindsInPlace(from, from.duration_ms, elsewhere))) {
                        fail(std::format("a seek from keyframe {} into chapter {} is planned with no load", k + 1, elsewhere.chapter + 1));
                    }
                }
            }
            proof.in_place += in_place;
            proof.rewinds += rewinds;
            if (verbose) {
                printf("chapter %zu: %zu seeks in place (forward from an earlier keyframe, no load), %zu of them not the whole stream's state\n", chapter + 1, in_place, in_place_differing);
                printf("chapter %zu: %zu rewinds in place (back from a later time, no load), %zu of them not the state the same keyframe loaded gives\n", chapter + 1, rewinds, rewinds_differing);
            }
        }
        if (verbose) {
            printf(
                "positions: %zu placement messages posted by the seeks; in place, %zu agents placed more than 1 unit from the load's placement (%s); the placement at later keyframes against their client places: with the file's samples, %s; orders only, %s\n",
                proof.placed, proof.placed_apart, contents.motions.empty() ? "no samples: orders-only estimates from different keyframes" : "a failure each", ErrorSummary(proof.sampled_errors).c_str(), ErrorSummary(proof.orders_errors).c_str()
            );
            printf(
                "seek: %zu seeks, %zu refused (each with its reason), %zu in place, %zu rewinds in place (%zu NPC and gadget class definitions and rows of agents never made kept by the client's tables, which a load empties; %zu props' states set after the time; the instance's age the played load's); the most an in-place seek posts at once: %zu stream records, %zu bytes; %zu failures: %s\n",
                proof.seeks, proof.refused, proof.in_place, proof.rewinds, proof.rewind_kept, proof.rewind_props, proof.most_records, proof.most_bytes, proof.failed, proof.failed || !proof.seeks ? "FAILED" : "passed"
            );
        }
        return proof;
    }

    void TestWorld(const std::filesystem::path& folder)
    {
        std::mt19937 random(20261124);
        ReplayFile::MessageTable table(488);
        for (uint32_t message_id = 0; message_id < table.size(); message_id++) {
            table[message_id] = {message_id};
        }
        for (const auto& [message_id, layout] : world_layouts) {
            table[message_id].insert(table[message_id].end(), layout.begin(), layout.end());
        }
        const auto records = SyntheticWorld(table, random);
        auto header = SyntheticHeader(table);
        header.duration_ms = records.back().time_ms;
        header.record_count = static_cast<uint32_t>(records.size());
        header.flags |= ReplayFile::Complete;
        const auto path = folder / L"self-test-world.gwrec";
        Check(WriteRecording(path, header, table, records, true), "the synthetic world is written");
        ReplayFile::Contents contents;
        std::string error;
        Check(ReplayFile::Read(path, contents, error) && contents.records == records, "the synthetic world reads back");

        ReplayState::World world(contents.table);
        for (const auto& record : contents.records) {
            world.Apply(record);
        }
        Check(!world.LayoutErrors(), "every synthetic record decodes");
        const auto join = world.Join();
        const auto has = [&join](const uint16_t message_id) {
            return std::ranges::any_of(join, [message_id](const ReplayState::Message& message) {
                return message.id == message_id;
            });
        };
        Check(!has(318) && !has(319) && !has(327) && !has(93) && !has(12) && !has(30), "a join holds no bag content, transient message or clock advance");
        Check(has(324) && has(328) && has(114) && has(115), "a join holds the inventories' creation, the active weapon set and the heroes");
        Check(CountLeaks(contents.table, JoinRecords(contents.table, world, 0)).Total() == 0, "a join passes the no-inventory rule");
        const auto attribute_state = std::ranges::find(join, uint16_t{55}, &ReplayState::Message::id);
        const auto connect = std::ranges::find(join, uint16_t{31}, &ReplayState::Message::id);
        Check(
            attribute_state != join.end() && attribute_state < connect && attribute_state->U(0) == 10 && attribute_state->U(2) == 35 && has(58) && has(59) && has(183) && has(182) && has(218),
            "a join gives the controlled character its attribute state (57 folded in), attributes, professions and skillbar before 31"
        );
        Check(CheckClientNeeds(join).empty(), "a join holds what the client needs before 34");
        Check(CheckOrder(contents.table, join).empty(), "a join holds the order of real loads");
        const auto info = std::ranges::find(join, uint16_t{410}, &ReplayState::Message::id);
        Check(info != join.end() && info->U(5) == 0, "a join's 410 is a player's, its observer byte left to the plan");
        const auto reporter = std::ranges::find(join, uint16_t{34}, &ReplayState::Message::id);
        Check(reporter != join.end() && reporter->U(0) == 10, "a join's reporter is the recorder's character");
        ReplayState::World replayed(contents.table);
        for (const auto& message : join) {
            replayed.Apply(message);
        }
        const auto rebuilt = Compare(join, replayed.Join());
        Check(
            std::ranges::all_of(
                rebuilt | std::views::values,
                [](const Difference& difference) {
                    return !difference.Differs() && !difference.repeated;
                }
            ),
            "the model fed its own join builds the same messages"
        );

        ReplayState::World stateless(contents.table);
        for (const auto& record : contents.records) {
            if (ReplayState::Message message; !ReplayState::Decode(contents.table, record.body, message) || message.id != CharAttributeState || message.U(0) != 10) {
                stateless.Apply(record);
            }
        }
        const auto fallback = stateless.Join();
        Check(
            stateless.ControlledAgent() == 0 && std::ranges::none_of(
                                                    fallback,
                                                    [](const ReplayState::Message& message) {
                                                        return message.id == GAME_SMSG_AGENT_SET_PLAYER || (message.id == GAME_SMSG_AGENT_CREATE_PLAYER && message.U(1) == 10) || (message.id == GAME_SMSG_AGENT_SPAWNED && message.U(0) == 10);
                                                    }
                                                ),
            "a controlled character with no attribute state is left out of the join: no 34, nor its 89 and 32"
        );
        Check(CheckClientNeeds(fallback).empty() && CheckOrder(contents.table, fallback).empty(), "the join without the controlled character holds what the client needs and the order of real loads");

        ReplayState::World allies(contents.table);
        allies.Apply(ReplayState::Message{155, {{{88}, {}}, {{}, L"01"}}});
        allies.Apply(ReplayState::Message{170, {{{88}, {}}, {{1}, {}}, {{2}, {}}}});
        allies.Apply(ReplayState::Message{30, {{{100}, {}}}});
        allies.Apply(ReplayState::Message{31, {{{100}, {}}}});
        Check(
            std::ranges::any_of(
                allies.Join(),
                [](const ReplayState::Message& message) {
                    return message.id == GAME_SMSG_AGENT_CREATE_NPC && message.U(0) == 88;
                }
            ),
            "an agent's rows stay in the model across a 30 until its 33, as the client keeps them: a party ally's 170 is there for its 62"
        );
    }
    ReplayWire::Plan Rebuilt(const ReplayFile::Contents& contents, const ReplayWire::Plan& from, const std::function<void(std::vector<std::vector<uint8_t>>&)>& change)
    {
        std::vector<std::vector<uint8_t>> bodies;
        std::vector<std::pair<ReplayWire::Stage, uint32_t>> places;
        for (const auto& post : from.posts) {
            std::string error;
            for (auto& body : Split(contents.table, post.bytes, error)) {
                bodies.push_back(std::move(body));
                places.emplace_back(post.stage, post.time_ms);
            }
        }
        change(bodies);
        auto mutant = from;
        mutant.posts.clear();
        for (size_t i = 0; i < bodies.size(); i++) {
            const auto& [stage, time_ms] = places[std::min(i, places.size() - 1)];
            mutant.posts.push_back({stage, time_ms, 1, bodies[i]});
        }
        return mutant;
    }

    void TestNameStripper()
    {
        const auto stripped = [](ReplayState::NameStripper& stripper, const uint16_t message_id, std::wstring text, const uint32_t capacity) {
            stripper.Apply(message_id, text, capacity);
            return text;
        };
        ReplayState::NameStripper both(true, true, L"Own Name");
        Check(stripped(both, 89, L"Own Name", 32) == L"Recorder", "the recorder's name becomes the placeholder");
        Check(stripped(both, 89, L"Someone Else", 32) == L"Player 1", "another player's name becomes a numbered label");
        Check(stripped(both, 89, L"Someone Else", 32) == L"Player 1", "a name keeps its label");
        Check(stripped(both, 381, L"Load Name", 20) == L"Recorder", "the instance's player name is the recorder's");
        Check(stripped(both, 314, L"Load Name", 32) == L"Recorder", "the instance's player name stays the recorder's in later messages");
        Check(stripped(both, 48, L"Account", 32) == L"Recorder", "the account's name becomes the placeholder");
        Check(stripped(both, 295, L"Account", 20) == L"Recorder", "the account's name stays the placeholder in later messages");
        Check(stripped(both, 116, L"Alt Character", 32) == L"Mercenary 1", "a mercenary hero's name becomes a numbered label");
        Check(stripped(both, 289, L"Our Guild", 32) == L"Guild 1", "a guild's name becomes a numbered label");
        Check(stripped(both, 289, L"OG", 6) == L"G1", "a guild's tag becomes a numbered tag");
        Check(stripped(both, 96, L"OG", 6) == L"G1", "a chat line's tag is a guild tag");
        Check(stripped(both, 291, L"Inviter", 20) == L"Player 2", "a guild invite's inviter is a player");
        const std::wstring encoded = {0x8102, 0x107, L'O', L'w', L'n', L' ', L'N', L'a', L'm', L'e', 0x1, 0x2};
        Check(stripped(both, 76, encoded, 128) == std::wstring{0x8102, 0x107, L'R', L'e', L'c', L'o', L'r', L'd', L'e', L'r', 0x1, 0x2}, "a name inside an encoded text is replaced");
        Check(stripped(both, 76, std::wstring{0x8102, 0x2}, 128) == std::wstring{0x8102, 0x2}, "an encoded text without a literal is kept");
        ReplayState::NameStripper others_only(true, false, L"Own Name");
        Check(stripped(others_only, 89, L"Own Name", 32) == L"Own Name", "the recorder's name is kept when only other players' are stripped");
        Check(stripped(others_only, 48, L"Account", 32) == L"Player 1", "the account's name is still hidden when only other players' are stripped");
        Check(stripped(others_only, 288, L"Their Guild", 32) == L"Guild 1", "guild names are replaced with other players' names");
        ReplayState::NameStripper own_only(false, true, L"Own Name");
        Check(stripped(own_only, 89, L"Someone Else", 32) == L"Someone Else", "other players' names are kept when only the recorder's are stripped");
        Check(stripped(own_only, 89, L"Own Name", 32) == L"Recorder", "the recorder's name is replaced alone");
        Check(stripped(own_only, 289, L"OG", 6) == L"G1", "guild tags are replaced with the recorder's names");
    }

    void TestWire(const std::filesystem::path& folder)
    {
        ReplayWire::Rc4 known;
        const uint8_t key[] = {'K', 'e', 'y'};
        InitRc4(known, key, sizeof(key));
        uint8_t text[] = {'P', 'l', 'a', 'i', 'n', 't', 'e', 'x', 't'};
        known.Apply(text, sizeof(text));
        const uint8_t expected[] = {0xBB, 0xF3, 0x16, 0xE8, 0xD9, 0x40, 0xAF, 0x0A, 0xD3};
        Check(memcmp(text, expected, sizeof(text)) == 0, "the RC4 step is the textbook cipher (key \"Key\", \"Plaintext\")");

        ReplayFile::Contents contents;
        std::string error;
        Check(ReplayFile::Read(folder / L"self-test-world.gwrec", contents, error), "the synthetic world is read for the wire");
        const auto plan = ReplayWire::MakeKeyframePlan(contents, ModelKeyframe(contents, 0, 2100));
        Check(plan.error.empty() && !plan.posts.empty(), std::format("the synthetic world has a feeder's plan from the model's keyframe after its load ({})", plan.error).c_str());
        if (plan.posts.empty()) {
            return;
        }
        Check(plan.controlled_agent == 10 && plan.map_type == ReplayWire::game_map_type, "the plan names the recorder's character and a game map");
        Check(ProveFeed(contents, plan, UINT32_MAX, false), "the synthetic world's plan holds its stages in order, what the client needs and the order of real loads");
        const auto proof = ProveWire(contents, plan, 20261125);
        Check(proof.error.empty() && proof.records > 0, std::format("the synthetic world's wire round trip is byte for byte ({})", proof.error).c_str());

        const auto guild_first = Rebuilt(contents, plan, [](std::vector<std::vector<uint8_t>>& bodies) {
            const auto guild = std::ranges::find_if(bodies, [](const std::vector<uint8_t>& body) {
                return (body[0] | body[1] << 8) == 280;
            });
            if (guild != bodies.end()) {
                std::rotate(bodies.begin(), guild, guild + 1);
            }
        });
        const auto guild_failures = CheckOrder(contents.table, Fed(contents, guild_first));
        Check(
            std::ranges::any_of(
                guild_failures,
                [](const OrderFailure& failure) {
                    return failure.field.message == 280 && failure.field.kind == ReplayState::IdKind::Guild;
                }
            ) && !ProveFeed(contents, guild_first, UINT32_MAX, false),
            "a plan with its guild message (280) moved first fails the order of real loads"
        );

        for (const auto join_ms : {10000u, 30000u, 60000u, UINT32_MAX}) {
            const auto keyframe_plan = ReplayWire::MakeKeyframePlan(contents, ModelKeyframe(contents, 0, join_ms));
            const auto failures = CheckOrder(contents.table, Fed(contents, keyframe_plan));
            Check(
                keyframe_plan.error.empty() && failures.empty() && ProveFeed(contents, keyframe_plan, UINT32_MAX, false) && ProveWire(contents, keyframe_plan, 20261126).error.empty(),
                std::format("a keyframe built by the model at {} plays: its stages in order, what the client needs, the order of real loads and the wire ({})", JoinTime(join_ms), OrderSummary(failures, JoinMessages(keyframe_plan))).c_str()
            );
        }
        const auto keyframe_plan = ReplayWire::MakeKeyframePlan(contents, ModelKeyframe(contents, 0, 30000));
        Check(
            keyframe_plan.controlled_agent == 10 && keyframe_plan.keyframe_records > 0 && std::ranges::count(keyframe_plan.changes, ReplayWire::ChangeKind::ObserverByte, &ReplayWire::Change::kind) == 1,
            "a keyframe's plan names the recorder's character and sets its 410's observer byte"
        );
        auto without_state = keyframe_plan;
        for (auto& post : without_state.posts) {
            std::string split_error;
            std::vector<uint8_t> kept;
            post.records = 0;
            for (const auto& body : Split(contents.table, post.bytes, split_error)) {
                if (ReplayState::Message message; ReplayState::Decode(contents.table, body, message) && message.id == CharAttributeState && message.U(0) == 10) {
                    continue;
                }
                kept.insert(kept.end(), body.begin(), body.end());
                post.records++;
            }
            post.bytes = std::move(kept);
        }
        Check(!ProveFeed(contents, without_state, UINT32_MAX, false), "a plan whose 34 names an agent without its attribute state fails the feed check");

        ReplayWire::Rc4 sender;
        InitRc4(sender, key, sizeof(key));
        auto receiver = sender;
        auto bytes = plan.posts.front().bytes;
        sender.Apply(bytes.data(), bytes.size());
        uint8_t skipped = 0;
        receiver.Apply(&skipped, 1);
        receiver.Apply(bytes.data(), bytes.size());
        Check(bytes != plan.posts.front().bytes, "a receive state one byte off does not decrypt the stream");
    }

    const std::vector<uint32_t> live_row_fields = {0x19e, 0x404, 0x204, 0x204, 0x104, 0x104, 0x104, 0x20c, 0x104, 0x104, 0x104, 0x104, 0x104, 0x104, 0x104, 0x204, 0x104, 0x3017};

    void TestRows(const std::filesystem::path& folder)
    {
        const auto rows_folder = folder / L"rows";
        std::error_code error_code;
        std::filesystem::remove_all(rows_folder, error_code);
        std::filesystem::create_directories(rows_folder, error_code);
        ReplayFile::MessageTable table(488);
        for (uint32_t message_id = 0; message_id < table.size(); message_id++) {
            table[message_id] = {message_id};
        }
        table[ReplayObserve::row_message_id] = live_row_fields;
        const std::vector<ReplayFile::Record> records = {{0, {12, 0}}, {40, {12, 0}}};
        const auto now = NowUnixMs();
        auto header = SyntheticHeader(table);
        header.client_build = 39167;
        header.flags |= ReplayFile::Complete;
        header.record_count = static_cast<uint32_t>(records.size());
        const auto write = [&](const wchar_t* name, const ReplayFile::Header& written, const ReplayFile::MessageTable& written_table, const bool close) {
            Check(WriteRecording(rows_folder / name, written, written_table, records, close), "a rows recording is written");
        };
        const auto write_maps = [&](const wchar_t* name, ReplayFile::Header written, const std::vector<ReplayFile::Chapter>& chapters) {
            ReplayFile::Writer writer;
            auto ok = writer.Open(rows_folder / name, written, table);
            for (const auto& chapter : chapters) {
                ok &= writer.AppendChapter(chapter) && (!chapter.is_explorable || writer.AppendKeyframe(chapter.time_ms, ReplayFile::KeyframeKind::AfterLoad, {records.front()}));
                for (const auto& record : records) {
                    ok &= writer.Append(chapter.time_ms + record.time_ms, record.body.data(), record.body.size());
                }
            }
            written.record_count = static_cast<uint32_t>(records.size() * chapters.size());
            Check(ok && writer.Close(written), "a rows recording with maps and keyframes is written");
        };
        const uint32_t listed[] = {148, 430, 857, 148, 639};
        for (size_t i = 0; i < std::size(listed); i++) {
            auto row_header = header;
            row_header.map_id = listed[i];
            row_header.start_time_unix_ms = now - (i + 1) * 3600000;
            write_maps(std::format(L"listed-{}.gwrec", i).c_str(), row_header, {{0, 0, listed[i], 1, 2}});
        }
        auto two_maps = header;
        two_maps.start_time_unix_ms = now - 70 * 3600000ull;
        write_maps(L"two-maps.gwrec", two_maps, {{0, 0, 449, 0, 10}, {0, 1000, 448, 1, 2}});
        auto other_build = header;
        other_build.client_build = 39112;
        other_build.start_time_unix_ms = now - 100 * 3600000ull;
        write(L"other-build.gwrec", other_build, table, true);
        auto unfinished = header;
        unfinished.flags &= ~ReplayFile::Complete;
        write(L"unfinished.gwrec", unfinished, table, false);
        unfinished.start_time_unix_ms = now - 110 * 3600000ull;
        write(L"cut.gwrec", unfinished, table, false);
        auto other_table = table;
        other_table[ReplayObserve::row_message_id].push_back(0x404);
        auto other_table_header = header;
        other_table_header.table_digest = ReplayFile::TableDigest(other_table);
        write(L"other-table.gwrec", other_table_header, other_table, true);
        auto outpost = header;
        outpost.is_explorable = 0;
        outpost.start_time_unix_ms = now - 50 * 3600000ull;
        write(L"outpost.gwrec", outpost, table, true);
        auto empty = header;
        empty.record_count = 0;
        Check(WriteRecording(rows_folder / L"empty.gwrec", empty, table, {}, true), "an empty recording is written");
        if (const auto file = _wfopen((rows_folder / L"not-a-recording.gwrec").c_str(), L"wb")) {
            fwrite("not a recording", 1, 15, file);
            fclose(file);
        }
        if (const auto file = _wfopen((rows_folder / L"notes.txt").c_str(), L"wb")) {
            fclose(file);
        }

        const std::set<uint32_t> server_keys = {ReplayObserve::first_key, ReplayObserve::first_key + 2};
        const auto scan = ReplayObserve::ScanFolder(rows_folder, 39167, ReplayFile::TableDigest(table), server_keys, now, rows_folder / L"unfinished.gwrec");
        Check(scan.rows.size() == std::size(listed) + 3 && scan.left_out.size() == 5, "the rows folder gives one row per recording (eight) and leaves five files out");
        if (scan.rows.empty()) {
            return;
        }
        const auto proof = ProveRows(table, scan, false);
        Check(!proof.layout_errors && !proof.disallowed_types && !proof.keys_out_of_range && !proof.duplicate_keys, "every row is valid by the live 414 table, its type allowed, its key ours and unique");
        Check(
            std::ranges::count(scan.rows, std::optional<size_t>(), &ReplayObserve::Row::keyframe) == 2 && std::ranges::count(scan.rows, std::optional<size_t>(0), &ReplayObserve::Row::keyframe) == std::ssize(listed) + 1,
            "a recording of outposts alone and one cut short before its first keyframe are listed, not playable; the others play from a keyframe"
        );
        const auto two = std::ranges::find(scan.rows, std::filesystem::path(L"two-maps.gwrec"), [](const ReplayObserve::Row& row) {
            return row.path.filename();
        });
        Check(two != scan.rows.end() && two->chapter == 1 && two->keyframe == 0 && two->maps == 2 && two->header.map_id == 448, "a recording of an outpost then an explorable map is one row of two maps, played from the explorable map's first keyframe");
        Check(
            std::ranges::none_of(
                scan.rows,
                [&server_keys](const ReplayObserve::Row& row) {
                    return server_keys.contains(row.message.key);
                }
            ),
            "no row takes a key a server row of the same list took"
        );
        Check(
            std::ranges::is_sorted(
                scan.rows, std::ranges::greater(),
                [](const ReplayObserve::Row& row) {
                    return row.header.start_time_unix_ms;
                }
            ),
            "the rows are the newest first"
        );
        Check(scan.rows.size() > 1 && scan.rows[0].message.age_minutes == 60 && scan.rows[1].message.age_minutes == 120, "a row's age is the minutes since its recording began");
        const auto left_out_for = [&scan](const wchar_t* name) {
            const auto found = std::ranges::find(scan.left_out, std::filesystem::path(name), [](const ReplayObserve::LeftOut& left_out) {
                return left_out.path.filename();
            });
            return found == scan.left_out.end() ? std::string("listed") : found->reason;
        };
        Check(left_out_for(L"other-build.gwrec") == "recorded on client build 39112, this client is build 39167", "another build's recording is left out, the builds named");
        Check(left_out_for(L"unfinished.gwrec") == "being recorded", "the recording being made is left out");
        const auto cut = std::ranges::find(scan.rows, rows_folder / L"cut.gwrec", &ReplayObserve::Row::path);
        Check(left_out_for(L"cut.gwrec") == "listed" && cut != scan.rows.end() && cut->header.duration_ms > 0, "a recording cut short with no end written (a power loss) is listed, as long as its last record");
        Check(left_out_for(L"other-table.gwrec") == "its message table is not this client's", "a recording of another message table is left out");
        Check(left_out_for(L"empty.gwrec") == "no messages", "an empty recording is left out");
        Check(left_out_for(L"not-a-recording.gwrec") == "not a recording (no magic)", "a file failing the header check is left out");
        Check(
            left_out_for(L"notes.txt") == "listed" && std::ranges::none_of(
                                                          scan.rows,
                                                          [](const ReplayObserve::Row& row) {
                                                              return row.path.extension() != L".gwrec";
                                                          }
                                                      ),
            "files other than recordings are not looked at"
        );

        const auto row = scan.rows.front().message;
        Check(!ReplayObserve::CheckRow(other_table, row).empty(), "a 414 table with one more field refuses the row");
        auto cut_table = table;
        cut_table[ReplayObserve::row_message_id].pop_back();
        Check(!ReplayObserve::CheckRow(cut_table, row).empty(), "a 414 table without the team name refuses the row");
        auto busy_table = table;
        busy_table[ReplayObserve::rows_committed_message_id].push_back(0x404);
        Check(!ReplayObserve::CheckRow(busy_table, row).empty(), "a 415 carrying fields refuses the row");
        for (const auto type : {6u, 10u, 13u}) {
            auto refused = row;
            refused.type = type;
            Check(!ReplayObserve::CheckRow(table, refused).empty(), std::format("type {} is refused", type).c_str());
        }
        auto outside = row;
        outside.key = ReplayObserve::first_key + ReplayObserve::key_span;
        Check(!ReplayObserve::CheckRow(table, outside).empty(), "a key outside our range is refused");
        auto wide = row;
        wide.map_id = 0x10000;
        Check(!ReplayObserve::CheckRow(table, wide).empty(), "a map id the 414 cannot carry is refused");
    }

    bool PlaysAll(const ReplayFile::Contents& contents, const bool verbose)
    {
        auto passed = true;
        const auto starts = PlayableStarts(contents);
        for (size_t s = 0; s < starts.plans.size(); s++) {
            const auto& plan = starts.plans[s];
            if (verbose) {
                printf("== %s\n", starts.names[s].c_str());
            }
            if (starts.Refused(s)) {
                passed &= s < starts.file_keyframes;
                continue;
            }
            passed &= ProveFeed(contents, plan, verbose ? 0 : UINT32_MAX, verbose);
            passed &= ProveWire(contents, plan, 20261127).error.empty();
        }
        return passed && !starts.plans.empty();
    }

    void TestFormat(const std::filesystem::path& folder)
    {
        ReplayFile::Contents world;
        std::string error;
        Check(ReplayFile::Read(folder / L"self-test-world.gwrec", world, error), "the synthetic world is read for the format");
        const auto stream = Filtered(world);
        Check(stream.records.size() < world.records.size() && CountLeaks(stream.table, stream.records).Total() == 0, "the no-inventory rule leaves the synthetic world's bags, gold and bag items out of its stream");
        Check(PlaysAll(stream, false) && ProveOrder(stream), "the synthetic world's stream as the recorder now keeps it plays from its load");

        const auto after_load = ModelKeyframe(stream, 0, 2100);
        const auto periodic = ModelKeyframe(stream, 0, 30000);
        auto header = world.header;
        header.flags = ReplayFile::NamesStripped | ReplayFile::ChatDropped | ReplayFile::OwnNamesStripped;
        const auto path = folder / L"self-test-v2.gwrec";
        ReplayFile::Writer writer;
        std::vector<ReplayFile::Record> written;
        std::vector<ReplayFile::Chapter> chapters;
        std::vector<ReplayFile::Keyframe> keyframes;
        auto ok = writer.Open(path, header, stream.table);
        const auto chapter = [&](const uint32_t map_id, const uint32_t explorable) {
            chapters.push_back({written.size(), written.empty() ? 0 : written.back().time_ms, map_id, explorable, 2});
            ok &= writer.AppendChapter(chapters.back());
        };
        const auto append = [&](const ReplayFile::Record& record) {
            ok &= writer.Append(record.time_ms, record.body.data(), record.body.size());
            written.push_back(record);
        };
        chapter(148, 1);
        for (size_t i = 0; i < stream.records.size(); i++) {
            for (const auto* keyframe : {&after_load, &periodic}) {
                if (keyframe->record_index == i) {
                    ok &= writer.AppendKeyframe(keyframe->time_ms, keyframe == &after_load ? ReplayFile::KeyframeKind::AfterLoad : ReplayFile::KeyframeKind::Periodic, keyframe->records);
                    keyframes.push_back(*keyframe);
                    keyframes.back().record_index = written.size();
                    keyframes.back().kind = keyframe == &after_load ? ReplayFile::KeyframeKind::AfterLoad : ReplayFile::KeyframeKind::Periodic;
                }
            }
            append(stream.records[i]);
        }
        chapter(149, 0);
        const auto offset = written.back().time_ms + 1000;
        for (const auto& record : stream.records) {
            append({record.time_ms + offset, record.body});
        }
        header.record_count = static_cast<uint32_t>(written.size());
        header.duration_ms = written.back().time_ms;
        header.flags |= ReplayFile::Complete;
        ok &= writer.Close(header);
        Check(ok, "a v2 recording with two chapters and two keyframes is written");
        ReplayFile::Contents v2;
        Check(ReplayFile::Read(path, v2, error), std::format("the v2 recording reads back ({})", error).c_str());
        Check(v2.header == header && v2.records == written, "its header and stream read back as written");
        Check(v2.chapters == chapters && v2.keyframes == keyframes, "its chapters and keyframes read back as written, each before the record it was written before");
        ReplayFile::Contents index;
        Check(
            ReplayFile::Read(path, index, error, false) && index.chapters == chapters && index.keyframes.size() == keyframes.size() && index.records.size() == written.size() && index.keyframes[1].records.empty(),
            "its index (no bodies) holds its chapters, keyframes and record times"
        );
        Check(PlaysAll(v2, false), "every start of the v2 recording plays: each keyframe and the model's after each load, their stages in order, what the client needs, the order of real loads and the wire");
        Check(ProveOrder(v2), "the v2 recording's starts and model keyframes hold the order of real loads");
        Check(ProveCompare(v2) == 0, "the v2 recording compares: its loads and keyframes against the model, no bag content, its names placeholders");
        auto named = v2;
        for (auto& record : named.keyframes.front().records) {
            if (ReplayState::Message message; ReplayState::Decode(named.table, record.body, message) && message.id == GAME_SMSG_INSTANCE_LOAD_PLAYER_NAME && !message.fields.empty()) {
                message.fields[0].text = L"Real Name";
                ReplayState::Encode(named.table, message, record.body);
            }
        }
        Check(CountNames(named.table, named.keyframes.front().records, named.header.flags).not_placeholders == 1, "a name that is not a placeholder in a keyframe is found, by count");
        auto no_map_file = v2.keyframes.front();
        for (auto& record : no_map_file.records) {
            if (ReplayState::Message message; ReplayState::Decode(v2.table, record.body, message) && message.id == GAME_SMSG_INSTANCE_LOAD_SPAWN_POINT) {
                message.Set(0, 0);
                ReplayState::Encode(v2.table, message, record.body);
            }
        }
        Check(!ReplayWire::MakeKeyframePlan(v2, no_map_file).error.empty(), "a keyframe whose 406 names no map file is refused: the game's map load asserts on it");

        constexpr uint32_t fighter = 10;
        const auto& mid_fight = v2.keyframes[1];
        ReplayState::World snapshot(v2.table);
        size_t model_intervals = 0;
        for (const auto& message : Decoded(v2.table, mid_fight.records)) {
            if (message.id == CharAttackInterval) {
                model_intervals++;
                continue;
            }
            snapshot.Apply(message);
        }
        snapshot.Apply(ReplayState::Message{53, {N(fighter), N(std::bit_cast<uint32_t>(1.33f)), N(std::bit_cast<uint32_t>(1.f))}});
        auto from_writer = mid_fight;
        from_writer.records = JoinRecords(v2.table, snapshot, mid_fight.time_ms);
        const auto written_messages = Decoded(v2.table, from_writer.records);
        const auto created_at = std::ranges::find_if(written_messages, [](const ReplayState::Message& message) {
            return message.id == GAME_SMSG_AGENT_SPAWNED && message.U(0) == fighter;
        });
        const auto interval_at = std::ranges::find_if(written_messages, [](const ReplayState::Message& message) {
            return message.id == CharAttackInterval;
        });
        const auto attacks_after = std::ranges::count_if(v2.records.begin() + static_cast<std::ptrdiff_t>(mid_fight.record_index), v2.records.begin() + static_cast<std::ptrdiff_t>(v2.ChapterEnd(0)), [&](const ReplayFile::Record& record) {
            ReplayState::Message message;
            return ReplayState::Decode(v2.table, record.body, message) && message.id == GAME_SMSG_AGENT_ATTR_UPDATE_INT_TARGET && message.U(0) == 4 && message.U(1) == fighter;
        });
        Check(
            model_intervals == 1 && attacks_after > 0 && created_at < interval_at && interval_at != written_messages.end() && ReplayWire::MakeKeyframePlan(v2, from_writer).error.empty() && CheckClientNeeds(written_messages).empty() &&
                StreamNeeds(v2, from_writer).empty(),
            std::format("a keyframe taken in a fight, the attacker's 53 written after its 32 as the fixed writer does, plays its stream's {} attacks with no assert broken", attacks_after).c_str()
        );
        auto unset = mid_fight;
        std::erase_if(unset.records, [](const ReplayFile::Record& record) {
            return record.MessageId() == CharAttackInterval;
        });
        const auto unset_plan = ReplayWire::MakeKeyframePlan(v2, unset);
        const auto unset_needs = StreamNeeds(v2, unset);
        Check(
            unset_plan.error.contains("(message 160)") && unset_plan.error.contains("agent 10 starts an attack") && unset_plan.error.contains("AvChar.cpp:4791") && !unset_needs.empty() &&
                std::ranges::all_of(
                    unset_needs,
                    [](const std::pair<size_t, std::string>& need) {
                        return need.second.contains("(message 160)") && need.second.contains("agent 10 ");
                    }
                ),
            "a keyframe taken in a fight with no 53, as the writer before made them, is refused and fails the check, by record and agent"
        );

        const ReplayState::ClientItem client_item = {
            .item_id = 100,
            .model_file = 700,
            .type = 2,
            .formula = 485,
            .multiplier = 1,
            .interaction = 0x22000040,
            .value = 12,
            .model_id = 1197,
            .quantity = 1,
            .name = L"\x108\x107Player 3\x1",
            .code = {0x2400, 0x2500, 0xC0000000},
            .profession = 11,
            .customizer = L"Recorder",
        };
        const auto report = ReplayState::ItemReport(v2.table, client_item);
        Check(
            report.size() == 2 && report[0].U(5) == 486 && report[0].U(6) == 1 && report[0].U(12) == 2 && report[0].fields.size() == 15 && report[0].U(14) == 0x2500 && report[1].id == GAME_SMSG_ITEM_UPDATE_NAME,
            "the fixed writer gives an item's 353 as the wire's from the client's memory: its formula plus one, its mods without the item code's terminator, no 346 for profession 11 (any), its customizer as a 314"
        );
        auto no_formula = client_item;
        no_formula.formula = 0x5E5;
        no_formula.code = {0xC0000000};
        no_formula.profession = 3;
        const auto bare = ReplayState::ItemReport(v2.table, no_formula);
        Check(
            bare.size() == 3 && bare[0].U(5) == 0 && bare[0].U(12) == 0 && bare[0].fields.size() == 13 && bare[1].id == ItemProfession && bare[1].U(1) == 3,
            "the fixed writer gives formula 0 for the client's 0x5E5 (none), no mod for a lone terminator, and a 346 for a profession"
        );

        auto from_memory = v2.keyframes.front();
        size_t rewritten = 0;
        for (auto& record : from_memory.records) {
            ReplayState::Message message;
            if (!ReplayState::Decode(v2.table, record.body, message) || message.id != GAME_SMSG_ITEM_GENERAL_INFO) {
                continue;
            }
            ReplayState::ClientItem kept = {
                .item_id = message.U(0),
                .model_file = message.U(1),
                .type = message.U(2),
                .dye_tint = message.U(3),
                .dye_colours = message.U(4),
                .formula = message.U(5) ? message.U(5) - 1 : 0x5E5,
                .multiplier = message.U(5) ? message.U(6) : 0,
                .interaction = message.U(7),
                .value = message.U(8),
                .model_id = message.U(9),
                .quantity = message.U(10),
                .name = message.fields[11].text,
                .code = {},
                .profession = 11,
                .customizer = {},
            };
            for (size_t mod = 13; mod < message.fields.size(); mod++) {
                kept.code.push_back(message.U(mod));
            }
            kept.code.push_back(0xC0000000);
            const auto written = ReplayState::ItemReport(v2.table, kept);
            rewritten += !written.empty() && ReplayState::Encode(v2.table, written.front(), record.body);
        }
        Check(
            rewritten > 0 && ReplayWire::MakeKeyframePlan(v2, from_memory).error.empty() && CheckClientNeeds(Decoded(v2.table, from_memory.records)).empty(),
            std::format("a keyframe whose {} items the fixed writer made from the client's memory plays and breaks none of the game's asserts", rewritten).c_str()
        );
        auto terminated = v2.keyframes.front();
        for (auto& record : terminated.records) {
            if (ReplayState::Message message; ReplayState::Decode(v2.table, record.body, message) && message.id == GAME_SMSG_ITEM_GENERAL_INFO) {
                message.Set(12, message.U(12) + 1);
                message.fields.push_back({{0xC0000000}, {}});
                ReplayState::Encode(v2.table, message, record.body);
            }
        }
        const auto terminated_plan = ReplayWire::MakeKeyframePlan(v2, terminated);
        const auto terminated_needs = CheckClientNeeds(Decoded(v2.table, terminated.records));
        Check(
            terminated_plan.error.contains("(id 353)") && terminated_plan.error.contains("f12") && !terminated_needs.empty() &&
                std::ranges::all_of(
                    terminated_needs,
                    [](const std::string& need) {
                        return need.contains("(message 353)") && need.contains("ItemCode.h:516");
                    }
                ),
            "a keyframe whose 353s end their mods with the item code's terminator, as live test 7's writer made them, is refused and fails the check, by id and field"
        );

        ReplayState::InventoryFilter filter(v2.table);
        std::vector<ReplayState::Message> shown_before;
        std::vector<uint32_t> unknown_items;
        const ReplayState::Message load_head{380, {N(1), N(2), N(3), N(4)}};
        const ReplayState::Message worn{110, {N(10), N(100), N(0), N(0), N(0), N(0), N(0), N(0), N(0), N(0)}};
        const auto first_map = filter.Keep(load_head, shown_before, unknown_items) && filter.Keep(worn, shown_before, unknown_items) && filter.Keep(report[0], shown_before, unknown_items);
        Check(
            first_map && filter.Keep(load_head, shown_before, unknown_items) && !filter.Keep(report[0], shown_before, unknown_items),
            "an item shown in one map is not in the next: the same id's details there are held until shown (item ids are the instance's)"
        );
        std::vector<ReplayFile::Record> two_maps;
        for (const auto* message : {&load_head, &worn, &report[0], &load_head, &report[0]}) {
            ReplayFile::Record record;
            ReplayState::Encode(v2.table, *message, record.body);
            two_maps.push_back(std::move(record));
        }
        Check(CountLeaks(v2.table, two_maps).held_details == 1, "the no-inventory check counts the details of an item a later map never shows, though an earlier one showed its id");

        auto without_item = v2.keyframes[1];
        uint32_t taken = 0;
        for (uint32_t item = 100; item < 105 && !taken; item++) {
            const auto names_it = [&](const ReplayFile::Record& record) {
                ReplayState::Message message;
                return ReplayState::Decode(v2.table, record.body, message) && message.id != GAME_SMSG_ITEM_GENERAL_INFO && std::ranges::any_of(ReplayState::IdFields(v2.table, message.id), [&](const ReplayState::IdField& field) {
                           return field.kind == ReplayState::IdKind::Item && field.field < message.fields.size() && std::ranges::contains(message.fields[field.field].numbers, item);
                       });
            };
            const auto detailed_before = std::ranges::any_of(v2.records.begin(), v2.records.begin() + static_cast<std::ptrdiff_t>(without_item.record_index), [&](const ReplayFile::Record& record) {
                ReplayState::Message message;
                return ReplayState::Decode(v2.table, record.body, message) && message.id == GAME_SMSG_ITEM_GENERAL_INFO && message.U(0) == item;
            });
            const auto shown_after = std::ranges::any_of(v2.records.begin() + static_cast<std::ptrdiff_t>(without_item.record_index), v2.records.begin() + static_cast<std::ptrdiff_t>(v2.ChapterEnd(0)), names_it);
            if (detailed_before && shown_after && std::ranges::none_of(without_item.records, names_it)) {
                std::erase_if(without_item.records, [&](const ReplayFile::Record& record) {
                    ReplayState::Message message;
                    return ReplayState::Decode(v2.table, record.body, message) && message.id == GAME_SMSG_ITEM_GENERAL_INFO && message.U(0) == item;
                });
                taken = item;
            }
        }
        const auto brought_plan = ReplayWire::MakeKeyframePlan(v2, without_item);
        Check(
            taken && brought_plan.error.empty() && std::ranges::count(brought_plan.changes, ReplayWire::ChangeKind::BroughtIn, &ReplayWire::Change::kind) > 0 && CheckOrder(v2.table, Fed(v2, brought_plan)).empty(),
            std::format("a record showing an item the keyframe lacks gets the item's details from the stream before the keyframe (item {})", taken).c_str()
        );
        const auto insert = [](ReplayFile::Contents& contents, const size_t at, const uint16_t id, const std::vector<uint32_t>& values) {
            auto message = ReplayState::Blank(contents.table, id);
            size_t field = 0;
            for (const auto value : values) {
                message.Set(field++, value);
            }
            std::vector<uint8_t> body;
            ReplayState::Encode(contents.table, message, body);
            contents.records.insert(contents.records.begin() + static_cast<std::ptrdiff_t>(at), {contents.records[at].time_ms, std::move(body)});
            for (auto& chapter : contents.chapters) {
                chapter.first_record += chapter.first_record > at;
            }
            for (auto& keyframe : contents.keyframes) {
                keyframe.record_index += keyframe.record_index >= at;
            }
        };
        constexpr uint32_t gadget_agent = 9001;
        constexpr uint32_t gadget_class = 901;
        auto gadget = v2;
        insert(gadget, gadget.keyframes[1].record_index, 263, {gadget_class, 0x1234, 0, 100});
        insert(gadget, gadget.keyframes[1].record_index, 32, {gadget_agent, gadget_class, 2, 1});
        insert(gadget, gadget.keyframes[1].record_index + 3, 33, {gadget_agent});
        insert(gadget, gadget.keyframes[1].record_index + 4, 32, {gadget_agent, gadget_class, 2, 1});
        const auto gadget_plan = ReplayWire::MakeKeyframePlan(gadget, gadget.keyframes[1]);
        const auto gadget_named = std::format("agent {} is a gadget of class {} that no 263 defined", gadget_agent, gadget_class);
        const auto names_gadget = [&gadget_named](const std::string& what) {
            return what.contains(gadget_named) && what.contains("File.cpp:367");
        };
        const auto gadget_stream = StreamNeeds(gadget, gadget.keyframes[1]);
        auto with_class = gadget;
        auto class_define = ReplayState::Blank(with_class.table, GadgetClassDefine);
        for (const auto [field, value] : {std::pair<size_t, uint32_t>{0, gadget_class}, {1, 0x1234}, {3, 100}}) {
            class_define.Set(field, value);
        }
        std::vector<uint8_t> class_body;
        ReplayState::Encode(with_class.table, class_define, class_body);
        auto& with_class_records = with_class.keyframes[1].records;
        with_class_records.insert(with_class_records.end() - 1, {with_class.keyframes[1].time_ms, class_body});
        const auto with_class_plan = ReplayWire::MakeKeyframePlan(with_class, with_class.keyframes[1]);
        Check(
            names_gadget(gadget_plan.error) && std::ranges::any_of(gadget_stream | std::views::values, names_gadget) && with_class_plan.error.empty() && CheckClientNeeds(Fed(with_class, with_class_plan)).empty() &&
                StreamNeeds(with_class, with_class.keyframes[1]).empty(),
            "a gadget made again after a keyframe that lacks its class (every keyframe before this session's writer) is refused, by record, agent and class; with the class written in the keyframe, as the client's table gives it, it plays"
        );
        auto dressed = v2;
        const auto mercenary =
            std::ranges::find_if(dressed.records.begin() + static_cast<std::ptrdiff_t>(dressed.chapters[0].first_record), dressed.records.begin() + static_cast<std::ptrdiff_t>(dressed.keyframes[0].record_index), [&](const ReplayFile::Record& record) {
                ReplayState::Message message;
                return ReplayState::Decode(dressed.table, record.body, message) && message.id == GAME_SMSG_AGENT_SPAWNED && message.U(2) == 1 && (message.U(1) & 0xF0000000) == 0x20000000;
            });
        Check(mercenary != dressed.records.begin() + static_cast<std::ptrdiff_t>(dressed.keyframes[0].record_index), "the v2 file's load creates an NPC agent before its first keyframe");
        ReplayState::Message mercenary_create;
        ReplayState::Decode(dressed.table, mercenary->body, mercenary_create);
        const auto dressed_agent = mercenary_create.U(0);
        const auto at = static_cast<size_t>(mercenary - dressed.records.begin());
        const std::vector<std::vector<uint32_t>> look = {{159, 0, dressed_agent, 0x4b592115}, {159, 14, dressed_agent, 0x906e4}, {159, 15, dressed_agent, 10}, {160, 14, dressed_agent, 1, 0x906e6}, {160, 15, dressed_agent, 1, 10}};
        for (size_t i = 0; i < look.size(); i++) {
            const auto& values = look[i];
            insert(dressed, at + i, static_cast<uint16_t>(values[0]), {values.begin() + 1, values.end()});
        }
        ReplayState::World dressed_world(dressed.table);
        for (auto i = dressed.chapters[0].first_record; i < dressed.keyframes[0].record_index; i++) {
            dressed_world.Apply(dressed.records[i]);
        }
        const auto dressed_join = dressed_world.Join();
        const auto create_at = std::ranges::find_if(dressed_join, [&](const ReplayState::Message& message) {
            return message.id == GAME_SMSG_AGENT_SPAWNED && message.U(0) == dressed_agent;
        });
        const auto looks_before = std::ranges::count_if(dressed_join.begin(), create_at, [&](const ReplayState::Message& message) {
            return (message.id == GAME_SMSG_AGENT_ATTR_UPDATE_INT || message.id == GAME_SMSG_AGENT_ATTR_UPDATE_INT_TARGET) && message.U(1) == dressed_agent && std::ranges::contains(std::initializer_list<uint32_t>{0, 14, 15}, message.U(0));
        });
        auto undressed_join = dressed_join;
        std::erase_if(undressed_join, [&](const ReplayState::Message& message) {
            return (message.id == GAME_SMSG_AGENT_ATTR_UPDATE_INT || message.id == GAME_SMSG_AGENT_ATTR_UPDATE_INT_TARGET) && message.U(1) == dressed_agent && std::ranges::contains(std::initializer_list<uint32_t>{0, 14, 15}, message.U(0));
        });
        auto past_slots = ReplayState::Blank(dressed.table, GAME_SMSG_AGENT_ATTR_UPDATE_INT_TARGET);
        for (const auto [field, value] : {std::pair<size_t, uint32_t>{0, 14}, {1, dressed_agent}, {2, 5}, {3, 0x906e6}}) {
            past_slots.Set(field, value);
        }
        Check(
            create_at != dressed_join.end() && looks_before == static_cast<std::ptrdiff_t>(look.size()) && !PrintComparison(Compare(dressed_join, undressed_join), keyframe_differences) && ReplayState::FieldAssert(past_slots).contains("ChCliApi.cpp:2156"),
            "a mercenary's look (159 kinds 0, 14, 15 and 160 kinds 14, 15) is kept by the model and joined before its agent's 32; a keyframe without it fails --compare; a sixth armour slot breaks the game's assert"
        );
        const auto seeks = ProveSeek(v2, false);
        Check(seeks.seeks > 0 && !seeks.failed, std::format("every seek plan of the v2 file ({} seeks) passes the order checks and its state is the whole stream's at its time", seeks.seeks).c_str());
        Check(seeks.in_place > 0, std::format("forward seeks from an earlier keyframe play in place, with no load, and reach the whole stream's state ({} seeks)", seeks.in_place).c_str());
        const auto sought = ReplayWire::MakeSeekPlan(v2, v2.keyframes.back().time_ms + 1000);
        auto sought_fed = Fed(v2, sought, sought.seek_ms);
        sought_fed.resize(sought_fed.size() - sought.placed);
        ReplayState::World all(v2.table);
        ReplayState::World one_short(v2.table);
        for (size_t i = 0; i < sought_fed.size(); i++) {
            all.Apply(sought_fed[i]);
            if (i + 1 < sought_fed.size()) {
                one_short.Apply(sought_fed[i]);
            }
        }
        Check(
            sought.error.empty() && sought_fed.size() > sought.keyframe_records && std::ranges::any_of(Compare(all.Join(), one_short.Join()) | std::views::values, &Difference::Differs),
            "a seek's state without its last message differs from the whole stream's"
        );
        Check(
            ReplayWire::MakeSeekPlan(v2, 0).keyframe == 0 && ReplayWire::MakeSeekPlan(v2, v2.chapters[1].time_ms - 1).keyframe + 1 == v2.keyframes.size() && !ReplayWire::MakeSeekPlan(v2, v2.chapters[1].time_ms).error.empty(),
            "a seek to the start plays the first keyframe, to the explorable map's end its last, into an outpost with no explorable map after it nothing"
        );
        auto other = v2;
        other.header.flags = 0;
        Check(CountNames(other.table, named.keyframes.front().records, other.header.flags).not_placeholders == 0, "with the names settings off, names are not checked");
    }

    void TestPositions(const std::filesystem::path& folder)
    {
        ReplayFile::Contents plain;
        std::string error;
        Check(ReplayFile::Read(folder / L"self-test-v2.gwrec", plain, error) && plain.keyframes.size() > 1 && plain.motions.empty(), "the recording without samples is read for the positions");
        auto plan = ReplayWire::MakeKeyframePlan(plain, plain.keyframes[1]);
        plan.keyframe = 1;
        const auto sample_record = plain.keyframes[1].record_index + 40;
        // The client's own walks, sampled as the recorder does: each living agent off the orders' straight line, on a detour leg before its order's point.
        constexpr float off_x = 37.f;
        constexpr float off_y = -21.f;
        constexpr float leg_x = 200.f;
        constexpr float leg_y = 100.f;
        constexpr float pace = 288.f;
        ReplayFile::Motions sample{sample_record, plain.records[sample_record].time_ms, plain.records[sample_record].time_ms, {}};
        const auto model = ReplayWire::Placement(plain, plan, plain.records[sample_record - 1].time_ms - plan.start_ms);
        for (const auto& message : model) {
            if (message.id != GAME_SMSG_AGENT_UPDATE_POSITION) {
                continue;
            }
            const auto x = message.F(1, 0) + off_x;
            const auto y = message.F(1, 1) + off_y;
            sample.agents.push_back({message.U(0), x, y, message.U(2), pace, 1.f, 0, true, x + leg_x, y + leg_y, message.U(2), x + 3 * leg_x, y + 3 * leg_y, message.U(2), 0});
        }
        auto sampled = plain;
        sampled.motions = {sample};
        const auto path = folder / L"self-test-positions.gwrec";
        ReplayFile::Writer writer;
        auto ok = writer.Open(path, plain.header, plain.table);
        for (size_t i = 0; i <= plain.records.size(); i++) {
            for (const auto& chapter : plain.chapters) {
                ok &= chapter.first_record != i || writer.AppendChapter(chapter);
            }
            for (const auto& keyframe : plain.keyframes) {
                ok &= keyframe.record_index != i || writer.AppendKeyframe(keyframe.time_ms, keyframe.kind, keyframe.records);
            }
            ok &= i != sample_record || writer.AppendMotions(sample.time_ms, sample.clock, sample.agents);
            ok &= i == plain.records.size() || writer.Append(plain.records[i].time_ms, plain.records[i].body.data(), plain.records[i].body.size());
        }
        ok &= writer.Close(plain.header);
        ReplayFile::Contents read;
        Check(
            ok && ReplayFile::Read(path, read, error) && read.motions == sampled.motions && read.records == plain.records && read.keyframes == plain.keyframes,
            "samples written by the recorder's writer (AppendMotions) read back as written, before the record they were written before"
        );

        // Each agent the stream leaves alone after the sample walks its leg, then on to its order's point, at its pace: the client's straight walk.
        std::set<uint32_t> moved;
        uint32_t walked_ms = 0;
        std::vector<std::pair<uint32_t, std::map<uint32_t, std::pair<float, float>>>> expected_at;
        for (auto i = sample_record; i < plain.ChapterEnd(0) && expected_at.size() < 6; i++) {
            ReplayState::Message message;
            ReplayState::Decode(plain.table, plain.records[i].body, message);
            if (message.id == GAME_SMSG_AGENT_MOVEMENT_TICK) {
                walked_ms += message.U(0);
            }
            else if (message.id >= 32 && message.id <= 46) {
                moved.insert(message.U(0));
            }
            if (i + 1 < plain.records.size() && plain.records[i + 1].time_ms == plain.records[i].time_ms) {
                continue;
            }
            if (i % 13) {
                continue;
            }
            std::map<uint32_t, std::pair<float, float>> expected;
            for (const auto& motion : sample.agents) {
                if (moved.contains(motion.agent)) {
                    continue;
                }
                auto left = pace * static_cast<float>(walked_ms) / 1000.f;
                const auto leg = std::hypot(leg_x, leg_y);
                const auto t = std::min(left, leg) / leg;
                left = std::max(left - leg, 0.f);
                auto x = motion.x + leg_x * t;
                auto y = motion.y + leg_y * t;
                const auto rest = std::hypot(2 * leg_x, 2 * leg_y);
                x += 2 * leg_x * std::min(left, rest) / rest;
                y += 2 * leg_y * std::min(left, rest) / rest;
                expected[motion.agent] = {x, y};
            }
            expected_at.emplace_back(plain.records[i].time_ms, std::move(expected));
        }
        size_t agents = 0;
        size_t on_place = 0;
        size_t estimate_apart = 0;
        size_t in_place_apart = 0;
        uint64_t digest = 0xCBF29CE484222325;
        auto later = ReplayWire::MakeKeyframePlan(read, read.keyframes[0]);
        later.keyframe = 0;
        for (const auto& [time_ms, expected] : expected_at) {
            const auto placed = Placed(ReplayWire::Placement(read, plan, time_ms - plan.start_ms));
            const auto estimated = Placed(ReplayWire::Placement(plain, plan, time_ms - plan.start_ms));
            const auto from_earlier = Placed(ReplayWire::Placement(read, later, time_ms - later.start_ms));
            for (const auto& [agent_id, place] : expected) {
                agents++;
                const auto near = [&place](const std::map<uint32_t, std::pair<float, float>>& at, const uint32_t id) {
                    const auto found = at.find(id);
                    return found != at.end() && std::hypot(found->second.first - place.first, found->second.second - place.second) < 0.1f;
                };
                on_place += near(placed, agent_id);
                estimate_apart += !near(estimated, agent_id);
                in_place_apart += !near(from_earlier, agent_id);
            }
            for (const auto& post : ReplayWire::PlacementPosts(read, plan, time_ms - plan.start_ms)) {
                for (const auto byte : post.bytes) {
                    digest = (digest ^ byte) * 0x100000001B3;
                }
            }
        }
        const auto again = Placed(ReplayWire::Placement(read, plan, expected_at.back().first - plan.start_ms));
        Check(agents > 5 && on_place == agents, std::format("every seek's placement after the sample is the sample walked on by its leg and order ({} of {} agents at {} times)", on_place, agents, expected_at.size()).c_str());
        Check(in_place_apart == 0 && again == Placed(ReplayWire::Placement(read, plan, expected_at.back().first - plan.start_ms)), "the same time from an earlier keyframe (in place) gives the same placement, and the same seek twice the same");
        Check(estimate_apart == agents, std::format("without samples the placement is the orders' estimate, not the client's: {} of {} agents off", estimate_apart, agents).c_str());
        const auto sought = ReplayWire::MakeSeekPlan(read, expected_at.back().first);
        Check(sought.error.empty() && sought.placed > 0 && CheckClientNeeds(Fed(read, sought, sought.seek_ms)).empty(), "a seek's plan posts its placement after the stream to its time, every agent it names in the client");
        printf("the placement's digest at the self-test's seek times: %016llx (the same every run)\n", static_cast<unsigned long long>(digest));
    }

    int SelfTest(const std::filesystem::path& folder)
    {
        std::error_code error_code;
        std::filesystem::create_directories(folder, error_code);
        TestPacking();
        TestNameStripper();
        TestWorld(folder);
        TestRows(folder);
        TestWire(folder);
        TestFormat(folder);
        TestPositions(folder);

        std::mt19937 random(20261123);
        const auto table = SyntheticTable(random);
        const auto records = SyntheticRecords(random, 20000, static_cast<uint16_t>(table.size()));
        auto header = SyntheticHeader(table);
        header.duration_ms = records.back().time_ms;
        header.record_count = static_cast<uint32_t>(records.size());
        header.layout_errors = 3;
        auto finished = header;
        finished.flags |= ReplayFile::Complete;

        const auto complete_path = folder / L"self-test-complete.gwrec";
        Check(WriteRecording(complete_path, finished, table, records, true), "the complete recording is written");
        ReplayFile::Contents contents;
        std::string error;
        Check(ReplayFile::Read(complete_path, contents, error), std::format("the complete recording reads back ({})", error).c_str());
        Check(contents.header == finished, "its header reads back as written");
        Check(contents.table == table, "its message table reads back as written");
        Check(contents.records == records, "its records read back as written");
        Check(!contents.truncated_tail, "its last record is whole");

        const auto unfinished_path = folder / L"self-test-unfinished.gwrec";
        Check(WriteRecording(unfinished_path, header, table, records, false), "the unfinished recording is written");
        if (const auto file = _wfopen(unfinished_path.c_str(), L"ab")) {
            fwrite("\x10\x00\x00\x00\x40\x00\x05\x00", 1, 8, file);
            fclose(file);
        }
        ReplayFile::Contents unfinished;
        Check(ReplayFile::Read(unfinished_path, unfinished, error), "an unfinished recording reads back");
        Check(!(unfinished.header.flags & ReplayFile::Complete) && unfinished.truncated_tail && unfinished.records == records, "an unfinished recording keeps every whole record and reports the cut one");

        auto corrupt = finished;
        corrupt.table_digest ^= 1;
        const auto corrupt_path = folder / L"self-test-corrupt.gwrec";
        Check(WriteRecording(corrupt_path, corrupt, table, records, true), "the corrupt recording is written");
        ReplayFile::Contents refused;
        Check(!ReplayFile::Read(corrupt_path, refused, error), "a recording whose table does not match its digest is refused");

        printf("\nself-test on %ls: %s\n", folder.c_str(), failures ? "FAILED" : "passed");
        return failures ? 1 : 0;
    }
} // namespace

int wmain(const int argc, const wchar_t* argv[])
{
    if (argc == 3 && wcscmp(argv[1], L"--self-test") == 0) {
        return SelfTest(argv[2]);
    }
    if ((argc == 3 || argc == 4) && wcscmp(argv[1], L"--rows") == 0) {
        return Rows(argv[2], argc == 4 ? static_cast<uint32_t>(_wtoi(argv[3])) : 0);
    }
    const auto command = argc > 2 ? std::wstring(argv[1]) : L"";
    const auto arguments = command == L"--compare" || command == L"--wire" || command == L"--order" || command == L"--seek" ? 3 : command == L"--feed" ? 4 : 0;
    if (!arguments || (command == L"--seek" ? argc < arguments : argc != arguments)) {
        printf(
            "usage: replay_check --compare <recording.gwrec>\n"
            "       replay_check --feed <recording.gwrec> <ms>\n"
            "       replay_check --wire <recording.gwrec>\n"
            "       replay_check --order <recording.gwrec>\n"
            "       replay_check --seek <recording.gwrec> [ms...]\n"
            "       replay_check --rows <folder> [client build]\n"
            "       replay_check --self-test <folder>\n"
        );
        return 2;
    }
    ReplayFile::Contents contents;
    std::string error;
    if (!ReplayFile::Read(argv[2], contents, error)) {
        printf("%ls: %s\n", argv[2], error.c_str());
        return 1;
    }
    if (command == L"--compare") {
        return ProveCompare(contents);
    }
    if (command == L"--order") {
        return ProveOrder(contents) ? 0 : 1;
    }
    if (command == L"--seek") {
        std::set<uint32_t> asked_ms;
        for (auto a = arguments; a < argc; a++) {
            asked_ms.insert(static_cast<uint32_t>(_wtoi(argv[a])));
        }
        const auto proof = ProveSeek(WithModelKeyframes(contents, true), true, asked_ms);
        return proof.failed || !proof.seeks ? 1 : 0;
    }
    const auto starts = PlayableStarts(contents);
    auto passed = !starts.plans.empty();
    for (size_t s = 0; s < starts.plans.size(); s++) {
        const auto& plan = starts.plans[s];
        if (starts.Refused(s)) {
            passed &= s < starts.file_keyframes;
            continue;
        }
        if (command == L"--wire") {
            const auto proof = ProveWire(contents, plan, 20261123);
            printf(
                "%s: wire: %zu posts, %zu messages, %zu bytes encrypted post by post, decrypted, cut back into messages and encoded again: %s%s\n", starts.names[s].c_str(), proof.posts, proof.records, proof.bytes,
                proof.error.empty() ? "passed, byte for byte" : "FAILED: ", proof.error.c_str()
            );
            passed &= proof.error.empty();
            continue;
        }
        printf("== %s\n", starts.names[s].c_str());
        passed &= ProveFeed(contents, plan, static_cast<uint32_t>(_wtoi(argv[3])), true);
    }
    printf("%s: %s\n", command == L"--wire" ? "wire" : "feed", passed ? "passed" : "FAILED");
    return passed ? 0 : 1;
}
