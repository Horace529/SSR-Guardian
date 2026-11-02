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

// Offline reference pass: full-screen global RT reflection, opaque-only.
// One thread per pixel, one GGX VNDF sample per invocation. The host accumulates
// N invocations with the same pose and different blue-noise seeds.

#include "declarations.h"
#include "common_types.h"
#include "raytracinglightfunctions.hlsl"

SamplerState samBRDF : register(s0);
SamplerState samIrradianceCube : register(s1);
SamplerState samPrefilteredCube : register(s2);
SamplerComparisonState SamShadow : register(s3);
SamplerState g_samplers[] : DECLARE_SAMPLER(SAMPLER_BEGIN_SLOT);

Texture2D<float4>               g_gbuffer_depth : register(t0);
Texture2D<float4>               g_gbuffer_normal : register(t1);
RaytracingAccelerationStructure g_global : register(t2);
Texture2D<float4>               g_brdf_lut : register(t3);
TextureCube                     g_atmosphere_lut : register(t4);
TextureCube                     g_atmosphere_dif : register(t5);
Texture2D<float4>               g_random_number_image : register(t6);
Texture2D<float4>               g_gbuffer_arm : register(t7);
StructuredBuffer<Material_Info> g_material_info : DECLARE_SRV(RAYTRACING_INFO_MATERIAL);
StructuredBuffer<Instance_Info> g_instance_info : DECLARE_SRV(RAYTRACING_INFO_INSTANCE);
StructuredBuffer<uint>          g_surface_id : DECLARE_SRV(RAYTRACING_INFO_SURFACE_ID);
StructuredBuffer<Surface_Info>  g_surface_info : DECLARE_SRV(RAYTRACING_INFO_SURFACE);

Texture2D                       g_textures[200] : DECLARE_SRV(TEXTURE_BEGIN_SLOT);
Texture2D    ShadowMapTextures[MAX_SHADOW_MAP_TEXTURES_COUNT] : DECLARE_SRV(SHADOW_MAP_BEGIN_SLOT);

StructuredBuffer<uint>  g_index_buffer[MAX_BUFFER_COUNT] : DECLARE_SRV(INDEX_BUFFER_BEGIN_SLOT);
StructuredBuffer<float> g_vertex_buffer[] : DECLARE_SRV(VERTEX_BUFFER_BEGIN_SLOT);

RWTexture2D<float4> g_rw_reference : register(u0);

ConstantBuffer<FrameInfo> g_frame_info_cb[1] : register(b0);
cbuffer CBSceneInformation : register(b1)
{
    SceneInformation SceneInfo;
};
cbuffer CBLightInformation : register(b2)
{
    SceneLightingInformation LightInfo;
}

#define brdfTexture g_brdf_lut
#define irradianceCube g_atmosphere_dif
#define prefilteredCube g_atmosphere_lut
#define myPerFrame g_frame_info.perFrame
#define g_linear_sampler samPrefilteredCube

#include "common.hlsl"

float4 SampleBRDFTexture(float2 uv)
{
    return brdfTexture.SampleLevel(samBRDF, uv, 0);
}

float4 SampleIrradianceCube(float3 n)
{
    return irradianceCube.SampleLevel(samIrradianceCube, n, 0);
}

float4 SamplePrefilteredCube(float3 reflection, float lod)
{
    return prefilteredCube.SampleLevel(samPrefilteredCube, reflection, lod);
}

float2 Hash22(float2 p)
{
    p = frac(p * float2(123.34f, 456.21f));
    p += dot(p, p + 45.32f);
    return frac(float2(p.x * p.y, p.x + p.y));
}

// R2 low-discrepancy sequence in [0, 1)^2, indexed by subframe. Used for sub-pixel
// jitter so the accumulated samples cover the pixel footprint uniformly.
float2 R2Sequence(uint frameIndex)
{
    const float2 a = float2(0.7548776662466927f, 0.5698402909980532f);
    return frac(a * float(frameIndex + 1u));
}

// Sub-pixel offset in [-0.5, 0.5) pixels, de-correlated per pixel.
float2 SubpixelJitter(int2 pixel, uint frameIndex)
{
    const float2 rotation = Hash22(float2(pixel));
    return frac(R2Sequence(frameIndex) + rotation) - 0.5f;
}

// Same GGX VNDF sampling as Common.hlsl, but with a per-subframe decorrelation offset so
// that N > 256 subframes keeps producing new samples instead of repeating the 256-entry
// blue-noise sequence. Mirror surfaces are clamped to a tiny non-zero alpha so the
// subframes still integrate a small cone instead of repeating one fixed direction.
float3 SampleReflectionVectorSeeded(float3 view_direction, float3 normal, float roughness, int2 pixel, uint frameIndex)
{
    const float minAlpha = 0.0025f;
    float       alpha    = max(roughness, minAlpha);

    float3x3 tbn                = CreateTBN(normal);
    float3   view_direction_tbn = mul(-view_direction, tbn);
    float2   u                  = SampleRandomVector2DBaked(pixel);
    const float2 offset         = Hash22(float2(pixel) + float2(float(frameIndex) * 0.61803398875f,
                                                                float(frameIndex) * 0.75487766625f));
    u = frac(u + offset);

    float3 sampled_normal_tbn    = Sample_GGX_VNDF_Hemisphere(view_direction_tbn, alpha, u.x, u.y);
    float3 reflected_direction_tbn = reflect(-view_direction_tbn, sampled_normal_tbn);
    return mul(reflected_direction_tbn, transpose(tbn));
}

#define IBL_INDEX b3
#include "lightingcommon.h"

struct LocalBasis
{
    float2 uv;
    float2 uv_ddx;
    float2 uv_ddy;
    float3 normal;
    float4 tangent;
};

void GetPBRPixelInfoFromMaterial(out PBRPixelInfo pixelInfo, in LocalBasis local_basis, in Material_Info material)
{
    float3 normal  = normalize(local_basis.normal);
    float3 tangent = local_basis.tangent.xyz;
    if (material.normal_tex_id >= 0 && any(abs(tangent.xyz) > 0.0f))
    {
        tangent = normalize(tangent);
        float3 binormal = normalize(cross(normal, tangent) * local_basis.tangent.w);
        float3 normal_rgb = float3(0.0f, 0.0f, 1.0f);
        normal_rgb = g_textures[NonUniformResourceIndex(material.normal_tex_id)].SampleLevel(g_samplers[material.normal_tex_sampler_id], local_basis.uv, 2.0).rgb;
        normal_rgb.z = sqrt(saturate(1.0 - normal_rgb.r * normal_rgb.r - normal_rgb.g * normal_rgb.g));
        normal = normalize(normal_rgb.z * normal + (2.0f * normal_rgb.x - 1.0f) * tangent + (2.0f * normal_rgb.y - 1.0f) * binormal);
    }

    pixelInfo.pixelBaseColorAlpha = float4(material.albedo_factor_x, material.albedo_factor_y, material.albedo_factor_z, material.albedo_factor_w);
    if (material.albedo_tex_id >= 0)
        pixelInfo.pixelBaseColorAlpha *= g_textures[NonUniformResourceIndex(material.albedo_tex_id)].SampleLevel(g_samplers[material.albedo_tex_sampler_id], local_basis.uv, 2.0);

    float3 arm = float3(1.0f, material.arm_factor_y, material.arm_factor_z);
    if (material.arm_tex_id >= 0)
        arm *= g_textures[NonUniformResourceIndex(material.arm_tex_id)].SampleLevel(g_samplers[material.arm_tex_sampler_id], local_basis.uv, 2.0).xyz;

    float3 emission = (0.0f).xxx;
    if (material.emission_tex_id >= 0)
        emission = g_textures[NonUniformResourceIndex(material.emission_tex_id)].SampleLevel(g_samplers[material.emission_tex_sampler_id], local_basis.uv, 2.0).xyz;

    pixelInfo.pixelNormal              = float4(normal, 1.f);
    pixelInfo.pixelAoRoughnessMetallic = float3(arm.r, saturate(arm.g), arm.b);
}

[numthreads(8, 8, 1)]
void main(uint3 tid : SV_DispatchThreadID)
{
    uint width, height;
    g_rw_reference.GetDimensions(width, height);
    if (tid.x >= width || tid.y >= height)
        return;

    // Sub-pixel jitter per subframe: holding the pose means the G-buffer is re-rendered
    // identically, so shifting the reconstructed ray inside the pixel footprint is what
    // anti-aliases the reflected image over the accumulated samples.
    const float2 subpixel = SubpixelJitter(int2(tid.xy), g_frame_info.frame_index);
    float2 screen_uv = (float2(tid.xy) + float2(0.5f, 0.5f) + subpixel) / float2(width, height);

    // The reference target is 2x supersampled; the G-buffer is at output resolution.
    // Take depth from the nearest G-buffer texel and interpolate normal/roughness at the
    // sub-pixel position so the 2x2 samples actually see different surface bases.
    const int2 base_pixel = int2(tid.xy) >> 1;
    float  z         = g_gbuffer_depth.Load(int3(base_pixel, 0)).x;

    if (FFX_DNSR_Reflections_IsBackground(z))
    {
        g_rw_reference[tid.xy] = float4(0.0f, 0.0f, 0.0f, 0.0f);
        return;
    }

    float3 screen_uv_space_ray_origin = float3(screen_uv, z);
    float3 view_space_hit             = ScreenSpaceToViewSpace(screen_uv_space_ray_origin, g_inv_proj);
    float3 world_space_hit            = mul(g_inv_view, float4(view_space_hit, 1.0f)).xyz;
    float3 camera_position            = ViewSpaceToWorldSpace(float4(0.0f, 0.0f, 0.0f, 1.0f), g_inv_view);
    float3 world_direction            = normalize(world_space_hit - camera_position);

    float3 world_normal = normalize(g_gbuffer_normal.SampleLevel(samBRDF, screen_uv, 0).xyz * 2.0f - (1.0f).xxx);
    float4 arm          = g_gbuffer_arm.SampleLevel(samBRDF, screen_uv, 0);
    float  roughness    = saturate(arm.g);   // G-Buffer stores linear roughness squared

    float3 reflected_direction = SampleReflectionVectorSeeded(world_direction, world_normal, roughness, int2(tid.xy), g_frame_info.frame_index);

    RayQuery<RAY_FLAG_CULL_NON_OPAQUE | RAY_FLAG_SKIP_PROCEDURAL_PRIMITIVES> query;
    RayDesc ray;
    const float max_t = g_frame_info.max_raytraced_distance * exp(-roughness * g_frame_info.ray_length_exp_factor);
    ray.Origin    = world_space_hit + world_normal * 3.0e-3f * length(view_space_hit);
    ray.Direction = reflected_direction;
    ray.TMin      = 3.0e-3f;
    ray.TMax      = max_t;
    query.TraceRayInline(g_global, 0, 0xff, ray);
    query.Proceed();

    float3 radiance = (0.0f).xxx;
    float  valid    = 1.0f;   // alpha is a coverage mask: 1 for non-background, 0 for background

    if (query.CommittedStatus() == COMMITTED_TRIANGLE_HIT)
    {
        const uint          instance_id = query.CommittedInstanceID();
        const uint          geometry_id = query.CommittedGeometryIndex();
        const uint          triangle_id = query.CommittedPrimitiveIndex();
        const float2        bary        = query.CommittedTriangleBarycentrics();
        const float3x3      obj_to_world = float3x3(
            query.CommittedObjectToWorld3x4()[0].xyz,
            query.CommittedObjectToWorld3x4()[1].xyz,
            query.CommittedObjectToWorld3x4()[2].xyz);

        Instance_Info instance = g_instance_info.Load(instance_id);
        const uint    surface_id = g_surface_id.Load(instance.surface_id_table_offset + geometry_id);
        Surface_Info  surface    = g_surface_info.Load(surface_id);
        Material_Info material   = g_material_info.Load(surface.material_id);

        uint3 face3;
        if (surface.index_type == SURFACE_INFO_INDEX_TYPE_U16)
            FFX_Fetch_Face_Indices_U16(face3, surface.index_offset, triangle_id);
        else
            FFX_Fetch_Face_Indices_U32(face3, surface.index_offset, triangle_id);

        float3 normal;
        float4 tangent;
        float2 uv;
        FFX_Fetch_Local_Basis(surface, face3, bary, uv, normal, tangent);

        normal      = normalize(rotate(obj_to_world, normal));
        tangent.xyz = normalize(rotate(obj_to_world, tangent.xyz));

        LocalBasis local_basis;
        local_basis.uv     = uv;
        local_basis.uv_ddx = 1.0e-3f;
        local_basis.uv_ddy = 1.0e-3f;
        local_basis.normal = normal;
        local_basis.tangent = tangent;

        PBRPixelInfo pixelInfo;
        GetPBRPixelInfoFromMaterial(pixelInfo, local_basis, material);
        pixelInfo.pixelWorldPos    = float4(ray.Origin + ray.Direction * query.CommittedRayT(), 1.0f);
        pixelInfo.pixelCoordinates = float4(0.0f, 0.0f, 0.0f, 0.0f);

        radiance = PBRLighting(pixelInfo, ShadowMapTextures);

        float4 emission = float4(material.emission_factor_x, material.emission_factor_y, material.emission_factor_z, 1.0f);
        if (material.emission_tex_id >= 0)
            emission *= g_textures[NonUniformResourceIndex(material.emission_tex_id)].SampleLevel(g_samplers[material.emission_tex_sampler_id], uv, 0).xyzw;

        radiance += emission.xyz * g_emissiveFactor;
        valid = 1.0f;
    }
    else
    {
        radiance = FFX_GetEnvironmentSample(world_space_hit, reflected_direction, roughness);
    }

    g_rw_reference[tid.xy] = float4(radiance, valid);
}
