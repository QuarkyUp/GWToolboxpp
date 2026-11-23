#include <Utils/ReplayFile.h>

#include <chrono>
#include <cstring>
#include <format>
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
        printf("\n%8s %10s %12s\n", "id", "count", "bytes");
        uint64_t total_bytes = 0;
        for (const auto& [message_id, count] : CountPerMessage(contents.records)) {
            printf("%8u %10u %12llu\n", message_id, count.count, static_cast<unsigned long long>(count.bytes));
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

    int SelfTest(const std::filesystem::path& folder)
    {
        std::error_code error_code;
        std::filesystem::create_directories(folder, error_code);
        TestPacking();

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
    if (argc != 2) {
        printf("usage: replay_check <recording.gwrec>\n       replay_check --self-test <folder>\n");
        return 2;
    }
    ReplayFile::Contents contents;
    std::string error;
    if (!ReplayFile::Read(argv[1], contents, error)) {
        printf("%ls: %s\n", argv[1], error.c_str());
        return 1;
    }
    PrintRecording(contents);
    return 0;
}
