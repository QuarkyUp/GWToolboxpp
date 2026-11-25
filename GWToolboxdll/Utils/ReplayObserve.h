#pragma once

#include <Utils/ReplayFile.h>
#include <Utils/ReplayMessages.h>

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <set>
#include <string>
#include <vector>

namespace ReplayObserve {
    constexpr uint16_t row_message_id = ReplayMessages::MissionToClient414;
    constexpr uint16_t rows_committed_message_id = ReplayMessages::MissionToClient415;
    constexpr uint32_t first_key = 0x52500000;
    constexpr uint32_t key_span = 0x10000;
    constexpr size_t max_rows = 100;
    constexpr uint32_t row_type = 0;
    constexpr uint32_t row_title = 0;
    constexpr size_t team_name_size = 48;

    struct Team {
        uint32_t flags = 0;
        uint32_t team_id = 0;
        uint32_t tabard[7] = {};
        wchar_t name[team_name_size] = {};
    };

    struct RowMessage {
        uint32_t header = row_message_id;
        uint32_t key = 0;
        uint32_t map_id = 0;
        uint32_t age_minutes = 0;
        uint32_t type = row_type;
        uint32_t title = row_title;
        uint32_t h0018 = 0;
        uint32_t team_count = 0;
        Team teams[2] = {};
    };
    // The offsets the client's 414 row add reads (MsCliObserve.cpp): teams from +0x20, 0x84 bytes each, a team's name at +0x24.
    static_assert(sizeof(Team) == 0x84 && offsetof(Team, name) == 0x24 && offsetof(RowMessage, teams) == 0x20 && sizeof(RowMessage) == 0x128);

    struct Row {
        std::filesystem::path path;
        ReplayFile::Header header;
        size_t chapter = 0;
        std::optional<size_t> keyframe;
        size_t maps = 0;
        RowMessage message;
    };

    struct LeftOut {
        std::filesystem::path path;
        std::string reason;
    };

    struct Scan {
        std::vector<Row> rows;
        std::vector<LeftOut> left_out;
    };

    [[nodiscard]] constexpr bool IsOurKey(const uint32_t key)
    {
        return key - first_key < key_span;
    }
    [[nodiscard]] constexpr bool IsAllowedType(const uint32_t type)
    {
        return type < 10 && type != 6;
    }
    [[nodiscard]] uint32_t Category(uint32_t type, uint32_t title);
    std::wstring Duration(uint32_t milliseconds);

    Scan ScanFolder(const std::filesystem::path& folder, uint32_t client_build, uint64_t table_digest, const std::set<uint32_t>& taken_keys, uint64_t now_unix_ms, const std::filesystem::path& recording);
    std::string CheckRow(const ReplayFile::MessageTable& table, const RowMessage& message);
} // namespace ReplayObserve
