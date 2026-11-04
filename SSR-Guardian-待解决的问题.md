# SSR-Guardian — 待解决的问题

> 更新：2026-11-02
> 用途：上下文交接。记录当前 Reference C / 静态采集 / Label / On-policy 数据流中尚未闭合的问题、已定决策和下一步顺序。
> 相关文档：`SSR-Guardian-网络与数据规格.md`、`SSR-Guardian代码改造说明.md`、`SSR-Guardian技术路线.md`

---

## 0. 当前状态

### 0.1 Reference C 已实现

- 全屏全局 RT 参考 pass，最终输出走：
  ```text
  场景光照 + RT 反射
    -> apply_reflections（BRDF/Fresnel/reflection_factor）
    -> Translucency
    -> ToneMapping
    -> SwapChainProxy
    -> PNG
  ```
- 当前代码（`d17c158` 之后）：
  - 最终输出强制 `DISPLAYMODE_LDR`，`SwapChainProxy` 为 RGBA8；
  - **2x2 SSAA**：reference 内部按 2 倍分辨率渲染，合成时 box downsample；
  - 子像素 jitter + mirror 微小 alpha，用于 anti-alias；
  - 256 子帧累积；1920×1080；输出 PNG。
- 已有旧数据：
  - `G:\SSR-Guardian\capture_beauty_1080p_256`：**4949 帧，1x（2x2 SSAA 之前）**，1080p/256，PNG，约 22.57 GiB。
  - 这批旧数据可以作为 **1x reference** 使用；当前代码的新 reference（2x2 SSAA）正在另一台机器上跑。
- 需要在 reference manifest 中记录：commit hash、`supersampling`、`exposure`、`tonemap mode`、`subframes`、分辨率、posebin hash。

### 0.2 已确认的关键结论

- **HC 不是静态场景特征**，而是当前路由策略的状态。启发式分类器的 HC 与 Guardian 的 HC 分布不同，不能直接混用。
- **A/E 是网络输出**，训练前没有最终形态。
- 因此数据要分成两层：
  - **静态数据（routing-independent）**：Reference、Pure SW SSR、G-Buffer/HiZ/MV、F/v、Label。
  - **On-policy 数据（routing-dependent）**：Guardian HC、A/E、实际 mixed SSR、实际路由。
- v0 建议先不接 HC/A/E，只用 F/v/MV 训练静态误差预测；v1 再用 on-policy HC 做 DAgger/微调。

---

## 1. P0：必须先对齐的问题

### 1.1 Pure SSR 与 Reference 的空间定义

- Reference 是“场景光照 + RT 反射 × apply_reflections + tonemap”的最终 SDR 图。
- Pure SSR 必须是**同一条最终显示管线**，只把反射路径换成纯 SW：
  ```text
  场景光照 + pure SW SSR 反射
    -> apply_reflections
    -> Translucency
    -> 同一个 ToneMapping / exposure
    -> 同一个输出分辨率 / AA
    -> SDR 图
  ```
- **不能**拿 raw reflection buffer 直接和 reference PNG 比，否则直接光照、tonemap、合成方式都会进入差值。
- 待定：Pure SSR 是否同时保留 raw radiance / denoised reflection 作为分析输出。

### 1.2 Exposure / ToneMapping 对齐

- Reference 在另一台机器跑，Pure SSR 在本机跑；自动曝光状态可能不同。
- 待定：
  - 数据生成阶段固定 exposure；
  - 或记录 reference exposure，并在 Pure SSR 侧强制复用；
  - 至少在 manifest 记录 `exposure`、`tonemap mode`。
- 若曝光差异是全局单调的，per-image 归一化可以缓解；非线性 tone curve 差异不能完全靠归一化。

### 1.3 AA 对齐

- 当前 reference 是 **2x2 SSAA**；Pure SSR 目前是 **1x**。
- 直接逐像素比会把 AA 边缘差异算成 SSR 误差，归一化不能消除局部高频差异。
- 待定方案：
  - **A. Pure SSR 也上 2x2 SSAA**（最公平，工作量最大）；
  - **B. Label 用 1x reference**（旧 4949 帧数据正好是 1x），2x2 SSAA PNG 仅做 QA；
  - **C. 两边用同一个 box filter downsample 到 1x**；SSR aliasing 会被算进 label，但至少 filter 一致。

### 1.4 Label 归一化、聚合与 Loss

- 目标已明确：**网络只需要排序“哪里错得最明显”，不需要预测绝对误差。**
- 推荐：
  - Per-image **全局单调归一化**：`e_tile / p99(e_all_tiles)` 或 percentile rank；
  - **不要**用 per-pixel 相对误差 `|L_ssr-L_ref|/(L_ref+eps)` 作为主 label，它会过度放大暗部、改变排序；
  - 可先做全局曝光匹配（median/percentile）再求差；
  - tile 聚合建议 `0.5*mean + 0.5*p95`，或直接用 `p95/max`（更贴近“最刺眼”）；
  - Loss 建议 normalized regression + pairwise/listwise ranking 的混合，而不是纯 MSE。

公式草案：

```text
L = dot(linear_rgb, [0.2126, 0.7152, 0.0722])
e_pixel = |L_ssr - L_ref|
e_tile = 0.5 * mean(e_pixel over 8x8) + 0.5 * p95(e_pixel over 8x8)
label_tile = e_tile / (p99(e_all_tiles) + eps)
# optional: label_tile = percentile_rank(e_tile)
```

- Fresnel/recency 加权：
  ```text
  w_fresnel = F0 + (1-F0) * (1 - NdotV)^5
  e_pixel *= w_fresnel
  ```
  需要 G-Buffer normal/ARM；v0 可以先用 `w=1`。
- 待定：
  - tile 聚合用 `mean+p95` 还是 `p95/max`；
  - 是否 v0 就加 Fresnel/recency；
  - ranking loss 的具体形式（pairwise hinge / listwise softmax）。

### 1.5 Reference 与 Pure SSR 的 provenance 对齐

- 另一台机器的 reference 必须和本机 Pure SSR 使用：
  - 同一 posebin；
  - 同一 `pose_record_index` 序列；
  - 同一分辨率、exposure、tonemap、AA factor；
  - 尽量同一代码 commit。
- 需要在 manifest 中记录：`commit`、`posebin hash`、`exposure`、`tonemap`、`supersampling`、`subframes`、`resolution`。
- 旧 4949 帧是 1x；新 reference 是 2x2 SSAA；**不要混用**。

---

## 2. 静态采集待实现

### 2.1 Pure SSR 静态采集

- 模式：`m_Mask &= ~HSR_FLAGS_USE_RAY_TRACING`，保持 hybrid ON，走 `ExecuteHybridDeferred`。
- 顺序回放，让 hit counter / variance / radiance 历史真实演化。
- 输出：
  - 最终 SDR composite（与 reference 同管线，用于 label）；
  - raw radiance `m_pRadianceA`（分析/raw label 可选）；
  - denoised `m_pOutput`（分析可选）。
- 每个 pacing 变体重跑；reference 跨变体共享。
- 待定：
  - stride 5 基准，还是 stride 1；
  - uniform 1–5 / random X–Y 的具体变体与 seed 列表。

### 2.2 F/v 统计预 pass

- 文档 §3.2/§3.3 的 16ch F + 6+6 v 尚未实现。
- 待定：
  - 在渲染器里实现 tile stats pre-pass；
  - 还是采原始 G-Buffer/HiZ/MV，离线计算。
- 需要 HiZ 解析特征：`offScreenRayRatio`、`traceStepBound`、`grazingCos.mean/var`。
- 存储：F/v/HC 约 2.1 MB/帧；raw G-Buffer/HiZ/MV 更大。

### 2.3 HiZ / MV / G-Buffer

- HiZ：存全部 mip，还是前 3–4 层？
- MV：全分辨率 `RG16_FLOAT`。
- G-Buffer：normal/ARM/depth 是否存，取决于：
  - 是否做 Fresnel 加权；
  - F/v 是否离线计算。
- 需要决定格式、分辨率、是否压缩。

### 2.4 A / E

- 已确认不能训练前采最终形态。
- v0：不接 A/E。
- v1：
  - A(T-1) 先用 Oracle/启发式 teacher；
  - E(T-1) 先用 label/0 占位；
  - 网络稳定后再切自回归。
- 训练时对 A/E 做 dropout/随机置零，保证冷启动稳定。

---

## 3. On-policy 数据（后续阶段）

- 用 v0 模型的路由跑真实渲染，采：
  - Guardian 自己的 HC（四字段）；
  - A(T-1) / E(T-1)；
  - 实际 mixed SSR 结果；
  - 实际路由掩码。
- 用 DAgger/on-policy 微调，把 HC/A/E 加入训练。
- 可选 teacher：
  - 启发式 HC；
  - pure SW HC；
  - Oracle Scheduler 的 A。
  - **只做 teacher/ablation，不当最终分布。**
- 冷启动：第一帧 HC/A/E 置零或启发式初始化；训练时随机 dropout。

---

## 4. 文档需要同步修改

### 4.1 `SSR-Guardian-网络与数据规格.md`

- §2.3/§3.4/§4.2：把 HC/A/E 标为 **on-policy**，不是 M2 静态采集。
- §3.6：明确 label 的 color space（SDR/linear）、AA factor、normalization 公式。
- §4.2：
  - M2 输出改为 Pure SSR 最终 SDR + raw/denoised + F/v/MV；
  - HC 只做诊断/teacher；
  - 明确 reference 是 1x 还是 2x2 SSAA。
- §5 实现状态：补充 F/v pre-pass、Pure SSR capture、on-policy rollout 的状态。

### 4.2 `SSR-Guardian代码改造说明.md`

- 补充 `d17c158` 的 LDR/2x2 SSAA/anti-alias 说明。
- 补充 Pure SSR / F/v 采集命令（实现后）。
- 明确旧 4949 帧是 1x、新 reference 是 2x2 SSAA。

---

## 5. 已定 vs 待定

| 项 | 状态 |
|---|---|
| Reference 输出 LDR/SDR PNG | ✅ 已定 |
| Reference 2x2 SSAA + 256 子帧 | ✅ 当前代码 |
| Reference 跨 pacing 变体共享 | ✅ 已定 |
| HC/A/E 不能训练前采最终形态 | ✅ 已定 |
| v0 不接 HC/A/E | ✅ 推荐 |
| Pure SSR 最终 SDR + 同管线 | ✅ 已定方向 |
| Label per-image 全局归一化 | ✅ 已定方向 |
| Label tile 聚合方式 | ◻ 待定（mean+p95 / p95 / max） |
| AA 对齐方案 | ◻ 待定（A/B/C） |
| Exposure 固定/复用 | ◻ 待定 |
| F/v 渲染器内 vs 离线 | ◻ 待定 |
| Pure SSR stride 5 vs 1 | ◻ 待定 |
| Fresnel/recency 是否 v0 上 | ◻ 待定 |
| ranking loss 形式 | ◻ 待定 |

---

## 6. 建议的下一步顺序

1. 确认 §1.1–§1.5 的 5 个 P0 决策；
2. 更新 `SSR-Guardian-网络与数据规格.md`（HC/A/E、label 空间、AA、normalization）；
3. 实现 Pure SSR 静态采集（同管线 SDR + raw/denoised）；
4. 实现 F/v tile stats pre-pass（或先离线）；
5. 用同一 pose 做小样本对齐验证：reference vs Pure SSR，检查 exposure/AA/label；
6. 生成 label，训练 v0（F/v/MV，不接 HC/A/E）；
7. 跑 v0 路由，采 on-policy HC/A/E，做 DAgger/微调；
8. 闭环评测误差-预算曲线。

---

## 7. 关键代码锚点

- `samples/hybridreflections/hybridreflectionsrendermodule.cpp`
  - `ParseReferenceCaptureEnv` / `InitReferenceCapture`
  - `ExecuteReferenceRayTracing` / `ExecuteReferenceAccumulate`
  - `CaptureBeautyCallback` / `ReadbackReferenceToDisk`
  - `Execute` 的 ReferenceMode 分支
- `samples/hybridreflections/shaders/reference_ray_tracing.hlsl`
  - 2x2 SSAA / subpixel jitter / GGX VNDF / ray origin/TMin
- `samples/hybridreflections/shaders/accumulate_reference.hlsl`
  - N 子帧累积与归一化
- `framework/cauldron/framework/inc/core/autocamera.h` / `src/core/autocamera.cpp`
  - `SetPlaybackHold` / `AdvancePlaybackFrame` / `IsPlaybackBound` / `GetPlaybackFrame`
- `samples/hybridreflections/config/hybridreflectionsconfig.json`
  - `Presentation` 1920×1080 / `DISPLAYMODE_LDR`
- 旧 reference 数据：`G:\SSR-Guardian\capture_beauty_1080p_256`（1x，4949 帧）
- 新 reference：另一台机器，2x2 SSAA，需记录 provenance

---

## 8. 一句话总结

**Reference 和 Pure SSR 必须在同一 SDR 空间、同一曝光、同一 AA 下比较；label 用 per-image 全局单调归一化 + tile 峰值聚合，服务“排序”而不是“绝对误差”；HC/A/E 是 on-policy 状态，等 v0 模型出来后再走自回归/DAgger。**
