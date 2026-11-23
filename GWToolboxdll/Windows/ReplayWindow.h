#pragma once

#include <ToolboxWindow.h>

class ReplayWindow : public ToolboxWindow {
    ReplayWindow() = default;
    ~ReplayWindow() override = default;

public:
    static ReplayWindow& Instance()
    {
        static ReplayWindow instance;
        return instance;
    }

    [[nodiscard]] const char* Name() const override { return "Replay"; }
    [[nodiscard]] const char* Icon() const override { return ICON_FA_VIDEO; }
    [[nodiscard]] const char* Description() const override { return "The controls of a replay playing in the observer mode, shown while it plays"; }

    void Initialize() override;
    void Draw(IDirect3DDevice9* device) override;
};
