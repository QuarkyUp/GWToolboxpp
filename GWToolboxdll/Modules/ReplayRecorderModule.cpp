#include "stdafx.h"

#include <GWCA/Context/CharContext.h>

#include <GWCA/GameEntities/Agent.h>
#include <GWCA/GameEntities/Map.h>

#include <GWCA/Managers/AgentMgr.h>
#include <GWCA/Managers/MapMgr.h>
#include <GWCA/Managers/MemoryMgr.h>
#include <GWCA/Managers/StoCMgr.h>

#include <GWCA/Packets/StoC.h>

#include <GWToolbox.h>
#include <ImGuiAddons.h>
#include <Logger.h>
#include <Modules/ReplayRecorderModule.h>
#include <Modules/Resources.h>
#include <Utils/ReplayFile.h>
#include <Utils/StoCHandlers.h>
#include <Utils/TextUtils.h>

namespace {
    using Clock = std::chrono::steady_clock;
    using GW::Packet::StoC::PacketBase;

    constexpr auto idle_timeout = std::chrono::seconds(60);
    constexpr auto flush_interval = std::chrono::seconds(5);
    constexpr auto registration_retry_interval = std::chrono::seconds(1);
    constexpr wchar_t literal_start = 0x107;
    constexpr wchar_t literal_end = 0x1;

    ReplayRecorderModule::Settings settings;

    GW::HookEntry stoc_hook_entry;
    bool callbacks_registered = false;
    ReplayFile::MessageTable message_table;
    uint64_t message_table_digest = 0;
    Clock::time_point next_registration_attempt;

    std::mutex recording_mutex;
    ReplayFile::Writer writer;
    ReplayFile::Header header;
    std::filesystem::path recording_path;
    Clock::time_point recording_start;
    Clock::time_point last_record_at;
    Clock::time_point last_flush_at;
    std::vector<uint8_t> body;
    std::vector<uint8_t> pending_chat;
    uint32_t pending_chat_time_ms = 0;
    bool has_pending_chat = false;
    std::wstring own_name;
    std::unordered_map<std::wstring, std::wstring> player_labels;
    std::string status = "Waiting for the next map";

    std::filesystem::path RecordingsFolder()
    {
        return settings.folder.empty() ? Resources::GetSettingsFolderPath() / L"replays" : std::filesystem::path(TextUtils::StringToWString(settings.folder));
    }

    uint32_t ElapsedMs(const Clock::time_point since)
    {
        return static_cast<uint32_t>(std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - since).count());
    }

    std::string RecordingName()
    {
        return TextUtils::WStringToString(recording_path.filename().wstring());
    }

    bool IsPlainText(const std::wstring_view text)
    {
        return !text.empty() && std::ranges::all_of(text, [](const wchar_t c) {
            return c >= 0x20 && c < 0x100;
        });
    }

    void StripPlayerNames(std::wstring& text)
    {
        const auto label_of = [](const std::wstring_view name) {
            if (name == own_name) {
                return std::wstring(name);
            }
            auto& label = player_labels[std::wstring(name)];
            if (label.empty()) {
                label = std::format(L"Player {}", player_labels.size());
            }
            return label;
        };
        if (IsPlainText(text)) {
            text = label_of(text);
            return;
        }
        std::wstring stripped;
        size_t position = 0;
        for (auto start = text.find(literal_start); start != std::wstring::npos; start = text.find(literal_start, position)) {
            const auto end = text.find(literal_end, start + 1);
            if (end == std::wstring::npos) {
                break;
            }
            stripped.append(text, position, start + 1 - position);
            const auto literal = std::wstring_view(text).substr(start + 1, end - start - 1);
            stripped += IsPlainText(literal) ? label_of(literal) : std::wstring(literal);
            position = end;
        }
        stripped.append(text, position);
        text = std::move(stripped);
    }

    void FinishRecording()
    {
        if (!writer.IsOpen()) {
            return;
        }
        has_pending_chat = false;
        header.duration_ms = ElapsedMs(recording_start);
        header.flags |= ReplayFile::Complete;
        const auto size = writer.Size();
        if (!writer.Close(header)) {
            status = std::format("Could not finish {}", RecordingName());
            Log::Error("Replay Recorder: %s", status.c_str());
            return;
        }
        status = std::format("Saved {}: {} messages, {} KB, {} layout errors{}", RecordingName(), header.record_count, size / 1024, header.layout_errors,
                             header.flags & ReplayFile::SizeCapReached ? " (size cap reached)" : "");
        Log::Info("Replay Recorder: %s", status.c_str());
    }

    void StartRecording(const GW::Packet::StoC::GameSrvTransfer* transfer)
    {
        const auto map_id = static_cast<GW::Constants::MapID>(transfer->map_id);
        const auto map_info = GW::Map::GetMapInfo(map_id);
        if (!map_info || GWToolbox::ShouldDisableToolbox(map_id)) {
            status = std::format("Not recording map {}: PvP or unknown", transfer->map_id);
            return;
        }
        const auto explorable = transfer->is_explorable != 0;
        if (!(explorable ? settings.record_explorables : settings.record_outposts) || message_table.empty()) {
            status = std::format("Not recording map {}", transfer->map_id);
            return;
        }
        const auto folder = RecordingsFolder();
        std::wstring error;
        if (!Resources::EnsureFolderExists(folder, error)) {
            status = TextUtils::WStringToString(error);
            Log::Error("Replay Recorder: %s", status.c_str());
            return;
        }
        recording_path = folder / std::format(L"{}_map{}.gwrec", TextUtils::StringToWString(TextUtils::FilenameTimestamp()), transfer->map_id);
        header = {};
        header.client_build = GW::MemoryMgr::GetGWVersion();
        header.table_digest = message_table_digest;
        header.start_time_unix_ms = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count();
        header.map_id = transfer->map_id;
        header.map_region_type = static_cast<uint32_t>(map_info->type);
        header.is_explorable = explorable;
        header.region = transfer->region;
        header.flags = settings.strip_names_and_chat ? ReplayFile::NamesStripped | ReplayFile::ChatDropped : 0;
        strncpy(header.toolbox_version, GWTOOLBOXDLL_VERSION GWTOOLBOXDLL_VERSION_BETA, sizeof(header.toolbox_version) - 1);
        if (!writer.Open(recording_path, header, message_table)) {
            status = std::format("Could not create {}", RecordingName());
            Log::Error("Replay Recorder: %s", status.c_str());
            return;
        }
        const auto char_context = GW::GetCharContext();
        own_name = char_context ? char_context->player_name : L"";
        player_labels.clear();
        has_pending_chat = false;
        recording_start = last_record_at = last_flush_at = Clock::now();
        status = std::format("Recording {}", RecordingName());
    }

    void WriteRecord(const uint32_t time_ms, const std::vector<uint8_t>& bytes)
    {
        const auto size_cap = static_cast<uint64_t>(settings.size_cap_mb) << 20;
        if (writer.Size() + bytes.size() + 6 > size_cap) {
            header.flags |= ReplayFile::SizeCapReached;
            FinishRecording();
            return;
        }
        if (!writer.Append(time_ms, bytes.data(), bytes.size())) {
            FinishRecording();
            return;
        }
        header.record_count++;
        last_record_at = Clock::now();
    }

    void PackRecord(const PacketBase* packet, std::vector<uint8_t>& out)
    {
        const auto& fields = message_table[packet->header];
        const auto names_stripped = (header.flags & ReplayFile::NamesStripped) != 0;
        if (!ReplayFile::PackMessage(packet->header, fields.data(), static_cast<uint32_t>(fields.size()), reinterpret_cast<const uint8_t*>(packet), out,
                                     names_stripped ? ReplayFile::TextFilter(StripPlayerNames) : ReplayFile::TextFilter())) {
            out = {static_cast<uint8_t>(packet->header), static_cast<uint8_t>(packet->header >> 8)};
            header.layout_errors++;
        }
    }

    void OnServerMessage(GW::HookStatus*, PacketBase* packet, void*)
    {
        std::scoped_lock lock(recording_mutex);
        if (packet->header == GAME_SMSG_TRANSFER_GAME_SERVER_INFO) {
            FinishRecording();
            StartRecording(static_cast<const GW::Packet::StoC::GameSrvTransfer*>(packet));
            return;
        }
        if (!writer.IsOpen() || packet->header >= message_table.size()) {
            return;
        }
        if (packet->header == GAME_SMSG_INSTANCE_LOAD_INFO) {
            const auto info = static_cast<const GW::Packet::StoC::InstanceLoadInfo*>(packet);
            if (info->is_observer) {
                writer.Close(header);
                std::error_code error;
                std::filesystem::remove(recording_path, error);
                status = "Not recording: observing a match";
                return;
            }
            header.district = info->district;
            header.language = info->language;
        }
        const auto time_ms = ElapsedMs(recording_start);
        if (header.flags & ReplayFile::ChatDropped) {
            switch (packet->header) {
                case GAME_SMSG_CHAT_MESSAGE_CORE:
                    PackRecord(packet, pending_chat);
                    pending_chat_time_ms = time_ms;
                    has_pending_chat = true;
                    return;
                case GAME_SMSG_CHAT_MESSAGE_GLOBAL:
                case GAME_SMSG_CHAT_MESSAGE_LOCAL:
                    has_pending_chat = false;
                    return;
                case GAME_SMSG_SPEECH_BUBBLE: {
                    const auto agent = GW::Agents::GetAgentByID(static_cast<const GW::Packet::StoC::SpeechBubble*>(packet)->agent_id);
                    const auto living = agent ? agent->GetAsAgentLiving() : nullptr;
                    if (living && living->IsPlayer()) {
                        return;
                    }
                    break;
                }
                default:
                    break;
            }
            if (has_pending_chat) {
                has_pending_chat = false;
                WriteRecord(pending_chat_time_ms, pending_chat);
                if (!writer.IsOpen()) {
                    return;
                }
            }
        }
        PackRecord(packet, body);
        WriteRecord(time_ms, body);
    }

    void RegisterCallbacks()
    {
        if (Clock::now() < next_registration_attempt) {
            return;
        }
        next_registration_attempt = Clock::now() + registration_retry_interval;
        const auto table = GetStoCHandlerArray();
        if (!(table && table->size())) {
            return;
        }
        message_table.clear();
        for (const auto& handler : *table) {
            message_table.emplace_back(handler.fields, handler.fields + handler.field_count);
        }
        message_table_digest = ReplayFile::TableDigest(message_table);
        for (uint32_t message_id = 0; message_id < message_table.size(); message_id++) {
            if (!GW::StoC::RegisterPacketCallback(&stoc_hook_entry, message_id, OnServerMessage, nullptr, 0x9000)) {
                GW::StoC::RemoveCallbacks(&stoc_hook_entry);
                message_table.clear();
                return;
            }
        }
        callbacks_registered = true;
    }
}

void ReplayRecorderModule::Initialize()
{
    ToolboxModule::Initialize();
    SettingsRegistry::Register(this, settings);
    std::scoped_lock lock(recording_mutex);
    RegisterCallbacks();
}

void ReplayRecorderModule::SignalTerminate()
{
    GW::StoC::RemoveCallbacks(&stoc_hook_entry);
    std::scoped_lock lock(recording_mutex);
    FinishRecording();
    callbacks_registered = false;
}

void ReplayRecorderModule::Update(float)
{
    std::scoped_lock lock(recording_mutex);
    if (!callbacks_registered) {
        RegisterCallbacks();
    }
    if (!writer.IsOpen()) {
        return;
    }
    const auto now = Clock::now();
    if (now - last_record_at > idle_timeout) {
        FinishRecording();
        return;
    }
    if (now - last_flush_at > flush_interval) {
        writer.Flush();
        last_flush_at = now;
    }
}

void ReplayRecorderModule::LoadSettings(SettingsDoc& doc, ToolboxIni* legacy)
{
    ToolboxModule::LoadSettings(doc, legacy);
    doc.GetStruct(Name(), settings);
}

void ReplayRecorderModule::SaveSettings(SettingsDoc& doc)
{
    ToolboxModule::SaveSettings(doc);
    doc.SetStruct(Name(), settings);
}

void ReplayRecorderModule::DrawSettingsInternal()
{
    std::scoped_lock lock(recording_mutex);
    ImGui::Checkbox("Record explorable areas", &settings.record_explorables);
    ImGui::Checkbox("Record outposts", &settings.record_outposts);
    ImGui::Checkbox("Keep other players' names and chat out of recordings", &settings.strip_names_and_chat);
    ImGui::ShowHelp("Other players' names become \"Player 1\", \"Player 2\"...; player chat and speech bubbles are left out.");
    ImGui::InputText("Folder", settings.folder, MAX_PATH);
    ImGui::SameLine();
    if (ImGui::Button("Open")) {
        const auto folder = RecordingsFolder();
        if (Resources::EnsureFolderExists(folder)) {
            ShellExecuteW(nullptr, L"open", folder.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
        }
    }
    if (settings.folder.empty()) {
        ImGui::TextDisabled("Empty: %s", TextUtils::WStringToString(RecordingsFolder().wstring()).c_str());
    }
    if (ImGui::InputInt("Size cap per recording (MB)", &settings.size_cap_mb)) {
        settings.size_cap_mb = std::clamp(settings.size_cap_mb, 1, 4096);
    }
    ImGui::TextDisabled("PvP maps are never recorded. A recording starts on entering a map and ends on leaving it.");
    if (writer.IsOpen()) {
        ImGui::Text("Recording %s: %u messages, %llu KB, %u layout errors", RecordingName().c_str(), header.record_count, writer.Size() / 1024, header.layout_errors);
    }
    else {
        ImGui::Text("%s", status.c_str());
    }
}
