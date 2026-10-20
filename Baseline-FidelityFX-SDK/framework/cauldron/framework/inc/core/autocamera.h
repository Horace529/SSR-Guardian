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

#pragma once

#include "misc/math.h"

#include <string>
#include <vector>
#include <fstream>

namespace cauldron
{
    class CameraComponent;
    class Entity;

    /**
     * @struct AutoCamKeyframe
     *
     * One sampled pose of a camera path: normalized time plus eye and look-at positions.
     *
     * @ingroup CauldronCore
     */
    struct AutoCamKeyframe
    {
        double t = 0.0;     ///< Time stamp (seconds from path start)
        Vec4   pos;         ///< Eye position
        Vec4   look;        ///< Look-at target
    };

    /**
     * @struct AutoCamPath
     *
     * A time-series camera path loaded from configs/autocamera/*.json. Paths are
     * generated offline against the scene's voxelized geometry so they never clip
     * through the hangar, and carry per-path speed stats in their "meta" block.
     *
     * @ingroup CauldronCore
     */
    struct AutoCamPath
    {
        std::string                name;
        std::string                description;
        double                     duration = 0.0;   ///< Seconds per lap
        std::vector<AutoCamKeyframe> keys;
    };

    /**
     * @class AutoCameraController
     *
     * Plays precomputed, collision-free camera paths for repeatable capture runs
     * (e.g. SSR validation). Paths are time-series JSON files (see
     * bin/configs/autocamera/generate_paths.py) with baked-in speed profiles:
     * linear and angular velocities both vary with max >= 4x min.
     *
     * Every lap start - first play, loop-around, F8 restart or scene change -
     * resets the scene animation time so animated objects (drones, hatches)
     * follow an identical motion pattern on each run.
     *
     * Controls:
     *  - UI window "Auto Camera Paths" (drawn every frame): one button per path
     *  - F7: stop automation, restore free camera
     *  - F8: restart the current path lap
     *
     * @ingroup CauldronCore
     */
    class AutoCameraController
    {
    public:
        /**
         * @brief   Controller instance accessor (loads path files on first use).
         */
        static AutoCameraController& Get();

        /**
         * @brief   Per-frame update, called from CameraComponent::Update for the active
         *          camera. While a path is playing it drives the camera pose and returns
         *          true (caller must bypass free-cam input). Returns false when idle.
         */
        bool Update(double deltaTime, CameraComponent* pCamera);

        /**
         * @brief   Draws the path-selection ImGui window. Called from UIBackend::Update.
         */
        void DrawUI();

        /**
         * @brief   Starts playing path[index] from its beginning.
         */
        void StartPath(size_t index);

        /**
         * @brief   Stops automation and restores free camera control.
         */
        void StopAutomation();

        bool IsPlaying() const { return m_Playing; }

        /**
         * @brief   Writes one pose-sequence record if a recording session is active. A session
         *          exists only while the recorder is armed AND a path is playing, so ordering
         *          matters: arm first (UI checkbox, F9, or CAULDRON_AUTO_RECORD=1), then start
         *          a path. Called from the framework AFTER all component managers have updated,
         *          so camera and animated entity transforms are final for the rendered frame.
         *
         *          Record layout (little-endian, after a JSON header line):
         *          frame_index u64 | lap u32 | anim_clock f64 | delta_time f64 | path_time f64 |
         *          cam_pos f32[3] | cam_look f32[3] | cam_inv_view f32[16] | entity_count u32 |
         *          entity_count x world_matrix f32[16] (row-major listing, order == header table)
         */
        void RecordFrame(double deltaTime);

        bool IsRecording() const { return m_Recording; }

        /**
         * @brief   Loads a .posebin take for playback: the camera and every animated entity
         *          are driven from the recorded sequence (transforms overridden after the
         *          component update pass) instead of live path evaluation. Enabled via
         *          CAULDRON_AUTO_PLAYBACK=<posebin path>. F7 stops, F8 restarts from frame 0.
         * @returns true if the file was parsed and playback will start.
         */
        bool LoadPosePlayback(const std::wstring& filePath);

        bool IsPlayback() const { return m_PlaybackLoaded; }

        /**
         * @brief   Applies the pending playback entity transforms (called from the framework
         *          right before RecordFrame, so overridden transforms and any round-trip
         *          recording both see the exact recorded state).
         */
        void ApplyPlaybackTransforms();

    private:
        AutoCameraController();
        AutoCameraController(const AutoCameraController&) = delete;
        AutoCameraController& operator=(const AutoCameraController&) = delete;

        void LoadPaths();
        void RestartLap();
        bool Evaluate(double t, Vec4& pos, Vec4& look) const;
        bool UpdatePlayback(CameraComponent* pCamera);

        void StartRecordingSession();
        void EndRecordingSession();
        static std::string ToUtf8(const std::wstring& text);

        std::vector<AutoCamPath> m_Paths;
        int                      m_ActivePath = -1;
        bool                     m_Playing = false;
        double                   m_PathTime = 0.0;
        uint32_t                 m_Lap = 0;
        const void*              m_pLastScene = nullptr;

        // Pose playback (M1): drives camera + animated entities from a .posebin take
        bool                      m_PlaybackLoaded = false;
        bool                      m_PlaybackBound = false;     // entity name binding done
        bool                      m_PlaybackWarned = false;    // bind-failure warning emitted
        bool                      m_PlaybackPendingValid = false;
        std::ifstream             m_PlaybackStream;
        std::streamoff            m_PlaybackDataStart = 0;
        uint64_t                  m_PlaybackStride = 0;
        uint64_t                  m_PlaybackFrameCount = 0;
        uint64_t                  m_PlaybackFrame = 0;         // next record to apply
        uint32_t                  m_PlaybackEntityCount = 0;
        double                    m_PlaybackDuration = 0.0;
        std::string               m_PlaybackSourceName;        // take's origin path name
        std::vector<std::wstring> m_PlaybackEntityNames;
        std::vector<Entity*>      m_PlaybackEntities;          // bound scene entities, table order
        std::vector<Mat4>         m_PlaybackApplied;           // last applied transforms (PrevTransform source)
        std::vector<Mat4>         m_PlaybackPending;           // current record's transforms
        std::vector<char>         m_PlaybackRecord;

        // Pose sequence recording
        bool                      m_RecordArmed = false;
        bool                      m_Recording = false;
        uint64_t                  m_FramesWritten = 0;
        uint32_t                  m_SessionEntityCount = 0;
        std::vector<std::wstring> m_SessionEntityNames;
        std::vector<char>         m_RecordScratch;
        std::ofstream             m_RecordStream;
        std::wstring              m_RecordFilePath;
        std::string               m_RecordFileTag;
        Vec4                      m_LastPos = Vec4(0.f, 0.f, 0.f, 0.f);
        Vec4                      m_LastLook = Vec4(0.f, 1.f, 0.f, 0.f);
        class CameraComponent*    m_pLastCamera = nullptr;
    };

} // namespace cauldron
