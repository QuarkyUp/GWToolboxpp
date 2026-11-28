#pragma once

#include <array>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <functional>
#include <set>
#include <string>
#include <string_view>
#include <vector>

namespace ReplayFile {
    constexpr char magic[8] = {'G', 'W', 'T', 'B', 'R', 'E', 'C', '\0'};
    constexpr uint16_t format_version = 4;
    constexpr uint16_t header_size = 96;
    constexpr size_t max_body_size = 0xFFFF;
    constexpr uint16_t chapter_entry_id = 0xF001;
    constexpr uint16_t keyframe_entry_id = 0xF002;
    constexpr uint16_t motions_entry_id = 0xF003;
    constexpr uint16_t clip_entry_id = 0xF004;
    constexpr uint16_t heights_entry_id = 0xF005;
    constexpr uint16_t camera_entry_id = 0xF006;
    constexpr uint16_t bodies_entry_id = 0xF007;
    constexpr uint16_t skills_entry_id = 0xF008;
    constexpr uint16_t effects_entry_id = 0xF009;
    constexpr uint16_t auras_entry_id = 0xF00A;
    constexpr uint16_t maps_entry_id = 0xF00B;
    constexpr uint16_t texts_entry_id = 0xF00C;
    constexpr uint32_t unknown_time = 0xFFFFFFFF;

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

    // A field descriptor's low four bits: 8, 9 and 10 are coded as 4, 5 and 6, and the client keeps them apart.
    enum class StoCFieldType : uint32_t {
        Id = 0,
        Float = 1,
        Vect2 = 2,
        Vect3 = 3,
        Int = 4,
        Blob = 5,
        ArrayData = 6,
        String16 = 7,
        IntB = 8,
        BlobB = 9,
        ArrayDataB = 10,
        ArrayCount = 11,
        NestedStruct = 12,
    };

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

    // The clip a living agent's body plays as the client's agent view holds it once the clip starts (IAgentView::CCharAgent): the animation, its sequence's id and index in the model (the variant picked), its length in seconds, the speed it plays at.
    struct Clip {
        uint32_t agent = 0;
        uint32_t animation = 0;
        uint32_t sequence = 0;
        uint32_t sequence_index = 0;
        float length = 0.f;
        float scale = 0.f;
        uint32_t model_state = 0;
        float rotation = 0.f;

        bool operator==(const Clip&) const = default;
    };

    struct ClipStart {
        size_t record_index = 0;
        uint32_t time_ms = 0;
        Clip clip;

        bool operator==(const ClipStart&) const = default;
    };

    struct VisibleEffect {
        uint32_t kind = 0;
        uint32_t effect = 0;
        uint32_t ended = 0;

        bool operator==(const VisibleEffect&) const = default;
    };

    // A living agent's body at a keyframe: its clip and when the recording saw it start (unknown_time: before the recording, its phase is not known), its height, the effects shown on it.
    struct Body {
        Clip clip;
        uint32_t started_ms = unknown_time;
        float z = 0.f;
        std::vector<VisibleEffect> effects;

        bool operator==(const Body&) const = default;
    };

    struct Keyframe {
        size_t record_index = 0;
        uint32_t time_ms = 0;
        KeyframeKind kind = KeyframeKind::Periodic;
        std::vector<Record> records;
        std::vector<Record> props;
        std::vector<Body> bodies;

        bool operator==(const Keyframe&) const = default;
    };

    // An agent's walk as the client's server world keeps it (IAgent::CAgent): where it is at the sample, and while walking its leg's end and its order's point and agent; when its body is drawn, the body's height and facing.
    struct Motion {
        uint32_t agent = 0;
        float x = 0.f;
        float y = 0.f;
        uint32_t plane = 0;
        float max_speed = 0.f;
        float move_speed = 0.f;
        uint32_t facing = 0;
        bool walking = false;
        float leg_x = 0.f;
        float leg_y = 0.f;
        uint32_t leg_plane = 0;
        float target_x = 0.f;
        float target_y = 0.f;
        uint32_t target_plane = 0;
        uint32_t target_agent = 0;
        bool drawn = false;
        float z = 0.f;
        float rotation = 0.f;

        bool operator==(const Motion&) const = default;
    };

    struct Motions {
        size_t record_index = 0;
        uint32_t time_ms = 0;
        uint32_t clock = 0;
        std::vector<Motion> agents;

        bool operator==(const Motions&) const = default;
    };

    struct Height {
        uint32_t agent = 0;
        float z = 0.f;

        bool operator==(const Height&) const = default;
    };

    struct Heights {
        size_t record_index = 0;
        uint32_t time_ms = 0;
        std::vector<Height> agents;

        bool operator==(const Heights&) const = default;
    };

    struct Camera {
        std::array<float, 3> position{};
        std::array<float, 3> look_at{};
        float yaw = 0.f;
        float pitch = 0.f;
        float distance = 0.f;
        float field_of_view = 0.f;

        bool operator==(const Camera&) const = default;
    };

    struct CameraSample {
        size_t record_index = 0;
        uint32_t time_ms = 0;
        Camera camera;

        bool operator==(const CameraSample&) const = default;
    };

    // The rows of the client's own tables a recording names, as the client holds them: a reader with Gw.dat needs no Gw.exe for them.
    struct SkillRow {
        uint32_t skill = 0;
        uint32_t type = 0;
        uint32_t special = 0;
        uint32_t profession = 0;
        uint32_t attribute = 0;
        uint32_t name = 0;
        uint32_t description = 0;
        std::array<uint32_t, 3> icon_files{};
        std::array<uint32_t, 6> visuals{};
        float activation = 0.f;
        float aftercast = 0.f;
        uint32_t recharge = 0;

        bool operator==(const SkillRow&) const = default;
    };

    struct EffectRow {
        uint32_t effect = 0;
        uint32_t model_file = 0;
        uint32_t territory_model_file = 0;
        uint32_t raw = 0;

        bool operator==(const EffectRow&) const = default;
    };

    struct AuraRow {
        uint32_t aura = 0;
        uint32_t model_file = 0;
        uint32_t raw = 0;

        bool operator==(const AuraRow&) const = default;
    };

    struct MapRow {
        uint32_t map = 0;
        uint32_t campaign = 0;
        uint32_t continent = 0;
        uint32_t region = 0;
        uint32_t type = 0;
        uint32_t flags = 0;
        uint32_t name = 0;
        uint32_t description = 0;

        bool operator==(const MapRow&) const = default;
    };

    struct LanguageFiles {
        uint32_t language = 0;
        std::vector<uint32_t> files;

        bool operator==(const LanguageFiles&) const = default;
    };

    // What decoding a text from Gw.dat takes beside the archive: each language's string files (string n is entry n mod 1024 of file n div 1024) and the characters a packed string's small values stand for.
    struct TextFiles {
        uint32_t language = 0;
        std::wstring characters;
        std::vector<LanguageFiles> languages;

        bool operator==(const TextFiles&) const = default;
    };

    enum class EntryKind : uint8_t {
        Stream,
        Chapter,
        Keyframe,
        Props,
        Bodies,
        Motions,
        Clip,
        Heights,
        Camera,
        Skills,
        Effects,
        Auras,
        Maps,
        Texts,
        Count,
    };
    const char* EntryKindName(EntryKind kind);

    struct EntryTotal {
        uint64_t count = 0;
        uint64_t bytes = 0;
    };

    struct Contents {
        Header header;
        MessageTable table;
        std::vector<Record> records;
        std::vector<Chapter> chapters;
        std::vector<Keyframe> keyframes;
        std::vector<Motions> motions;
        std::vector<ClipStart> clips;
        std::vector<Heights> heights;
        std::vector<CameraSample> cameras;
        std::vector<SkillRow> skills;
        std::vector<EffectRow> effects;
        std::vector<AuraRow> auras;
        std::vector<MapRow> maps;
        TextFiles texts;
        std::array<EntryTotal, static_cast<size_t>(EntryKind::Count)> totals{};
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
        bool AppendKeyframe(const Keyframe& keyframe);
        bool AppendMotions(uint32_t time_ms, uint32_t clock, const std::vector<Motion>& agents);
        bool AppendClip(uint32_t time_ms, const Clip& clip);
        bool AppendHeights(uint32_t time_ms, const std::vector<Height>& agents);
        bool AppendCamera(uint32_t time_ms, const Camera& camera);
        bool AppendSkills(uint32_t time_ms, const std::vector<SkillRow>& rows);
        bool AppendEffects(uint32_t time_ms, const std::vector<EffectRow>& rows);
        bool AppendAuras(uint32_t time_ms, const std::vector<AuraRow>& rows);
        bool AppendMaps(uint32_t time_ms, const std::vector<MapRow>& rows);
        bool AppendTexts(uint32_t time_ms, const TextFiles& texts);
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
