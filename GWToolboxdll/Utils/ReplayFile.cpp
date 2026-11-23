#include <Utils/ReplayFile.h>

#include <algorithm>
#include <cstring>
#include <cwchar>
#include <format>

namespace ReplayFile {
    namespace {
        constexpr size_t max_decoded_size = 0x10000;
        constexpr uint32_t max_messages = 0x1000;
        constexpr uint32_t max_descriptors = 0x400;
        constexpr size_t record_prefix_size = 6;

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
            void operator()(T& value) { Put(out, value, sizeof(T)); }
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
                    filter(text);
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
    }

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

    bool Read(const std::filesystem::path& path, Contents& out, std::string& error)
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

        if (bytes.size() < header_size || memcmp(bytes.data(), magic, sizeof(magic)) != 0) {
            error = "not a recording (no magic)";
            return false;
        }
        HeaderReader header_reader{bytes.data() + sizeof(magic)};
        uint16_t version = 0;
        uint16_t size = 0;
        VisitHeader(header_reader, out.header, version, size);
        if (version != format_version || size != header_size) {
            error = std::format("format version {} with a header of {} bytes, expected {} and {}", version, size, format_version, header_size);
            return false;
        }

        size_t offset = header_size;
        const auto remaining = [&] { return bytes.size() - offset; };
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

        while (remaining()) {
            if (remaining() < record_prefix_size) {
                out.truncated_tail = true;
                break;
            }
            Record record;
            record.time_ms = static_cast<uint32_t>(take(4));
            const auto body_size = static_cast<size_t>(take(2));
            if (body_size < 2) {
                error = std::format("record {} has no message id", out.records.size());
                return false;
            }
            if (remaining() < body_size) {
                out.truncated_tail = true;
                break;
            }
            record.body.assign(bytes.begin() + offset, bytes.begin() + offset + body_size);
            offset += body_size;
            if (record.MessageId() >= message_count) {
                error = std::format("record {} has message id {}, beyond the table's {}", out.records.size(), record.MessageId(), message_count);
                return false;
            }
            out.records.push_back(std::move(record));
        }
        if ((out.header.flags & Complete) && out.header.record_count != out.records.size()) {
            error = std::format("the header counts {} records, the file holds {}", out.header.record_count, out.records.size());
            return false;
        }
        return true;
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
        return Write(HeaderBytes(header)) && Write(TableBytes(table));
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
}
