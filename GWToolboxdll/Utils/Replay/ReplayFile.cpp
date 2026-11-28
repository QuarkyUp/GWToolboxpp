#include <Utils/Replay/ReplayFile.h>
#include <Utils/Replay/ReplayMessages.h>
#include <Utils/StoCHandlers.h>

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
        constexpr size_t keyframe_entry_size = 15;
        constexpr size_t motions_entry_size = 8;
        constexpr size_t standing_motion_size = 32;
        constexpr size_t walking_motion_size = 56;
        constexpr size_t rows_entry_size = 4;
        constexpr size_t clip_size = 32;
        constexpr size_t height_size = 8;
        constexpr size_t camera_size = 40;
        constexpr size_t body_size_without_effects = clip_size + 10;
        constexpr size_t visible_effect_size = 12;
        constexpr size_t skill_row_size = 76;
        constexpr size_t effect_row_size = 16;
        constexpr size_t aura_row_size = 12;
        constexpr size_t map_row_size = 32;
        constexpr size_t texts_entry_size = 10;
        constexpr uint8_t motion_walking = 1;
        constexpr uint8_t motion_drawn = 2;

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

        struct FieldWriter {
            std::vector<uint8_t>& out;

            template <typename T>
            void operator()(T& value)
            {
                Put(out, value, sizeof(T));
            }
            void operator()(float& value) { Put(out, std::bit_cast<uint32_t>(value), 4); }
            template <typename T, size_t N>
            void operator()(std::array<T, N>& values)
            {
                for (auto& value : values) {
                    (*this)(value);
                }
            }
            void operator()(char (&text)[24]) { out.insert(out.end(), text, text + sizeof(text)); }
        };

        struct FieldReader {
            const uint8_t* bytes;
            size_t offset = 0;

            template <typename T>
            void operator()(T& value)
            {
                value = static_cast<T>(Get(bytes + offset, sizeof(T)));
                offset += sizeof(T);
            }
            void operator()(float& value)
            {
                value = std::bit_cast<float>(static_cast<uint32_t>(Get(bytes + offset, 4)));
                offset += 4;
            }
            template <typename T, size_t N>
            void operator()(std::array<T, N>& values)
            {
                for (auto& value : values) {
                    (*this)(value);
                }
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

        template <typename Visitor>
        void Visit(Visitor& visit, Clip& clip)
        {
            visit(clip.agent);
            visit(clip.animation);
            visit(clip.sequence);
            visit(clip.sequence_index);
            visit(clip.length);
            visit(clip.scale);
            visit(clip.model_state);
            visit(clip.rotation);
        }

        template <typename Visitor>
        void Visit(Visitor& visit, Height& height)
        {
            visit(height.agent);
            visit(height.z);
        }

        template <typename Visitor>
        void Visit(Visitor& visit, Camera& camera)
        {
            visit(camera.position);
            visit(camera.look_at);
            visit(camera.yaw);
            visit(camera.pitch);
            visit(camera.distance);
            visit(camera.field_of_view);
        }

        template <typename Visitor>
        void Visit(Visitor& visit, VisibleEffect& effect)
        {
            visit(effect.kind);
            visit(effect.effect);
            visit(effect.ended);
        }

        template <typename Visitor>
        void Visit(Visitor& visit, SkillRow& row)
        {
            visit(row.skill);
            visit(row.type);
            visit(row.special);
            visit(row.profession);
            visit(row.attribute);
            visit(row.name);
            visit(row.description);
            visit(row.icon_files);
            visit(row.visuals);
            visit(row.activation);
            visit(row.aftercast);
            visit(row.recharge);
        }

        template <typename Visitor>
        void Visit(Visitor& visit, EffectRow& row)
        {
            visit(row.effect);
            visit(row.model_file);
            visit(row.territory_model_file);
            visit(row.raw);
        }

        template <typename Visitor>
        void Visit(Visitor& visit, AuraRow& row)
        {
            visit(row.aura);
            visit(row.model_file);
            visit(row.raw);
        }

        template <typename Visitor>
        void Visit(Visitor& visit, MapRow& row)
        {
            visit(row.map);
            visit(row.campaign);
            visit(row.continent);
            visit(row.region);
            visit(row.type);
            visit(row.flags);
            visit(row.name);
            visit(row.description);
        }

        template <typename Row>
        bool AppendRows(Writer& writer, const uint16_t entry_id, const uint32_t time_ms, const std::vector<Row>& rows, const size_t row_size)
        {
            const auto rows_an_entry = (max_body_size - rows_entry_size) / row_size;
            for (size_t first = 0; first < rows.size(); first += rows_an_entry) {
                const auto count = std::min(rows_an_entry, rows.size() - first);
                std::vector<uint8_t> body;
                Put(body, entry_id, 2);
                Put(body, count, 2);
                FieldWriter field_writer{body};
                for (size_t i = 0; i < count; i++) {
                    auto row = rows[first + i];
                    Visit(field_writer, row);
                }
                if (!writer.Append(time_ms, body.data(), body.size())) {
                    return false;
                }
            }
            return true;
        }

        template <typename Row>
        bool ReadRows(const uint8_t* body, const size_t body_size, const size_t row_size, std::vector<Row>& out)
        {
            if (body_size < rows_entry_size || body_size != rows_entry_size + Get(body + 2, 2) * row_size) {
                return false;
            }
            FieldReader field_reader{body + rows_entry_size};
            for (auto count = Get(body + 2, 2); count; count--) {
                Visit(field_reader, out.emplace_back());
            }
            return true;
        }

        bool ReadMotions(const uint8_t* body, const size_t body_size, std::vector<Motion>& out)
        {
            const auto count = static_cast<size_t>(Get(body + 6, 2));
            auto at = motions_entry_size;
            for (; out.size() < count && at + standing_motion_size <= body_size;) {
                FieldReader field_reader{body + at};
                auto& motion = out.emplace_back();
                uint16_t plane = 0;
                uint8_t facing = 0;
                uint8_t flags = 0;
                field_reader(motion.agent);
                field_reader(motion.x);
                field_reader(motion.y);
                field_reader(plane);
                field_reader(motion.max_speed);
                field_reader(motion.move_speed);
                field_reader(facing);
                field_reader(flags);
                field_reader(motion.z);
                field_reader(motion.rotation);
                motion.plane = plane;
                motion.facing = facing;
                motion.walking = (flags & motion_walking) != 0;
                motion.drawn = (flags & motion_drawn) != 0;
                at += standing_motion_size;
                if (!motion.walking) {
                    continue;
                }
                if (at + walking_motion_size - standing_motion_size > body_size) {
                    return false;
                }
                uint16_t leg_plane = 0;
                uint16_t target_plane = 0;
                field_reader(motion.leg_x);
                field_reader(motion.leg_y);
                field_reader(leg_plane);
                field_reader(motion.target_x);
                field_reader(motion.target_y);
                field_reader(target_plane);
                field_reader(motion.target_agent);
                motion.leg_plane = leg_plane;
                motion.target_plane = target_plane;
                at += walking_motion_size - standing_motion_size;
            }
            return out.size() == count && at == body_size;
        }

        bool ReadBodies(const uint8_t* body, const size_t body_size, std::vector<Body>& out)
        {
            auto at = rows_entry_size;
            for (auto count = Get(body + 2, 2); count; count--) {
                if (at + body_size_without_effects > body_size) {
                    return false;
                }
                FieldReader field_reader{body + at};
                auto& read = out.emplace_back();
                uint16_t effects = 0;
                Visit(field_reader, read.clip);
                field_reader(read.started_ms);
                field_reader(read.z);
                field_reader(effects);
                at += body_size_without_effects + effects * visible_effect_size;
                if (at > body_size) {
                    return false;
                }
                read.effects.resize(effects);
                for (auto& effect : read.effects) {
                    Visit(field_reader, effect);
                }
            }
            return at == body_size;
        }

        bool ReadTexts(const uint8_t* body, const size_t body_size, TextFiles& out)
        {
            const auto characters = static_cast<size_t>(Get(body + 6, 2));
            const auto languages = static_cast<size_t>(Get(body + 8, 2));
            auto at = texts_entry_size + characters * 2;
            if (at > body_size) {
                return false;
            }
            out = {static_cast<uint32_t>(Get(body + 2, 4)), {}, {}};
            for (size_t i = 0; i < characters; i++) {
                out.characters.push_back(static_cast<wchar_t>(Get(body + texts_entry_size + i * 2, 2)));
            }
            for (size_t i = 0; i < languages; i++) {
                if (at + 6 > body_size || at + 6 + Get(body + at + 4, 2) * 4 > body_size) {
                    return false;
                }
                auto& language = out.languages.emplace_back(static_cast<uint32_t>(Get(body + at, 4)));
                language.files.resize(static_cast<size_t>(Get(body + at + 4, 2)));
                at += 6;
                for (auto& file : language.files) {
                    file = static_cast<uint32_t>(Get(body + at, 4));
                    at += 4;
                }
            }
            return at == body_size;
        }

        bool ParseHeader(const uint8_t* bytes, const size_t size, Header& out, uint16_t& version, std::string& error)
        {
            if (size < header_size || memcmp(bytes, magic, sizeof(magic)) != 0) {
                error = "not a recording (no magic)";
                return false;
            }
            FieldReader header_reader{bytes + sizeof(magic)};
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
            FieldWriter writer{out};
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
                    const auto type = static_cast<StoCFieldType>(fields[i] & 0xF);
                    const auto sub = fields[i] >> 4 & 0xF;
                    const auto size = fields[i] >> 8;
                    switch (type) {
                        case StoCFieldType::Id:
                        case StoCFieldType::Float:
                            Align(4);
                            Copy(4);
                            break;
                        case StoCFieldType::Vect2:
                            Align(4);
                            Copy(8);
                            break;
                        case StoCFieldType::Vect3:
                            Align(4);
                            Copy(12);
                            break;
                        case StoCFieldType::Int:
                        case StoCFieldType::IntB:
                            if (size != 1 && size != 2 && size != 4) {
                                return false;
                            }
                            Align(4);
                            Put(out, ReadDword(), size);
                            break;
                        case StoCFieldType::Blob:
                        case StoCFieldType::BlobB:
                            Copy(size);
                            break;
                        case StoCFieldType::ArrayData:
                        case StoCFieldType::ArrayDataB:
                            break;
                        case StoCFieldType::String16:
                            if (!PackText(size)) {
                                return false;
                            }
                            break;
                        case StoCFieldType::ArrayCount:
                            if (!PackArray(sub, size)) {
                                return false;
                            }
                            break;
                        case StoCFieldType::NestedStruct: {
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

    const char* EntryKindName(const EntryKind kind)
    {
        switch (kind) {
            case EntryKind::Stream:
                return "the stream";
            case EntryKind::Chapter:
                return "chapters";
            case EntryKind::Keyframe:
                return "keyframes";
            case EntryKind::Props:
                return "keyframes' props";
            case EntryKind::Bodies:
                return "keyframes' bodies";
            case EntryKind::Motions:
                return "motions";
            case EntryKind::Clip:
                return "clips";
            case EntryKind::Heights:
                return "heights";
            case EntryKind::Camera:
                return "the camera";
            case EntryKind::Skills:
                return "skill rows";
            case EntryKind::Effects:
                return "effect rows";
            case EntryKind::Auras:
                return "aura rows";
            case EntryKind::Maps:
                return "map rows";
            case EntryKind::Texts:
                return "text files";
            case EntryKind::Count:
                break;
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
        size_t keyframe_messages = 0;
        size_t keyframe_props = 0;
        size_t keyframe_bodies = 0;
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
            const auto total = [&out, body_size](const EntryKind kind, const bool entry = true) {
                out.totals[static_cast<size_t>(kind)].count += entry;
                out.totals[static_cast<size_t>(kind)].bytes += record_prefix_size + body_size;
            };
            if (keyframe_messages || keyframe_props) {
                if (message_id >= message_count) {
                    error = std::format("a keyframe message before record {} has message id {}, beyond the table's {}", record_count, message_id, message_count);
                    return false;
                }
                auto& keyframe = out.keyframes.back();
                total(keyframe_messages ? EntryKind::Keyframe : EntryKind::Props, !keyframe_messages);
                if (bodies) {
                    (keyframe_messages ? keyframe.records : keyframe.props).push_back({time_ms, {body, body + body_size}});
                }
                (keyframe_messages ? keyframe_messages : keyframe_props)--;
                continue;
            }
            auto whole = true;
            if (keyframe_bodies) {
                const auto count = message_id == bodies_entry_id && body_size >= rows_entry_size ? static_cast<size_t>(Get(body + 2, 2)) : 0;
                if (!count || count > keyframe_bodies || (bodies && !ReadBodies(body, body_size, out.keyframes.back().bodies))) {
                    error = std::format("the keyframe before record {} does not hold its bodies", record_count);
                    return false;
                }
                keyframe_bodies -= count;
                total(EntryKind::Bodies);
                continue;
            }
            switch (message_id) {
                case chapter_entry_id:
                    whole = body_size == chapter_entry_size;
                    if (whole) {
                        out.chapters.push_back({record_count, time_ms, static_cast<uint32_t>(Get(body + 2, 4)), static_cast<uint32_t>(Get(body + 6, 4)), static_cast<uint32_t>(Get(body + 10, 4))});
                    }
                    total(EntryKind::Chapter);
                    break;
                case keyframe_entry_id:
                    whole = body_size == keyframe_entry_size;
                    if (whole) {
                        out.keyframes.push_back({record_count, time_ms, static_cast<KeyframeKind>(body[2]), {}, {}, {}});
                        keyframe_messages = static_cast<size_t>(Get(body + 3, 4));
                        keyframe_props = static_cast<size_t>(Get(body + 7, 4));
                        keyframe_bodies = static_cast<size_t>(Get(body + 11, 4));
                    }
                    total(EntryKind::Keyframe);
                    break;
                case motions_entry_id:
                    whole = body_size >= motions_entry_size && (!bodies || ReadMotions(body, body_size, out.motions.emplace_back(record_count, time_ms, static_cast<uint32_t>(Get(body + 2, 4))).agents));
                    total(EntryKind::Motions);
                    break;
                case clip_entry_id:
                    whole = body_size == 2 + clip_size;
                    if (whole && bodies) {
                        FieldReader field_reader{body + 2};
                        Visit(field_reader, out.clips.emplace_back(record_count, time_ms).clip);
                    }
                    total(EntryKind::Clip);
                    break;
                case heights_entry_id:
                    whole = bodies ? ReadRows(body, body_size, height_size, out.heights.emplace_back(record_count, time_ms).agents) : body_size >= rows_entry_size;
                    total(EntryKind::Heights);
                    break;
                case camera_entry_id:
                    whole = body_size == 2 + camera_size;
                    if (whole && bodies) {
                        FieldReader field_reader{body + 2};
                        Visit(field_reader, out.cameras.emplace_back(record_count, time_ms).camera);
                    }
                    total(EntryKind::Camera);
                    break;
                case skills_entry_id:
                    whole = ReadRows(body, body_size, skill_row_size, out.skills);
                    total(EntryKind::Skills);
                    break;
                case effects_entry_id:
                    whole = ReadRows(body, body_size, effect_row_size, out.effects);
                    total(EntryKind::Effects);
                    break;
                case auras_entry_id:
                    whole = ReadRows(body, body_size, aura_row_size, out.auras);
                    total(EntryKind::Auras);
                    break;
                case maps_entry_id:
                    whole = ReadRows(body, body_size, map_row_size, out.maps);
                    total(EntryKind::Maps);
                    break;
                case texts_entry_id:
                    whole = body_size >= texts_entry_size && ReadTexts(body, body_size, out.texts);
                    total(EntryKind::Texts);
                    break;
                default:
                    if (message_id >= message_count) {
                        error = std::format("record {} has message id {}, beyond the table's {}", record_count, message_id, message_count);
                        return false;
                    }
                    out.records.push_back({time_ms, bodies ? std::vector<uint8_t>(body, body + body_size) : std::vector<uint8_t>()});
                    record_count++;
                    total(EntryKind::Stream);
                    break;
            }
            if (!whole) {
                error = std::format("the entry {:#x} before record {} is not whole: {} bytes", message_id, record_count, body_size);
                return false;
            }
        }
        if (keyframe_messages || keyframe_props || keyframe_bodies) {
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

    bool IsPlainText(const std::wstring_view text)
    {
        return !text.empty() && std::ranges::all_of(text, [](const wchar_t c) {
            return c >= 0x20 && c < 0x100;
        });
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

    bool Writer::AppendKeyframe(const Keyframe& keyframe)
    {
        std::vector<uint8_t> body;
        Put(body, keyframe_entry_id, 2);
        Put(body, static_cast<uint8_t>(keyframe.kind), 1);
        Put(body, keyframe.records.size(), 4);
        Put(body, keyframe.props.size(), 4);
        Put(body, keyframe.bodies.size(), 4);
        const auto append = [this, &keyframe](const Record& record) {
            return Append(keyframe.time_ms, record.body.data(), record.body.size());
        };
        if (!Append(keyframe.time_ms, body.data(), body.size()) || !std::ranges::all_of(keyframe.records, append) || !std::ranges::all_of(keyframe.props, append)) {
            return false;
        }
        for (size_t first = 0; first < keyframe.bodies.size();) {
            body.clear();
            Put(body, bodies_entry_id, 2);
            size_t count = 0;
            std::vector<uint8_t> packed;
            FieldWriter field_writer{packed};
            for (; first + count < keyframe.bodies.size(); count++) {
                auto written = keyframe.bodies[first + count];
                if (rows_entry_size + packed.size() + body_size_without_effects + written.effects.size() * visible_effect_size > max_body_size) {
                    break;
                }
                auto effects = static_cast<uint16_t>(written.effects.size());
                Visit(field_writer, written.clip);
                field_writer(written.started_ms);
                field_writer(written.z);
                field_writer(effects);
                for (auto& effect : written.effects) {
                    Visit(field_writer, effect);
                }
            }
            Put(body, count, 2);
            body.insert(body.end(), packed.begin(), packed.end());
            if (!count || !Append(keyframe.time_ms, body.data(), body.size())) {
                return false;
            }
            first += count;
        }
        return true;
    }

    bool Writer::AppendMotions(const uint32_t time_ms, const uint32_t clock, const std::vector<Motion>& agents)
    {
        for (size_t first = 0; first < agents.size();) {
            std::vector<uint8_t> body;
            Put(body, motions_entry_id, 2);
            Put(body, clock, 4);
            size_t count = 0;
            std::vector<uint8_t> packed;
            FieldWriter field_writer{packed};
            for (; first + count < agents.size() && motions_entry_size + packed.size() + walking_motion_size <= max_body_size; count++) {
                auto motion = agents[first + count];
                auto plane = static_cast<uint16_t>(motion.plane);
                auto facing = static_cast<uint8_t>(motion.facing);
                auto flags = static_cast<uint8_t>((motion.walking ? motion_walking : 0) | (motion.drawn ? motion_drawn : 0));
                field_writer(motion.agent);
                field_writer(motion.x);
                field_writer(motion.y);
                field_writer(plane);
                field_writer(motion.max_speed);
                field_writer(motion.move_speed);
                field_writer(facing);
                field_writer(flags);
                field_writer(motion.z);
                field_writer(motion.rotation);
                if (motion.walking) {
                    auto leg_plane = static_cast<uint16_t>(motion.leg_plane);
                    auto target_plane = static_cast<uint16_t>(motion.target_plane);
                    field_writer(motion.leg_x);
                    field_writer(motion.leg_y);
                    field_writer(leg_plane);
                    field_writer(motion.target_x);
                    field_writer(motion.target_y);
                    field_writer(target_plane);
                    field_writer(motion.target_agent);
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

    bool Writer::AppendClip(const uint32_t time_ms, const Clip& clip)
    {
        std::vector<uint8_t> body;
        Put(body, clip_entry_id, 2);
        FieldWriter field_writer{body};
        auto written = clip;
        Visit(field_writer, written);
        return Append(time_ms, body.data(), body.size());
    }

    bool Writer::AppendHeights(const uint32_t time_ms, const std::vector<Height>& agents)
    {
        return AppendRows(*this, heights_entry_id, time_ms, agents, height_size);
    }

    bool Writer::AppendCamera(const uint32_t time_ms, const Camera& camera)
    {
        std::vector<uint8_t> body;
        Put(body, camera_entry_id, 2);
        FieldWriter field_writer{body};
        auto written = camera;
        Visit(field_writer, written);
        return Append(time_ms, body.data(), body.size());
    }

    bool Writer::AppendSkills(const uint32_t time_ms, const std::vector<SkillRow>& rows)
    {
        return AppendRows(*this, skills_entry_id, time_ms, rows, skill_row_size);
    }

    bool Writer::AppendEffects(const uint32_t time_ms, const std::vector<EffectRow>& rows)
    {
        return AppendRows(*this, effects_entry_id, time_ms, rows, effect_row_size);
    }

    bool Writer::AppendAuras(const uint32_t time_ms, const std::vector<AuraRow>& rows)
    {
        return AppendRows(*this, auras_entry_id, time_ms, rows, aura_row_size);
    }

    bool Writer::AppendMaps(const uint32_t time_ms, const std::vector<MapRow>& rows)
    {
        return AppendRows(*this, maps_entry_id, time_ms, rows, map_row_size);
    }

    bool Writer::AppendTexts(const uint32_t time_ms, const TextFiles& texts)
    {
        std::vector<uint8_t> body;
        Put(body, texts_entry_id, 2);
        Put(body, texts.language, 4);
        Put(body, texts.characters.size(), 2);
        Put(body, texts.languages.size(), 2);
        for (const auto character : texts.characters) {
            Put(body, character, 2);
        }
        for (const auto& language : texts.languages) {
            Put(body, language.language, 4);
            Put(body, language.files.size(), 2);
            for (const auto file : language.files) {
                Put(body, file, 4);
            }
        }
        return Append(time_ms, body.data(), body.size());
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
