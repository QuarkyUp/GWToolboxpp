#pragma once

#include <Utils/ReplayFile.h>

#include <array>
#include <cstdint>
#include <map>
#include <set>
#include <string>
#include <utility>
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

    size_t DecodePrefix(const ReplayFile::MessageTable& table, const uint8_t* bytes, size_t size, Message& out, const ReplayFile::TextFilter& filter = {});
    bool Decode(const ReplayFile::MessageTable& table, const std::vector<uint8_t>& body, Message& out, const ReplayFile::TextFilter& filter = {});
    bool Encode(const ReplayFile::MessageTable& table, const Message& message, std::vector<uint8_t>& out);
    Message Blank(const ReplayFile::MessageTable& table, uint16_t message_id);

    struct ClientItem {
        uint32_t item_id = 0;
        uint32_t model_file = 0;
        uint32_t type = 0;
        uint32_t dye_tint = 0;
        uint32_t dye_colours = 0;
        uint32_t formula = 0;
        uint32_t multiplier = 0;
        uint32_t interaction = 0;
        uint32_t value = 0;
        uint32_t model_id = 0;
        uint32_t quantity = 0;
        std::wstring name;
        std::vector<uint32_t> code;
        uint32_t profession = 0;
        std::wstring customizer;
    };

    std::vector<Message> ItemReport(const ReplayFile::MessageTable& table, const ClientItem& item);

    std::string FieldAssert(const Message& message);
    std::vector<std::string> CheckClientNeeds(const std::vector<Message>& messages);

    enum class IdKind : uint8_t { Agent, Item, Bag, Inventory, Guild, Party, Player, Quest, Hero, Npc, Prop, Skill };
    const char* IdKindName(IdKind kind);

    struct IdField {
        uint16_t message = 0;
        uint8_t field = 0;
        IdKind kind = IdKind::Agent;

        auto operator<=>(const IdField&) const = default;
    };

    std::vector<IdField> IdFields(const ReplayFile::MessageTable& table, uint16_t message_id);
    bool BringsIdsIn(const IdField& field);
    bool OrderChecked(IdKind kind);

    struct OrderFailure {
        size_t record = 0;
        IdField field;
        uint32_t id = 0;
    };

    struct Order {
        std::set<std::pair<IdKind, uint32_t>> brought;

        [[nodiscard]] std::vector<std::pair<IdField, uint32_t>> Unbrought(const ReplayFile::MessageTable& table, const Message& message) const;
        void Bring(const ReplayFile::MessageTable& table, const Message& message);
    };

    std::vector<OrderFailure> CheckOrder(const ReplayFile::MessageTable& table, const std::vector<Message>& messages);
    std::string Describe(const OrderFailure& failure);

    std::vector<uint32_t> Identity(const Message& message);

    class World {
    public:
        explicit World(const ReplayFile::MessageTable& table) : table(&table) {}

        bool Apply(const ReplayFile::Record& record);
        void Apply(const Message& message);
        [[nodiscard]] std::vector<Message> Join() const;
        [[nodiscard]] uint32_t LayoutErrors() const { return layout_errors; }
        [[nodiscard]] uint32_t ControlledAgent() const;

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

        struct Character {
            Message attribute_state;
            std::map<uint16_t, Message> latest;
            std::map<uint32_t, Message> attribute_updates;
        };

        struct PlayerFlags {
            uint32_t value = 0;
            uint32_t mask = 0;
        };

        void Tick(uint32_t milliseconds);
        void ApplyToAgent(Agent& agent, const Message& message);
        void JoinRows(uint32_t agent_id, const Agent& agent, bool before_connect, std::vector<Message>& out, std::set<Key>& emitted) const;
        void JoinAgent(uint32_t agent_id, const Agent& agent, std::vector<Message>& out, std::set<Key>& emitted) const;
        void JoinItem(uint32_t item_id, std::vector<Message>& out, std::set<Key>& emitted) const;
        void JoinInventory(uint32_t inventory_id, std::vector<Message>& out, std::set<Key>& emitted) const;
        void JoinCharacter(uint32_t agent_id, std::vector<Message>& out, std::set<Key>& emitted) const;
        [[nodiscard]] std::set<uint32_t> ShownItems() const;
        [[nodiscard]] bool Alive(uint32_t agent_id) const;
        [[nodiscard]] uint32_t ReportedAgent() const;
        [[nodiscard]] uint32_t LeftOutAgent() const;

        const ReplayFile::MessageTable* table;
        uint32_t clock = 0;
        bool connected = false;
        uint32_t layout_errors = 0;
        std::vector<Message> entry;
        std::vector<Message> map_init;
        std::map<Key, Message> instance;
        std::map<uint32_t, Agent> agents;
        std::map<uint32_t, Character> characters;
        std::map<uint32_t, Message> players;
        std::map<uint32_t, PlayerFlags> player_flags;
        std::map<uint32_t, std::map<uint16_t, Message>> items;
        Message reporter;
    };

    class InventoryFilter {
    public:
        explicit InventoryFilter(const ReplayFile::MessageTable& table) : table(&table) {}

        bool Keep(const Message& message, std::vector<Message>& shown_before, std::vector<uint32_t>& unknown_items);
        [[nodiscard]] bool Holds(const Message& message) const;
        [[nodiscard]] size_t Held() const;

    private:
        const ReplayFile::MessageTable* table;
        std::set<uint32_t> shown;
        std::map<uint32_t, std::map<uint16_t, Message>> held;
    };

    bool IsBagContent(const Message& message);
    std::vector<uint32_t> ShownItems(const Message& message);

    std::vector<ReplayFile::Record> JoinRecords(const ReplayFile::MessageTable& table, const World& world, uint32_t time_ms);
} // namespace ReplayState
