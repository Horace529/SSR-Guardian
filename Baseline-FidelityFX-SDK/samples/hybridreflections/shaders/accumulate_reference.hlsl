// This file is part of the FidelityFX SDK.
//
// Copyright (C) 2024 Advanced Micro Devices, Inc.
//
// Permission is hereby granted, free of charge, to any person obtaining a copy
// of this software and associated documentation files(the "Software"), to deal
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

// Accumulates N reference samples (same pose, different blue-noise seeds) and
// writes the normalized mean on the last subframe.

Texture2D<float4>   g_reference : register(t0);
RWTexture2D<float4> g_accum     : register(u0);
RWTexture2D<float4> g_final     : register(u1);

cbuffer ReferenceAccumConstants : register(b0)
{
    uint g_subframe;
    uint g_sampleCount;
    uint g_pad0;
    uint g_pad1;
};

[numthreads(8, 8, 1)]
void main(uint3 tid : SV_DispatchThreadID)
{
    uint width, height;
    g_reference.GetDimensions(width, height);
    if (tid.x >= width || tid.y >= height)
        return;

    const float4 sampleRadiance = g_reference.Load(int3(tid.xy, 0));

    if (g_subframe == 0)
        g_accum[tid.xy] = sampleRadiance;
    else
        g_accum[tid.xy] = g_accum[tid.xy] + sampleRadiance;

    if (g_subframe + 1u >= g_sampleCount)
        g_final[tid.xy] = g_accum[tid.xy] / float(g_sampleCount);
}
