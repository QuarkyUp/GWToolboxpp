#include "stdafx.h"

#include <GWCA/GameEntities/Skill.h>

#include <GWCA/Managers/AgentMgr.h>
#include <GWCA/Managers/SkillbarMgr.h>

#include <ImGuiAddons.h>
#include <Modules/ReplayRecorderModule.h>
#include <Modules/Resources.h>
#include <Utils/EncString.h>
#include <Windows/ReplayWindow.h>

namespace {
    using Playback = ReplayRecorderModule::PlaybackView;

    GuiUtils::EncString followed_name;
    uint32_t named_agent = 0;
    std::optional<uint32_t> dragged_ms;

    std::string ChapterName(const ReplayRecorderModule::TimelineChapter& chapter)
    {
        const auto& name = Resources::GetMapName(static_cast<GW::Constants::MapID>(chapter.map_id))->string();
        return std::format("{} ({})", name.empty() ? std::format("map {}", chapter.map_id) : name, ReplayRecorderModule::TimeText(chapter.end_ms - chapter.start_ms));
    }

    const ReplayRecorderModule::TimelineChapter* ChapterAt(const Playback& playback, const uint32_t time_ms)
    {
        const auto found = std::ranges::find_if(playback.chapters, [time_ms](const ReplayRecorderModule::TimelineChapter& chapter) {
            return time_ms >= chapter.start_ms && time_ms < chapter.end_ms;
        });
        return found == playback.chapters.end() ? nullptr : &*found;
    }

    void DrawTimeline(ReplayRecorderModule& recorder, const Playback& playback)
    {
        const auto width = std::max(ImGui::GetContentRegionAvail().x, 1.0f);
        const auto height = ImGui::GetFrameHeight() * 1.5f;
        const auto origin = ImGui::GetCursorScreenPos();
        const auto bottom = origin.y + height;
        ImGui::InvisibleButton("##timeline", ImVec2(width, height));
        const auto length = static_cast<float>(std::max(playback.length_ms, 1u));
        const auto x_of = [&](const uint32_t time_ms) {
            return origin.x + width * std::min(static_cast<float>(time_ms) / length, 1.0f);
        };
        const auto hovered_ms = static_cast<uint32_t>(std::clamp((ImGui::GetIO().MousePos.x - origin.x) / width, 0.0f, 1.0f) * length);
        if (ImGui::IsItemActive() && playback.playing) {
            dragged_ms = hovered_ms;
        }
        if (ImGui::IsItemDeactivated() && dragged_ms) {
            recorder.SeekPlayback(*dragged_ms);
            dragged_ms.reset();
        }
        const auto draw = ImGui::GetWindowDrawList();
        draw->AddRectFilled(origin, ImVec2(origin.x + width, bottom), ImGui::GetColorU32(ImGuiCol_FrameBg));
        for (size_t i = 0; i < playback.chapters.size(); i++) {
            const auto& chapter = playback.chapters[i];
            const ImVec2 from(x_of(chapter.start_ms), origin.y);
            const ImVec2 to(x_of(chapter.end_ms), bottom);
            draw->AddRectFilled(from, to, ImGui::GetColorU32(!chapter.explorable ? ImGuiCol_FrameBgHovered : i == playback.chapter ? ImGuiCol_ButtonActive : ImGuiCol_Button));
            draw->AddLine(from, ImVec2(from.x, bottom), ImGui::GetColorU32(ImGuiCol_Border));
            draw->PushClipRect(from, to, true);
            draw->AddText(ImVec2(from.x + 3.0f, origin.y + 1.0f), ImGui::GetColorU32(chapter.explorable ? ImGuiCol_Text : ImGuiCol_TextDisabled), ChapterName(chapter).c_str());
            draw->PopClipRect();
        }
        draw->AddRectFilled(ImVec2(origin.x, bottom - 4.0f), ImVec2(x_of(playback.position_ms), bottom), ImGui::GetColorU32(ImGuiCol_PlotHistogram));
        for (const auto keyframe_ms : playback.keyframes_ms) {
            const auto x = x_of(keyframe_ms);
            draw->AddLine(ImVec2(x, bottom - height * 0.4f), ImVec2(x, bottom), ImGui::GetColorU32(ImGuiCol_PlotLines));
        }
        const auto cursor_x = x_of(dragged_ms.value_or(playback.position_ms));
        draw->AddLine(ImVec2(cursor_x, origin.y), ImVec2(cursor_x, bottom), ImGui::GetColorU32(ImGuiCol_Text), 2.0f);
        if (ImGui::IsItemHovered()) {
            const auto chapter = ChapterAt(playback, hovered_ms);
            ImGui::SetTooltip(
                "%s%s\nClick or drag to go there; the ticks are keyframes.", ReplayRecorderModule::TimeText(hovered_ms).c_str(),
                chapter ? std::format(", {}{}", ChapterName(*chapter), chapter->explorable ? "" : ": an outpost, skipped (not played yet)").c_str() : ""
            );
        }
    }
} // namespace

void ReplayWindow::Initialize()
{
    ToolboxWindow::Initialize();
    has_closebutton = false;
}

void ReplayWindow::Draw(IDirect3DDevice9*)
{
    auto& recorder = ReplayRecorderModule::Instance();
    const auto playback = recorder.GetPlayback();
    if (!playback.active) {
        return;
    }
    ImGui::SetNextWindowSize(ImVec2(420.0f, 0.0f), ImGuiCond_FirstUseEver);
    if (!ImGui::Begin(Name(), nullptr, GetWinFlags())) {
        ImGui::End();
        return;
    }
    ImGui::TextUnformatted(playback.name.c_str());
    DrawTimeline(recorder, playback);
    const auto chapter = playback.chapter < playback.chapters.size() ? &playback.chapters[playback.chapter] : nullptr;
    ImGui::Text(
        "%s / %s%s", ReplayRecorderModule::TimeText(playback.position_ms).c_str(), ReplayRecorderModule::TimeText(playback.length_ms).c_str(),
        chapter ? std::format(", chapter {} of {}: {}", playback.chapter + 1, playback.chapters.size(), ChapterName(*chapter)).c_str() : ""
    );
    if (!playback.note.empty()) {
        ImGui::TextWrapped("%s", playback.note.c_str());
    }
    if (!playback.playing) {
        ImGui::TextDisabled("Loading the map at %s: the keyframe at or before it, then the recording to it at once", ReplayRecorderModule::TimeText(playback.position_ms).c_str());
        ImGui::End();
        return;
    }
    ImGui::BeginDisabled(playback.ended);
    if (ImGui::Button(playback.paused ? "Resume" : "Pause")) {
        recorder.SetPlaybackPaused(!playback.paused);
    }
    ImGui::EndDisabled();
    for (const auto speed : {1, 2, 4, 8}) {
        ImGui::SameLine();
        if (ImGui::RadioButton(std::format("x{}", speed).c_str(), playback.speed == speed)) {
            recorder.SetPlaybackSpeed(speed);
        }
    }
    ImGui::SameLine();
    ImGui::ShowHelp("Above x1 the recording's messages come faster, while the game still moves and animates agents at its own pace: they jump from one recorded position to the next.");
    std::optional<uint32_t> previous_ms;
    std::optional<uint32_t> next_ms;
    for (const auto keyframe_ms : playback.keyframes_ms) {
        const auto at = ChapterAt(playback, keyframe_ms);
        if (!at || !at->explorable) {
            continue;
        }
        if (keyframe_ms + 1000 < playback.position_ms) {
            previous_ms = keyframe_ms;
        }
        else if (keyframe_ms > playback.position_ms + 500 && !next_ms) {
            next_ms = keyframe_ms;
        }
    }
    ImGui::BeginDisabled(!previous_ms);
    if (ImGui::Button("<< Keyframe")) {
        recorder.SeekPlayback(*previous_ms);
    }
    ImGui::EndDisabled();
    ImGui::SameLine();
    if (ImGui::Button("-10 s")) {
        auto time_ms = playback.position_ms > 10000 ? playback.position_ms - 10000 : 0;
        // Outposts are not played: going back into one goes on back to the end of the explorable map before it.
        for (auto chapter = playback.chapters.rbegin(); chapter != playback.chapters.rend() && !(chapter->start_ms <= time_ms && chapter->explorable); ++chapter) {
            if (chapter->start_ms <= time_ms) {
                time_ms = chapter->start_ms ? chapter->start_ms - 1 : 0;
            }
        }
        recorder.SeekPlayback(time_ms);
    }
    ImGui::SameLine();
    if (ImGui::Button("+10 s")) {
        recorder.SeekPlayback(std::min(playback.position_ms + 10000, playback.length_ms));
    }
    ImGui::SameLine();
    ImGui::BeginDisabled(!next_ms);
    if (ImGui::Button("Keyframe >>")) {
        recorder.SeekPlayback(*next_ms);
    }
    ImGui::EndDisabled();
    ImGui::SameLine();
    ImGui::ShowHelp(
        "The bar is the whole recording, one part per map. Going anywhere loads the keyframe at or before that moment (a tick), then plays the recording from it to that moment at once; speed and pause stay as they are. At the end of a map the replay goes on in the next one. Outposts (greyed) are skipped: the observer mode does not play them yet."
    );
    if (ImGui::Button("Stop")) {
        recorder.StopPlayback();
    }
    ImGui::SameLine();
    if (ImGui::Button("Restart")) {
        recorder.RestartPlayback();
    }
    ImGui::SameLine();
    ImGui::ShowHelp("Stop brings you back to your outpost, as stop observing does. Restart goes back to the recording's start without leaving the replay.");
    if (playback.ended) {
        ImGui::TextUnformatted("The recording has ended: this is its last moment.");
    }

    ImGui::Separator();
    if (!playback.can_follow) {
        ImGui::TextDisabled("The view follows the recorder's character: this client has no viewpoint message the toolbox knows.");
    }
    else {
        if (named_agent != playback.followed_agent) {
            named_agent = playback.followed_agent;
            followed_name.reset(GW::Agents::GetAgentEncNameById(named_agent));
        }
        const auto& name = followed_name.string();
        ImGui::Text("Following: %s%s", name.empty() ? std::format("agent {}", playback.followed_agent).c_str() : name.c_str(), playback.followed_agent == playback.recorder_agent ? " (the recorder)" : "");
        ImGui::Checkbox("Follow the agent I select", recorder.FollowSelectedAgent());
        ImGui::SameLine();
        ImGui::ShowHelp("Select a hero, a henchman or anyone else (a click, the party window, a key) and the view follows it, as an observer's view follows the player it picks.");
        ImGui::BeginDisabled(playback.followed_agent == playback.recorder_agent);
        if (ImGui::Button("Back to the recorder's character")) {
            recorder.FollowAgent(playback.recorder_agent);
        }
        ImGui::EndDisabled();
    }

    ImGui::Separator();
    const auto target = GW::Agents::GetTargetId();
    const auto shown = target ? target : playback.followed_agent;
    ImGui::Text("Skills of %s", target ? "the selected agent" : "the followed agent");
    const auto skillbar = GW::SkillbarMgr::GetSkillbar(shown);
    if (!skillbar) {
        ImGui::TextDisabled("None in the recording: the server sends the skill bars of the recorder and its heroes only.");
    }
    else {
        for (const auto& skill : skillbar->skills) {
            if (skill.skill_id == GW::Constants::SkillID::No_Skill) {
                continue;
            }
            const auto recharge = skill.GetRecharge();
            const auto& skill_name = Resources::GetSkillName(skill.skill_id)->string();
            ImGui::Text("%s%s", skill_name.c_str(), recharge ? std::format(" ({} s)", (recharge + 999) / 1000).c_str() : "");
        }
    }
    ImGui::End();
}
