#pragma once

#include <Utils/ReplayFile.h>

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace ReplayWire {
    constexpr size_t max_post_size = 0x1000;
    constexpr uint8_t ready_record_type = 1;
    constexpr uint8_t ready_record_size = 0x16;
    constexpr size_t ready_secret_size = 20;

    struct Rc4 {
        uint32_t i = 0;
        uint32_t j = 0;
        uint8_t s[256] = {};

        void Init(const uint8_t* key, size_t size);
        void Apply(uint8_t* bytes, size_t size);
    };
    static_assert(sizeof(Rc4) == 0x108);

    enum class Stage : uint8_t {
        BeforeLoad,
        Load,
        Entry,
        Stream,
    };
    const char* StageName(Stage stage);

    struct Post {
        Stage stage = Stage::Stream;
        uint32_t time_ms = 0;
        uint32_t records = 0;
        std::vector<uint8_t> bytes;
    };

    struct Plan {
        uint32_t start_ms = 0;
        uint32_t duration_ms = 0;
        size_t skeleton_records = 0;
        size_t private_records_dropped = 0;
        size_t own_records_streamed = 0;
        uint32_t controlled_agent = 0;
        std::vector<Post> posts;
        std::string error;
    };

    Plan MakePlan(const ReplayFile::Contents& contents);
    std::vector<std::vector<uint8_t>> Split(const ReplayFile::MessageTable& table, const std::vector<uint8_t>& stream, std::string& error);
}
