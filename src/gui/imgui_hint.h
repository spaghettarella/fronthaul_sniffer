#pragma once

#include <imgui.h>

// Help for a setting: a few dimmed words, usually beside the widget (call
// ImGui::SameLine() first), with the full explanation in a tooltip while the
// pointer is over them. The tooltip wraps at a readable width, so long_text is
// written as plain sentences rather than hand-broken lines.
//
// Only for describing what a setting does. Live values - counts, rates,
// derived resolutions, status - are drawn as ordinary text, not through this.
inline void gui_hint(const char* short_text, const char* long_text) {
    ImGui::TextDisabled("%s", short_text);
    if (long_text != nullptr && ImGui::IsItemHovered()) {
        ImGui::BeginTooltip();
        ImGui::PushTextWrapPos(ImGui::GetFontSize() * 32.0f);
        ImGui::TextUnformatted(long_text);
        ImGui::PopTextWrapPos();
        ImGui::EndTooltip();
    }
}
