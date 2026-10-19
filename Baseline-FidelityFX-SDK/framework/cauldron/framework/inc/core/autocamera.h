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

namespace cauldron
{
    class CameraComponent;

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

    private:
        AutoCameraController();
        AutoCameraController(const AutoCameraController&) = delete;
        AutoCameraController& operator=(const AutoCameraController&) = delete;

        void LoadPaths();
        void RestartLap();
        bool Evaluate(double t, Vec4& pos, Vec4& look) const;

        std::vector<AutoCamPath> m_Paths;
        int                      m_ActivePath = -1;
        bool                     m_Playing = false;
        double                   m_PathTime = 0.0;
        uint32_t                 m_Lap = 0;
        const void*              m_pLastScene = nullptr;
    };

} // namespace cauldron
