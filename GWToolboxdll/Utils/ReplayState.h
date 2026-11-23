#pragma once

#include <Utils/ReplayFile.h>

#include <array>
#include <cstdint>
#include <map>
#include <set>
#include <string>
#include <vector>

namespace ReplayState {
    struct Field {
        std::vector<uint32_t> numbers;
        std::wstring text;

        bool operator==(const Field&) const = default;
    };

    struct Message {
        uint16_t id = 0;
        std::vector<Field> fields;

        [[nodiscard]] uint32_t U(size_t field, size_t item = 0) const;
        [[nodiscard]] float F(size_t field, size_t item = 0) const;
        void Set(size_t field, uint32_t value, size_t item = 0);
        void SetF(size_t field, float value, size_t item = 0);
        bool operator==(const Message&) const = default;
    };

    size_t DecodePrefix(const ReplayFile::MessageTable& table, const uint8_t* bytes, size_t size, Message& out);
    bool Decode(const ReplayFile::MessageTable& table, const std::vector<uint8_t>& body, Message& out);
    bool Encode(const ReplayFile::MessageTable& table, const Message& message, std::vector<uint8_t>& out);
    std::vector<uint32_t> NamedAgents(const ReplayFile::MessageTable& table, const Message& message);

    enum class Kind { Unknown, State, Transient, Private };
    Kind KindOf(uint16_t message_id);
    const char* KindName(Kind kind);

    constexpr uint32_t default_keyframe_interval_ms = 10000;

    class World {
    public:
        explicit World(const ReplayFile::MessageTable& table) : table(&table) {}

        bool Apply(const ReplayFile::Record& record);
        void Apply(const Message& message);
        [[nodiscard]] std::vector<Message> Join() const;
        [[nodiscard]] std::string Dump(bool with_texts) const;
        [[nodiscard]] uint32_t Clock() const { return clock; }
        [[nodiscard]] size_t AgentCount() const { return agents.size(); }
        [[nodiscard]] uint32_t LayoutErrors() const { return layout_errors; }

    private:
        using Key = std::vector<uint32_t>;

        struct Effect {
            Message applied;
            uint32_t expiry = 0;
        };

        struct Agent {
            Message create;
            Message move;
            std::map<Key, Message> slots;
            std::map<uint32_t, Effect> effects;
            std::array<uint32_t, 9> equipment{};
            bool equipped = false;
            float health = 1.f;
            float energy = 1.f;
        };

        void Tick(uint32_t milliseconds);
        void ApplyToAgent(Agent& agent, const Message& message);
        void JoinAgent(const Agent& agent, std::vector<Message>& out, std::set<Key>& emitted) const;
        void JoinItem(uint32_t item_id, std::vector<Message>& out, std::set<Key>& emitted) const;
        [[nodiscard]] std::set<uint32_t> ShownItems() const;

        const ReplayFile::MessageTable* table;
        uint32_t clock = 0;
        bool connected = false;
        uint32_t layout_errors = 0;
        std::vector<Message> entry;
        std::map<Key, Message> instance;
        std::map<uint32_t, Agent> agents;
        std::map<uint32_t, Message> players;
        std::map<uint32_t, uint32_t> player_flags;
        std::map<uint32_t, std::map<uint16_t, Message>> items;
        std::set<uint32_t> shown_items;
        std::set<uint32_t> inventory_items;
        Message reporter;
    };

    struct Keyframe {
        size_t record_index = 0;
        uint32_t time_ms = 0;
        World world;
    };

    std::vector<Keyframe> Keyframes(const ReplayFile::Contents& contents, uint32_t interval_ms);
    std::vector<ReplayFile::Record> JoinRecords(const ReplayFile::MessageTable& table, const World& world, uint32_t time_ms);
    std::vector<ReplayFile::Record> Seek(const ReplayFile::Contents& contents, const std::vector<Keyframe>& keyframes, uint32_t time_ms);

    struct JoinCheck {
        std::vector<std::string> failures;
        size_t join_records = 0;
        size_t stream_rows_before_creation = 0;
        size_t player_rows_without_agent = 0;
    };

    JoinCheck CheckJoin(const ReplayFile::Contents& contents);
}
