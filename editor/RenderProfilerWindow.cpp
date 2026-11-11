// Editor-only translation unit (a firmware build compiles nothing from it).
#ifdef DEKI_EDITOR

// Render Profiler: what each object in the scene costs to draw, from the
// Play view or from a board on a serial port. Each row is one renderer (or one
// render pass on one object): its share of finding what changed, of making and
// drawing its pixels, and why its area was drawn again, per frame.

#include <deki-editor/DeviceProfile.h>
#include <deki-editor/EditorUI.h>
#include <deki-editor/EditorWindow.h>
#include <deki-editor/Localization.h>
#include <deki-editor/SerialPorts.h>
#include "../RenderProfiler.h"

#include <algorithm>
#include <cctype>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

using namespace DekiRendering;

namespace DekiEditor
{

namespace
{
uint64_t SteadyMicros()
{
    return static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now().time_since_epoch())
            .count());
}

bool ContainsNoCase(const std::string& text, const std::string& part)
{
    if (part.empty())
    {
        return true;
    }
    auto lower = [](std::string s)
    {
        std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        return s;
    };
    return lower(text).find(lower(part)) != std::string::npos;
}
}  // namespace

class RenderProfilerWindow : public EditorWindow
{
public:
    const char* GetTitle() override { return "Render Profiler"; }
    const char* GetMenuPath() override { return "Rendering/Render Profiler"; }

    void OnOpen() override
    {
        if (!RenderProfiler::IsEnabled())
        {
            RenderProfiler::Enable(&SteadyMicros);
            m_EnabledHere = true;
        }
        m_Ports = EnumerateSerialPorts();
        if (m_Port.empty() && !m_Ports.empty())
        {
            m_Port = m_Ports.front();
        }
    }

    void OnClose() override
    {
        if (m_EnabledHere)
        {
            RenderProfiler::Enable(nullptr);
            m_EnabledHere = false;
        }
    }

    void OnUpdate(float deltaTime) override
    {
        // The Play view's numbers, once a second, per frame. Kept while the
        // board's are shown.
        m_Since += deltaTime;
        if (m_Source != Source::PlayView || m_Since < 1.0f || !RenderProfiler::IsEnabled())
        {
            return;
        }
        m_Since = 0.0f;
        uint32_t frames = 0;
        RenderProfiler::TakePaths();
        std::vector<RenderProfiler::ObjectEntry> objects = RenderProfiler::TakeObjects();
        RenderProfiler::Take(frames);
        if (frames == 0)
        {
            return;  // nothing drawn: keep the last numbers
        }
        const double per = 1.0 / frames;
        m_Rows.clear();
        for (const RenderProfiler::ObjectEntry& o : objects)
        {
            Row r;
            r.path = o.path;
            r.type = o.type ? o.type : "";
            r.order = o.order;
            r.measureUs = o.measureUs * per;
            r.contentUs = o.contentUs * per;
            r.blitUs = o.blitUs * per;
            r.pixels = o.pixels * per;
            r.draws = o.draws * per;
            for (int i = 0; i < RenderProfiler::kRedrawReasons; ++i)
            {
                r.redraws[i] = o.redraws[i] * per;
            }
            m_Rows.push_back(std::move(r));
        }
        m_Frames = frames;
        m_Total = static_cast<int>(objects.size());
        m_Sorted = false;
    }

    void OnGUI() override
    {
        auto& ui = EditorUI::Get();
        const float dpi = ui.GetDpiScale();
        ui.SetNextWindowSize(980.0f * dpi, 520.0f * dpi, true);
        ui.Begin(TrLabel(GetTitle()), &m_IsOpen);

        TakeDeviceResult();

        // Where the numbers come from.
        if (ImGui::RadioButton(Tr("Play view"), m_Source == Source::PlayView))
        {
            m_Source = Source::PlayView;
            m_Rows.clear();
            m_Since = 1.0f;
        }
        ImGui::SameLine();
        if (ImGui::RadioButton(Tr("Board"), m_Source == Source::Device))
        {
            m_Source = Source::Device;
            m_Rows.clear();
        }
        if (m_Source == Source::Device)
        {
            ImGui::SameLine();
            ImGui::SetNextItemWidth(120.0f * dpi);
            if (ImGui::BeginCombo("##port", m_Port.empty() ? Tr("No port") : m_Port.c_str()))
            {
                m_Ports = EnumerateSerialPorts();
                for (const std::string& p : m_Ports)
                {
                    if (ImGui::Selectable(p.c_str(), p == m_Port))
                    {
                        m_Port = p;
                    }
                }
                ImGui::EndCombo();
            }
            ImGui::SameLine();
            ImGui::BeginDisabled(m_Measuring || m_Port.empty());
            if (ImGui::Button(m_Measuring ? Tr("Measuring...") : Tr("Measure 3 s")))
            {
                StartDeviceMeasure();
            }
            ImGui::EndDisabled();
        }
        if (!m_Error.empty())
        {
            ImGui::TextWrapped("%s", m_Error.c_str());
        }

        // Totals and the filter.
        double measure = 0, content = 0, blit = 0;
        for (const Row& r : m_Rows)
        {
            measure += r.measureUs;
            content += r.contentUs;
            blit += r.blitUs;
        }
        ImGui::Text(Tr("%d objects over %.0f frames. Per frame: finding changes %.2f ms, making pixels %.2f ms, "
                       "drawing %.2f ms."),
                    m_Total, m_Frames, measure / 1000.0, content / 1000.0, blit / 1000.0);
        ImGui::SetNextItemWidth(-1.0f);
        char filter[256] = {};
        std::snprintf(filter, sizeof(filter), "%s", m_Filter.c_str());
        if (ImGui::InputTextWithHint("##filter", Tr("Filter by object or type"), filter, sizeof(filter)))
        {
            m_Filter = filter;
        }

        DrawTable(dpi);
        ui.End();
    }

private:
    enum class Source
    {
        PlayView,
        Device
    };
    struct Row
    {
        std::string path, type;
        double order = 0, measureUs = 0, contentUs = 0, blitUs = 0, pixels = 0, draws = 0;
        double redraws[RenderProfiler::kRedrawReasons] = {};
        double Total() const { return measureUs + contentUs + blitUs; }
    };
    struct DeviceJob
    {
        std::mutex mutex;
        bool done = false;
        bool ok = false;
        std::string error;
        DeviceProfile profile;
    };

    void StartDeviceMeasure()
    {
        m_Measuring = true;
        m_Error.clear();
        auto job = std::make_shared<DeviceJob>();
        m_Job = job;
        const std::string port = m_Port;
        std::thread(
            [job, port]()
            {
                DeviceProfile profile;
                std::string error;
                const bool ok = QueryDeviceProfile(port, 3, profile, error);
                std::lock_guard<std::mutex> lock(job->mutex);
                job->ok = ok;
                job->error = error;
                job->profile = std::move(profile);
                job->done = true;
            })
            .detach();
    }

    void TakeDeviceResult()
    {
        if (!m_Job)
        {
            return;
        }
        std::lock_guard<std::mutex> lock(m_Job->mutex);
        if (!m_Job->done)
        {
            return;
        }
        m_Measuring = false;
        if (!m_Job->ok)
        {
            m_Error = m_Job->error;
        }
        else if (m_Source == Source::Device)
        {
            m_Rows.clear();
            for (const DeviceProfileRow& d : m_Job->profile.objects)
            {
                auto value = [&](const char* key)
                {
                    const auto it = d.values.find(key);
                    return it == d.values.end() ? 0.0 : it->second;
                };
                Row r;
                r.path = d.name;
                r.type = d.type;
                r.order = value("order");
                r.measureUs = value("measureUs");
                r.contentUs = value("contentUs");
                r.blitUs = value("blitUs");
                r.pixels = value("pixels");
                r.draws = value("draws");
                r.redraws[0] = value("new");
                r.redraws[1] = value("changed");
                r.redraws[2] = value("still");
                r.redraws[3] = value("overPin");
                m_Rows.push_back(std::move(r));
            }
            m_Frames = m_Job->profile.frames;
            m_Total = m_Job->profile.objectsTotal;
            m_Sorted = false;
        }
        m_Job.reset();
    }

    void DrawTable(float dpi)
    {
        enum Column
        {
            kObject,
            kType,
            kOrder,
            kMeasure,
            kContent,
            kBlit,
            kTotal,
            kPixels,
            kDraws,
            kNew,
            kChanged,
            kStill,
            kOverPin,
            kColumns
        };
        if (!ImGui::BeginTable("RenderProfilerRows", kColumns,
                               ImGuiTableFlags_Resizable | ImGuiTableFlags_Sortable | ImGuiTableFlags_BordersInnerV |
                                   ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY | ImGuiTableFlags_ScrollX))
        {
            return;
        }
        const float num = 74.0f * dpi;
        ImGui::TableSetupScrollFreeze(1, 1);
        ImGui::TableSetupColumn(TrLabel("Object"), ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableSetupColumn(TrLabel("Type"), ImGuiTableColumnFlags_WidthFixed, 130.0f * dpi);
        ImGui::TableSetupColumn(TrLabel("Order"), ImGuiTableColumnFlags_WidthFixed, 50.0f * dpi);
        ImGui::TableSetupColumn(TrLabel("Changes (us)"), ImGuiTableColumnFlags_WidthFixed, num);
        ImGui::TableSetupColumn(TrLabel("Make (us)"), ImGuiTableColumnFlags_WidthFixed, num);
        ImGui::TableSetupColumn(TrLabel("Draw (us)"), ImGuiTableColumnFlags_WidthFixed, num);
        ImGui::TableSetupColumn(TrLabel("Total (us)"),
                                ImGuiTableColumnFlags_WidthFixed | ImGuiTableColumnFlags_DefaultSort |
                                    ImGuiTableColumnFlags_PreferSortDescending,
                                num);
        ImGui::TableSetupColumn(TrLabel("Pixels"), ImGuiTableColumnFlags_WidthFixed, num);
        ImGui::TableSetupColumn(TrLabel("Draws"), ImGuiTableColumnFlags_WidthFixed, 56.0f * dpi);
        ImGui::TableSetupColumn(TrLabel("New"), ImGuiTableColumnFlags_WidthFixed, 50.0f * dpi);
        ImGui::TableSetupColumn(TrLabel("Changed"), ImGuiTableColumnFlags_WidthFixed, 64.0f * dpi);
        ImGui::TableSetupColumn(TrLabel("Still"), ImGuiTableColumnFlags_WidthFixed, 50.0f * dpi);
        ImGui::TableSetupColumn(TrLabel("Over pin"), ImGuiTableColumnFlags_WidthFixed, 64.0f * dpi);
        ImGui::TableHeadersRow();

        if (ImGuiTableSortSpecs* specs = ImGui::TableGetSortSpecs())
        {
            if (specs->SpecsCount > 0 && (specs->SpecsDirty || !m_Sorted))
            {
                const ImGuiTableColumnSortSpecs& s = specs->Specs[0];
                const bool up = s.SortDirection == ImGuiSortDirection_Ascending;
                auto key = [&](const Row& r) -> double
                {
                    switch (s.ColumnIndex)
                    {
                        case kOrder: return r.order;
                        case kMeasure: return r.measureUs;
                        case kContent: return r.contentUs;
                        case kBlit: return r.blitUs;
                        case kPixels: return r.pixels;
                        case kDraws: return r.draws;
                        case kNew: return r.redraws[0];
                        case kChanged: return r.redraws[1];
                        case kStill: return r.redraws[2];
                        case kOverPin: return r.redraws[3];
                        default: return r.Total();
                    }
                };
                std::stable_sort(m_Rows.begin(), m_Rows.end(),
                                 [&](const Row& a, const Row& b)
                                 {
                                     if (s.ColumnIndex == kObject)
                                     {
                                         return up ? a.path < b.path : b.path < a.path;
                                     }
                                     if (s.ColumnIndex == kType)
                                     {
                                         return up ? a.type < b.type : b.type < a.type;
                                     }
                                     return up ? key(a) < key(b) : key(b) < key(a);
                                 });
                specs->SpecsDirty = false;
                m_Sorted = true;
            }
        }

        for (const Row& r : m_Rows)
        {
            if (!ContainsNoCase(r.path, m_Filter) && !ContainsNoCase(r.type, m_Filter))
            {
                continue;
            }
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            ImGui::TextUnformatted(r.path.c_str());
            ImGui::TableNextColumn();
            ImGui::TextUnformatted(r.type.c_str());
            ImGui::TableNextColumn();
            ImGui::Text("%.0f", r.order);
            for (double v : { r.measureUs, r.contentUs, r.blitUs, r.Total() })
            {
                ImGui::TableNextColumn();
                ImGui::Text("%.1f", v);
            }
            ImGui::TableNextColumn();
            ImGui::Text("%.0f", r.pixels);
            ImGui::TableNextColumn();
            ImGui::Text("%.2f", r.draws);
            for (double v : r.redraws)
            {
                ImGui::TableNextColumn();
                ImGui::Text("%.2f", v);
            }
        }
        ImGui::EndTable();
    }

    Source m_Source = Source::PlayView;
    bool m_EnabledHere = false;
    float m_Since = 0.0f;
    std::vector<Row> m_Rows;
    bool m_Sorted = false;
    double m_Frames = 0;
    int m_Total = 0;
    std::string m_Filter;
    std::vector<std::string> m_Ports;
    std::string m_Port;
    bool m_Measuring = false;
    std::string m_Error;
    std::shared_ptr<DeviceJob> m_Job;
};

REGISTER_EDITOR_WINDOW(RenderProfilerWindow, "Render Profiler", "Rendering/Render Profiler")

}  // namespace DekiEditor

#endif  // DEKI_EDITOR
