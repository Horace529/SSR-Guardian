// This file is part of the FidelityFX SDK.
//
// Copyright (C) 2024 Advanced Micro Devices, Inc.
//
// Permission is hereby granted, free of charge, to any person obtaining a copy
// of this software and associated documentation files (the "Software"), to deal
// in the Software without restriction, including without limitation the rights
// to use, copy, modify, merge, publish, distribute, sublicense, and /or sell
// copies of the Software, and to permit persons to whom the Software is
// furnished to do so, subject to the following conditions :
//
// The above copyright notice and this permission notice shall be included in
// all copies or substantial portions of the Software.
//
// THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
// IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
// FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
// AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
// LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
// OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
// THE SOFTWARE.

#include "core/autocamera.h"
#include "core/components/cameracomponent.h"
#include "core/components/animationcomponent.h"
#include "core/scene.h"
#include "core/framework.h"
#include "core/inputmanager.h"
#include "misc/assert.h"

#include "json/json.h"
#include "imgui/imgui.h"

#include <algorithm>
#include <filesystem>
#include <fstream>

namespace cauldron
{
    // Candidate config locations, in priority order (both relative to the exe's
    // working directory, which is bin/): the bin copy used during development,
    // then the tracked source folder for fresh checkouts.
    static constexpr const wchar_t* kAutoCamConfigDirs[] = { L"configs/autocamera", L"../autocamera" };

    AutoCameraController& AutoCameraController::Get()
    {
        static AutoCameraController s_Controller;
        return s_Controller;
    }

    AutoCameraController::AutoCameraController()
    {
        LoadPaths();
    }

    void AutoCameraController::LoadPaths()
    {
        m_Paths.clear();

        std::error_code ec;
        std::filesystem::path dir;
        for (const wchar_t* candidate : kAutoCamConfigDirs)
        {
            std::filesystem::path p(candidate);
            if (std::filesystem::exists(p, ec))
            {
                dir = p;
                break;
            }
        }
        if (dir.empty())
        {
            CauldronWarning(L"AutoCamera: no config directory found (tried 'configs/autocamera', '../autocamera') - no camera paths loaded.");
            return;
        }

        std::vector<std::filesystem::path> files;
        for (const auto& entry : std::filesystem::directory_iterator(dir, ec))
        {
            if (entry.is_regular_file() && entry.path().extension() == ".json")
                files.push_back(entry.path());
        }
        std::sort(files.begin(), files.end());

        for (const auto& file : files)
        {
            std::ifstream stream(file);
            if (!stream)
                continue;

            json doc;
            try
            {
                stream >> doc;
            }
            catch (const std::exception&)
            {
                CauldronWarning(L"AutoCamera: failed to parse '%ls'.", file.c_str());
                continue;
            }

            AutoCamPath path;
            path.name = doc.value("name", file.stem().string());
            path.description = doc.value("description", "");
            path.duration = doc.value("duration", 0.0);

            const auto it = doc.find("keyframes");
            if (it == doc.end() || !it->is_array() || it->empty() || path.duration <= 0.0)
            {
                CauldronWarning(L"AutoCamera: '%ls' has no usable keyframes - skipped.", file.c_str());
                continue;
            }

            path.keys.reserve(it->size());
            for (const auto& k : *it)
            {
                const auto& p = k.at("pos");
                const auto& l = k.at("look");
                AutoCamKeyframe key;
                key.t = k.value("t", 0.0);
                key.pos = Vec4(p[0].get<float>(), p[1].get<float>(), p[2].get<float>(), 0.f);
                key.look = Vec4(l[0].get<float>(), l[1].get<float>(), l[2].get<float>(), 0.f);
                path.keys.push_back(key);
            }

            // Skip duplicate names (e.g. manually copied files next to the originals)
            if (std::any_of(m_Paths.begin(), m_Paths.end(),
                            [&path](const AutoCamPath& existing) { return existing.name == path.name; }))
                continue;

            CauldronWarning(L"AutoCamera: loaded '%hs' (%zu keys, %.1fs).", path.name.c_str(), path.keys.size(), path.duration);
            m_Paths.push_back(std::move(path));
        }
    }

    void AutoCameraController::StartPath(size_t index)
    {
        if (index >= m_Paths.size())
            return;

        m_ActivePath = static_cast<int>(index);
        m_Playing = true;
        RestartLap();
    }

    void AutoCameraController::StopAutomation()
    {
        if (!m_Playing)
            return;
        m_Playing = false;
        CauldronWarning(L"AutoCamera: stopped - free camera control restored (F7).");
    }

    void AutoCameraController::RestartLap()
    {
        m_PathTime = 0.0;
        ++m_Lap;

        // Restore every animated object (drones, hatches, ...) to the start of its
        // animation so each automated run sees identical object motion.
        if (AnimationComponentMgr::Get() != nullptr)
            AnimationComponentMgr::Get()->ResetAnimationTime();

        if (m_ActivePath >= 0)
            CauldronWarning(L"AutoCamera: playing '%hs' (lap %u, scene animations reset).",
                            m_Paths[m_ActivePath].name.c_str(), m_Lap);
    }

    bool AutoCameraController::Evaluate(double t, Vec4& pos, Vec4& look) const
    {
        const AutoCamPath& path = m_Paths[m_ActivePath];
        const std::vector<AutoCamKeyframe>& keys = path.keys;
        if (keys.empty())
            return false;

        if (t <= keys.front().t)
        {
            pos = keys.front().pos;
            look = keys.front().look;
            return true;
        }
        if (t >= keys.back().t)
        {
            pos = keys.back().pos;
            look = keys.back().look;
            return true;
        }

        // Binary search for the bracketing pair
        size_t hi = static_cast<size_t>(
            std::upper_bound(keys.begin(), keys.end(), t,
                             [](double value, const AutoCamKeyframe& k) { return value < k.t; }) - keys.begin());
        size_t i = hi - 1;

        const double span = keys[i + 1].t - keys[i].t;
        const float u = span > 1e-9 ? static_cast<float>((t - keys[i].t) / span) : 0.f;

        // Catmull-Rom over time for smooth position and velocity continuity
        const size_t n = keys.size();
        const size_t i0 = i > 0 ? i - 1 : i;
        const size_t i3 = i + 2 < n ? i + 2 : i + 1;
        const float u2 = u * u;
        const float u3 = u2 * u;
        const float c0 = 0.5f * (-u + 2.f * u2 - u3);
        const float c1 = 0.5f * (2.f - 5.f * u2 + 3.f * u3);
        const float c2 = 0.5f * (u + 4.f * u2 - 3.f * u3);
        const float c3 = 0.5f * (-u2 + u3);

        const Vec4& p0 = keys[i0].pos;   const Vec4& p1 = keys[i].pos;
        const Vec4& p2 = keys[i + 1].pos; const Vec4& p3 = keys[i3].pos;
        const Vec4& l0 = keys[i0].look;  const Vec4& l1 = keys[i].look;
        const Vec4& l2 = keys[i + 1].look; const Vec4& l3 = keys[i3].look;

        pos = p0 * c0 + p1 * c1 + p2 * c2 + p3 * c3;
        look = l0 * c0 + l1 * c1 + l2 * c2 + l3 * c3;
        return true;
    }

    bool AutoCameraController::Update(double deltaTime, CameraComponent* pCamera)
    {
        if (!m_Playing || pCamera == nullptr || m_ActivePath < 0)
            return false;

        const InputState& inputState = GetInputManager()->GetInputState();
        if (inputState.GetKeyUpState(Key_F7) || inputState.GetGamePadButtonUpState(Pad_Back))
        {
            StopAutomation();
            return false;
        }
        if (inputState.GetKeyUpState(Key_F8))
            RestartLap();

        // A scene change invalidates the run; restart the lap for repeatability
        const void* pScene = GetScene();
        if (pScene != m_pLastScene)
        {
            m_pLastScene = pScene;
            RestartLap();
        }

        const AutoCamPath& path = m_Paths[m_ActivePath];
        m_PathTime += deltaTime;
        if (m_PathTime >= path.duration)
        {
            m_PathTime -= path.duration;   // seamless loop
            ++m_Lap;
            if (AnimationComponentMgr::Get() != nullptr)
                AnimationComponentMgr::Get()->ResetAnimationTime();
        }

        Vec4 pos, look;
        if (Evaluate(m_PathTime, pos, look))
            pCamera->SetAnimatedPose(pos, look);
        return true;
    }

    void AutoCameraController::DrawUI()
    {
        if (m_Paths.empty())
            return;

        ImGui::SetNextWindowSize(ImVec2(400.f, 0.f), ImGuiCond_FirstUseEver);
        if (!ImGui::Begin("Auto Camera Paths", nullptr, ImGuiWindowFlags_NoCollapse))
        {
            ImGui::End();
            return;
        }

        ImGui::TextUnformatted("Playback resets scene animations every lap for repeatable runs.");
        ImGui::Spacing();

        for (size_t i = 0; i < m_Paths.size(); ++i)
        {
            const AutoCamPath& path = m_Paths[i];
            ImGui::PushID(static_cast<int>(i));
            const bool active = m_Playing && m_ActivePath == static_cast<int>(i);
            if (active)
                ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.0f, 0.45f, 0.2f, 1.0f));
            if (ImGui::Button(path.name.c_str(), ImVec2(-1.f, 0.f)))
                StartPath(i);
            if (active)
                ImGui::PopStyleColor();
            if (!path.description.empty() && ImGui::IsItemHovered())
                ImGui::SetTooltip("%s", path.description.c_str());
            ImGui::PopID();
        }

        ImGui::Spacing();
        if (ImGui::Button("Stop / Free Camera (F7)", ImVec2(-1.f, 0.f)))
            StopAutomation();

        ImGui::Spacing();
        if (m_Playing && m_ActivePath >= 0)
        {
            const AutoCamPath& path = m_Paths[m_ActivePath];
            ImGui::Text("Playing: %s  [lap %u]", path.name.c_str(), m_Lap);
            ImGui::Text("Time: %.2f / %.1f s   (F8 restarts lap)", m_PathTime, path.duration);
        }
        else
        {
            ImGui::TextUnformatted("Idle - free camera (F1 toggles this UI's main window)");
        }

        ImGui::End();
    }

} // namespace cauldron
