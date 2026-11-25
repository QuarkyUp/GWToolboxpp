#pragma once

#include <Utils/Replay/ReplayFile.h>
#include <Utils/Replay/ReplayState.h>

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

    enum class ChangeKind : uint8_t {
        LeftOut,
        BroughtIn,
        ObserverByte,
    };

    struct Change {
        ChangeKind kind = ChangeKind::LeftOut;
        size_t record = 0;
        size_t count = 1;
        std::string why;
    };

    struct Plan {
        uint32_t start_ms = 0;
        uint32_t duration_ms = 0;
        uint32_t seek_ms = 0;
        size_t chapter = 0;
        size_t keyframe = 0;
        size_t first_record = 0;
        size_t end_record = 0;
        size_t keyframe_records = 0;
        uint32_t controlled_agent = 0;
        uint32_t map_type = 0;
        std::vector<Change> changes;
        std::vector<Post> posts;
        std::vector<size_t> stream_records;
        size_t placed = 0;
        std::string error;
    };

    constexpr uint32_t game_map_type = 1;

    Plan MakeKeyframePlan(const ReplayFile::Contents& contents, const ReplayFile::Keyframe& keyframe);
    Plan MakeSeekPlan(const ReplayFile::Contents& contents, uint32_t time_ms);
    bool SeeksInPlace(const Plan& playing, uint32_t played_ms, const Plan& sought);
    bool RewindsInPlace(const Plan& playing, uint32_t played_ms, const Plan& sought);
    std::vector<ReplayState::Message> Rewind(const ReplayFile::Contents& contents, const Plan& playing, uint32_t played_ms, const Plan& sought);
    std::vector<Post> RewindPosts(const ReplayFile::Contents& contents, const Plan& playing, uint32_t played_ms, const Plan& sought);
    std::vector<ReplayState::Message> Placement(const ReplayFile::Contents& contents, const Plan& plan, uint32_t played_ms);
    std::vector<Post> PlacementPosts(const ReplayFile::Contents& contents, const Plan& plan, uint32_t played_ms);
} // namespace ReplayWire
