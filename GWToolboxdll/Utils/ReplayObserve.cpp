#include <Utils/ReplayObserve.h>
#include <Utils/ReplayState.h>

#include <algorithm>
#include <ctime>
#include <cwchar>
#include <format>

namespace ReplayObserve {
    uint32_t Category(const uint32_t type, const uint32_t title)
    {
        switch (type) {
            case 0:
                return title >= 1 && title <= 24 ? 0 : std::min(10 + title, 12u);
            case 1:
                return 7;
            case 2:
                return 3;
            case 3:
                return 4;
            case 4:
                return 5;
            case 5:
                return 8;
            case 6:
                return 9;
            case 7:
                return 6;
            case 8:
                return 2;
            case 9:
                return 1;
            default:
                return 13;
        }
    }

    std::wstring Duration(const uint32_t milliseconds)
    {
        const auto seconds = milliseconds / 1000;
        return seconds >= 3600 ? std::format(L"{}:{:02}:{:02}", seconds / 3600, seconds / 60 % 60, seconds % 60) : std::format(L"{}:{:02}", seconds / 60, seconds % 60);
    }

    Scan ScanFolder(const std::filesystem::path& folder, const uint32_t client_build, const uint64_t table_digest, const std::set<uint32_t>& taken_keys, const uint64_t now_unix_ms)
    {
        Scan scan;
        std::error_code error_code;
        for (const auto& entry : std::filesystem::directory_iterator(folder, error_code)) {
            if (!entry.is_regular_file(error_code) || entry.path().extension() != L".gwrec") {
                continue;
            }
            Row row{entry.path(), {}, {}};
            const auto& header = row.header;
            std::string error;
            auto reason = !ReplayFile::ReadHeader(row.path, row.header, error) ? error
                          : header.client_build != client_build                ? std::format("recorded on client build {}, this client is build {}", header.client_build, client_build)
                          : header.table_digest != table_digest                ? "its message table is not this client's"
                          : !(header.flags & ReplayFile::Complete)             ? "unfinished: still being recorded, or the recording stopped before its end"
                          : !header.record_count                               ? "no messages"
                          : !header.map_id || header.map_id > 0xFFFF           ? std::format("map {} is not a map", header.map_id)
                                                                               : std::string();
            if (reason.empty()) {
                scan.rows.push_back(std::move(row));
            }
            else {
                scan.left_out.push_back({std::move(row.path), std::move(reason)});
            }
        }
        std::ranges::sort(scan.rows, [](const Row& a, const Row& b) {
            return a.header.start_time_unix_ms != b.header.start_time_unix_ms ? a.header.start_time_unix_ms > b.header.start_time_unix_ms : a.path < b.path;
        });
        for (size_t i = max_rows; i < scan.rows.size(); i++) {
            scan.left_out.push_back({scan.rows[i].path, std::format("older than the newest {} recordings", max_rows)});
        }
        scan.rows.resize(std::min(scan.rows.size(), max_rows));

        auto key = first_key;
        for (auto& row : scan.rows) {
            while (taken_keys.contains(key)) {
                key++;
            }
            const auto& header = row.header;
            const auto started = static_cast<__time64_t>(header.start_time_unix_ms / 1000);
            std::tm local{};
            wchar_t started_text[32] = L"?";
            if (_localtime64_s(&local, &started) == 0) {
                wcsftime(started_text, std::size(started_text), L"%Y-%m-%d %H:%M", &local);
            }
            auto& message = row.message;
            message.key = key++;
            message.map_id = header.map_id;
            message.age_minutes = static_cast<uint32_t>(now_unix_ms > header.start_time_unix_ms ? std::min<uint64_t>((now_unix_ms - header.start_time_unix_ms) / 60000, 0xFFFF) : 0);
            message.team_count = 1;
            message.teams[0].team_id = 1;
            const auto name = std::format(L"\x108\x107Replay {} ({})\x1", started_text, Duration(header.duration_ms));
            wcsncpy(message.teams[0].name, name.c_str(), team_name_size - 1);
        }
        return scan;
    }

    std::string CheckRow(const ReplayFile::MessageTable& table, const RowMessage& message)
    {
        if (table.size() <= rows_committed_message_id || table[row_message_id].empty() || table[rows_committed_message_id].size() != 1) {
            return "the client's table has no 414 row or a 415 with fields";
        }
        if (!IsOurKey(message.key)) {
            return std::format("key {:#x} is outside {:#x} to {:#x}", message.key, first_key, first_key + key_span - 1);
        }
        const auto category = Category(message.type, message.title);
        if (!IsAllowedType(message.type) || category == 9) {
            return std::format("type {} (category {}) would make the window assert", message.type, category);
        }
        if (message.team_count > std::size(message.teams)) {
            return std::format("{} teams", message.team_count);
        }
        const auto& fields = table[row_message_id];
        std::vector<uint8_t> wire;
        if (!ReplayFile::PackMessage(row_message_id, fields.data(), static_cast<uint32_t>(fields.size()), reinterpret_cast<const uint8_t*>(&message), wire, {})) {
            return "the row does not pack by the client's 414 table";
        }
        ReplayState::Message decoded;
        if (!ReplayState::Decode(table, wire, decoded)) {
            return "the packed row does not decode by the client's 414 table";
        }
        std::vector<ReplayState::Field> expected = {{{message.key}, {}}, {{message.map_id}, {}}, {{message.age_minutes}, {}}, {{message.type}, {}}, {{message.title}, {}}, {{message.h0018}, {}}, {{message.team_count}, {}}};
        for (uint32_t i = 0; i < message.team_count; i++) {
            const auto& team = message.teams[i];
            expected.push_back({{team.flags}, {}});
            expected.push_back({{team.team_id}, {}});
            for (const auto value : team.tabard) {
                expected.push_back({{value}, {}});
            }
            expected.push_back({{}, std::wstring(team.name, wcsnlen(team.name, team_name_size))});
        }
        if (decoded.fields != expected) {
            return "the client's 414 table reads the row's fields differently from the row's layout";
        }
        return {};
    }
} // namespace ReplayObserve
