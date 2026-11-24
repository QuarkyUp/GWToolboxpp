#include <Utils/ReplayWire.h>

#include <Utils/ReplayState.h>

#include <algorithm>
#include <format>
#include <optional>
#include <utility>

namespace ReplayWire {
    namespace {
        enum MessageId : uint16_t {
            AgentConnect = 31,
            AgentReporter = 34,
            MissionInstanceLoadHead = 380,
            MissionLoadMap = 406,
            MissionInstanceInfo = 410,
        };
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

        bool IsLoad(const uint16_t id)
        {
            return id == MissionInstanceLoadHead || id == MissionInstanceInfo || id == MissionLoadMap;
        }

        bool SetObserver(const ReplayFile::MessageTable& table, ReplayState::Message& message, std::vector<uint8_t>& body, Plan& plan)
        {
            plan.map_type = message.U(instance_info_map_type);
            if (message.U(instance_info_observer) == 1) {
                return false;
            }
            message.Set(instance_info_observer, 1);
            if (!ReplayState::Encode(table, message, body)) {
                plan.error = "410 does not encode with its observer byte set";
            }
            return true;
        }
    } // namespace

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

    Plan MakePlan(const ReplayFile::Contents& contents, const size_t chapter)
    {
        Plan plan;
        const auto& records = contents.records;
        const auto begin = contents.chapters[chapter].first_record;
        const auto end = contents.ChapterEnd(chapter);
        const auto find = [&records](const uint16_t id, const size_t from, const size_t to) {
            for (auto i = from; i < to; i++) {
                if (records[i].MessageId() == id) {
                    return i;
                }
            }
            return to;
        };
        const auto load = find(MissionLoadMap, begin, end);
        const auto connect = find(AgentConnect, load, end);
        if (connect >= end) {
            plan.error = "no instance load in the chapter (no 406 followed by a 31)";
            return plan;
        }
        plan.first_record = begin;
        if (const auto head = find(MissionInstanceLoadHead, begin, load); head < load && head > begin) {
            plan.first_record = head;
            plan.changes.push_back({ChangeKind::LeftOut, begin, head - begin, "they came on the previous map's connection, after its transfer and before this connection's first message (380)"});
        }
        plan.end_record = end;
        for (auto i = connect + 1; i < end; i++) {
            if (IsLoad(records[i].MessageId())) {
                plan.end_record = i;
                plan.changes.push_back({ChangeKind::LeftOut, i, end - i, "a second instance load: the game would load a map it never asked for"});
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
                if (SetObserver(contents.table, message, body, plan)) {
                    plan.changes.push_back({ChangeKind::ObserverByte, i, 1, "the game enters observer mode and keeps the map it came from to go back to (410's handler)"});
                }
                if (!plan.error.empty()) {
                    return plan;
                }
            }
            if (message.id == AgentReporter && !plan.controlled_agent) {
                plan.controlled_agent = message.U(0);
            }
            const auto stage = i < load ? Stage::BeforeLoad : i < connect ? Stage::Load : records[i].time_ms <= plan.entry_ms ? Stage::Entry : Stage::Stream;
            Append(plan.posts, stage, stage == Stage::Stream ? records[i].time_ms - plan.entry_ms : 0, body);
        }
        if (!observer) {
            plan.error = "no 410 before the 406";
        }
        return plan;
    }

    Plan MakeKeyframePlan(const ReplayFile::Contents& contents, const ReplayFile::Keyframe& keyframe)
    {
        Plan plan;
        const auto& records = contents.records;
        const auto chapter = std::ranges::upper_bound(contents.chapters, keyframe.record_index, {}, &ReplayFile::Chapter::first_record) - contents.chapters.begin();
        const auto end = contents.ChapterEnd(static_cast<size_t>(std::max<std::ptrdiff_t>(chapter - 1, 0)));
        std::vector<ReplayState::Message> messages(keyframe.records.size());
        for (size_t i = 0; i < messages.size(); i++) {
            if (!ReplayState::Decode(contents.table, keyframe.records[i].body, messages[i])) {
                plan.error = std::format("the keyframe's message {} (id {}) does not decode against the file's table", i, keyframe.records[i].MessageId());
                return plan;
            }
        }
        const auto is = [](const uint16_t id) {
            return [id](const ReplayState::Message& message) {
                return message.id == id;
            };
        };
        const auto load = static_cast<size_t>(std::ranges::find_if(messages, is(MissionLoadMap)) - messages.begin());
        const auto connect = static_cast<size_t>(std::find_if(messages.begin() + static_cast<std::ptrdiff_t>(load), messages.end(), is(AgentConnect)) - messages.begin());
        if (connect >= messages.size()) {
            plan.error = "the keyframe has no 406 followed by a 31";
            return plan;
        }
        plan.first_record = keyframe.record_index;
        plan.end_record = end;
        plan.keyframe_records = messages.size();
        plan.start_ms = keyframe.time_ms;
        plan.entry_ms = keyframe.time_ms;
        ReplayState::Order order;
        bool observer = false;
        for (size_t i = 0; i < messages.size(); i++) {
            auto& message = messages[i];
            auto body = keyframe.records[i].body;
            if (message.id == MissionInstanceInfo && i < load) {
                observer = true;
                if (SetObserver(contents.table, message, body, plan)) {
                    plan.changes.push_back({ChangeKind::ObserverByte, i, 1, "the keyframe's 410: the game enters observer mode and keeps the map it came from to go back to"});
                }
                if (!plan.error.empty()) {
                    return plan;
                }
            }
            if (message.id == AgentReporter && !plan.controlled_agent) {
                plan.controlled_agent = message.U(0);
            }
            order.Bring(contents.table, message);
            Append(plan.posts, i < load ? Stage::BeforeLoad : i < connect ? Stage::Load : Stage::Entry, 0, body);
        }
        if (!observer) {
            plan.error = "the keyframe has no 410 before its 406";
            return plan;
        }
        std::optional<Change> left_out;
        const auto flush = [&] {
            if (left_out) {
                plan.changes.push_back(*left_out);
                left_out.reset();
            }
        };
        auto last_ms = keyframe.time_ms;
        for (auto i = keyframe.record_index; i < end; i++) {
            if (IsLoad(records[i].MessageId())) {
                flush();
                plan.end_record = i;
                plan.changes.push_back({ChangeKind::LeftOut, i, end - i, "a second instance load: the game would load a map it never asked for"});
                break;
            }
            ReplayState::Message message;
            if (!ReplayState::Decode(contents.table, records[i].body, message)) {
                plan.error = std::format("record {} (message {}) does not decode against the file's table", i, records[i].MessageId());
                return plan;
            }
            if (!order.Unbrought(contents.table, message).empty()) {
                if (!left_out) {
                    left_out = Change{ChangeKind::LeftOut, i, 0, "they name an id that neither the keyframe nor a record before them brought in"};
                }
                left_out->count++;
                continue;
            }
            flush();
            order.Bring(contents.table, message);
            last_ms = std::max(last_ms, records[i].time_ms);
            Append(plan.posts, Stage::Stream, records[i].time_ms - keyframe.time_ms, records[i].body);
        }
        flush();
        plan.duration_ms = last_ms - keyframe.time_ms;
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
} // namespace ReplayWire
