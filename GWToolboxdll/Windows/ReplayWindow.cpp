#include "stdafx.h"

#include <GWCA/GameEntities/Skill.h>

#include <GWCA/Managers/AgentMgr.h>
#include <GWCA/Managers/SkillbarMgr.h>

#include <ImGuiAddons.h>
#include <Modules/ReplayRecorderModule.h>
#include <Modules/Resources.h>
#include <Utils/EncString.h>
#include <Utils/ReplayObserve.h>
#include <Utils/TextUtils.h>
#include <Windows/ReplayWindow.h>

namespace {
    GuiUtils::EncString followed_name;
    uint32_t named_agent = 0;
}

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
    ImGui::SetNextWindowSize(ImVec2(360.0f, 0.0f), ImGuiCond_FirstUseEver);
    if (!ImGui::Begin(Name(), nullptr, GetWinFlags())) {
        ImGui::End();
        return;
    }
    ImGui::TextUnformatted(playback.name.c_str());
    if (playback.restarting) {
        ImGui::TextDisabled("Restarting: back to the outpost, then the same recording from its start");
    }
    if (!playback.playing) {
        ImGui::TextDisabled("%s", playback.status.c_str());
        ImGui::End();
        return;
    }
    const auto time = std::format("{} / {}", TextUtils::WStringToString(ReplayObserve::Duration(playback.played_ms)), TextUtils::WStringToString(ReplayObserve::Duration(playback.length_ms)));
    ImGui::ProgressBar(playback.length_ms ? static_cast<float>(playback.played_ms) / static_cast<float>(playback.length_ms) : 0.0f, ImVec2(-1.0f, 0.0f), time.c_str());
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
    if (ImGui::Button("Stop")) {
        recorder.StopPlayback();
    }
    ImGui::SameLine();
    if (ImGui::Button("Restart")) {
        recorder.RestartPlayback();
    }
    ImGui::SameLine();
    ImGui::ShowHelp("Stop brings you back to your outpost, as stop observing does. Restart does the same, then plays this recording again from its start.");
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
