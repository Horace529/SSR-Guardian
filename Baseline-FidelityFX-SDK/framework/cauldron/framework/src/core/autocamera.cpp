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
        // startup, CAULDRON_AUTO_PLAY=<index or name substring> starts that path automatically,
        // CAULDRON_AUTO_PLAYBACK=<posebin path> replays a recorded take instead of live paths
        // (takes precedence over CAULDRON_AUTO_PLAY).
        wchar_t envValue[64] = {};
        if (GetEnvironmentVariableW(L"CAULDRON_AUTO_RECORD", envValue, 64) > 0 && wcscmp(envValue, L"0") != 0)
        {
            m_RecordArmed = true;
            CauldronWarning(L"AutoCamera: pose recording armed via CAULDRON_AUTO_RECORD.");
        }

        LoadPaths();
        LoadPlaybackFileList();

        wchar_t playbackPath[MAX_PATH] = {};
        if (GetEnvironmentVariableW(L"CAULDRON_AUTO_PLAYBACK", playbackPath, MAX_PATH) > 0 && playbackPath[0] != L'\0')
        {
            LoadPosePlayback(playbackPath);
        }

        if (!m_PlaybackLoaded && GetEnvironmentVariableW(L"CAULDRON_AUTO_PLAY", envValue, 64) > 0 && envValue[0] != L'\0')
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

        // Leave playback mode if active - the path branch owns the camera from here on
        m_PlaybackLoaded = false;
        if (m_PlaybackStream.is_open())
            m_PlaybackStream.close();
        EndRecordingSession();

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

        if (m_PlaybackLoaded)
            return UpdatePlayback(pCamera);

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
        if (m_PlaybackLoaded && m_Playing)
        {
            ImGui::TextColored(ImVec4(0.4f, 0.7f, 1.0f, 1.0f), "PLAYBACK: %s", m_PlaybackSourceName.c_str());
            ImGui::Text("Frame %llu / %llu  (lap %u, F8 restarts)",
                        static_cast<unsigned long long>(m_PlaybackFrame), static_cast<unsigned long long>(m_PlaybackFrameCount), m_Lap);
        }
        ImGui::Spacing();

        for (size_t i = 0; i < m_Paths.size(); ++i)
        {
            const AutoCamPath& path = m_Paths[i];
            ImGui::PushID(static_cast<int>(i));
            const bool active = m_Playing && !m_PlaybackLoaded && m_ActivePath == static_cast<int>(i);
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

        // Recorded takes available for playback (PoseRecorder output, newest first)
        ImGui::Separator();
        ImGui::TextUnformatted("Playback Takes");
        if (ImGui::SmallButton("Refresh"))
            LoadPlaybackFileList();
        ImGui::SameLine();
        ImGui::TextDisabled("%u take(s)", static_cast<uint32_t>(m_PlaybackFiles.size()));

        for (size_t i = 0; i < m_PlaybackFiles.size(); ++i)
        {
            ImGui::PushID(static_cast<int>(1000 + i));
            const std::filesystem::path takePath(m_PlaybackFiles[i]);
            const std::string takeLabel = WStringToString(takePath.stem().wstring());
            const bool active = m_Playing && m_PlaybackLoaded;
            if (active)
                ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.15f, 0.3f, 0.55f, 1.0f));
            if (ImGui::Button(takeLabel.c_str(), ImVec2(-1.f, 0.f)))
            {
                StopAutomation();               // stop any live path + its recording first
                LoadPosePlayback(m_PlaybackFiles[i]);
            }
            if (active)
                ImGui::PopStyleColor();
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("Replay this take (camera + animated entities) from the recording.");
            ImGui::PopID();
        }
        if (m_PlaybackFiles.empty())
            ImGui::TextDisabled("No .posebin takes found - arm recording and run a path first.");

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
        std::string sourceName;
        double duration = 0.0;
        if (m_PlaybackLoaded)
        {
            sourceName = m_PlaybackSourceName + "_playback";
            duration = m_PlaybackDuration;
        }
        else
        {
            if (m_ActivePath < 0)
                return;
            sourceName = m_Paths[m_ActivePath].name;
            duration = m_Paths[m_ActivePath].duration;
        }

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
        const bool usePlaybackNames = m_PlaybackLoaded && m_PlaybackEntityNames.size() == components.size();
        for (size_t i = 0; i < components.size(); ++i)
        {
            const std::wstring name = usePlaybackNames ? m_PlaybackEntityNames[i] : components[i]->GetOwner()->GetName();
            m_SessionEntityNames.push_back(name);
            entityJson += (i > 0 ? "," : "");
            entityJson += "{\"id\":" + std::to_string(i) + ",\"name\":\"" + ToUtf8(name) + "\"}";
        }
        m_SessionEntityCount = static_cast<uint32_t>(m_SessionEntityNames.size());

        m_RecordFileTag = sourceName + "_lap" + std::to_string(m_Lap) + "_" + TimestampTag();
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
        header += ",\"path\":\"" + sourceName + "\"";
        header += ",\"lap\":" + std::to_string(m_Lap);
        header += ",\"duration\":" + std::to_string(duration);
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
        // Session lifecycle: exists exactly while armed AND (a path is playing OR a
        // playback take is running). This enforces the ordering rule - arm first.
        // Playback takes additionally wait for entity binding so the take starts at
        // the first truly overridden frame (no pre-bind scene-load tail).
        const bool sessionWanted = m_Playing && m_RecordArmed
                                   && (m_ActivePath >= 0 || m_PlaybackLoaded)
                                   && (!m_PlaybackLoaded || m_PlaybackBound);
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

    void AutoCameraController::LoadPlaybackFileList()
    {
        m_PlaybackFiles.clear();

        // Newest takes first so a fresh capture is always at the top of the UI
        std::vector<std::pair<std::filesystem::file_time_type, std::wstring>> found;
        for (const wchar_t* candidate : kAutoCamRecordingDirs)
        {
            std::filesystem::path dir(candidate);
            std::error_code ec;
            if (!std::filesystem::exists(dir, ec))
                continue;
            for (const auto& entry : std::filesystem::directory_iterator(dir, ec))
            {
                if (entry.is_regular_file(ec) && entry.path().extension() == L".posebin")
                    found.emplace_back(entry.last_write_time(ec), entry.path().wstring());
            }
        }
        std::sort(found.begin(), found.end(), [](const auto& a, const auto& b) { return a.first > b.first; });
        for (auto& f : found)
            m_PlaybackFiles.push_back(f.second);

        if (!m_PlaybackFiles.empty())
            CauldronWarning(L"AutoCamera: %u playback take(s) available (F9-recorder output).",
                            static_cast<uint32_t>(m_PlaybackFiles.size()));
    }

    bool AutoCameraController::LoadPosePlayback(const std::wstring& filePath)
    {
        std::error_code ec;
        if (!std::filesystem::exists(filePath, ec))
        {
            CauldronWarning(L"AutoCamera: playback file not found - '%ls'", filePath.c_str());
            return false;
        }

        // Switching takes mid-playback: close the previous stream and reset binding state
        if (m_PlaybackStream.is_open())
            m_PlaybackStream.close();
        if (m_Recording)
            EndRecordingSession();
        m_PlaybackBound = false;
        m_PlaybackWarned = false;
        m_PlaybackApplied.clear();
        m_PlaybackPending.clear();
        m_PlaybackPendingValid = false;

        m_PlaybackStream.open(filePath, std::ios::binary);
        if (!m_PlaybackStream)
        {
            CauldronWarning(L"AutoCamera: failed to open playback file '%ls'", filePath.c_str());
            return false;
        }

        json header;
        try
        {
            m_PlaybackStream >> header;
        }
        catch (const std::exception&)
        {
            CauldronWarning(L"AutoCamera: playback header parse failed - '%ls'", filePath.c_str());
            m_PlaybackStream.close();
            return false;
        }
        if (header.value("magic", std::string()) != "CAULDRON_POSE_V1")
        {
            CauldronWarning(L"AutoCamera: unsupported playback magic in '%ls'", filePath.c_str());
            m_PlaybackStream.close();
            return false;
        }

        const auto entitiesIt = header.find("entities");
        if (entitiesIt == header.end() || !entitiesIt->is_array() || entitiesIt->empty())
        {
            CauldronWarning(L"AutoCamera: playback file has no entity table - '%ls'", filePath.c_str());
            m_PlaybackStream.close();
            return false;
        }

        m_PlaybackEntityNames.clear();
        for (const auto& e : *entitiesIt)
            m_PlaybackEntityNames.push_back(StringToWString(e.value("name", "")));
        m_PlaybackEntityCount = static_cast<uint32_t>(m_PlaybackEntityNames.size());

        // Consume the remainder of the header line; records start right after it
        char c = 0;
        while (m_PlaybackStream.get(c))
        {
            if (c == '\n')
                break;
        }
        m_PlaybackDataStart = m_PlaybackStream.tellg();

        m_PlaybackStream.seekg(0, std::ios::end);
        const std::streamoff fileSize = m_PlaybackStream.tellg();
        const std::streamoff bodySize = fileSize - m_PlaybackDataStart;
        m_PlaybackStride = 8 + 4 + 8 + 8 + 8 + 12 + 12 + 64 + 4 + uint64_t(m_PlaybackEntityCount) * 64;
        if (bodySize < static_cast<std::streamoff>(m_PlaybackStride))
        {
            CauldronWarning(L"AutoCamera: playback file too small (%lld bytes body) - '%ls'",
                            static_cast<long long>(bodySize), filePath.c_str());
            m_PlaybackStream.close();
            return false;
        }
        m_PlaybackFrameCount = static_cast<uint64_t>(bodySize) / m_PlaybackStride;   // torn tail dropped
        m_PlaybackDuration = header.value("duration", 0.0);
        m_PlaybackSourceName = header.value("path", "take");
        m_PlaybackFrame = 0;
        m_PlaybackApplied.clear();
        m_PlaybackPending.clear();
        m_PlaybackPendingValid = false;
        m_PlaybackLoaded = true;
        m_Playing = true;
        m_Lap = 1;

        CauldronWarning(L"AutoCamera: playback loaded '%ls' - %llu frames, %u entities, %.1fs (%hs).",
                        filePath.c_str(), static_cast<unsigned long long>(m_PlaybackFrameCount),
                        m_PlaybackEntityCount, m_PlaybackDuration, m_PlaybackSourceName.c_str());
        return true;
    }

    bool AutoCameraController::UpdatePlayback(CameraComponent* pCamera)
    {
        const InputState& inputState = GetInputManager()->GetInputState();
        if (inputState.GetKeyUpState(Key_F7) || inputState.GetGamePadButtonUpState(Pad_Back))
        {
            m_Playing = false;
            EndRecordingSession();
            CauldronWarning(L"AutoCamera: playback stopped - free camera control restored (F7).");
            return false;
        }
        if (inputState.GetKeyUpState(Key_F8))
        {
            m_PlaybackFrame = 0;
            if (m_Recording)
                EndRecordingSession();   // F8 = fresh take
        }

        // A scene change invalidates entity bindings; rebind on the next apply
        const void* pScene = GetScene();
        if (pScene != m_pLastScene)
        {
            m_pLastScene = pScene;
            m_PlaybackBound = false;
        }

        if (!m_Playing || pCamera == nullptr)
            return false;

        // Hold at the current record until the scene's animated entities are all present
        // (the binding condition of ApplyPlaybackTransforms). Otherwise the frame counter
        // advances during scene loading and the take no longer aligns with its source.
        if (!m_PlaybackBound)
        {
            AnimationComponentMgr* pAnimMgr = AnimationComponentMgr::Get();
            if (pAnimMgr == nullptr || pAnimMgr->GetComponentCount() != m_PlaybackEntityCount)
                return false;
        }

        if (m_PlaybackFrame >= m_PlaybackFrameCount)
        {
            m_PlaybackFrame = 0;    // seamless loop
            ++m_Lap;
            if (m_Recording)
                EndRecordingSession();   // per-lap takes
        }

        m_PlaybackStream.clear();
        m_PlaybackStream.seekg(m_PlaybackDataStart + static_cast<std::streamoff>(m_PlaybackFrame * m_PlaybackStride));
        m_PlaybackRecord.resize(static_cast<size_t>(m_PlaybackStride));
        m_PlaybackStream.read(m_PlaybackRecord.data(), static_cast<std::streamsize>(m_PlaybackStride));
        if (m_PlaybackStream.gcount() != static_cast<std::streamsize>(m_PlaybackStride))
        {
            CauldronWarning(L"AutoCamera: playback read failed at frame %llu - stopping.",
                            static_cast<unsigned long long>(m_PlaybackFrame));
            m_Playing = false;
            EndRecordingSession();
            return false;
        }

        const char* rec = m_PlaybackRecord.data();
        float camPos[3], camLook[3];
        memcpy(camPos, rec + 36, sizeof(camPos));
        memcpy(camLook, rec + 48, sizeof(camLook));
        uint32_t count = 0;
        memcpy(&count, rec + 124, sizeof(count));
        if (count != m_PlaybackEntityCount)
        {
            CauldronWarning(L"AutoCamera: playback entity count mismatch at frame %llu - stopping.",
                            static_cast<unsigned long long>(m_PlaybackFrame));
            m_Playing = false;
            EndRecordingSession();
            return false;
        }

        m_PlaybackPending.resize(m_PlaybackEntityCount);
        for (uint32_t i = 0; i < m_PlaybackEntityCount; ++i)
        {
            float vals[16];
            memcpy(vals, rec + 128 + static_cast<size_t>(i) * 64, sizeof(vals));
            Mat4 m = Mat4::identity();
            for (int row = 0; row < 4; ++row)
                for (int col = 0; col < 4; ++col)
                    m.setElem(col, row, vals[row * 4 + col]);
            m_PlaybackPending[i] = m;
        }
        m_PlaybackPendingValid = true;

        m_LastPos = Vec4(camPos[0], camPos[1], camPos[2], 0.f);
        m_LastLook = Vec4(camLook[0], camLook[1], camLook[2], 0.f);
        pCamera->SetAnimatedPose(m_LastPos, m_LastLook);

        ++m_PlaybackFrame;
        return true;
    }

    void AutoCameraController::ApplyPlaybackTransforms()
    {
        if (!m_PlaybackLoaded || !m_Playing || !m_PlaybackPendingValid)
            return;

        AnimationComponentMgr* pAnimMgr = AnimationComponentMgr::Get();
        if (pAnimMgr == nullptr)
            return;

        if (!m_PlaybackBound)
        {
            // Bind by index: the recorder wrote transforms in GetComponentList() order, which
            // is deterministic for the same scene. Names are verified as a scene-identity check.
            const std::vector<Component*>& components = pAnimMgr->GetComponentList();
            if (components.size() != m_PlaybackEntityCount)
            {
                if (!m_PlaybackWarned)
                {
                    m_PlaybackWarned = true;
                    CauldronWarning(L"AutoCamera: playback entity count mismatch (%u vs %u) - transforms not applied.",
                                    m_PlaybackEntityCount, static_cast<uint32_t>(components.size()));
                }
                return;
            }
            m_PlaybackEntities.assign(m_PlaybackEntityCount, nullptr);
            for (uint32_t i = 0; i < m_PlaybackEntityCount; ++i)
            {
                Entity* pEntity = components[i]->GetOwner();
                if (wcscmp(pEntity->GetName(), m_PlaybackEntityNames[i].c_str()) != 0)
                {
                    CauldronWarning(L"AutoCamera: playback entity name mismatch at [%u] ('%ls' vs '%ls') - "
                                    L"scene differs from the recording, transforms not applied.",
                                    i, pEntity->GetName(), m_PlaybackEntityNames[i].c_str());
                    return;
                }
                m_PlaybackEntities[i] = pEntity;
            }
            m_PlaybackApplied.assign(m_PlaybackEntityCount, Mat4::identity());
            for (uint32_t i = 0; i < m_PlaybackEntityCount; ++i)
                m_PlaybackApplied[i] = m_PlaybackPending[i];   // first frame: no motion yet
            m_PlaybackBound = true;
            CauldronWarning(L"AutoCamera: playback bound %u animated entities.", m_PlaybackEntityCount);
        }

        for (uint32_t i = 0; i < m_PlaybackEntityCount; ++i)
        {
            Entity* pEntity = m_PlaybackEntities[i];
            pEntity->SetPrevTransform(m_PlaybackApplied[i]);   // motion vectors come from here
            pEntity->SetTransform(m_PlaybackPending[i]);
        }
        m_PlaybackApplied = m_PlaybackPending;
        m_PlaybackPendingValid = false;
    }

} // namespace cauldron
