#include <Utils/ReplayFile.h>
#include <Utils/ReplayMessages.h>

#include <algorithm>
#include <bit>
#include <cstring>
#include <cwchar>
#include <format>

namespace ReplayFile {
    namespace {
        using namespace ReplayMessages;

        constexpr size_t max_decoded_size = 0x10000;
        constexpr uint32_t max_messages = 0x1000;
        constexpr uint32_t max_descriptors = 0x400;
        constexpr size_t record_prefix_size = 6;
        constexpr size_t chapter_entry_size = 14;
        constexpr size_t keyframe_entry_size = 7;
        constexpr size_t motions_entry_size = 8;
        constexpr size_t standing_motion_size = 24;
        constexpr size_t walking_motion_size = 48;

        void Put(std::vector<uint8_t>& out, const uint64_t value, const size_t size)
        {
            for (size_t i = 0; i < size; i++) {
                out.push_back(static_cast<uint8_t>(value >> (8 * i)));
            }
        }

        uint64_t Get(const uint8_t* bytes, const size_t size)
        {
            uint64_t value = 0;
            for (size_t i = 0; i < size; i++) {
                value |= static_cast<uint64_t>(bytes[i]) << (8 * i);
            }
            return value;
        }

        struct HeaderWriter {
            std::vector<uint8_t>& out;

            template <typename T>
            void operator()(T& value)
            {
                Put(out, value, sizeof(T));
            }
            void operator()(char (&text)[24]) { out.insert(out.end(), text, text + sizeof(text)); }
        };

        struct HeaderReader {
            const uint8_t* bytes;
            size_t offset = 0;

            template <typename T>
            void operator()(T& value)
            {
                value = static_cast<T>(Get(bytes + offset, sizeof(T)));
                offset += sizeof(T);
            }
            void operator()(char (&text)[24])
            {
                memcpy(text, bytes + offset, sizeof(text));
                text[sizeof(text) - 1] = '\0';
                offset += sizeof(text);
            }
        };

        template <typename Visitor>
        void VisitHeader(Visitor& visit, Header& header, uint16_t& version, uint16_t& size)
        {
            visit(version);
            visit(size);
            visit(header.client_build);
            visit(header.table_digest);
            visit(header.start_time_unix_ms);
            visit(header.duration_ms);
            visit(header.map_id);
            visit(header.map_region_type);
            visit(header.is_explorable);
            visit(header.district);
            visit(header.region);
            visit(header.language);
            visit(header.record_count);
            visit(header.layout_errors);
            visit(header.flags);
            visit(header.toolbox_version);
        }

        bool ParseHeader(const uint8_t* bytes, const size_t size, Header& out, uint16_t& version, std::string& error)
        {
            if (size < header_size || memcmp(bytes, magic, sizeof(magic)) != 0) {
                error = "not a recording (no magic)";
                return false;
            }
            HeaderReader header_reader{bytes + sizeof(magic)};
            uint16_t stored_size = 0;
            VisitHeader(header_reader, out, version, stored_size);
            if (version != format_version || stored_size != header_size) {
                error = std::format("format version {} with a header of {} bytes, expected {} and {}: made by an older toolbox, its recordings no longer play", version, stored_size, format_version, header_size);
                return false;
            }
            return true;
        }

        std::vector<uint8_t> HeaderBytes(Header header)
        {
            std::vector<uint8_t> out(magic, magic + sizeof(magic));
            HeaderWriter writer{out};
            auto version = format_version;
            auto size = header_size;
            VisitHeader(writer, header, version, size);
            return out;
        }

        std::vector<uint8_t> TableBytes(const MessageTable& table)
        {
            std::vector<uint8_t> out;
            Put(out, table.size(), 4);
            for (const auto& descriptors : table) {
                Put(out, descriptors.size(), 4);
                for (const auto descriptor : descriptors) {
                    Put(out, descriptor, 4);
                }
            }
            return out;
        }

        struct Packer {
            const uint8_t* decoded;
            std::vector<uint8_t>& out;
            const TextFilter& filter;
            size_t offset = 4;

            void Align(const size_t alignment) { offset = (offset + alignment - 1) & ~(alignment - 1); }

            uint32_t ReadDword()
            {
                uint32_t value;
                memcpy(&value, decoded + offset, sizeof(value));
                offset += sizeof(value);
                return value;
            }

            void Copy(const size_t size)
            {
                out.insert(out.end(), decoded + offset, decoded + offset + size);
                offset += size;
            }

            bool PackText(const uint32_t size)
            {
                Align(2);
                std::wstring text(size, L'\0');
                memcpy(text.data(), decoded + offset, size * sizeof(wchar_t));
                offset += size * sizeof(wchar_t);
                const auto length = wcsnlen(text.data(), size);
                if (length >= size) {
                    return false;
                }
                text.resize(length);
                if (filter && length) {
                    filter(text, size);
                    text.resize(std::min<size_t>(text.size(), size - 1));
                }
                Put(out, text.size(), 2);
                const auto text_bytes = reinterpret_cast<const uint8_t*>(text.data());
                out.insert(out.end(), text_bytes, text_bytes + text.size() * sizeof(wchar_t));
                return true;
            }

            bool PackArray(const uint32_t item_shift, const uint32_t size)
            {
                if (item_shift > 2) {
                    return false;
                }
                const size_t item_size = size_t{1} << item_shift;
                Align(4);
                const auto count = ReadDword();
                if (count > size) {
                    return false;
                }
                Put(out, count, 2);
                Align(item_size);
                const auto items = offset;
                Copy(count * item_size);
                offset = items + size * item_size;
                return true;
            }

            bool Pack(const uint32_t* fields, const uint32_t field_count)
            {
                for (uint32_t i = 0; i < field_count; i++) {
                    if (offset > max_decoded_size) {
                        return false;
                    }
                    const auto type = fields[i] & 0xF;
                    const auto sub = fields[i] >> 4 & 0xF;
                    const auto size = fields[i] >> 8;
                    switch (type) {
                        case 0:
                        case 1:
                            Align(4);
                            Copy(4);
                            break;
                        case 2:
                            Align(4);
                            Copy(8);
                            break;
                        case 3:
                            Align(4);
                            Copy(12);
                            break;
                        case 4:
                        case 8:
                            if (size != 1 && size != 2 && size != 4) {
                                return false;
                            }
                            Align(4);
                            Put(out, ReadDword(), size);
                            break;
                        case 5:
                        case 9:
                            Copy(size);
                            break;
                        case 6:
                        case 10:
                            break;
                        case 7:
                            if (!PackText(size)) {
                                return false;
                            }
                            break;
                        case 11:
                            if (!PackArray(sub, size)) {
                                return false;
                            }
                            break;
                        case 12: {
                            Align(4);
                            const auto repetitions = ReadDword();
                            if (repetitions > size || repetitions > 0xFF) {
                                return false;
                            }
                            Put(out, repetitions, 1);
                            for (uint32_t r = 0; r < repetitions; r++) {
                                Align(4);
                                if (!Pack(fields + i + 1, field_count - i - 1)) {
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
    } // namespace

    uint64_t TableDigest(const MessageTable& table)
    {
        uint64_t digest = 0xcbf29ce484222325;
        for (const auto byte : TableBytes(table)) {
            digest = (digest ^ byte) * 0x100000001b3;
        }
        return digest;
    }

    bool PackMessage(const uint32_t message_id, const uint32_t* fields, const uint32_t field_count, const uint8_t* decoded, std::vector<uint8_t>& out, const TextFilter& filter)
    {
        out.clear();
        Put(out, message_id, 2);
        if (!field_count) {
            return true;
        }
        Packer packer{decoded, out, filter};
        return packer.Pack(fields + 1, field_count - 1) && out.size() <= max_body_size;
    }

    const char* KeyframeKindName(const KeyframeKind kind)
    {
        switch (kind) {
            case KeyframeKind::Start:
                return "start";
            case KeyframeKind::AfterLoad:
                return "after a load";
            case KeyframeKind::Periodic:
                return "periodic";
        }
        return "?";
    }

    size_t Contents::ChapterOf(const size_t record_index) const
    {
        const auto after = std::ranges::upper_bound(chapters, record_index, {}, &Chapter::first_record) - chapters.begin();
        return after ? static_cast<size_t>(after - 1) : 0;
    }

    bool Read(const std::filesystem::path& path, Contents& out, std::string& error, const bool bodies)
    {
        out = {};
        const auto file = _wfopen(path.c_str(), L"rb");
        if (!file) {
            error = "cannot open the file";
            return false;
        }
        std::vector<uint8_t> bytes;
        uint8_t chunk[0x10000];
        size_t read;
        while ((read = fread(chunk, 1, sizeof(chunk), file)) > 0) {
            bytes.insert(bytes.end(), chunk, chunk + read);
        }
        fclose(file);

        uint16_t version = 0;
        if (!ParseHeader(bytes.data(), bytes.size(), out.header, version, error)) {
            return false;
        }

        size_t offset = header_size;
        const auto remaining = [&] {
            return bytes.size() - offset;
        };
        const auto take = [&](const size_t n) {
            const auto value = Get(bytes.data() + offset, n);
            offset += n;
            return value;
        };
        if (remaining() < 4) {
            error = "no message table";
            return false;
        }
        const auto message_count = static_cast<uint32_t>(take(4));
        if (message_count > max_messages) {
            error = std::format("{} messages in the table", message_count);
            return false;
        }
        out.table.resize(message_count);
        for (auto& descriptors : out.table) {
            if (remaining() < 4) {
                error = "message table cut short";
                return false;
            }
            const auto descriptor_count = static_cast<uint32_t>(take(4));
            if (descriptor_count > max_descriptors || remaining() < descriptor_count * 4) {
                error = "message table cut short";
                return false;
            }
            descriptors.resize(descriptor_count);
            for (auto& descriptor : descriptors) {
                descriptor = static_cast<uint32_t>(take(4));
            }
        }
        if (TableDigest(out.table) != out.header.table_digest) {
            error = "the message table does not match its digest";
            return false;
        }

        size_t record_count = 0;
        uint32_t keyframe_messages = 0;
        while (remaining()) {
            if (remaining() < record_prefix_size) {
                out.truncated_tail = true;
                break;
            }
            const auto time_ms = static_cast<uint32_t>(take(4));
            const auto body_size = static_cast<size_t>(take(2));
            if (body_size < 2) {
                error = std::format("record {} has no message id", record_count);
                return false;
            }
            if (remaining() < body_size) {
                out.truncated_tail = true;
                break;
            }
            const auto body = bytes.data() + offset;
            const auto message_id = static_cast<uint16_t>(Get(body, 2));
            offset += body_size;
            if (!keyframe_messages && message_id == chapter_entry_id && body_size == chapter_entry_size) {
                out.chapters.push_back({record_count, time_ms, static_cast<uint32_t>(Get(body + 2, 4)), static_cast<uint32_t>(Get(body + 6, 4)), static_cast<uint32_t>(Get(body + 10, 4))});
                continue;
            }
            if (!keyframe_messages && message_id == keyframe_entry_id && body_size == keyframe_entry_size) {
                out.keyframes.push_back({record_count, time_ms, static_cast<KeyframeKind>(body[2]), {}});
                keyframe_messages = static_cast<uint32_t>(Get(body + 3, 4));
                continue;
            }
            if (!keyframe_messages && message_id == motions_entry_id && body_size >= motions_entry_size) {
                if (!bodies) {
                    continue;
                }
                auto& motions = out.motions.emplace_back(record_count, time_ms, static_cast<uint32_t>(Get(body + 2, 4)));
                const auto count = static_cast<size_t>(Get(body + 6, 2));
                auto at = motions_entry_size;
                for (; motions.agents.size() < count && at + standing_motion_size <= body_size;) {
                    const auto read = [&body, &at](const size_t size) {
                        const auto value = Get(body + at, size);
                        at += size;
                        return value;
                    };
                    const auto real = [&read] {
                        return std::bit_cast<float>(static_cast<uint32_t>(read(4)));
                    };
                    auto& motion = motions.agents.emplace_back();
                    motion.agent = static_cast<uint32_t>(read(4));
                    motion.x = real();
                    motion.y = real();
                    motion.plane = static_cast<uint32_t>(read(2));
                    motion.max_speed = real();
                    motion.move_speed = real();
                    motion.facing = static_cast<uint32_t>(read(1));
                    motion.walking = read(1) != 0;
                    if (!motion.walking) {
                        continue;
                    }
                    if (at + walking_motion_size - standing_motion_size > body_size) {
                        motions.agents.pop_back();
                        break;
                    }
                    motion.leg_x = real();
                    motion.leg_y = real();
                    motion.leg_plane = static_cast<uint32_t>(read(2));
                    motion.target_x = real();
                    motion.target_y = real();
                    motion.target_plane = static_cast<uint32_t>(read(2));
                    motion.target_agent = static_cast<uint32_t>(read(4));
                }
                if (motions.agents.size() != count || at != body_size) {
                    error = std::format("the motions before record {} do not hold their {} agents", record_count, count);
                    return false;
                }
                continue;
            }
            if (message_id >= message_count) {
                error = std::format("record {} has message id {}, beyond the table's {}", record_count, message_id, message_count);
                return false;
            }
            if (keyframe_messages) {
                keyframe_messages--;
                if (bodies) {
                    out.keyframes.back().records.push_back({time_ms, {body, body + body_size}});
                }
                continue;
            }
            out.records.push_back({time_ms, bodies ? std::vector<uint8_t>(body, body + body_size) : std::vector<uint8_t>()});
            record_count++;
        }
        if (keyframe_messages) {
            out.keyframes.pop_back();
            out.truncated_tail = true;
        }
        if (out.chapters.empty() || out.chapters.front().first_record) {
            error = "its stream does not begin with a chapter";
            return false;
        }
        if ((out.header.flags & Complete) && out.header.record_count != record_count) {
            error = std::format("the header counts {} records, the file holds {}", out.header.record_count, record_count);
            return false;
        }
        return true;
    }

    namespace {
        constexpr uint32_t tag_capacity = 6;
        constexpr uint32_t guild_name_min_capacity = 32;
        constexpr uint16_t guild_messages[] = {GAME_SMSG_TOWN_ALLIANCE_OBJECT, GAME_SMSG_GUILD_ALLIANCE_INFO, GAME_SMSG_GUILD_GENERAL_INFO, GAME_SMSG_GUILD_INVITE_RECEIVED, MissionAutoTournPartyAdd, MissionToClient431};
        constexpr uint16_t guild_tag_messages[] = {GAME_SMSG_CHAT_MESSAGE_GLOBAL, GAME_SMSG_TOWN_ALLIANCE_OBJECT, GAME_SMSG_GUILD_ALLIANCE_INFO, GAME_SMSG_GUILD_GENERAL_INFO, GAME_SMSG_GUILD_INVITE_RECEIVED, MissionAutoTournPartyAdd, MissionToClient431};
    } // namespace

    bool IsPlainText(const std::wstring_view text)
    {
        return !text.empty() && std::ranges::all_of(text, [](const wchar_t c) {
            return c >= 0x20 && c < 0x100;
        });
    }

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
        if (message_id == CharAccountNameData || message_id == GAME_SMSG_HERO_ACCOUNT_NAME) {
            account_names.emplace(name);
        }
        if (message_id == GAME_SMSG_INSTANCE_LOAD_PLAYER_NAME) {
            recorder_names.emplace(name);
        }
        if (account_names.contains(name)) {
            return own_names ? own_name_placeholder : other_players ? numbered(players, L"Player ") : std::wstring(name);
        }
        if (message_id == GAME_SMSG_MERCENARY_INFO) {
            return own_names ? numbered(mercenaries, L"Mercenary ") : other_players ? numbered(players, L"Player ") : std::wstring(name);
        }
        if (recorder_names.contains(name)) {
            return own_names ? own_name_placeholder : std::wstring(name);
        }
        return other_players ? numbered(players, L"Player ") : std::wstring(name);
    }

    void NameStripper::Apply(const uint16_t message_id, std::wstring& text, const uint32_t capacity)
    {
        if (IsPlainText(text)) {
            text = Label(message_id, text, capacity);
            return;
        }
        std::wstring stripped;
        size_t position = 0;
        for (auto start = text.find(literal_start); start != std::wstring::npos; start = text.find(literal_start, position)) {
            const auto end = text.find(literal_end, start + 1);
            if (end == std::wstring::npos) {
                break;
            }
            stripped.append(text, position, start + 1 - position);
            const auto literal = std::wstring_view(text).substr(start + 1, end - start - 1);
            stripped += IsPlainText(literal) ? Label(message_id, literal, 0) : std::wstring(literal);
            position = end;
        }
        stripped.append(text, position);
        text = std::move(stripped);
    }

    TextFilter NameStripper::For(const uint16_t message_id)
    {
        return [this, message_id](std::wstring& text, const uint32_t capacity) {
            Apply(message_id, text, capacity);
        };
    }

    bool ReadHeader(const std::filesystem::path& path, Header& out, std::string& error)
    {
        out = {};
        const auto file = _wfopen(path.c_str(), L"rb");
        if (!file) {
            error = "cannot open the file";
            return false;
        }
        uint8_t bytes[header_size];
        const auto read = fread(bytes, 1, sizeof(bytes), file);
        fclose(file);
        uint16_t version = 0;
        return ParseHeader(bytes, read, out, version, error);
    }

    Writer::~Writer()
    {
        if (file) {
            fclose(file);
        }
    }

    bool Writer::Write(const std::vector<uint8_t>& bytes)
    {
        if (fwrite(bytes.data(), 1, bytes.size(), file) != bytes.size()) {
            return false;
        }
        size += bytes.size();
        return true;
    }

    bool Writer::Open(const std::filesystem::path& path, const Header& header, const MessageTable& table)
    {
        if (file) {
            return false;
        }
        file = _wfopen(path.c_str(), L"wb");
        if (!file) {
            return false;
        }
        size = 0;
        setvbuf(file, nullptr, _IOFBF, 0x40000);
        return Write(HeaderBytes(header)) && Write(TableBytes(table)) && fflush(file) == 0;
    }

    bool Writer::Append(const uint32_t time_ms, const uint8_t* body, const size_t body_size)
    {
        if (!file || body_size < 2 || body_size > max_body_size) {
            return false;
        }
        std::vector<uint8_t> bytes;
        bytes.reserve(record_prefix_size + body_size);
        Put(bytes, time_ms, 4);
        Put(bytes, body_size, 2);
        bytes.insert(bytes.end(), body, body + body_size);
        return Write(bytes);
    }

    bool Writer::AppendChapter(const Chapter& chapter)
    {
        std::vector<uint8_t> body;
        Put(body, chapter_entry_id, 2);
        Put(body, chapter.map_id, 4);
        Put(body, chapter.is_explorable, 4);
        Put(body, chapter.map_region_type, 4);
        return Append(chapter.time_ms, body.data(), body.size());
    }

    bool Writer::AppendKeyframe(const uint32_t time_ms, const KeyframeKind kind, const std::vector<Record>& records)
    {
        std::vector<uint8_t> body;
        Put(body, keyframe_entry_id, 2);
        Put(body, static_cast<uint8_t>(kind), 1);
        Put(body, records.size(), 4);
        return Append(time_ms, body.data(), body.size()) && std::ranges::all_of(records, [this, time_ms](const Record& record) {
                   return Append(time_ms, record.body.data(), record.body.size());
               });
    }

    bool Writer::AppendMotions(const uint32_t time_ms, const uint32_t clock, const std::vector<Motion>& agents)
    {
        const auto real = [](std::vector<uint8_t>& out, const float value) {
            Put(out, std::bit_cast<uint32_t>(value), 4);
        };
        for (size_t first = 0; first < agents.size();) {
            std::vector<uint8_t> body;
            Put(body, motions_entry_id, 2);
            Put(body, clock, 4);
            size_t count = 0;
            std::vector<uint8_t> packed;
            for (; first + count < agents.size() && motions_entry_size + packed.size() + walking_motion_size <= max_body_size; count++) {
                const auto& motion = agents[first + count];
                Put(packed, motion.agent, 4);
                real(packed, motion.x);
                real(packed, motion.y);
                Put(packed, motion.plane, 2);
                real(packed, motion.max_speed);
                real(packed, motion.move_speed);
                Put(packed, motion.facing, 1);
                Put(packed, motion.walking, 1);
                if (motion.walking) {
                    real(packed, motion.leg_x);
                    real(packed, motion.leg_y);
                    Put(packed, motion.leg_plane, 2);
                    real(packed, motion.target_x);
                    real(packed, motion.target_y);
                    Put(packed, motion.target_plane, 2);
                    Put(packed, motion.target_agent, 4);
                }
            }
            Put(body, count, 2);
            body.insert(body.end(), packed.begin(), packed.end());
            if (!Append(time_ms, body.data(), body.size())) {
                return false;
            }
            first += count;
        }
        return true;
    }

    bool Writer::Flush()
    {
        return file && fflush(file) == 0;
    }

    bool Writer::Close(const Header& header)
    {
        if (!file) {
            return false;
        }
        const auto bytes = HeaderBytes(header);
        const auto written = fflush(file) == 0 && fseek(file, 0, SEEK_SET) == 0 && fwrite(bytes.data(), 1, bytes.size(), file) == bytes.size();
        const auto closed = fclose(file) == 0;
        file = nullptr;
        return written && closed;
    }
} // namespace ReplayFile
