#include <Utils/Replay/ReplayMessages.h>
#include <Utils/Replay/ReplayWire.h>

#include <Utils/Replay/ReplayState.h>

#include <algorithm>
#include <format>
#include <map>
#include <numeric>
#include <optional>
#include <ranges>
#include <set>
#include <utility>

namespace ReplayWire {
    namespace {
        using namespace ReplayMessages;

        constexpr size_t instance_info_map_type = 2;
        constexpr size_t instance_info_observer = 5;
        // A second one asserts (48 ChCliApi.cpp:385, 430 MsnTeam.cpp:105, 324 ItCliApi.cpp:2010) or is the instance's load, which a rewind keeps.
        constexpr uint16_t rewind_left_out[] = {
            CharAccountNameData,
            MissionTeam,
            GAME_SMSG_ITEM_STREAM_CREATE,
            GAME_SMSG_INSTANCE_LOAD_HEAD,
            GAME_SMSG_INSTANCE_LOAD_PLAYER_NAME,
            MissionConfigEnd,
            GAME_SMSG_READY_FOR_MAP_SPAWN,
            GAME_SMSG_MISSION_STREAM_START,
            GAME_SMSG_INSTANCE_LOAD_FINISH,
            GAME_SMSG_INSTANCE_LOAD_SPAWN_POINT,
            MissionManifestChunk,
            MissionManifestComplete,
            MissionManifestPhase,
            GAME_SMSG_INSTANCE_LOAD_INFO
        };
        constexpr uint16_t item_detail_ids[] = {GAME_SMSG_ITEM_UPDATE_OWNER, GAME_SMSG_ITEM_UPDATE_QUANTITY, GAME_SMSG_ITEM_UPDATE_NAME, ItemProfession, ItemReportDetailLow, GAME_SMSG_ITEM_GENERAL_INFO, GAME_SMSG_ITEM_REUSE_ID};

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
            return id == GAME_SMSG_INSTANCE_LOAD_HEAD || id == GAME_SMSG_INSTANCE_LOAD_INFO || id == GAME_SMSG_INSTANCE_LOAD_SPAWN_POINT;
        }

        // The recorder samples every walk the client changed, checked every frame (WriteMotions): a move no sample of its agent follows before the agent's next
        // movement, while the sampler wrote others, left the client's walk as it was.
        std::set<size_t> UnchangedMoves(const ReplayFile::Contents& contents, const size_t first, const size_t end)
        {
            struct Move {
                size_t record = 0;
                size_t samples = 0;
            };
            std::set<size_t> unchanged;
            std::map<uint32_t, Move> moves;
            size_t samples = 0;
            const auto settle = [&](const Move& move) {
                if (samples > move.samples) {
                    unchanged.insert(move.record);
                }
            };
            auto motions = std::ranges::upper_bound(contents.motions, first, {}, &ReplayFile::Motions::record_index);
            for (auto i = first; i <= end && i <= contents.records.size(); i++) {
                for (; motions != contents.motions.end() && motions->record_index <= i; ++motions) {
                    samples++;
                    for (const auto& motion : motions->agents) {
                        moves.erase(motion.agent);
                    }
                }
                if (i == end || i == contents.records.size()) {
                    continue;
                }
                const auto id = contents.records[i].MessageId();
                ReplayState::Message message;
                if ((id != GAME_SMSG_AGENT_SPAWNED && id != GAME_SMSG_AGENT_DESPAWNED && (id < GAME_SMSG_AGENT_UPDATE_DIRECTION || id > GAME_SMSG_AGENT_UPDATE_POSITION)) || !ReplayState::Decode(contents.table, contents.records[i].body, message)) {
                    continue;
                }
                if (const auto found = moves.find(message.U(0)); found != moves.end()) {
                    settle(found->second);
                    moves.erase(found);
                }
                if (message.id == GAME_SMSG_AGENT_MOVE_TO_POINT || message.id == GAME_SMSG_AGENT_UPDATE_DESTINATION) {
                    moves[message.U(0)] = {i, samples};
                }
            }
            for (const auto& move : moves | std::views::values) {
                settle(move);
            }
            return unchanged;
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
        const auto load = static_cast<size_t>(std::ranges::find_if(messages, is(GAME_SMSG_INSTANCE_LOAD_SPAWN_POINT)) - messages.begin());
        const auto connect = static_cast<size_t>(std::find_if(messages.begin() + static_cast<std::ptrdiff_t>(load), messages.end(), is(GAME_SMSG_AGENT_INSTANCE_TIMER)) - messages.begin());
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
            if (message.id == GAME_SMSG_INSTANCE_LOAD_INFO && i < load) {
                observer = true;
                if (SetObserver(contents.table, message, body, plan)) {
                    plan.changes.push_back({ChangeKind::ObserverByte, i, 1, "the keyframe's 410: the game enters observer mode and keeps the map it came from to go back to"});
                }
                if (!plan.error.empty()) {
                    return plan;
                }
            }
            if (message.id == GAME_SMSG_AGENT_SET_PLAYER && !plan.controlled_agent) {
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
                        plan.stream_records.push_back(detail);
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
            plan.stream_records.push_back(i);
        }
        flush();
        plan.duration_ms = last_ms - keyframe.time_ms;
        return plan;
    }

    bool SeeksInPlace(const Plan& playing, const uint32_t played_ms, const Plan& sought)
    {
        return sought.chapter == playing.chapter && sought.end_record == playing.end_record && sought.start_ms + sought.seek_ms >= playing.start_ms + played_ms;
    }

    bool RewindsInPlace(const Plan& playing, const uint32_t played_ms, const Plan& sought)
    {
        return sought.chapter == playing.chapter && sought.end_record == playing.end_record && sought.start_ms + sought.seek_ms < playing.start_ms + played_ms;
    }

    std::vector<ReplayState::Message> Rewind(const ReplayFile::Contents& contents, const Plan& playing, const uint32_t played_ms, const Plan& sought)
    {
        constexpr uint32_t reporter_game_view = 2;
        constexpr size_t buff_added_field = 3;
        ReplayState::ClientAgents held;
        std::set<uint32_t> quests;
        std::set<std::pair<uint32_t, uint32_t>> effects;
        std::set<std::pair<uint32_t, uint32_t>> upkeeps;
        bool party_locked = false;
        const auto hold = [&](const std::vector<uint8_t>& body) {
            ReplayState::Message message;
            if (!ReplayState::Decode(contents.table, body, message)) {
                return;
            }
            held.Breaks(message);
            switch (message.id) {
                case GAME_SMSG_QUEST_ADD:
                case GAME_SMSG_QUEST_GENERAL_INFO:
                    quests.insert(message.U(0));
                    break;
                case GAME_SMSG_QUEST_REMOVE:
                    quests.erase(message.U(0));
                    break;
                case GAME_SMSG_EFFECT_APPLIED:
                    effects.insert({message.U(0), message.U(buff_added_field)});
                    break;
                case GAME_SMSG_EFFECT_REMOVED:
                    effects.erase({message.U(0), message.U(1)});
                    break;
                case GAME_SMSG_EFFECT_UPKEEP_ADDED:
                case GAME_SMSG_EFFECT_UPKEEP_APPLIED:
                    upkeeps.insert({message.U(0), message.U(buff_added_field)});
                    break;
                case GAME_SMSG_EFFECT_UPKEEP_REMOVED:
                    upkeeps.erase({message.U(0), message.U(1)});
                    break;
                case GAME_SMSG_PARTY_LOCK:
                    party_locked = message.U(0) != 0;
                    break;
                default:
                    break;
            }
        };
        for (const auto& record : contents.keyframes[playing.keyframe].records) {
            hold(record.body);
        }
        for (const auto index : playing.stream_records) {
            if (contents.records[index].time_ms > playing.start_ms + played_ms) {
                break;
            }
            hold(contents.records[index].body);
        }
        std::vector<ReplayState::Message> rewind;
        // What the 33s leave in the char and party clients goes by the server's own removals, as a load's teardown frees it: a re-fed 66 for a held buff is ignored.
        // Posted while their agents are up: GmCam's buff remove calls the camera's controller, which the followed agent's 33 frees.
        const auto remove = [&](const uint16_t id, const uint32_t first, const std::optional<uint32_t> second = std::nullopt) {
            auto& removal = rewind.emplace_back(ReplayState::Blank(contents.table, id));
            removal.Set(0, first);
            if (second) {
                removal.Set(1, *second);
            }
        };
        for (const auto quest : quests) {
            remove(GAME_SMSG_QUEST_REMOVE, quest);
        }
        // A gone agent's buffs stay as in play, where the server removes none after its 33 (the client keeps them, m03).
        const auto on_held = [&held](const std::pair<uint32_t, uint32_t>& buff) {
            return held.Created().contains(buff.first);
        };
        for (const auto& [agent, effect] : effects | std::views::filter(on_held)) {
            remove(GAME_SMSG_EFFECT_REMOVED, agent, effect);
        }
        for (const auto& [agent, upkeep] : upkeeps | std::views::filter(on_held)) {
            remove(GAME_SMSG_EFFECT_UPKEEP_REMOVED, agent, upkeep);
        }
        if (party_locked) {
            remove(GAME_SMSG_PARTY_LOCK, 0);
        }
        for (const auto agent : held.Created()) {
            auto& destroy = rewind.emplace_back(ReplayState::Blank(contents.table, GAME_SMSG_AGENT_DESPAWNED));
            destroy.Set(0, agent);
        }
        const auto& keyframe = contents.keyframes[sought.keyframe];
        std::vector<ReplayState::Message> messages(keyframe.records.size());
        for (size_t i = 0; i < messages.size(); i++) {
            ReplayState::Decode(contents.table, keyframe.records[i].body, messages[i]);
        }
        const auto connect = std::find_if(std::ranges::find(messages, GAME_SMSG_INSTANCE_LOAD_SPAWN_POINT, &ReplayState::Message::id), messages.end(), [](const ReplayState::Message& kept) {
            return kept.id == GAME_SMSG_AGENT_INSTANCE_TIMER;
        });
        if (connect == messages.end()) {
            return {};
        }
        // The client's own way into a time with the map kept: every agent gone by its 33, the clock set by 31 (its notify queue empty), the keyframe fed again.
        rewind.push_back(*connect);
        std::vector<ReplayState::Message> buffs;
        for (auto kept = messages.begin(); kept != messages.end(); ++kept) {
            if (kept == connect || std::ranges::contains(rewind_left_out, kept->id)) {
                continue;
            }
            // GmCam adds the controlled agent's buffs through the camera's controller, which only a 34 makes again after the followed agent's 33.
            if (kept->id == GAME_SMSG_EFFECT_APPLIED || kept->id == GAME_SMSG_EFFECT_UPKEEP_APPLIED) {
                buffs.push_back(*kept);
                continue;
            }
            rewind.push_back(*kept);
            // A second 55 asserts (ChCliAttrib.cpp:369): the attribute state kept, its points given back by the messages that change them.
            if (kept->id == CharAttributeState) {
                rewind.back() = ReplayState::Blank(contents.table, CharAttribPointsAvail);
                rewind.back().Set(0, kept->U(0));
                rewind.back().Set(1, kept->U(1));
                auto& total = rewind.emplace_back(ReplayState::Blank(contents.table, CharAttribPointsTotal));
                total.Set(0, kept->U(0));
                total.Set(1, kept->U(2));
            }
            // Bit 1 asks for a game view and asserts while one is up (IUi::OnAgentInitReporting, UiGame.cpp:497); bit 0 sets the controlled agent and the camera on it.
            if (kept->id == GAME_SMSG_AGENT_SET_PLAYER) {
                rewind.back().Set(1, kept->U(1) & ~reporter_game_view);
                rewind.insert(rewind.end(), buffs.begin(), buffs.end());
                buffs.clear();
            }
        }
        rewind.insert(rewind.end(), buffs.begin(), buffs.end());
        return rewind;
    }

    std::vector<Post> RewindPosts(const ReplayFile::Contents& contents, const Plan& playing, const uint32_t played_ms, const Plan& sought)
    {
        std::vector<Post> posts;
        for (const auto& message : Rewind(contents, playing, played_ms, sought)) {
            if (std::vector<uint8_t> body; ReplayState::Encode(contents.table, message, body)) {
                Append(posts, Stage::Stream, 0, body);
            }
        }
        return posts;
    }

    std::vector<ReplayState::Message> Placement(const ReplayFile::Contents& contents, const Plan& plan, const uint32_t played_ms)
    {
        // Every agent where the latest sample at or before the time has it (the keyframe's, a motions entry's), walked by its order over the rest of what the plan posted.
        const auto& keyframe = contents.keyframes[plan.keyframe];
        const auto time_ms = plan.start_ms + played_ms;
        ReplayState::World world(contents.table);
        for (const auto& record : keyframe.records) {
            world.Apply(record);
        }
        auto motions = std::ranges::lower_bound(contents.motions, keyframe.record_index, {}, &ReplayFile::Motions::record_index);
        const auto sample = [&](const size_t before) {
            for (; motions != contents.motions.end() && motions->record_index <= before && motions->time_ms <= time_ms; ++motions) {
                for (const auto& motion : motions->agents) {
                    world.Apply(motion);
                }
            }
        };
        const auto unchanged = UnchangedMoves(contents, keyframe.record_index, plan.end_record);
        for (const auto index : plan.stream_records) {
            if (contents.records[index].time_ms > time_ms) {
                break;
            }
            sample(index);
            if (!unchanged.contains(index)) {
                world.Apply(contents.records[index]);
            }
        }
        sample(plan.end_record);
        return world.Placement();
    }

    std::vector<Post> PlacementPosts(const ReplayFile::Contents& contents, const Plan& plan, const uint32_t played_ms)
    {
        std::vector<Post> posts;
        for (const auto& message : Placement(contents, plan, played_ms)) {
            if (std::vector<uint8_t> body; ReplayState::Encode(contents.table, message, body)) {
                Append(posts, Stage::Stream, played_ms, body);
            }
        }
        return posts;
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
                if (plan.seek_ms) {
                    const auto after = std::ranges::find_if(plan.posts, [&plan](const Post& post) {
                        return post.stage == Stage::Stream && post.time_ms > plan.seek_ms;
                    });
                    auto placement = PlacementPosts(contents, plan, plan.seek_ms);
                    plan.placed = std::accumulate(placement.begin(), placement.end(), size_t{0}, [](const size_t sum, const Post& post) {
                        return sum + post.records;
                    });
                    plan.posts.insert(after, std::make_move_iterator(placement.begin()), std::make_move_iterator(placement.end()));
                }
                return plan;
            }
        }
        return refused;
    }

} // namespace ReplayWire
