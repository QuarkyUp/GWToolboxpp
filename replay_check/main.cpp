#include <Utils/ReplayFile.h>
#include <Utils/ReplayState.h>

#include <algorithm>
#include <bit>
#include <chrono>
#include <cstring>
#include <format>
#include <limits>
#include <map>
#include <random>
#include <string>
#include <vector>

namespace {
    struct MessageCount {
        uint32_t count = 0;
        uint64_t bytes = 0;

        bool operator==(const MessageCount&) const = default;
    };

    std::map<uint16_t, MessageCount> CountPerMessage(const std::vector<ReplayFile::Record>& records)
    {
        std::map<uint16_t, MessageCount> counts;
        for (const auto& record : records) {
            auto& count = counts[record.MessageId()];
            count.count++;
            count.bytes += record.body.size();
        }
        return counts;
    }

    void PrintRecording(const ReplayFile::Contents& contents)
    {
        const auto& header = contents.header;
        const auto start = std::chrono::sys_time<std::chrono::milliseconds>(std::chrono::milliseconds(header.start_time_unix_ms));
        printf("client build      %u\n", header.client_build);
        printf("toolbox version   %s\n", header.toolbox_version);
        printf("message table     %zu messages, digest %016llx\n", contents.table.size(), static_cast<unsigned long long>(header.table_digest));
        printf("map               %u (region type %u, %s), district %u, region %u, language %u\n", header.map_id, header.map_region_type,
               header.is_explorable ? "explorable" : "outpost", header.district, header.region, header.language);
        printf("start (UTC)       %s\n", std::format("{:%Y-%m-%d %H:%M:%S}", start).c_str());
        printf("duration          %.1f s\n", header.duration_ms / 1000.0);
        printf("flags             %s%s%s%s\n", header.flags & ReplayFile::Complete ? "complete" : "unfinished",
               header.flags & ReplayFile::NamesStripped ? ", names stripped" : "", header.flags & ReplayFile::ChatDropped ? ", chat dropped" : "",
               header.flags & ReplayFile::SizeCapReached ? ", size cap reached" : "");
        printf("records           %zu (header: %u), layout errors %u%s\n", contents.records.size(), header.record_count, header.layout_errors,
               contents.truncated_tail ? ", last record cut short" : "");
        printf("\n%8s %10s %12s  %s\n", "id", "count", "bytes", "kind");
        uint64_t total_bytes = 0;
        for (const auto& [message_id, count] : CountPerMessage(contents.records)) {
            printf("%8u %10u %12llu  %s\n", message_id, count.count, static_cast<unsigned long long>(count.bytes), ReplayState::KindName(ReplayState::KindOf(message_id)));
            total_bytes += count.bytes;
        }
        printf("%8s %10zu %12llu\n", "total", contents.records.size(), static_cast<unsigned long long>(total_bytes));
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
        Descriptor(4, 0, 2),
        Descriptor(4, 0, 1),
        Descriptor(4, 0, 2),
        Descriptor(4, 0, 4),
        Descriptor(0, 1, 0),
        Descriptor(1, 0, 0),
        Descriptor(2, 0, 0),
        Descriptor(3, 0, 0),
        Descriptor(5, 0, 3),
        Descriptor(7, 1, 5),
        Descriptor(4, 0, 4),
        Descriptor(11, 0, 4),
        Descriptor(6, 0, 0),
        Descriptor(4, 0, 4),
        Descriptor(11, 1, 3),
        Descriptor(6, 0, 0),
        Descriptor(11, 2, 2),
        Descriptor(6, 0, 0),
        Descriptor(12, 0, 3),
        Descriptor(4, 0, 1),
        Descriptor(7, 1, 4),
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

        const ReplayFile::TextFilter rename = [](std::wstring& text) {
            if (text == L"Ann") {
                text = L"Pl 1";
            }
        };
        Check(packed(rename, out), "a filtered message packs");
        Check(out == ExpectedWire(decoded, L"Pl 1", L"Bo", {0x107, L'C', 0x1}), "the filter replaces the text it is given");

        const ReplayFile::TextFilter too_long = [](std::wstring& text) {
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
        Check(ReplayFile::PackMessage(300, header_only, 1, reinterpret_cast<const uint8_t*>(&decoded), out, {}) && out == std::vector<uint8_t>{0x2C, 0x01},
              "a message of no field is its id");
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

    bool WriteRecording(const std::filesystem::path& path, const ReplayFile::Header& header, const ReplayFile::MessageTable& table,
                        const std::vector<ReplayFile::Record>& records, const bool close)
    {
        ReplayFile::Writer writer;
        if (!writer.Open(path, header, table)) {
            return false;
        }
        for (const auto& record : records) {
            if (!writer.Append(record.time_ms, record.body.data(), record.body.size())) {
                return false;
            }
        }
        return close ? writer.Close(header) : writer.Flush();
    }

    uint64_t Digest(const std::string& text)
    {
        uint64_t digest = 0xcbf29ce484222325;
        for (const auto c : text) {
            digest = (digest ^ static_cast<uint8_t>(c)) * 0x100000001b3;
        }
        return digest;
    }

    std::string FirstDifference(const std::string& expected, const std::string& actual)
    {
        size_t line = 0;
        size_t a = 0;
        size_t b = 0;
        while (a < expected.size() || b < actual.size()) {
            const auto a_end = std::min(expected.find('\n', a), expected.size());
            const auto b_end = std::min(actual.find('\n', b), actual.size());
            const auto expected_line = expected.substr(a, a_end - a);
            const auto actual_line = actual.substr(b, b_end - b);
            if (expected_line != actual_line) {
                const auto words = [](const std::string& text) {
                    const auto first = text.find(' ', 1);
                    return text.substr(0, first == std::string::npos ? text.size() : text.find(' ', first + 1));
                };
                return std::format("line {}: \"{}\" against \"{}\"", line, words(expected_line), words(actual_line));
            }
            a = a_end + 1;
            b = b_end + 1;
            line++;
        }
        return "none";
    }

    ReplayFile::Contents Joined(const ReplayFile::Contents& contents, std::vector<ReplayFile::Record> records, const size_t tail_start)
    {
        ReplayFile::Contents joined{contents.header, contents.table, std::move(records), false};
        joined.records.insert(joined.records.end(), contents.records.begin() + static_cast<std::ptrdiff_t>(tail_start), contents.records.end());
        joined.header.record_count = static_cast<uint32_t>(joined.records.size());
        joined.header.layout_errors = 0;
        joined.header.flags |= ReplayFile::Complete;
        return joined;
    }

    size_t RecordsUpTo(const ReplayFile::Contents& contents, const uint32_t time_ms)
    {
        size_t count = 0;
        while (count < contents.records.size() && contents.records[count].time_ms <= time_ms) {
            count++;
        }
        return count;
    }

    struct Proof {
        size_t keyframes = 0;
        size_t samples = 0;
        size_t mismatches = 0;
        size_t check_failures = 0;
        size_t seeks = 0;
        size_t layout_errors = 0;
    };

    Proof ProveKeyframes(const ReplayFile::Contents& contents, const uint32_t interval_ms, const uint32_t sample_ms, const bool verbose)
    {
        Proof proof;
        const auto& records = contents.records;
        for (const auto& record : records) {
            ReplayState::Message message;
            std::vector<uint8_t> encoded;
            if (!ReplayState::Decode(contents.table, record.body, message) || !ReplayState::Encode(contents.table, message, encoded) || encoded != record.body) {
                proof.layout_errors++;
            }
        }
        const auto end_ms = records.empty() ? 0 : records.back().time_ms;
        std::vector<uint32_t> sample_times;
        for (uint32_t time_ms = 0; time_ms < end_ms; time_ms += sample_ms) {
            sample_times.push_back(time_ms);
        }
        sample_times.push_back(end_ms);
        std::map<uint32_t, std::string> reference;
        ReplayState::World world(contents.table);
        size_t applied = 0;
        for (const auto time_ms : sample_times) {
            for (; applied < records.size() && records[applied].time_ms <= time_ms; applied++) {
                world.Apply(records[applied]);
            }
            reference[time_ms] = world.Dump(true);
        }

        const auto keyframes = ReplayState::Keyframes(contents, interval_ms);
        proof.keyframes = keyframes.size();
        for (size_t k = 0; k < keyframes.size(); k++) {
            const auto& keyframe = keyframes[k];
            const auto join = ReplayState::JoinRecords(contents.table, keyframe.world, keyframe.time_ms);
            const auto joined = Joined(contents, join, keyframe.record_index);
            const auto check = ReplayState::CheckJoin(joined);
            proof.check_failures += check.failures.size();

            ReplayState::World replayed(contents.table);
            for (const auto& record : join) {
                replayed.Apply(record);
            }
            size_t mismatches = 0;
            std::string first_mismatch;
            const auto compare = [&](const std::string& expected, const std::string& actual) {
                proof.samples++;
                if (Digest(expected) != Digest(actual)) {
                    if (!mismatches) {
                        first_mismatch = FirstDifference(expected, actual);
                    }
                    mismatches++;
                }
            };
            compare(keyframe.world.Dump(true), replayed.Dump(true));
            auto tail = keyframe.record_index;
            for (const auto time_ms : sample_times) {
                if (time_ms < keyframe.time_ms) {
                    continue;
                }
                for (; tail < records.size() && records[tail].time_ms <= time_ms; tail++) {
                    replayed.Apply(records[tail]);
                }
                compare(reference[time_ms], replayed.Dump(true));
            }
            const auto seek_ms = k + 1 < keyframes.size() ? (keyframe.time_ms + keyframes[k + 1].time_ms) / 2 : (keyframe.time_ms + end_ms) / 2;
            ReplayState::World sought(contents.table);
            for (const auto& record : ReplayState::Seek(contents, keyframes, seek_ms)) {
                sought.Apply(record);
            }
            ReplayState::World expected(contents.table);
            for (size_t i = 0; i < RecordsUpTo(contents, seek_ms); i++) {
                expected.Apply(records[i]);
            }
            compare(expected.Dump(true), sought.Dump(true));
            proof.seeks++;
            proof.mismatches += mismatches;
            if (verbose || mismatches || !check.failures.empty()) {
                printf("keyframe %2zu at %7u ms (record %zu): %zu join records, %zu agents; samples to the end %s; check-join %s (%zu join records, %zu stream rows before their agent, %zu player rows without agent)\n",
                       k, keyframe.time_ms, keyframe.record_index, join.size(), keyframe.world.AgentCount(),
                       mismatches ? std::format("{} differ, first {}", mismatches, first_mismatch).c_str() : "identical",
                       check.failures.empty() ? "passed" : std::format("{} failures, first {}", check.failures.size(), check.failures.front()).c_str(), check.join_records,
                       check.stream_rows_before_creation, check.player_rows_without_agent);
            }
        }
        printf("keyframes every %u ms: %zu keyframes, %zu samples compared, %zu differ; %zu seeks; check-join failures %zu; records that do not decode and encode back %zu: %s\n",
               interval_ms, proof.keyframes, proof.samples, proof.mismatches, proof.seeks, proof.check_failures, proof.layout_errors,
               proof.mismatches || proof.check_failures || proof.layout_errors ? "FAILED" : "passed");
        return proof;
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
        {154, {0x10, 0x404}},
        {155, {0x10, 0x2017}},
        {159, {0x404, 0x10, 0x404}},
        {162, {0x404, 0x10, 0x404}},
        {163, {0x404, 0x10, 0x10, 0x404}},
        {170, {0x10, 0x404, 0x404}},
        {218, {0x10, 0x82b, 0x6, 0x82b, 0x6, 0x104}},
        {240, {0x10, 0x404}},
        {241, {0x10, 0x404}},
        {242, {0x404}},
        {263, {0x10, 0x404, 0x404, 0x104, 0x2017}},
        {277, {0x404, 0x404}},
        {318, {0x204, 0x404, 0x204, 0x104}},
        {327, {0x204, 0x104, 0x404, 0x404}},
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
        WorldWriter write{table, {}};
        const auto unit = [&random] {
            return static_cast<float>(random() % 2001) / 1000.f - 1.f;
        };
        const auto point = [&] {
            return Floats({unit() * 3000.f, unit() * 3000.f});
        };
        const auto create = [&](const uint32_t agent, const uint32_t agent_class, const uint32_t def, const uint32_t team) {
            const auto infinity = std::numeric_limits<float>::infinity();
            write(32, {N(agent), N(agent_class), N(def), N(1), point(), N(0), Floats({1.f, 0.f}), N(1), Floats({288.f}), Floats({1.f}), N(0), N(team), N(0), N(0), N(0), N(0), N(0),
                       Floats({0.f, 0.f}), Floats({infinity, infinity}), N(0), N(0), Floats({infinity, infinity}), N(0)});
        };

        write(30, {N(100)});
        write(380, {N(1), N(2), N(3), N(4)});
        write(390, {});
        write(381, {Text(L"Recorder")});
        write(410, {N(1), N(148), N(1), N(1), N(0), N(0), N(0)});
        for (uint32_t item = 100; item < 105; item++) {
            write(353, {N(item), N(item * 7), N(2), N(0), N(0), N(0), N(1), N(0), N(10), N(item), N(1), Text(L"\x108\x107item\x1"), N(1), N(0x2400)});
            write(318, {N(1), N(item), N(1), N(item - 100)});
        }
        write(327, {N(1), N(0), N(103), N(104)});
        write(409, {N(1)});
        write(407, {Bytes(40, 7)});
        write(409, {N(2)});
        write(407, {Bytes(12, 9)});
        write(408, {N(1), N(2), N(3)});
        write(428, {N(7)});
        write(406, {N(0x1234), point(), N(0), N(1), N(0), Bytes(8, 1)});
        write(430, {N(1), N(players_team), N(0), N(1), N(2), Text(L"Team")});
        write(242, {N(players_team)});
        write(218, {N(recorder), {{1, 2, 3}, {}}, {{0, 0, 0}, {}}, N(1)});
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
        write(467, {N(1)});
        write(460, {N(1), N(1), N(0)});
        write(460, {N(1), N(2), N(0)});
        write(468, {N(1)});
        write(435, {N(1), N(0)});

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
                        write(93, {Text(L"chat")});
                        write(12, {});
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
        Check(!has(218) && !has(318) && !has(327) && !has(93) && !has(12) && !has(30), "a join holds no private or transient message and no clock advance");
        const auto info = std::ranges::find(join, uint16_t{410}, &ReplayState::Message::id);
        Check(info != join.end() && info->U(5) == 1, "a join's 410 carries the observer byte");
        const auto reporter = std::ranges::find(join, uint16_t{34}, &ReplayState::Message::id);
        Check(reporter != join.end() && reporter->U(0) == 10, "a join's reporter is the recorder's character");
        const auto joined = Joined(contents, ReplayState::JoinRecords(contents.table, world, contents.records.back().time_ms), contents.records.size());
        const auto check = ReplayState::CheckJoin(joined);
        Check(check.failures.empty(), std::format("the final join passes check-join ({})", check.failures.empty() ? "" : check.failures.front()).c_str());
        ReplayState::World replayed(contents.table);
        for (const auto& record : joined.records) {
            replayed.Apply(record);
        }
        Check(replayed.Dump(true) == world.Dump(true), "the model fed its own join is the model");

        for (const auto interval_ms : {ReplayState::default_keyframe_interval_ms, 1000u}) {
            const auto proof = ProveKeyframes(contents, interval_ms, 500, false);
            Check(proof.keyframes > 0 && !proof.mismatches && !proof.check_failures && !proof.layout_errors, "the synthetic world's keyframes are equivalent and their joins pass");
        }
    }

    int SelfTest(const std::filesystem::path& folder)
    {
        std::error_code error_code;
        std::filesystem::create_directories(folder, error_code);
        TestPacking();
        TestWorld(folder);

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
        Check(CountPerMessage(contents.records) == CountPerMessage(records), "its counts per message id are those written");

        const auto unfinished_path = folder / L"self-test-unfinished.gwrec";
        Check(WriteRecording(unfinished_path, header, table, records, false), "the unfinished recording is written");
        if (const auto file = _wfopen(unfinished_path.c_str(), L"ab")) {
            fwrite("\x10\x00\x00\x00\x40\x00\x05\x00", 1, 8, file);
            fclose(file);
        }
        ReplayFile::Contents unfinished;
        Check(ReplayFile::Read(unfinished_path, unfinished, error), "an unfinished recording reads back");
        Check(!(unfinished.header.flags & ReplayFile::Complete) && unfinished.truncated_tail && unfinished.records == records,
              "an unfinished recording keeps every whole record and reports the cut one");

        auto corrupt = finished;
        corrupt.table_digest ^= 1;
        const auto corrupt_path = folder / L"self-test-corrupt.gwrec";
        Check(WriteRecording(corrupt_path, corrupt, table, records, true), "the corrupt recording is written");
        ReplayFile::Contents refused;
        Check(!ReplayFile::Read(corrupt_path, refused, error), "a recording whose table does not match its digest is refused");

        PrintRecording(contents);
        printf("\nself-test on %ls: %s\n", complete_path.c_str(), failures ? "FAILED" : "passed");
        return failures ? 1 : 0;
    }
}

int wmain(const int argc, const wchar_t* argv[])
{
    if (argc == 3 && wcscmp(argv[1], L"--self-test") == 0) {
        return SelfTest(argv[2]);
    }
    const auto command = argc > 2 ? std::wstring(argv[1]) : L"";
    const auto arguments = command.empty() ? 1 : command == L"--state" ? 2 : command == L"--join" ? 3 : command == L"--check-join" ? 1 : command == L"--keyframes" ? 3 : 0;
    if (!arguments || argc != 2 + (command.empty() ? 0 : arguments)) {
        printf("usage: replay_check <recording.gwrec>\n"
               "       replay_check --state <recording.gwrec> <ms>\n"
               "       replay_check --join <recording.gwrec> <ms> <out.gwrec>\n"
               "       replay_check --check-join <recording.gwrec>\n"
               "       replay_check --keyframes <recording.gwrec> <interval ms> <sample ms>\n"
               "       replay_check --self-test <folder>\n");
        return 2;
    }
    const auto path = command.empty() ? argv[1] : argv[2];
    ReplayFile::Contents contents;
    std::string error;
    if (!ReplayFile::Read(path, contents, error)) {
        printf("%ls: %s\n", path, error.c_str());
        return 1;
    }
    if (command.empty()) {
        PrintRecording(contents);
        return 0;
    }
    if (command == L"--check-join") {
        const auto check = ReplayState::CheckJoin(contents);
        printf("join part %zu records, stream rows naming an agent not created %zu, player rows naming an agent not in the world %zu, failures %zu\n", check.join_records,
               check.stream_rows_before_creation, check.player_rows_without_agent, check.failures.size());
        for (size_t i = 0; i < check.failures.size() && i < 20; i++) {
            printf("  %s\n", check.failures[i].c_str());
        }
        printf("check-join: %s\n", check.failures.empty() ? "passed" : "FAILED");
        return check.failures.empty() ? 0 : 1;
    }
    if (command == L"--keyframes") {
        const auto proof = ProveKeyframes(contents, static_cast<uint32_t>(_wtoi(argv[3])), std::max(1u, static_cast<uint32_t>(_wtoi(argv[4]))), true);
        return proof.mismatches || proof.check_failures || proof.layout_errors ? 1 : 0;
    }
    const auto time_ms = static_cast<uint32_t>(_wtoi(argv[3]));
    const auto applied = RecordsUpTo(contents, time_ms);
    ReplayState::World world(contents.table);
    for (size_t i = 0; i < applied; i++) {
        world.Apply(contents.records[i]);
    }
    if (command == L"--state") {
        printf("state after %zu records (%u ms): clock %u, %zu agents, layout errors %u, digest %016llx\n%s", applied, time_ms, world.Clock(), world.AgentCount(), world.LayoutErrors(),
               static_cast<unsigned long long>(Digest(world.Dump(true))), world.Dump(false).c_str());
        return 0;
    }
    const auto join = ReplayState::JoinRecords(contents.table, world, time_ms);
    auto joined = Joined(contents, join, applied);
    joined.header.duration_ms = joined.records.empty() ? 0 : joined.records.back().time_ms;
    if (!WriteRecording(argv[4], joined.header, joined.table, joined.records, true)) {
        printf("%ls: cannot be written\n", argv[4]);
        return 1;
    }
    printf("%ls: the join at %u ms (%zu records) then %zu records of the stream\n", argv[4], time_ms, join.size(), contents.records.size() - applied);
    return 0;
}
