#include <Utils/ReplayWire.h>

#include <Utils/ReplayState.h>

#include <algorithm>
#include <format>
#include <optional>
#include <ranges>
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
            MissionInstanceLoadHead = 380,
            MissionLoadMap = 406,
            MissionInstanceInfo = 410,
        };
        constexpr uint32_t equipped_bag_type = 2;
        constexpr size_t instance_info_map_type = 2;
        constexpr size_t instance_info_observer = 5;

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
        const auto find = [&records](const uint16_t id, const size_t from, const size_t to) {
            for (auto i = from; i < to; i++) {
                if (records[i].MessageId() == id) {
                    return i;
                }
            }
            return to;
        };
        const auto load = find(MissionLoadMap, 0, records.size());
        const auto connect = find(AgentConnect, load, records.size());
        if (connect >= records.size()) {
            plan.error = "no instance load in the recording (no 406 followed by a 31)";
            return plan;
        }
        if (const auto head = find(MissionInstanceLoadHead, 0, load); head < load && head > 0) {
            plan.first_record = head;
            plan.changes.push_back({ChangeKind::LeftOut, 0, head, "they came on the previous map's connection, after its transfer and before this connection's first message (380)"});
        }
        plan.end_record = records.size();
        for (auto i = connect + 1; i < records.size(); i++) {
            const auto id = records[i].MessageId();
            if (id == MissionInstanceLoadHead || id == MissionInstanceInfo || id == MissionLoadMap) {
                plan.end_record = i;
                plan.changes.push_back({ChangeKind::LeftOut, i, records.size() - i, "a second instance load: the game would load a map it never asked for"});
                break;
            }
        }
        plan.start_ms = records[plan.first_record].time_ms;
        plan.entry_ms = records[connect].time_ms;
        plan.duration_ms = records[plan.end_record - 1].time_ms - plan.entry_ms;
        bool observer = false;
        for (auto i = plan.first_record; i < plan.end_record; i++) {
            ReplayState::Message message;
            if (!ReplayState::Decode(contents.table, records[i].body, message)) {
                plan.error = std::format("record {} (message {}) does not decode against the file's table: the game would read the next message's bytes as its fields", i, records[i].MessageId());
                return plan;
            }
            auto body = records[i].body;
            if (message.id == MissionInstanceInfo && i < load) {
                observer = true;
                plan.map_type = message.U(instance_info_map_type);
                if (message.U(instance_info_observer) != 1) {
                    message.Set(instance_info_observer, 1);
                    if (!ReplayState::Encode(contents.table, message, body)) {
                        plan.error = "410 does not encode with its observer byte set";
                        return plan;
                    }
                    plan.changes.push_back({ChangeKind::ObserverByte, i, 1, "the game enters observer mode and keeps the map it came from to go back to (410's handler)"});
                }
            }
            if (message.id == AgentReporter && !plan.controlled_agent) {
                plan.controlled_agent = message.U(0);
            }
            plan.own_records_streamed += ReplayState::KindOf(message.id) == ReplayState::Kind::Private;
            const auto stage = i < load ? Stage::BeforeLoad : i < connect ? Stage::Load : records[i].time_ms <= plan.entry_ms ? Stage::Entry : Stage::Stream;
            Append(plan.posts, stage, stage == Stage::Stream ? records[i].time_ms - plan.entry_ms : 0, body);
        }
        if (!observer) {
            plan.error = "no 410 before the 406";
        }
        return plan;
    }

    Plan MakeJoinPlan(const ReplayFile::Contents& contents, const uint32_t time_ms)
    {
        Plan plan;
        const auto& records = contents.records;
        const auto reporter = std::ranges::find(records, uint16_t{AgentReporter}, &ReplayFile::Record::MessageId);
        auto start = std::find_if(reporter, records.end(), [time_ms](const ReplayFile::Record& record) {
            return record.MessageId() == AgentAdvance && record.time_ms >= time_ms;
        });
        if (const auto last = std::ranges::find(records | std::views::reverse, uint16_t{AgentAdvance}, &ReplayFile::Record::MessageId); start == records.end() && last.base() > reporter) {
            start = std::prev(last.base());
        }
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
        plan.entry_ms = plan.start_ms;
        plan.first_record = static_cast<size_t>(start - records.begin());
        plan.end_record = records.size();
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
