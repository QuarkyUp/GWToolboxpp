#pragma once

#include <ToolboxModule.h>

class ReplayRecorderModule : public ToolboxModule {
    ReplayRecorderModule() = default;
    ~ReplayRecorderModule() override = default;

public:
    static ReplayRecorderModule& Instance()
    {
        static ReplayRecorderModule instance;
        return instance;
    }

    [[nodiscard]] const char* Name() const override { return "Replay Recorder"; }
    [[nodiscard]] const char* Icon() const override { return ICON_FA_VIDEO; }
    [[nodiscard]] const char* Description() const override { return "Records what the game server sends in PvE instances, one file per instance, for replays"; }

    struct Settings {
        bool record_explorables = true;
        bool record_outposts = false;
        bool strip_names_and_chat = true;
        std::string folder;
        int size_cap_mb = 256;
        bool list_in_observe_window = false;
        bool play_from_observe_window = false;
        bool follow_selected_agent = true;
    };

    struct PlaybackView {
        bool active = false;
        bool playing = false;
        bool ended = false;
        bool paused = false;
        bool restarting = false;
        bool can_follow = false;
        int speed = 1;
        uint32_t played_ms = 0;
        uint32_t length_ms = 0;
        uint32_t recorder_agent = 0;
        uint32_t followed_agent = 0;
        std::string name;
        std::string status;
    };

    [[nodiscard]] PlaybackView GetPlayback() const;
    void SetPlaybackPaused(bool paused);
    void SetPlaybackSpeed(int speed);
    void StopPlayback();
    void RestartPlayback();
    void FollowAgent(uint32_t agent_id);
    bool* FollowSelectedAgent();

    void Initialize() override;
    void SignalTerminate() override;
    bool CanTerminate() override;
    void Terminate() override;
    void Update(float) override;
    void LoadSettings(SettingsDoc& doc, ToolboxIni* legacy) override;
    void SaveSettings(SettingsDoc& doc) override;
    void DrawSettingsInternal() override;
};
