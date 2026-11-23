#include <Utils/ReplayWire.h>

#include <Utils/ReplayState.h>

#include <algorithm>
#include <format>
#include <optional>
#include <utility>

namespace ReplayWire {
    namespace {
        enum MessageId : uint16_t {
            AgentAdvance = 30,
            AgentConnect = 31,
            AgentReporter = 34,
            ItemInventoryAddBag = 319,
            ItemInventoryCreate = 324,
            ItemInventoryEquipSetActivate = 328,
            MissionLoadMap = 406,
        };
        constexpr uint32_t equipped_bag_type = 2;

        void Append(std::vector<Post>& posts, const Stage stage, const uint32_t time_ms, const std::vector<uint8_t>& body)
        {
            if (posts.empty() || posts.back().stage != stage || posts.back().time_ms != time_ms || posts.back().bytes.size() + body.size() > max_post_size) {
                posts.push_back({stage, time_ms, 0, {}});
            }
            auto& post = posts.back();
            post.bytes.insert(post.bytes.end(), body.begin(), body.end());
            post.records++;
        }

        std::vector<ReplayFile::Record> InventorySkeleton(const ReplayFile::Contents& contents)
        {
            std::vector<ReplayFile::Record> skeleton;
            std::optional<uint32_t> inventory;
            bool set_activated = false;
            for (const auto& record : contents.records) {
                ReplayState::Message message;
                const auto id = record.MessageId();
                if ((id != ItemInventoryCreate && id != ItemInventoryEquipSetActivate && id != ItemInventoryAddBag) || !ReplayState::Decode(contents.table, record.body, message)) {
                    continue;
                }
                if (id == ItemInventoryCreate && !inventory) {
                    inventory = message.U(0);
                    skeleton.push_back(record);
                }
                else if (inventory && message.U(0) == *inventory && id == ItemInventoryEquipSetActivate && !set_activated) {
                    set_activated = true;
                    skeleton.push_back(record);
                }
                else if (inventory && message.U(0) == *inventory && id == ItemInventoryAddBag && message.U(1) == equipped_bag_type) {
                    skeleton.push_back(record);
                }
            }
            return skeleton;
        }
    }

    void Rc4::Init(const uint8_t* key, const size_t size)
    {
        for (uint32_t k = 0; k < 256; k++) {
            s[k] = static_cast<uint8_t>(k);
        }
        uint8_t mixed = 0;
        for (uint32_t k = 0; k < 256; k++) {
            mixed = static_cast<uint8_t>(mixed + s[k] + key[k % size]);
            std::swap(s[k], s[mixed]);
        }
        i = 0;
        j = 0;
    }

    void Rc4::Apply(uint8_t* bytes, const size_t size)
    {
        for (size_t k = 0; k < size; k++) {
            i = (i + 1) & 0xFF;
            const auto a = s[i];
            j = (j + a) & 0xFF;
            s[i] = s[j];
            s[j] = a;
            bytes[k] ^= s[(s[i] + a) & 0xFF];
        }
    }

    const char* StageName(const Stage stage)
    {
        switch (stage) {
            case Stage::BeforeLoad:
                return "before 406 (after the ready record)";
            case Stage::Load:
                return "406 to 31 (after c2s 145)";
            case Stage::Entry:
                return "31 to the join's end (after c2s 144)";
            case Stage::Stream:
                return "the recording";
        }
        return "?";
    }

    Plan MakePlan(const ReplayFile::Contents& contents)
    {
        Plan plan;
        const auto& records = contents.records;
        const auto reporter = std::ranges::find(records, uint16_t{AgentReporter}, &ReplayFile::Record::MessageId);
        const auto start = std::find_if(reporter, records.end(), [](const ReplayFile::Record& record) {
            return record.MessageId() == AgentAdvance;
        });
        if (start == records.end()) {
            plan.error = "no instance entry in the recording (no 34 followed by a 30)";
            return plan;
        }
        ReplayState::World world(contents.table);
        for (auto record = records.begin(); record != start; ++record) {
            world.Apply(*record);
        }
        if (world.LayoutErrors()) {
            plan.error = std::format("{} messages before the start do not decode", world.LayoutErrors());
            return plan;
        }
        plan.start_ms = start->time_ms;
        plan.duration_ms = records.back().time_ms - plan.start_ms;
        plan.controlled_agent = world.ControlledAgent();
        const auto join = ReplayState::JoinRecords(contents.table, world, plan.start_ms);
        for (const auto& record : join) {
            if (ReplayState::Message message; !ReplayState::Decode(contents.table, record.body, message)) {
                plan.error = std::format("the join's message {} does not encode", record.MessageId());
                return plan;
            }
        }
        const auto load = std::ranges::find(join, uint16_t{MissionLoadMap}, &ReplayFile::Record::MessageId);
        const auto connect = std::find_if(load, join.end(), [](const ReplayFile::Record& record) {
            return record.MessageId() == AgentConnect;
        });
        if (connect == join.end()) {
            plan.error = "the join has no 406 followed by a 31";
            return plan;
        }
        const auto skeleton = InventorySkeleton(contents);
        plan.skeleton_records = skeleton.size();
        auto stage = Stage::BeforeLoad;
        for (auto record = join.begin(); record != join.end(); ++record) {
            if (record == load) {
                stage = Stage::Load;
            }
            if (record == connect) {
                stage = Stage::Entry;
            }
            Append(plan.posts, stage, 0, record->body);
            if (record == connect) {
                for (const auto& part : skeleton) {
                    Append(plan.posts, stage, 0, part.body);
                }
            }
        }
        for (auto record = start; record != records.end(); ++record) {
            if (!ReplayState::Streamed(contents.table, *record, plan.controlled_agent)) {
                plan.private_records_dropped++;
                continue;
            }
            plan.own_records_streamed += ReplayState::KindOf(record->MessageId()) == ReplayState::Kind::Private;
            Append(plan.posts, Stage::Stream, record->time_ms - plan.start_ms, record->body);
        }
        return plan;
    }

    std::vector<std::vector<uint8_t>> Split(const ReplayFile::MessageTable& table, const std::vector<uint8_t>& stream, std::string& error)
    {
        std::vector<std::vector<uint8_t>> bodies;
        size_t offset = 0;
        while (offset < stream.size()) {
            ReplayState::Message message;
            const auto used = ReplayState::DecodePrefix(table, stream.data() + offset, stream.size() - offset, message);
            if (!used) {
                error = std::format("the stream does not decode at byte {} (message {})", offset, message.id);
                return bodies;
            }
            bodies.emplace_back(stream.begin() + static_cast<std::ptrdiff_t>(offset), stream.begin() + static_cast<std::ptrdiff_t>(offset + used));
            offset += used;
        }
        return bodies;
    }
}
