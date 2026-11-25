#include <Utils/ReplayWire.h>

#include <Utils/ReplayState.h>

#include <algorithm>
#include <format>
#include <map>
#include <optional>
#include <ranges>
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
        constexpr uint16_t item_detail_ids[] = {309, 313, 314, 346, 350, 353, 354};

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

    Plan MakeKeyframePlan(const ReplayFile::Contents& contents, const ReplayFile::Keyframe& keyframe)
    {
        Plan plan;
        const auto& records = contents.records;
        const auto chapter = contents.ChapterOf(keyframe.record_index);
        const auto end = contents.ChapterEnd(chapter);
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
        ReplayState::ClientAgents agents;
        for (size_t i = 0; i < messages.size(); i++) {
            auto broken = ReplayState::FieldAssert(messages[i]);
            if (broken.empty()) {
                broken = agents.Breaks(messages[i]);
            }
            if (!broken.empty()) {
                plan.error = std::format("the keyframe's message {} (id {}) breaks an assert of the game's: {}", i, messages[i].id, broken);
                return plan;
            }
        }
        plan.first_record = keyframe.record_index;
        plan.end_record = end;
        plan.keyframe_records = messages.size();
        plan.start_ms = keyframe.time_ms;
        plan.chapter = chapter;
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
        std::map<uint32_t, std::vector<size_t>> earlier_details;
        for (auto i = contents.chapters[chapter].first_record; i < keyframe.record_index; i++) {
            if (ReplayState::Message message; std::ranges::contains(item_detail_ids, records[i].MessageId()) && ReplayState::Decode(contents.table, records[i].body, message)) {
                earlier_details[message.U(0)].push_back(i);
            }
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
            if (const auto broken = ReplayState::FieldAssert(message); !broken.empty()) {
                plan.error = std::format("record {} (message {}) breaks an assert of the game's: {}", i, message.id, broken);
                return plan;
            }
            auto unbrought = order.Unbrought(contents.table, message);
            if (!unbrought.empty() && std::ranges::all_of(unbrought, [&earlier_details](const std::pair<ReplayState::IdField, uint32_t>& field) {
                    return field.first.kind == ReplayState::IdKind::Item && earlier_details.contains(field.second);
                })) {
                flush();
                Change brought{ChangeKind::BroughtIn, i, 0, "an item's details from the stream before the keyframe, which the keyframe does not hold, posted before the record that shows it"};
                for (const auto& item : unbrought | std::views::values) {
                    const auto details = earlier_details.find(item);
                    if (details == earlier_details.end()) {
                        continue;
                    }
                    for (const auto detail : details->second) {
                        ReplayState::Message decoded;
                        ReplayState::Decode(contents.table, records[detail].body, decoded);
                        if (const auto broken = ReplayState::FieldAssert(decoded); !broken.empty()) {
                            plan.error = std::format("record {} (message {}) breaks an assert of the game's: {}", detail, decoded.id, broken);
                            return plan;
                        }
                        order.Bring(contents.table, decoded);
                        Append(plan.posts, Stage::Stream, records[i].time_ms - keyframe.time_ms, records[detail].body);
                        brought.count++;
                    }
                    earlier_details.erase(details);
                }
                plan.changes.push_back(brought);
                unbrought = order.Unbrought(contents.table, message);
            }
            if (!unbrought.empty()) {
                if (!left_out) {
                    left_out = Change{ChangeKind::LeftOut, i, 0, "they name an id that neither the keyframe nor a record before them brought in"};
                }
                left_out->count++;
                continue;
            }
            if (const auto broken = agents.Breaks(message); !broken.empty()) {
                plan.error = std::format("record {} (message {}) breaks an assert of the game's: {}", i, message.id, broken);
                return plan;
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

    bool SeeksInPlace(const Plan& playing, const uint32_t played_ms, const Plan& sought)
    {
        return sought.chapter == playing.chapter && sought.end_record == playing.end_record && sought.start_ms + sought.seek_ms >= playing.start_ms + played_ms;
    }

    Plan MakeSeekPlan(const ReplayFile::Contents& contents, uint32_t time_ms)
    {
        Plan refused;
        refused.error = "the recording holds no explorable map with a keyframe the game can play";
        const auto after = std::ranges::upper_bound(contents.chapters, time_ms, {}, &ReplayFile::Chapter::time_ms) - contents.chapters.begin();
        for (auto chapter = static_cast<size_t>(std::max<std::ptrdiff_t>(after - 1, 0)); chapter < contents.chapters.size(); chapter++, time_ms = 0) {
            if (!contents.chapters[chapter].is_explorable) {
                continue;
            }
            std::vector<size_t> starts;
            for (size_t k = 0; k < contents.keyframes.size(); k++) {
                if (contents.ChapterOf(contents.keyframes[k].record_index) == chapter && (starts.empty() || contents.keyframes[k].time_ms <= time_ms)) {
                    starts.push_back(k);
                }
            }
            // The keyframe at or before the time, or an earlier one of the map when the game would refuse it.
            for (const auto k : starts | std::views::reverse) {
                auto plan = MakeKeyframePlan(contents, contents.keyframes[k]);
                if (plan.error.empty() && plan.map_type != game_map_type) {
                    plan.error = std::format("its map type is {}, not a game map's: the observer's party window asserts the observed map is one", plan.map_type);
                }
                if (!plan.error.empty()) {
                    refused.error = std::format("keyframe {} at {} ms: {}", k + 1, contents.keyframes[k].time_ms, plan.error);
                    continue;
                }
                plan.keyframe = k;
                plan.seek_ms = std::max(time_ms, plan.start_ms) - plan.start_ms;
                return plan;
            }
        }
        return refused;
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
