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
#include "misc/helpers.h"

#include "json/json.h"
#include "imgui/imgui.h"

#include <algorithm>
#include <ctime>
#include <filesystem>
#include <fstream>

namespace cauldron
{
    // Candidate config locations, in priority order (both relative to the exe's
    // working directory, which is bin/): the bin copy used during development,
    // then the tracked source folder for fresh checkouts.
    static constexpr const wchar_t* kAutoCamConfigDirs[] = { L"configs/autocamera", L"../autocamera" };

    // Recording takes land next to the tracked trajectory files so a capture session
    // is easy to find and git-track; fallback keeps working from any cwd.
    static constexpr const wchar_t* kAutoCamRecordingDirs[] = { L"../autocamera/recordings", L"recordings" };

    static std::string TimestampTag()
    {
        std::time_t now = std::time(nullptr);
        std::tm local = {};
        localtime_s(&local, &now);
        char buf[32] = {};
        strftime(buf, sizeof(buf), "%Y%m%d_%H%M%S", &local);
        return buf;
    }

    AutoCameraController& AutoCameraController::Get()
    {
        static AutoCameraController s_Controller;
        return s_Controller;
    }

    AutoCameraController::AutoCameraController()
    {
        // Headless/batch data capture hooks: CAULDRON_AUTO_RECORD=1 arms pose recording at
        // startup, CAULDRON_AUTO_PLAY=<index or name substring> starts that path automatically.
        wchar_t envValue[64] = {};
        if (GetEnvironmentVariableW(L"CAULDRON_AUTO_RECORD", envValue, 64) > 0 && wcscmp(envValue, L"0") != 0)
        {
            m_RecordArmed = true;
            CauldronWarning(L"AutoCamera: pose recording armed via CAULDRON_AUTO_RECORD.");
        }

        LoadPaths();

        if (GetEnvironmentVariableW(L"CAULDRON_AUTO_PLAY", envValue, 64) > 0 && envValue[0] != L'\0')
        {
            bool started = false;
            if (iswdigit(envValue[0]))
            {
                const int index = _wtoi(envValue);
                if (index >= 0 && static_cast<size_t>(index) < m_Paths.size())
                {
                    StartPath(static_cast<size_t>(index));
                    started = true;
                }
            }
            if (!started)
            {
                const std::string sub = WStringToString(envValue);
                for (size_t i = 0; i < m_Paths.size(); ++i)
                {
                    if (m_Paths[i].name.find(sub) != std::string::npos)
                    {
                        StartPath(i);
                        started = true;
                        break;
                    }
                }
            }
            if (started)
                CauldronWarning(L"AutoCamera: path auto-started via CAULDRON_AUTO_PLAY.");
        }
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
        EndRecordingSession();
        CauldronWarning(L"AutoCamera: stopped - free camera control restored (F7).");
    }

    void AutoCameraController::RestartLap()
    {
        m_PathTime = 0.0;
        ++m_Lap;

        // Recording takes are per-lap: close the current file here, the next
        // RecordFrame opens a fresh one tagged with the new lap.
        if (m_Recording)
            EndRecordingSession();

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
        // The F9 arm toggle must work whether or not a path is playing
        if (pCamera != nullptr)
            m_pLastCamera = pCamera;
        {
            const InputState& inputState = GetInputManager()->GetInputState();
            if (inputState.GetKeyUpState(Key_F9))
            {
                m_RecordArmed = !m_RecordArmed;
                CauldronWarning(L"AutoCamera: pose recording %ls", m_RecordArmed ? L"ARMED - capture starts when a path plays" : L"disarmed");
            }
        }

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
            // Each lap is its own recording take
            if (m_Recording)
                EndRecordingSession();
        }

        Vec4 pos, look;
        if (Evaluate(m_PathTime, pos, look))
        {
            m_LastPos = pos;
            m_LastLook = look;
            pCamera->SetAnimatedPose(pos, look);
        }
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
        ImGui::Checkbox("Record Pose Sequence (F9)", &m_RecordArmed);
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("Arm before starting a path: one .posebin take is written per lap to autocamera/recordings.");
        if (m_Recording)
        {
            ImGui::TextColored(ImVec4(0.3f, 0.9f, 0.35f, 1.0f), "RECORDING: %s", m_RecordFileTag.c_str());
            ImGui::Text("Frames written: %llu", static_cast<unsigned long long>(m_FramesWritten));
        }
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
            ImGui::Text("Idle - free camera (F1 toggles this UI's main window)");
        }

        ImGui::End();
    }

    std::string AutoCameraController::ToUtf8(const std::wstring& text)
    {
        return WStringToString(text);
    }

    void AutoCameraController::StartRecordingSession()
    {
        if (m_ActivePath < 0)
            return;
        const AutoCamPath& path = m_Paths[m_ActivePath];

        AnimationComponentMgr* pAnimMgr = AnimationComponentMgr::Get();
        if (pAnimMgr == nullptr || pAnimMgr->GetComponentCount() == 0)
        {
            CauldronWarning(L"AutoCamera: recording armed but the scene has no animated entities - session skipped.");
            return;
        }

        std::filesystem::path dir;
        for (const wchar_t* candidate : kAutoCamRecordingDirs)
        {
            std::filesystem::path p(candidate);
            std::error_code ec;
            std::filesystem::create_directories(p, ec);
            if (std::filesystem::exists(p, ec))
            {
                dir = p;
                break;
            }
        }
        if (dir.empty())
        {
            CauldronWarning(L"AutoCamera: could not create a recordings directory - session skipped.");
            return;
        }

        // Entity table: names are written once in the header, transforms every frame
        // in exactly this order. Component insertion order is stable within a session.
        const std::vector<Component*>& components = pAnimMgr->GetComponentList();
        m_SessionEntityNames.clear();
        std::string entityJson;
        for (size_t i = 0; i < components.size(); ++i)
        {
            const std::wstring name = components[i]->GetOwner()->GetName();
            m_SessionEntityNames.push_back(name);
            entityJson += (i > 0 ? "," : "");
            entityJson += "{\"id\":" + std::to_string(i) + ",\"name\":\"" + ToUtf8(name) + "\"}";
        }
        m_SessionEntityCount = static_cast<uint32_t>(m_SessionEntityNames.size());

        m_RecordFileTag = path.name + "_lap" + std::to_string(m_Lap) + "_" + TimestampTag();
        m_RecordFilePath = dir / (m_RecordFileTag + ".posebin");

        m_RecordStream.open(m_RecordFilePath, std::ios::binary | std::ios::trunc);
        if (!m_RecordStream)
        {
            CauldronWarning(L"AutoCamera: failed to open '%ls' - session skipped.", m_RecordFilePath.c_str());
            m_RecordFilePath.clear();
            m_RecordFileTag.clear();
            return;
        }

        // JSON header (single line, closed by '\n'), then fixed-layout binary records
        std::string header = "{\"magic\":\"CAULDRON_POSE_V1\"";
        header += ",\"path\":\"" + path.name + "\"";
        header += ",\"lap\":" + std::to_string(m_Lap);
        header += ",\"duration\":" + std::to_string(path.duration);
        header += ",\"layout\":\"frame u64, lap u32, anim_clock f64, delta_time f64, path_time f64, cam_pos f32[3], cam_look f32[3], cam_inv_view f32[16], count u32, matrices f32[16]*count\"";
        header += ",\"matrix_order\":\"row-major listing m[r][c]=Matrix4.getElem(col=c,row=r), world space\"";
        header += ",\"entities\":[" + entityJson + "]}";
        m_RecordStream << header << '\n';
        if (!m_RecordStream)
        {
            CauldronWarning(L"AutoCamera: failed to write header to '%ls' - session skipped.", m_RecordFilePath.c_str());
            m_RecordStream.close();
            m_RecordFilePath.clear();
            m_RecordFileTag.clear();
            return;
        }

        m_FramesWritten = 0;
        m_Recording = true;
        CauldronWarning(L"AutoCamera: recording started - '%hs' (%u animated entities).",
                        m_RecordFileTag.c_str(), m_SessionEntityCount);
    }

    void AutoCameraController::EndRecordingSession()
    {
        if (!m_Recording)
            return;
        if (m_RecordStream.is_open())
            m_RecordStream.close();
        m_Recording = false;
        CauldronWarning(L"AutoCamera: recording saved - %llu frames in '%hs'",
                        static_cast<unsigned long long>(m_FramesWritten), m_RecordFileTag.c_str());
        m_RecordFileTag.clear();
        m_RecordFilePath.clear();
        m_SessionEntityNames.clear();
        m_SessionEntityCount = 0;
    }

    void AutoCameraController::RecordFrame(double deltaTime)
    {
        // Session lifecycle: exists exactly while armed AND a path is playing. This
        // enforces the ordering rule - arm first, then start the path.
        const bool sessionWanted = m_Playing && m_RecordArmed && m_ActivePath >= 0;
        if (!sessionWanted)
        {
            if (m_Recording)
                EndRecordingSession();
            return;
        }
        if (!m_Recording)
            StartRecordingSession();
        if (!m_Recording || !m_RecordStream.is_open())
            return;

        AnimationComponentMgr* pAnimMgr = AnimationComponentMgr::Get();
        if (pAnimMgr == nullptr || m_pLastCamera == nullptr)
            return;

        const std::vector<Component*>& components = pAnimMgr->GetComponentList();
        if (components.size() != m_SessionEntityCount)
        {
            // Scene content changed mid-take: keep the file self-consistent and stop.
            CauldronWarning(L"AutoCamera: animated entity count changed mid-take - closing recording.");
            EndRecordingSession();
            return;
        }

        m_RecordScratch.clear();
        auto appendValue = [this](const void* data, size_t bytes) {
            const char* first = static_cast<const char*>(data);
            m_RecordScratch.insert(m_RecordScratch.end(), first, first + bytes);
        };

        const uint64_t frameIndex = m_FramesWritten;   // 0-based within the take
        const uint32_t lap = m_Lap;
        const double animClock = pAnimMgr->GetElapsedTime();
        const double pathTime = m_PathTime;
        const float camPos[3]   = { m_LastPos.getElem(0), m_LastPos.getElem(1), m_LastPos.getElem(2) };
        const float camLook[3]  = { m_LastLook.getElem(0), m_LastLook.getElem(1), m_LastLook.getElem(2) };

        float invView[16];
        const Mat4& invViewMat = m_pLastCamera->GetInverseView();
        for (int row = 0; row < 4; ++row)
            for (int col = 0; col < 4; ++col)
                invView[row * 4 + col] = invViewMat.getElem(col, row);

        appendValue(&frameIndex, sizeof(frameIndex));
        appendValue(&lap, sizeof(lap));
        appendValue(&animClock, sizeof(animClock));
        appendValue(&deltaTime, sizeof(deltaTime));
        appendValue(&pathTime, sizeof(pathTime));
        appendValue(camPos, sizeof(camPos));
        appendValue(camLook, sizeof(camLook));
        appendValue(invView, sizeof(invView));
        appendValue(&m_SessionEntityCount, sizeof(m_SessionEntityCount));
        for (const Component* component : components)
        {
            const Mat4& world = component->GetOwner()->GetTransform();
            float vals[16];
            for (int row = 0; row < 4; ++row)
                for (int col = 0; col < 4; ++col)
                    vals[row * 4 + col] = world.getElem(col, row);
            appendValue(vals, sizeof(vals));
        }

        m_RecordStream.write(m_RecordScratch.data(), static_cast<std::streamsize>(m_RecordScratch.size()));
        ++m_FramesWritten;

        // Periodic flush so a crashed run still leaves a parseable take
        if ((m_FramesWritten % 600) == 0)
            m_RecordStream.flush();
    }

} // namespace cauldron
