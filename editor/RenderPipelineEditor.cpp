/**
 * @file RenderPipelineEditor.cpp
 * @brief AssetTypeEditor for RenderPipeline .asset files
 *
 * Provides inspector UI for editing render pipeline configuration:
 * - Renderer selection (dropdown of registered renderers)
 * - Render pass list with add/remove (dropdown of registered passes)
 * - Per-pass settings display
 */

#include <deki-editor/EditorExtension.h>
#include <deki-editor/EditorRegistry.h>
#include "../DekiRendererRegistry.h"
#include "../DekiRenderPassRegistry.h"
#include <nlohmann/json.hpp>
#include "imgui.h"

#include <vector>
#include <string>
#include <cstring>

using namespace DekiEditor;

class RenderPipelineEditor : public AssetTypeEditor
{
public:
    const char* GetTypeName() const override { return "RenderPipeline"; }
    const char* GetDisplayName() const override { return "Render Pipeline"; }
    const char* GetExtension() const override { return ".asset"; }

    const char* GetDefaultContent() const override
    {
        return R"({
  "type": "RenderPipeline",
  "renderer": "standard2d",
  "passes": []
})";
    }

    int GetCompileTarget() const override { return 3; }  // None

    bool Compile(const std::string& jsonData,
                 std::vector<uint8_t>& rgba,
                 int& width, int& height) override
    {
        return false;  // No texture compilation
    }

    bool OnInspectorGUI(std::string& jsonData,
                        const std::string& assetPath,
                        const std::string& assetGuid) override
    {
        auto data = nlohmann::json::parse(jsonData);
        bool modified = false;

        // --- Renderer dropdown ---
        std::vector<std::string> rendererNames;
        DekiRendererRegistry::GetAllNames(rendererNames);

        std::string currentRenderer = data.value("renderer", "");
        int currentRendererIdx = -1;
        for (int i = 0; i < static_cast<int>(rendererNames.size()); i++)
        {
            if (rendererNames[i] == currentRenderer)
            {
                currentRendererIdx = i;
                break;
            }
        }

        if (!rendererNames.empty())
        {
            auto getter = [](void* data, int idx) -> const char* {
                auto* names = static_cast<std::vector<std::string>*>(data);
                return (*names)[idx].c_str();
            };

            int selected = currentRendererIdx >= 0 ? currentRendererIdx : 0;
            if (ImGui::Combo("Renderer", &selected, getter, &rendererNames,
                             static_cast<int>(rendererNames.size())))
            {
                data["renderer"] = rendererNames[selected];
                modified = true;
            }
        }
        else
        {
            ImGui::TextDisabled("No renderers registered");
        }

        ImGui::Separator();
        ImGui::Text("Render Passes");
        ImGui::Spacing();

        // --- Pass list ---
        auto& passes = data["passes"];
        if (!passes.is_array())
            passes = nlohmann::json::array();

        // Cache pass names for dropdowns
        std::vector<std::string> passNames;
        DekiRenderPassRegistry::GetAllNames(passNames);

        int removeIndex = -1;
        for (int i = 0; i < static_cast<int>(passes.size()); i++)
        {
            ImGui::PushID(i);
            auto& pass = passes[i];
            if (!pass.is_object())
                pass = nlohmann::json::object();

            std::string passType = pass.value("type", "");

            // Collapsible header with built-in close button
            bool passOpen = true;
            bool open = ImGui::CollapsingHeader(
                passType.empty() ? "(select pass type)" : passType.c_str(),
                &passOpen,
                ImGuiTreeNodeFlags_DefaultOpen);

            if (!passOpen)
            {
                removeIndex = i;
                modified = true;
            }

            if (open)
            {
                ImGui::Indent();

                // Pass type dropdown
                if (!passNames.empty())
                {
                    int currentPassIdx = -1;
                    for (int j = 0; j < static_cast<int>(passNames.size()); j++)
                    {
                        if (passNames[j] == passType)
                        {
                            currentPassIdx = j;
                            break;
                        }
                    }

                    auto passGetter = [](void* data, int idx) -> const char* {
                        auto* names = static_cast<std::vector<std::string>*>(data);
                        return (*names)[idx].c_str();
                    };

                    int sel = currentPassIdx >= 0 ? currentPassIdx : 0;
                    if (ImGui::Combo("Pass Type", &sel, passGetter, &passNames,
                                     static_cast<int>(passNames.size())))
                    {
                        pass["type"] = passNames[sel];
                        modified = true;
                    }
                }
                else
                {
                    ImGui::TextDisabled("No passes registered");
                }

                // Settings display
                if (!pass.contains("settings"))
                    pass["settings"] = nlohmann::json::object();

                auto& settings = pass["settings"];
                if (!settings.empty())
                {
                    ImGui::Spacing();
                    ImGui::TextDisabled("Settings:");
                    for (auto& [key, val] : settings.items())
                    {
                        ImGui::BulletText("%s: %s", key.c_str(), val.dump().c_str());
                    }
                }

                ImGui::Unindent();
            }

            ImGui::PopID();
        }

        if (removeIndex >= 0)
        {
            passes.erase(passes.begin() + removeIndex);
            modified = true;
        }

        // Add pass button
        if (ImGui::Button("+ Add Pass"))
        {
            // Default to first registered pass if available
            std::string defaultType = passNames.empty() ? "" : passNames[0];
            passes.push_back({{"type", defaultType}, {"settings", nlohmann::json::object()}});
            modified = true;
        }

        if (modified)
            jsonData = data.dump(2);

        return modified;
    }
};

REGISTER_EDITOR(RenderPipelineEditor)
