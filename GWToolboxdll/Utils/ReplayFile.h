#pragma once

#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <functional>
#include <set>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace ReplayFile {
    constexpr char magic[8] = {'G', 'W', 'T', 'B', 'R', 'E', 'C', '\0'};
    constexpr uint16_t format_version = 2;
    constexpr uint16_t header_size = 96;
    constexpr size_t max_body_size = 0xFFFF;
    constexpr uint16_t chapter_entry_id = 0xF001;
    constexpr uint16_t keyframe_entry_id = 0xF002;

    enum Flags : uint32_t {
        NamesStripped = 1 << 0,
        ChatDropped = 1 << 1,
        Complete = 1 << 2,
        SizeCapReached = 1 << 3,
        OwnNamesStripped = 1 << 4,
    };

    struct Header {
        uint32_t client_build = 0;
        uint64_t table_digest = 0;
        uint64_t start_time_unix_ms = 0;
        uint32_t duration_ms = 0;
        uint32_t map_id = 0;
        uint32_t map_region_type = 0;
        uint32_t is_explorable = 0;
        uint32_t district = 0;
        uint32_t region = 0;
        uint32_t language = 0;
        uint32_t record_count = 0;
        uint32_t layout_errors = 0;
        uint32_t flags = 0;
        char toolbox_version[24] = {};

        bool operator==(const Header&) const = default;
    };

    using MessageTable = std::vector<std::vector<uint32_t>>;

    struct Record {
        uint32_t time_ms = 0;
        std::vector<uint8_t> body;

        [[nodiscard]] uint16_t MessageId() const { return static_cast<uint16_t>(body[0] | body[1] << 8); }
        bool operator==(const Record&) const = default;
    };

    struct Chapter {
        size_t first_record = 0;
        uint32_t time_ms = 0;
        uint32_t map_id = 0;
        uint32_t is_explorable = 0;
        uint32_t map_region_type = 0;

        bool operator==(const Chapter&) const = default;
    };

    enum class KeyframeKind : uint8_t {
        Start,
        AfterLoad,
        Periodic,
    };
    const char* KeyframeKindName(KeyframeKind kind);

    struct Keyframe {
        size_t record_index = 0;
        uint32_t time_ms = 0;
        KeyframeKind kind = KeyframeKind::Periodic;
        std::vector<Record> records;

        bool operator==(const Keyframe&) const = default;
    };

    struct Contents {
        Header header;
        MessageTable table;
        std::vector<Record> records;
        std::vector<Chapter> chapters;
        std::vector<Keyframe> keyframes;
        bool truncated_tail = false;

        [[nodiscard]] size_t ChapterEnd(size_t chapter) const { return chapter + 1 < chapters.size() ? chapters[chapter + 1].first_record : records.size(); }
        [[nodiscard]] size_t ChapterOf(size_t record_index) const;
    };

    uint64_t TableDigest(const MessageTable& table);

    using TextFilter = std::function<void(std::wstring& text, uint32_t capacity)>;
    bool PackMessage(uint32_t message_id, const uint32_t* fields, uint32_t field_count, const uint8_t* decoded, std::vector<uint8_t>& out, const TextFilter& filter);

    constexpr wchar_t own_name_placeholder[] = L"Recorder";
    constexpr wchar_t literal_start = 0x107;
    constexpr wchar_t literal_end = 0x1;
    constexpr size_t guild_tag_length = 5;

    bool IsPlainText(std::wstring_view text);
    bool IsPlaceholder(std::wstring_view text);

    class NameStripper {
    public:
        NameStripper(bool other_players, bool own_names, std::wstring own_name);

        void Apply(uint16_t message_id, std::wstring& text, uint32_t capacity);
        [[nodiscard]] TextFilter For(uint16_t message_id);

    private:
        std::wstring Label(uint16_t message_id, std::wstring_view name, uint32_t capacity);

        bool other_players;
        bool own_names;
        std::set<std::wstring, std::less<>> recorder_names;
        std::set<std::wstring, std::less<>> account_names;
        std::unordered_map<std::wstring, std::wstring> players;
        std::unordered_map<std::wstring, std::wstring> guilds;
        std::unordered_map<std::wstring, std::wstring> tags;
        std::unordered_map<std::wstring, std::wstring> mercenaries;
    };

    bool Read(const std::filesystem::path& path, Contents& out, std::string& error, bool bodies = true);
    bool ReadHeader(const std::filesystem::path& path, Header& out, std::string& error);

    class Writer {
    public:
        Writer() = default;
        Writer(const Writer&) = delete;
        Writer& operator=(const Writer&) = delete;
        ~Writer();

        bool Open(const std::filesystem::path& path, const Header& header, const MessageTable& table);
        bool Append(uint32_t time_ms, const uint8_t* body, size_t body_size);
        bool AppendChapter(const Chapter& chapter);
        bool AppendKeyframe(uint32_t time_ms, KeyframeKind kind, const std::vector<Record>& records);
        bool Flush();
        bool Close(const Header& header);

        [[nodiscard]] bool IsOpen() const { return file != nullptr; }
        [[nodiscard]] uint64_t Size() const { return size; }

    private:
        bool Write(const std::vector<uint8_t>& bytes);

        FILE* file = nullptr;
        uint64_t size = 0;
    };
} // namespace ReplayFile
