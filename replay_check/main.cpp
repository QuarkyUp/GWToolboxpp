#include <Utils/ReplayFile.h>
#include <Utils/ReplayObserve.h>
#include <Utils/ReplayState.h>
#include <Utils/ReplayWire.h>

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
        {318, {0x204, 0x404, 0x204, 0x104}},
        {319, {0x204, 0x104, 0x104, 0x204, 0x104, 0x404}},
        {324, {0x204, 0x104}},
        {327, {0x204, 0x104, 0x404, 0x404}},
        {328, {0x204, 0x104}},
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
        write(409, {N(2)});
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
        for (uint32_t tick = 0; tick < 600; tick++) {
            write.time_ms += 100;
            write(30, {N(100)});
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
        const auto scan = ReplayObserve::ScanFolder(folder, contents.header.client_build, contents.header.table_digest, {}, NowUnixMs());
        const auto proof = ProveRows(contents.table, scan, true);
        const auto passed = proof.rows && !proof.layout_errors && !proof.disallowed_types && !proof.keys_out_of_range && !proof.duplicate_keys;
        printf(
            "\nrows %zu, left out %zu; layout errors %zu, types not allowed %zu, keys outside %#x to %#x %zu, keys not unique %zu: %s\n", proof.rows, scan.left_out.size(), proof.layout_errors, proof.disallowed_types, ReplayObserve::first_key,
            ReplayObserve::first_key + ReplayObserve::key_span - 1, proof.keys_out_of_range, proof.duplicate_keys, passed ? "passed" : "FAILED"
        );
        return passed ? 0 : 1;
    }

    std::vector<ReplayState::Message> Fed(const ReplayFile::Contents& contents, const ReplayWire::Plan& plan)
    {
        std::vector<ReplayState::Message> fed;
        for (const auto& post : plan.posts) {
            std::string error;
            for (const auto& body : ReplayWire::Split(contents.table, post.bytes, error)) {
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

    std::string OrderSummary(const std::vector<ReplayState::OrderFailure>& failures, const size_t join_messages)
    {
        std::map<std::tuple<bool, uint16_t, ReplayState::IdKind>, size_t> counts;
        for (const auto& failure : failures) {
            counts[{failure.record >= join_messages, failure.field.message, failure.field.kind}]++;
        }
        std::string summary;
        for (const auto& [key, count] : counts) {
            const auto& [streamed, message_id, kind] = key;
            summary += std::format("{}{} {} {} x{}", summary.empty() ? "" : ", ", streamed ? "stream" : "join", message_id, ReplayState::IdKindName(kind), count);
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
        for (; i < end && (contents.records[i].time_ms < time_ms || contents.records[i].MessageId() != 30); i++) {
            world.Apply(contents.records[i]);
        }
        if (i == end && i > contents.chapters[chapter].first_record) {
            i--;
        }
        return {i, contents.records[std::min(i, contents.records.size() - 1)].time_ms, ReplayFile::KeyframeKind::Periodic, ReplayState::JoinRecords(contents.table, world, contents.records[std::min(i, contents.records.size() - 1)].time_ms)};
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
            if (ReplayState::IsBagContent(message)) {
                leaks.bag_content++;
            }
            else if (!filter.Keep(message, shown_before, unknown_items) && !filter.Holds(message)) {
                leaks.unshown_items++;
            }
        }
        leaks.held_details = filter.Held();
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
            constexpr uint32_t notice_message = 159;
            constexpr uint32_t one_time_notice = 22;
            auto& difference = differences[identity[0] == notice_message && identity.size() > 2 && identity[2] == one_time_notice ? notice_message | one_time_notice << 16 : identity[0]];
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

    const std::map<uint32_t, const char*> listed_differences = {
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
        {159 | 22 << 16, "a notice of kind 22, once in all the loads at hand: a one-time event, not kept by the model"},
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
        {113, "once, in an outpost's load: not kept by the model, so a keyframe built from it leaves it out; outposts are not played (session 10)"},
        {479, "the party search list: an outpost's window, not kept by the model"},
        {480, "the party search list: an outpost's window, not kept by the model"},
        {481, "the party search list: an outpost's window, not kept by the model"},
    };

    const std::map<uint32_t, const char*> keyframe_differences = {
        {31, "the clock: the model's 31 is the stream's clock at its last 30, the snapshot's the client's instance time"},
        {32,
         "gadgets (chests, doors, shrines) are left out: their classes (263) are not in GWCA; a moving agent's speed and its time field are not read (288 and 0 written), and a periodic keyframe's places are the client's where the model's are estimates"},
        {36, "an agent's class change since its 32: the snapshot writes the agent as the client holds it"},
        {37, "an agent's move since its 32: the snapshot writes it where it stands; it waits there until its next move"},
        {38, "an agent's move since its 32: the snapshot writes it where it stands; it waits there until its next move"},
        {39, "an agent's speed change: not read; its next move gives it again"},
        {41, "a move in progress: the snapshot writes the agent where it stands; it waits there until its next move"},
        {42, "a move in progress: the snapshot writes the agent where it stands; it waits there until its next move"},
        {43, "a move's pace: not read; its next move gives it again"},
        {46, "a rotation in progress: the 32 carries the agent's facing now"},
        {53, "an agent's attack pace: not read; its attacks play at their default pace until the stream's next 53"},
        {55, "the attribute points: not in GWCA; the attribute window, which an observer does not have (5c)"},
        {58, "made before 7b's fix: attributes were not written; now from GWCA's PartyAttribute (ids, base, levels)"},
        {59, "an attribute changed after the load: the 58 written from GWCA holds its level"},
        {60, "a player's row flags: not read; the party window's, which observers replace with ObservedPartiesHud (5c)"},
        {63, "made before 7b's fix: a maintained enchantment was written as 65 alone; now 63 and 65 from GWCA's buffs"},
        {65, "made before 7b's fix: effects without a duration were written as 66; now as the 65 the stream sends (no 66 at hand has no duration)"},
        {66, "made before 7b's fix: effects without a duration were written as 66; a periodic keyframe's durations are the client's remaining time"},
        {72, "a cape shown: not read; capes hidden until the stream's next 72"},
        {76, "quest descriptions: the quest log's text, which an observer does not have (5c)"},
        {80, "made before 7b's fix: the client's own bit 0 of a quest's state was written"},
        {81, "a quest's marker sent after the load: the 83 written holds the marker the client keeps"},
        {83, "made before 7b's fix: every quest got a marker, set or not; now only quests with a marker"},
        {86, "made before 7b's fix: only the NPCs present were defined; now every NPC the client holds"},
        {87, "made before 7b's fix: only the NPCs present were defined; now every NPC the client holds"},
        {89, "made before 7b's fix: the sixth field, 0xCCCCCCCC in all 280 player rows at hand (the server's unset bytes), was 0"},
        {101, "made before 7b's fix: not written; now [hero, 0] as in every load at hand"},
        {107, "made before 7b's fix: not written; now [0, 0] as in every load at hand"},
        {109, "made before 7b's fix: weapons were read from the agent view; now the char client's slots (k04, k05)"},
        {110, "made before 7b's fix: what agents wear was read from the agent view, filled only once models load and in another slot order; now the char client's slots (k04, k05), for players only as the stream sends"},
        {114, "made before 7b's fix: the hero's behaviour was 0; now GWCA's HeroFlag::hero_behavior"},
        {115, "a hero's six values (115's array) are not read: the hero panel's"},
        {116, "mercenaries' 116 is not written: their 115 is the hero data their 114 needs (5c)"},
        {124, "not read: unknown purpose, no use of it seen in the observer's view (session 7)"},
        {125, "not read: unknown purpose, no use of it seen in the observer's view (session 7)"},
        {138, "the explored areas: not written; the world map stays closed in replays (OnWorldMapToggle)"},
        {139, "the explored areas: not written; the world map stays closed in replays (OnWorldMapToggle)"},
        {141, "the mission map's points: the mission map stays closed in replays (6b, OnMissionMapCreate)"},
        {154, "an agent's scale: not read; models at their default size"},
        {155, "made before 7b's fix: NPCs named by their NPC's own name got a 155 too; a 155 sent after the load is in the snapshot's"},
        {156, "morale: not read (GWCA's morale_table is used nowhere yet); the party window's, which observers replace with ObservedPartiesHud (5c)"},
        {159, "notices 30 (guild), 0, 14 and 15 are not read; one-time notices are not kept"},
        {162, "made before 7b's fix: characters' health, energy and regeneration were not written; a periodic keyframe's values are the client's"},
        {166, "made before 7b's fix: agents with no profession got a 166"},
        {170, "an ally added after the load: not read; missing from the party's allies until the stream's next 170"},
        {177, "made before 7b's fix: not written; now GWCA's Player::party_leader_player_number"},
        {181, "the recorder's interface preferences: not the viewer's"},
        {182, "made before 7b's fix: characters with no unlocked profession got a 182"},
        {186, "made before 7b's fix: not written; now as in every load at hand"},
        {221, "the recorder's skill count: not read; the skills window's"},
        {233, "heroes' skill points: not in GWCA (0 written)"},
        {239, "the recorder's display settings: not the viewer's"},
        {240, "an agent's effect bits: the model keeps the stream's latest row (a 241), the snapshot writes them as a load does (240)"},
        {241, "an agent's effect bits: the model keeps the stream's latest row (a 241), the snapshot writes them as a load does (240)"},
        {243, "titles: not read; the hero panel's"},
        {246, "titles: not read; the hero panel's"},
        {263, "gadget classes: not in GWCA; gadgets are left out until the stream sends them"},
        {270, "props' animations: not in GWCA; props at their default state until the stream sends them"},
        {273, "props' layers: not in GWCA; props at their default state until the stream sends them"},
        {277, "gadgets' states: gadgets are left out (263)"},
        {280, "guilds: not written; guild names and capes not shown"},
        {284, "guilds: not written; guild names and capes not shown"},
        {288, "guilds: not written; guild names and capes not shown"},
        {289, "guilds: not written; guild names and capes not shown"},
        {302, "guilds: not written; guild names and capes not shown"},
        {314, "made before 7b's fix: worn items' customizer was not written; now GWCA's Item::customized"},
        {324, "the recorder's own inventory: no kept message names it"},
        {328, "the recorder's own weapon set: its inventory is not created (324)"},
        {346, "made before 7b's fix: worn items' profession was not written; now GWCA's Item::profession"},
        {353, "made before 7b's fix: worn items were missing with what they wore (110); a periodic keyframe's item values are the client's"},
        {354, "an item's details reset after the load: the 353 written holds the client's"},
        {380, "made before 7b's fix: the fields were read in memory order; 380's handler keeps them at +0x5c, +0x60, +0x54, +0x58 (gwlab's client-answers.md)"},
        {404, "made before 7b's fix: not written; now [0] as in every load at hand"},
        {406, "made before 7b's fix, the live test 6 crash: the map file was AreaInfo::file_id, 0 for PvE maps, which the map's load asserts (k01); the facing and the server's time were 0 (k02); its spawn is the controlled agent's place now"},
        {407, "the manifest's chunks: absent from 12 of 18 loads at hand, which loaded (session 7)"},
        {408, "the manifest's hash: 0, as in gwlab's loads, which loaded (session 7)"},
        {409, "the manifest's phases: written as gwlab's loads have them, which loaded (session 7)"},
        {428, "not read: gwlab's map 779 load had 0 and loaded (session 7)"},
        {430, "made before 7b's fix: the team row was not written; now as the 5 loads at hand have it"},
        {446, "made before 7b's fix: not written; now [0] as in every load at hand"},
        {159 | 22 << 16, "a one-time notice: not kept"},
    };

    const std::map<uint16_t, const char*> keyframe_layout_errors = {
        {353, "made before 7b's fix: an item's mods were written in its group's count field; now a count and one field per mod"},
    };

    bool PrintComparison(const std::map<uint32_t, Difference>& differences, const std::map<uint32_t, const char*>& reasons)
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
            unlisted += why == reasons.end();
            printf(
                "%6s %8zu %8zu %8zu %8zu %8zu %8zu %s; %s\n", (id >> 16 ? std::format("{}/{}", id & 0xFFFF, id >> 16) : std::format("{}", id)).c_str(), difference.expected, difference.actual, difference.equal, difference.repeated, difference.missing,
                difference.extra, fields.empty() ? " none" : fields.c_str(), why == reasons.end() ? "NOT LISTED" : why->second
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

    constexpr uint16_t own_name_messages[] = {48, 49, 116, 381};

    NameCount CountNames(const ReplayFile::MessageTable& table, const std::vector<ReplayFile::Record>& records, const uint32_t flags)
    {
        NameCount count;
        for (const auto& message : Decoded(table, records)) {
            const auto checked = (flags & ReplayFile::NamesStripped) || ((flags & ReplayFile::OwnNamesStripped) && std::ranges::contains(own_name_messages, message.id));
            for (const auto& field : message.fields) {
                const std::wstring_view text = field.text;
                if (ReplayFile::IsPlainText(text)) {
                    count.texts++;
                    count.not_placeholders += checked && !ReplayFile::IsPlaceholder(text);
                    continue;
                }
                for (auto start = text.find(ReplayFile::literal_start); start != std::wstring_view::npos; start = text.find(ReplayFile::literal_start, start + 1)) {
                    const auto end = text.find(ReplayFile::literal_end, start + 1);
                    if (end == std::wstring_view::npos) {
                        break;
                    }
                    if (const auto literal = text.substr(start + 1, end - start - 1); ReplayFile::IsPlainText(literal)) {
                        count.literals++;
                        count.not_placeholders += checked && !ReplayFile::IsPlaceholder(literal);
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
            const auto built = world.Join();
            printf("\nchapter %zu: its real load (records %zu to %zu) against the model's join built from it: %zu messages built, layout errors %u\n", c + 1, load->head, load->end, built.size(), world.LayoutErrors());
            passed &= PrintComparison(Compare(Decoded(contents.table, records), built), listed_differences);
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
            printf("\nkeyframe %zu (%s, at %u ms): the snapshot from memory against the model of the stream up to it (records %zu to %zu)\n", k + 1, ReplayFile::KeyframeKindName(keyframe.kind), keyframe.time_ms, load->head, keyframe.record_index);
            passed &= PrintComparison(Compare(stream.Join(), Decoded(contents.table, keyframe.records)), keyframe_differences);
        }
        for (size_t k = 0; k < contents.keyframes.size(); k++) {
            std::map<uint16_t, size_t> undecoded;
            for (const auto& record : contents.keyframes[k].records) {
                if (ReplayState::Message message; !ReplayState::Decode(contents.table, record.body, message)) {
                    undecoded[record.MessageId()]++;
                }
            }
            for (const auto& [id, count] : undecoded) {
                const auto why = keyframe_layout_errors.find(id);
                passed &= why != keyframe_layout_errors.end();
                printf("keyframe %zu: %zu messages %u do not decode against the file's table; %s\n", k + 1, count, id, why == keyframe_layout_errors.end() ? "NOT LISTED" : why->second);
            }
        }
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
        sender.Init(key, sizeof(key));
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
        const auto bodies = ReplayWire::Split(contents.table, received, error);
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
            const auto bodies = ReplayWire::Split(contents.table, post.bytes, error);
            if (!error.empty() || bodies.size() != post.records) {
                fail(std::format("post {} does not cut into its {} messages ({})", i, post.records, error));
            }
            for (const auto& body : bodies) {
                ReplayState::Message message;
                observer_info |= ReplayState::Decode(contents.table, body, message) && post.stage == ReplayWire::Stage::BeforeLoad && message.id == 410 && message.U(5) == 1;
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
        for (const auto& need : ReplayState::CheckClientNeeds(fed)) {
            fail(std::format("what the client needs: {}", need));
        }
        const auto order = ReplayState::CheckOrder(contents.table, fed);
        for (const auto& failure : order) {
            fail(std::format("the order of real loads: {}", ReplayState::Describe(failure)));
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
            const auto order = ReplayState::CheckOrder(contents.table, fed);
            const auto needs = ReplayState::CheckClientNeeds(fed);
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
                const auto order = ReplayState::CheckOrder(contents.table, fed);
                const auto needs = ReplayState::CheckClientNeeds(fed);
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
        Check(CountLeaks(contents.table, ReplayState::JoinRecords(contents.table, world, 0)).Total() == 0, "a join passes the no-inventory rule");
        const auto attribute_state = std::ranges::find(join, uint16_t{55}, &ReplayState::Message::id);
        const auto connect = std::ranges::find(join, uint16_t{31}, &ReplayState::Message::id);
        Check(
            attribute_state != join.end() && attribute_state < connect && attribute_state->U(0) == 10 && attribute_state->U(2) == 35 && has(58) && has(59) && has(183) && has(182) && has(218),
            "a join gives the controlled character its attribute state (57 folded in), attributes, professions and skillbar before 31"
        );
        Check(ReplayState::CheckClientNeeds(join).empty(), "a join holds what the client needs before 34");
        Check(ReplayState::CheckOrder(contents.table, join).empty(), "a join holds the order of real loads");
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
            if (ReplayState::Message message; !ReplayState::Decode(contents.table, record.body, message) || message.id != 55 || message.U(0) != 10) {
                stateless.Apply(record);
            }
        }
        const auto fallback = stateless.Join();
        Check(
            stateless.ControlledAgent() == 0 && std::ranges::none_of(
                                                    fallback,
                                                    [](const ReplayState::Message& message) {
                                                        return message.id == 34 || (message.id == 89 && message.U(1) == 10) || (message.id == 32 && message.U(0) == 10);
                                                    }
                                                ),
            "a controlled character with no attribute state is left out of the join: no 34, nor its 89 and 32"
        );
        Check(ReplayState::CheckClientNeeds(fallback).empty() && ReplayState::CheckOrder(contents.table, fallback).empty(), "the join without the controlled character holds what the client needs and the order of real loads");

        ReplayState::World allies(contents.table);
        allies.Apply(ReplayState::Message{155, {{{88}, {}}, {{}, L"01"}}});
        allies.Apply(ReplayState::Message{170, {{{88}, {}}, {{1}, {}}, {{2}, {}}}});
        allies.Apply(ReplayState::Message{30, {{{100}, {}}}});
        allies.Apply(ReplayState::Message{31, {{{100}, {}}}});
        Check(
            std::ranges::any_of(
                allies.Join(),
                [](const ReplayState::Message& message) {
                    return message.id == 170 && message.U(0) == 88;
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
            for (auto& body : ReplayWire::Split(contents.table, post.bytes, error)) {
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
        const auto stripped = [](ReplayFile::NameStripper& stripper, const uint16_t message_id, std::wstring text, const uint32_t capacity) {
            stripper.Apply(message_id, text, capacity);
            return text;
        };
        ReplayFile::NameStripper both(true, true, L"Own Name");
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
        ReplayFile::NameStripper others_only(true, false, L"Own Name");
        Check(stripped(others_only, 89, L"Own Name", 32) == L"Own Name", "the recorder's name is kept when only other players' are stripped");
        Check(stripped(others_only, 48, L"Account", 32) == L"Player 1", "the account's name is still hidden when only other players' are stripped");
        Check(stripped(others_only, 288, L"Their Guild", 32) == L"Guild 1", "guild names are replaced with other players' names");
        ReplayFile::NameStripper own_only(false, true, L"Own Name");
        Check(stripped(own_only, 89, L"Someone Else", 32) == L"Someone Else", "other players' names are kept when only the recorder's are stripped");
        Check(stripped(own_only, 89, L"Own Name", 32) == L"Recorder", "the recorder's name is replaced alone");
        Check(stripped(own_only, 289, L"OG", 6) == L"G1", "guild tags are replaced with the recorder's names");
    }

    void TestWire(const std::filesystem::path& folder)
    {
        ReplayWire::Rc4 known;
        const uint8_t key[] = {'K', 'e', 'y'};
        known.Init(key, sizeof(key));
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
        const auto guild_failures = ReplayState::CheckOrder(contents.table, Fed(contents, guild_first));
        Check(
            std::ranges::any_of(
                guild_failures,
                [](const ReplayState::OrderFailure& failure) {
                    return failure.field.message == 280 && failure.field.kind == ReplayState::IdKind::Guild;
                }
            ) && !ProveFeed(contents, guild_first, UINT32_MAX, false),
            "a plan with its guild message (280) moved first fails the order of real loads"
        );

        for (const auto join_ms : {10000u, 30000u, 60000u, UINT32_MAX}) {
            const auto keyframe_plan = ReplayWire::MakeKeyframePlan(contents, ModelKeyframe(contents, 0, join_ms));
            const auto failures = ReplayState::CheckOrder(contents.table, Fed(contents, keyframe_plan));
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
            for (const auto& body : ReplayWire::Split(contents.table, post.bytes, split_error)) {
                if (ReplayState::Message message; ReplayState::Decode(contents.table, body, message) && message.id == 55 && message.U(0) == 10) {
                    continue;
                }
                kept.insert(kept.end(), body.begin(), body.end());
                post.records++;
            }
            post.bytes = std::move(kept);
        }
        Check(!ProveFeed(contents, without_state, UINT32_MAX, false), "a plan whose 34 names an agent without its attribute state fails the feed check");

        ReplayWire::Rc4 sender;
        sender.Init(key, sizeof(key));
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
        const auto scan = ReplayObserve::ScanFolder(rows_folder, 39167, ReplayFile::TableDigest(table), server_keys, now);
        Check(scan.rows.size() == std::size(listed) + 2 && scan.left_out.size() == 5, "the rows folder gives one row per recording (seven) and leaves five files out");
        if (scan.rows.empty()) {
            return;
        }
        const auto proof = ProveRows(table, scan, false);
        Check(!proof.layout_errors && !proof.disallowed_types && !proof.keys_out_of_range && !proof.duplicate_keys, "every row is valid by the live 414 table, its type allowed, its key ours and unique");
        Check(
            std::ranges::count(scan.rows, std::optional<size_t>(), &ReplayObserve::Row::keyframe) == 1 && std::ranges::count(scan.rows, std::optional<size_t>(0), &ReplayObserve::Row::keyframe) == std::ssize(listed) + 1,
            "a recording of outposts alone is listed, not playable; the others play from a keyframe"
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
        Check(left_out_for(L"unfinished.gwrec").starts_with("unfinished"), "an unfinished recording is left out");
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
            if (ReplayState::Message message; ReplayState::Decode(named.table, record.body, message) && message.id == 381 && !message.fields.empty()) {
                message.fields[0].text = L"Real Name";
                ReplayState::Encode(named.table, message, record.body);
            }
        }
        Check(CountNames(named.table, named.keyframes.front().records, named.header.flags).not_placeholders == 1, "a name that is not a placeholder in a keyframe is found, by count");
        auto other = v2;
        other.header.flags = 0;
        Check(CountNames(other.table, named.keyframes.front().records, other.header.flags).not_placeholders == 0, "with the names settings off, names are not checked");
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
    const auto arguments = command == L"--compare" || command == L"--wire" || command == L"--order" ? 3 : command == L"--feed" ? 4 : 0;
    if (!arguments || argc != arguments) {
        printf(
            "usage: replay_check --compare <recording.gwrec>\n"
            "       replay_check --feed <recording.gwrec> <ms>\n"
            "       replay_check --wire <recording.gwrec>\n"
            "       replay_check --order <recording.gwrec>\n"
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
