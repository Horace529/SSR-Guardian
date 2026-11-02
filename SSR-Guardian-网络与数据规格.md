# SSR-Guardian — 网络任务、数据规格与数据流规格

> 更新：2026-10-21 ｜ 状态：录制端/回放/步长回放已实现（`6d4d96f`/`9043784`），网络本体与统计预 pass 为本规格定义的待实现项
> 姊妹文档：`SSR-Guardian技术路线.md`（决策与依据）、`SSR-Guardian代码改造说明.md`（实现记录）
> 本文是**输入/输出/数据格式/数据流**的单一事实源；与上述文档冲突时以本文为准（数据规格部分）

---

## 1. 基线介绍（HybridReflections 管线解剖）

### 1.1 三层路由结构（Guardian 要接管的第一层）

基线 = AMD FidelityFX SDK 1.1.4 的 Hybrid Reflections sample：SSR（SW）+ 光追（HW）混合反射，**每个像素只走一条路径**，结果写入同一份 radiance 缓冲。

| 层面 | 机制 | 代码锚点 |
|---|---|---|
| 瓦片级（8×8） | 时域状态机：hit counter 命中/丢失统计 → 瓦片三态 `FULL_SW / FULL_HW / HALF_SW(棋盘格过渡)`；3×3 安全带取邻域最优；重投影跟随表面运动矢量 | `ffx_classifier_reflections.h:146-221` |
| 像素级 | SW/HW 射线列表互斥（`else if`） | `:269-335` |
| 射线级（同帧） | SW 命中但置信度 < 0.9 → `do_hw` 升级为同帧光追着色 | `Intersect.hlsl:366` |

路由掩码 `m_Mask` 贯通全链（分类器路由、pass 分发、门控、indirect 分支）——单一事实源，**Guardian 的输出就是覆写这个掩码**。

### 1.2 运动矢量系统（MV 的来源，特征与回放的地基）

- **像素级公式**（`rendermodules/gbuffer/shaders/gbufferps.hlsl:126`）：`MV = NDC(T-1) − NDC(T) + (PrevJitter − CurrJitter)`，再乘 `(0.5, −0.5)` 转 UV 空间（Y 向下）。存于 `GBufferMotionVectorRT`（RG16_FLOAT），当配置 `MotionVectorGeneration == "GBufferRenderModule"` 时生成。
- **两个位置**（`transformVS.hlsl:121-126`）：同一顶点分别经（当前世界矩阵→当前 ViewProj）与（**上一帧世界矩阵**→上一帧 ViewProj）变换；蒙皮网格走 `HAS_PREV_POSITION` 顶点属性（Hangar 为刚体动画，不走此分支）。
- **PrevTransform 双缓冲维护链**（`entity.h`）：加载时 `SetPrevTransform(=当前)`（`gltfloader.cpp:1512`）；动画实体每帧成对更新（`animationcomponent.cpp:185`）；**PosePlayback 回放覆写**（`autocamera.cpp:904`）——回放必须成对写 prev/cur，否则 MV 静默失真（M1 铁律，往返校验 5243/5243 帧字节级一致）。
- 相机侧：`CameraComponent::Update` 每帧开头把当前矩阵存入 `m_PrevViewProjectionMatrix`；CameraInformation 常量缓冲携带 `PrevViewProjectionMatrix` 与 `CurrJitter/PrevJitter`。
- **消费方**：HSR 去噪器时域重投影（`hybridreflectionsrendermodule.cpp:1673/:1736`）、deferred rays 参数、TAA、分类器 hit counter 重投影。

### 1.3 HitCounter（时域统计，Guardian 的特征之一）

- **格式**：每 8×8 瓦片 1×u32 = 4×u8 字段：新 SW 命中 / 旧 SW 命中 / 新 SW 丢失 / 旧 SW 丢失（各 clamp 255）。定义于 `ffx_classifier_reflections_common.h:63-74`。
- **写入**：SW 射线 pass（`Intersect.hlsl:396-402`）——每像素按结果打 `SW_HIT_FLAG(bit0)/SW_MISS_FLAG(bit16)`，VRS 拷贝逐像素累加，**每 wave 一次** `WaveActiveSum` + 首车道 `InterlockedAdd` 写入 `(frame+1)%2` 的双缓冲槽。门控 `HSR_FLAGS_USE_HIT_COUNTER`。
- **"miss" 语义**：SW 打中但置信度 < 阈值（本应升级 HW）——非"没打中"。
- **读取**：分类 pass 每瓦片单线程：随机像素表面 MV 重投影取上一帧瓦片统计 → 3×3 安全带取最优 → `IsSW(new, rnd)` / `IsSW(old, rnd_last)` 双帧判定 → 三态状态机。
- **归属**：全部由**前级管线**（FFX Classifier 组件 + sample 的 Intersect.hlsl）维护；Guardian 只是消费者。SW pass 写入独立于路由决策门控，Guardian 接管路由后统计管道照常工作。
- **已知盲区**（Guardian 的价值依据）：读写错位一帧；miss 混入置信度误判；FULL_HW 期间统计冻结；8×8 聚合损耗；variance 有"低方差 ≠ 正确"盲区。

### 1.4 残差分类（研究对象 = 混合系统修不掉的错误）

1. 粗糙度 ≥ RT 阈值的像素永不走 HW，miss 即回退环境图
2. 非光泽表面环境图直通的系统性色偏（暗室 + 亮天空开口场景尤甚——Hangar 正是）
3. quad 共享射线预算：未 trace 的信息降噪器变不出来
4. SW/HW 着色来源不一致 + 混合安全带边缘接缝
5. 降噪器时域拖影；低方差缓存住的稳定错误不被重试

感知框架：玩家判断反射的标准是与已建立视觉记忆的**自洽**；感知严重性 ≈ 时域可观测性——损失函数据此加"最近可见性"权重（见 §2.5 标签）。

---

## 2. 核心网络架构（鉴别器规格）

### 2.1 任务定义

**反事实每瓦片误差回归**：在被追踪之前，预测"这个 8×8 瓦片如果只走屏幕空间追踪（SW），会错多少"。调度器按预测误差排序、切在 RT 预算线上，把有限 HW 预算分配给最需要的瓦片。

- **反事实**：部署后走 HW 的瓦片不执行 SW，该量只能从离线"纯 SW 渲染 vs 同栈 RT 参考"的差异学出（见 §3.6）。
- **偏差而非方差**：SW 误差主体是系统性有偏错误（信息缺失/回退色偏/时域失效），非 MC 噪声——多打 SW 射线不可修复，必须换算法。这是与自适应采样文献（方差引导 spp 分配）的第一性区别。
- **职责分离**：网络只做预测（回归，非二分类）；调度器一行贪心、无状态；时域惯性由输入端消化（序列特征 + 自回归通道）。

### 2.2 部署形态：异步优先（已定决策）

- **主形态（异步）**：帧 T 的推理在帧 T-1 尾部执行（用 T-1 已就绪的数据），路由在帧 T 开头消费结果。与 hit counter"消费上一帧统计"的既有语义对齐，推理时间完全落在渲染闲时，不占关键路径。
- **降级安全**：推理未按时完成 → 直接复用 A(T-1)（粘滞分配），系统退化而非崩溃。
- **回退形态（同步）**：同一网络结构，输入槽位换为 F(T)（GBuffer pass 后帧内推理，<0.5ms 预算），保留为编译期开关。

### 2.3 输入张量（帧 T 推理，异步形态）

**34 通道瓦片张量 + 标量条件**，240×135（1080p 时；随渲染分辨率缩放），fp16，NHWC，**≈ 2.1 MB**：

| 槽位 | 内容 | 通道 | 大小 |
|---|---|---|---|
| F(T-1) | 上一帧瓦片统计（最新几何/材质/运动证据，见 §3.2）| 16 | 0.99 MB |
| v1 | 速度寄存器：F 时间通道在 (T-2→T-1) 区间的一阶差分 / dt | 6 | 0.37 MB |
| v2 | 速度寄存器：(T-3→T-2) 区间（滚动更新，见 §4.2）| 6 | 0.37 MB |
| HC(T) | hit counter 四字段（T-1 的 SW pass 写入、T 可读的那份）| 4 | 0.13 MB (u32) |
| A(T-1) | 上一帧分配图（自回归）| 1 | 0.06 MB |
| E(T-1) | 上一帧预测误差（可选自回归）| 1 | 0.06 MB |
| 标量 | dt 最近已知值、jitter（条件注入，不占空间通道）| — | — |

取舍记录：GBuffer/HiZ/材质的 T-2 副本全部裁撤（时间不变量无历史价值）；运动二阶信息由 v1/v2 承载；SW 命中趋势由 HC 新/旧字段免费承载；variance、上一帧 radiance 留作消融。

### 2.4 网络本体

- **形态**：瓦片分辨率（240×135）小型 CNN：每瓦片特征 + 3×3 瓦片邻域卷积（空间连贯）+ 标量条件注入（FiLM 或拼接）。**不使用** RNN/隐状态——时序全部显式化（特征环存），保证离线可训练、回放可复现。
- **规模预算**：5~10 层、32~64 通道，参数量 0.1M~1.5M，fp16 体积 0.2~3 MB（10MB 上限宽裕，停止准则是"留出 take 验证误差不再下降"，不是体积）。宁可用 5 层大通道，不用 30 层小通道（dispatch 开销）。
- **推理预算（硬 SLO）**：全链 < 0.5ms GPU 时间、常驻 < 20 MB（权重 + TensorRT 工作区 + 状态张量），低配卡闲时吸收；超预算启用错峰/降频（预测每 2 帧 + 粘滞分配），不改模型。

### 2.5 输出与消费

| 输出 | 规格 | 去向 |
|---|---|---|
| **E(T)** 每瓦片 SW 误差回归 | 240×135×1 fp16 ≈ 64.8 KB | 调度器贪心：按误差排序切 RT 预算线 → A(T)（1bit/瓦片）→ 路由掩码；A(T)/E(T) 写回常驻供下一帧自回归 |
| U(T) 不确定性（可选，v1 不开） | 240×135×1 fp16 | 调度置信加权 |

调度阈值是**运行时旋钮**（对应今天的 `RTRoughnessThreshold`），换操作点不重训；换档位（高/低配）用小验证集重标定阈值即可。

---

## 3. 数据格式

### 3.1 姿态序列 `.posebin`（✅ 已实现，录制端唯一事实源）

首行 JSON 头（`magic: CAULDRON_POSE_V1`、轨迹名、圈次、时长、实体名字表、布局说明），`\n` 结束；其后定长二进制记录流（小端）：

```
frame_index u64 | lap u32 | anim_clock f64 | delta_time f64 | path_time f64
cam_pos f32[3] | cam_look f32[3] | cam_inv_view f32[16]
entity_count u32 | 各动画实体世界矩阵 f32[16] × N（行主序列出，顺序 == 头内实体表）
```

- 记录字节步长 = `136 + N×64`；矩阵 `m[r][c] = Matrix4::getElem(col=c, row=r)`（世界空间），平移在线性索引 `[3],[7],[11]`
- 每 600 帧强制 flush（崩溃最多损失尾帧）；解析端容忍 < stride 的残尾
- 会话语义：录制会话存在当且仅当 `武装 && 播放中`；圈边界自动切文件（**一圈一个 take**）
- 产出目录：`autocamera/recordings/<轨迹名>_lap<N>_<时间戳>.posebin`，已入 git
- 无头控制：`CAULDRON_AUTO_RECORD=1`、`CAULDRON_AUTO_PLAY=<索引/名子串>`、`CAULDRON_AUTO_PLAYBACK=<路径>`、`CAULDRON_AUTO_PLAYBACK_STRIDE=<S>`、`CAULDRON_AUTO_PLAYBACK_STRIDE_RAND=<X:Y>`

### 3.2 瓦片统计张量 F（◻ 规格，16 通道）

统计预 pass 从管线现有缓冲算出（GBuffer depth/normal/roughness/metallic/MV + HiZ），每 8×8 瓦片一行，**分布性统计而非均值**（保双峰瓦片的尾部信号）：

| # | 通道 | # | 通道 |
|---|---|---|---|
| 1 | depth.mean（线性化） | 9 | mv.mag.max |
| 2 | depth.min | 10 | mv.dir.variance |
| 3 | depth.max | 11 | mv.largeFrac（大 MV 像素占比）|
| 4 | normal.spread | 12 | depth.discontFrac |
| 5 | roughness.mean | 13 | hiz.offScreenRayRatio |
| 6 | **roughness.max** | 14 | hiz.traceStepBound |
| 7 | metallic.mean | 15 | hiz.grazingCos.mean |
| 8 | mv.mag.mean | 16 | hiz.grazingCos.var |

`hiz.*` 为按瓦片中心射线从 HiZ 金字塔 march 的解析特征（与预 pass 同 kernel 产出）。

### 3.3 速度寄存器 v1 / v2（◻ 规格，各 6 通道）

F 的**时间敏感子集**的一阶差分除以 dt（= 物理速度，帧距不变）：

```
v = [ d(mv.mag.mean)/dt, d(mv.mag.max)/dt, d(mv.dir.variance)/dt,
      d(mv.largeFrac)/dt, d(depth.discontFrac)/dt, d(depth.mean)/dt ]
```

滚动更新（每帧预 pass）：`v2 ← v1; v1 ← (F(T-1) − F(T-2))[时间通道] / dt_last`。速度/加速度（v1−v2）即 T-2 帧的全部必要遗产；GBuffer 其余通道不保留历史副本。

### 3.4 HitCounter（前级管线既有）

见 §1.3。Guardian 侧只读；双缓冲由 SW pass 维护，计入系统常驻（2×0.13 MB）。

### 3.5 A / E 状态图（Guardian 自有常驻）

- `A`：分配图，每瓦片 1 bit（打包 ≈ 4 KB）或 u8；调度器产出，下一帧自回归输入
- `E`：预测误差图，240×135×1 fp16；下一帧可选自回归输入

### 3.6 标签（误差图谱）

```
label(tile) = weight · | SSR_pureSW(tile) − RT_reference(tile) |
weight = Fresnel(roughness, N·V) × 最近可见性(recency)
```

- **纯 SW 渲染**：`m_Mask &= ~HSR_FLAGS_USE_RAY_TRACING`，保持 hybrid ON 走 `ExecuteHybridDeferred`（无 RT1.1 设备先例）
- **RT 参考**：纯 HW（UI-off 状态 + `RTRoughnessThreshold=1.0`），每输出帧冻结姿态跑 N 子帧蓝噪声累积平均
- **同一渲染栈公理**：两张图共享除反射路径外的一切（天空/IBL/曝光/色调映射/BRDF），标签 = 纯反射路径差异；外部离线渲染器 = 标签被毒化，禁止
- 参考 pass 与 pacing 变体无关（姿态冻结、无历史），**跨变体共享**；SSR pass 每个 pacing 变体重跑

### 3.7 轨迹 JSON（✅ 已实现）

`autocamera/*.json`：`name/description/duration/loop/meta{线性/角速度统计}/keyframes[{t,pos[3],look[3]}×501]`，20Hz 关键帧，运行时 Catmull-Rom 插值。5 条轨迹均经逐采样无穿模验证（体素化内部自由空间 + 4 单位余量）。

### 3.8 存储预算

| 项 | 每帧 | 试点（200 帧）|
|---|---|---|
| 特征侧（F+HC+A+标签图）| ≈ 1.25 MB | ≈ 250 MB |
| 训练样本窗口（t-2,t-1,t 三帧视图）| 3.3 MB 视图（不重复存）| — |
| RT 参考图（跨 pacing 变体共享）| 另计 | — |

通道裁剪前不量产；深度/法线/粗糙度打包、HiZ 存 3~4 层、radiance f16。

---

## 4. 采样流向（数据流安排）

### 4.1 推理数据流（异步形态，帧 T-1 内为帧 T 算）

```
[GBuffer(T-1)/HiZ/MV] ──▶ ① 统计预pass ──写──▶ F环形槽[T%3]
                                          └──更新──▶ v2←v1; v1←(F差分)/dt
② 装配：[F(T-1) | v1 | v2 | HC | A(T-1) | E(T-1)] 34ch + 标量 ≈ 2.1 MB
        ▼
③ TensorRT 前向（瓦片分辨率 CNN）──▶ E(T) 240×135×1 fp16
        ▼
④ 调度器：E(T) + RT预算 → 贪心阈值 ──▶ A(T) 写回常驻
                                  └──▶ 路由掩码（帧 T 开始时消费）
```

- 每帧滚动写入量：F 1.0 MB + v 2×0.37（覆写）+ A/E 0.12 + HC（前级）0.13
- 安全降级：推理未就绪 → 复用 A(T-1)
- 同步回退形态：同结构，槽位换 F(T)，推理挪到帧内 GBuffer 之后（<0.5ms）

### 4.2 采集数据流（M2/M3，离线）

```
posebin（步长回放：均匀1~5 / 随机X~Y，固定种子）
   │
   ├─▶ M2 SSR采集pass（纯SW掩码，顺序回放，hit counter/variance/radiance
   │      历史随序列演化）──▶ 原始radiance + 降噪后反射 + F特征 + HC + MV
   │                          （每个 pacing 变体重跑一遍）
   └─▶ M3 参考pass（纯HW，RTRoughnessThreshold=1.0，每输出帧冻结姿态
          N子帧蓝噪声累积）──▶ 累积参考图（跨 pacing 变体共享，只跑一遍）
   │
   ▼
标签 = 参考 − SSR（按变体），+ Fresnel/最近可见性加权 → 训练集
```

- 帧推进：时间下采样每 5 帧取 1（200fps→输出帧距 25ms）为基准，抽帧噪声由**步长回放在渲染层实现**（2026-10-21 决策变更，不再走组装层）——MV 帧距 == 真实帧距，特征/标签物理自洽
- M3 检查项：hybrid OFF 时 `USE_SCREEN_SPACE` 位未同步清除，需验证所有 needs_ray 像素都进 HW 列表

### 4.3 训练数据流

- 样本 = 时序窗口 (t-2, t-1, t) 的特征 + t 帧标签；按帧落盘、按窗口取视图
- pacing 变体在数据集层面均匀/随机采样，dt 作为条件输入
- 训练/评测分家：评测 take 冻结 + 打场景标签，永不进训练

### 4.4 确定性链条（系统地基）

```
posebin 回放（5243/5243 帧字节级一致）
  → PrevTransform 链正确 → MV 逐位正确 → F/v 特征确定
  → 步长回放帧距真实 → SSR 历史真实 → 标签确定
```

任一环松动（典型错误：覆写变换不写 PrevTransform），特征与标签**静默**失真。

---

## 5. 实现状态对照

| 组件 | 状态 |
|---|---|
| 自动镜头 + 5 条轨迹 JSON + 生成脚本 | ✅ `12eb658` |
| PoseRecorder / PosePlayback / 往返校验 | ✅ `6d4d96f` / `9043784` |
| 回放步长系统（均匀/随机，无头环境变量） | ✅ 2026-10-21 |
| M0 验证（蒙皮/掩码门控/vsync） | ✅ 2026-10-20 |
| 统计预 pass（F/v 产出） | ◻ 本规格，待实现 |
| M2 SSR 采集 pass / M3 参考 pass / M4 批处理 | ◻ |
| 鉴别器训练 + 误差-预算曲线评测 | ◻ |
