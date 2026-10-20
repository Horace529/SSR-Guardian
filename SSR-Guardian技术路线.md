# SSR-Guardian 技术路线

> 更新：2026-10-20 ｜ 基线：AMD FidelityFX SDK 1.1.4（`Baseline-FidelityFX-SDK/`，已含 VS2026 兼容补丁与 pose 录制器）
> 本文档是项目的单一事实源：记录已定的技术决策、代码证据锚点、里程碑状态与开放问题。

---

## 1. 项目定位

在 AMD FidelityFX SDK 的 Hybrid Reflections 管线（SSR + 光追混合反射）之上，构建一个**学习式鉴别器**：在像素/瓦片被追踪之前，预测"屏幕空间追踪（SSR）在这里会错多少"，据此把有限的光追（RT）预算分配给最需要的区域，超越现有启发式分类器的误差-预算曲线。

**成功判据（唯一记分牌）**：误差 vs RT 射线预算曲线，四条线同图对比——
全 SW ｜ AMD 启发式分类器 ｜ 本项目网络 ｜ 全 HW。
表述为"同等误差下 RT 射线开销降低 X%"。**不追求完美消除 SSR 错误**——那在信息论上不可能（屏幕空间缓冲不含屏幕外信息），实时全 RT 同样有错（1spp+降噪），完美只存在于离线路径追踪。

---

## 2. 基线管线解剖结论（HybridReflections）

### 2.1 路由是三层结构，结果写进同一份 radiance 缓冲

不存在"先渲染完整错误 SSR 图再修复"的两阶段流程。**每个像素只走一条路径**：

| 层面 | 机制 | 代码锚点 |
|---|---|---|
| 瓦片级（8×8） | 时域状态机：hit counter 命中/丢失统计 → 瓦片三态 `FULL_SW / FULL_HW / HALF_SW(棋盘格过渡)`；3×3 安全带取邻域最优；重投影跟随表面运动矢量 | `ffx_classifier_reflections.h:146-221` |
| 像素级 | SW/HW 射线列表互斥（`else if`），radiance 只有一个来源 | `:269-335` |
| 射线级（同帧） | SW 命中但置信度 < 0.9 → `do_hw` 升级为同帧光追着色 | `Intersect.hlsl:366` |

掩码 `m_Mask` 经 `dispatchParameters.mask` 贯通全链路（分类器路由、SW/HW pass 分发、do_hw 门控、indirect 分支）——**单一事实源，路由覆写 = 位操作**。

### 2.2 两个时域信号及其盲区（网络的输入特征，不是真值）

- **Hit counter**：每瓦片 32 位（新/旧命中 + 新/旧丢失），射线结果每 wave 一次原子加写入下一帧缓冲，分类器重投影读取 + 状态机决策。结构性失灵：读写错位一帧；重投影用表面运动矢量但 miss 由被反射内容决定（语义错位）；FULL_HW 期间统计冻结；"miss"定义混入置信度误判；8×8 聚合损耗。
- **Variance**：反射辐射度的时域估计方差。两个消费者：降噪器（滤波强度旋钮）+ 分类器 TVGT（已收敛像素省射线、高方差像素加射）。盲区：**方差低 ≠ 正确**——收敛到稳定错误值（如天空盒回退）时方差极小，系统认为"完美"。
- 两者衡量的都是"估计质量"，没有一个衡量"语义正确性"。**差异图谱（SSR vs RT 参考）恰好覆盖两者都看不见的部分**——这是本项目价值主张的数学本质。

### 2.3 残差分类（混合系统修不掉的错误，即研究对象）

1. 粗糙度 ≥ RT 阈值的像素永不走 HW（`ffx_classifier_reflections.h:276`），miss 即回退环境图
2. 非光泽表面直接 env 直通，颜色系统性失配（暗室内+亮天空开口场景尤甚——Hangar 正是）
3. quad 共享射线预算：未 trace 的信息降噪器变不出来
4. SW/HW 着色来源不一致 + 混合安全带边缘接缝（UI "Don't reshade" 控制的就是这个）
5. 降噪器时域拖影；低方差缓存住的稳定错误不被重试

### 2.4 感知框架：一致性 > 正确性

玩家判断反射的标准不是对真值的正确性，而是**与已建立视觉记忆的自洽**。关键对齐：**感知严重性 ≈ 时域可观测性**——"刚看过的东西从反射里消失"（最显眼的错误）恰好是 hit counter/历史里有痕迹的错误；从未入屏的内容既无痕迹也无人察觉。推论：损失函数应加"最近可见性"权重；评测需补无真值指标（时域稳定性、帧内空间连贯性）。

---

## 3. AI 鉴别器设计（已定决策）

### 3.1 职责分离：预言家 vs 调度器

- **网络只做预测**：输出每瓦片 SW **误差预测值**（回归，不是二分类）。这是反事实预测——部署后走 RT 的像素不执行 SW，该量只能从离线纯 SSR 渲染的标签学。
- **调度器一行贪心**：按预测误差排序、切在预算线上（每瓦片 RT 成本近似均匀，无需误差/成本比权衡）。阈值是**运行时旋钮**（对应今天的 `RTRoughnessThreshold`），换操作点不重训。
- 调度器无状态；时域惯性（滞后、粘滞）由网络输入端消化：**序列输入 + 上一帧分配图（自回归通道）**。

### 3.2 输入特征（决策时刻全部合法可得）

1. G-buffer（深度/法线/粗糙度/材质/运动矢量）——当前帧
2. HiZ 深度金字塔——当前帧
3. 时域历史：hit counter、variance、上一帧 radiance（来自上一帧，无鸡生蛋问题；demo 自己的状态机证明 hit counter 是最强单一先验）
4. 上一帧分配图（自回归状态通道）
5. 解析预计算特征（不让网络学投影几何）：每瓦片反射射线出屏比例、可追踪步数界、掠射角分布

信息论边界：单帧屏内信息原理上不能完全确定屏外失败；分解为解析可判（出屏射线）+ HiZ 走廊统计 + 时域痕迹三类后，不可约残差仅剩"从未入屏的反射体的第一帧"，且感知上恰好最不重要（见 2.4）。

### 3.3 训练策略：监督回归优先，RL 延后

- **标签**：每瓦片 SW 误差回归值 = 纯 SSR 结果 vs 同栈超采样 RT 参考，按 Fresnel/视觉权重加权 + 最近可见性加权。**不做二分类路由标签**（那会把误差幅度和预算策略焊死）。
- **否决在线 RL**：环境是渲染管线本身（每步一帧渲染，百万步需求不可行）+ 长时序稀疏奖励信用分配。若监督版出现系统性时域短视，再上**离线 RL 微调**（预计算候选未来）。
- 冻结 SW 配置（128 步预算、厚度启发式、置信度阈值 0.9）再产数据；SW 实现一变，标签作废。
- 训练/评测在数据生成阶段就分家：评测 take 冻结 + 打场景标签，永不进训练。

---

## 4. 数据工厂

### 4.1 架构公理

1. **姿态序列是唯一事实源（canonical asset）**：G-buffer/HiZ/SSR/参考全是它的确定性函数，可随时重算。姿态一次性求值存为数据，所有渲染 pass 只读这份数据，绝不各自重演动画。
2. **离线参考必须与纯 SSR 同一渲染栈**：标签 = 参考 − SSR，减法成立的前提是两张图共享除反射路径外的一切代码路径（天空/IBL/曝光/色调映射/BRDF）。外部离线渲染器 = 标签被着色差异毒化，禁止。
3. demo 的角色从"实时应用"降级为"无头渲染函数"：脱实时外壳（present/vsync/交互），不脱渲染内容。

### 4.2 录制端（✅ 已完成，commit 6d4d96f）

- `AutoCameraController`（基线已有）+ 5 条重型轨迹 JSON（`autocamera/*.json`，20Hz pos+look 关键帧，Catmull-Rom 运行时插值，循环播放，每圈重置动画时钟）
- **PoseRecorder**（本次新增，与镜头控制器同文件同 UI）：
  - 语义：**先武装后播放才录**（UI 复选框 / F9 / `CAULDRON_AUTO_RECORD=1`），`CAULDRON_AUTO_PLAY=<索引或名子串>` 自动开播
  - 每圈 lap 一个 `.posebin` 落 `autocamera/recordings/`；600 帧周期 flush，崩溃只损失尾帧
  - 格式：首行 JSON 头（实体名字表 + 布局说明）+ 定长二进制记录：`frame u64 | lap u32 | anim_clock f64 | delta_time f64 | path_time f64 | cam_pos f32[3] | cam_look f32[3] | cam_inv_view f32[16] | count u32 | 各动画实体世界矩阵 f32[16]×N`（行主序列出，列主序存储 `m[r][c]=getElem(col=c,row=r)`）
  - 钩子位置：`framework.cpp` 组件更新循环之后——相机与动画实体变换对该渲染帧已终局
- 实测验证：2756 条完整记录 @ ~201fps 捕获，时钟零回退、帧号零跳变；Hangar 有 **50 个动画节点**（3 无人机 + 闸门/舱门/飞船/液压臂）——动态反射内容剧本天然自备
- 消费端契约（M1 铁律）：离线 pass 直接 `SetTransform` 覆写 + `SetPrevTransform` = 序列前帧（运动矢量来源），**跳过动画求值**；PrevTransform 错误 = 运动矢量静默失真 = 最高优先级炸弹

### 4.3 离线渲染器（当前阶段，M0 ✅ / M1-M4 待做）

| Pass | 路由 | 帧推进 | 导出 |
|---|---|---|---|
| SSR 采集 | 纯 SW = `m_Mask &= ~HSR_FLAGS_USE_RAY_TRACING`（保持 hybrid ON 走 `ExecuteHybridDeferred`）；有现成先例（无 RT1.1 设备正是此配置） | **顺序回放全部帧**，hit counter/variance/radiance 历史随序列演化（与部署一致） | 原始 radiance（降噪前）、降噪后反射、G-buffer 特征、HiZ、hit counter |
| 参考 | 纯 HW = 现成 UI-off 状态（`ExecuteHybridDeferred` 跳过）+ `RTRoughnessThreshold=1.0`；每输出帧冻结姿态跑 N 子帧累积平均（蓝噪声随帧号独立） | 姿态冻结的子帧累积 | 累积参考图 |

**M0 验证结论（2026-10-20 全部通过）**：
1. Hangar gltf 无 skins 段，84 通道全为节点 TRS → 刚体覆写方案成立，无需关节矩阵
2. do_hw 升级被 `USE_RAY_TRACING` 掩码位硬门控（`Intersect.hlsl:366`），清除后原理上不可能触发；该配置有无 RT1.1 设备先例
3. Vsync 开关在 `cauldronconfig.json "Vsync"` + `swapchain.cpp:43`；主循环纯 PeekMessage 泵无节流，关 vsync 即自由帧率；`m_DeltaTime` 真实计时无害（位姿全由 posebin 覆写）
4. ⚠️ 计划修正：UI 开关 OFF 的真实语义是**纯 HW**（跳过的是 SW pass），不是纯 SW——两 pass 实现路径对调如上表

**M3 新增检查项**：hybrid OFF 时 `USE_SCREEN_SPACE` 位未同步清除，分类器仍填 SW 列表但无人消费——需验证所有 needs_ray 像素都进了 HW 列表，否则显式处理兜底。

### 4.4 预算与存储

- 25s 轨迹 @200fps ≈ 5000 记录帧 → **时间下采样每 5 帧取 1**（dt 存于记录，网络可见）→ ~1000 输出帧/take
- 参考累积 64 样本 + 半分辨率 ≈ 10 分钟/take；SSR pass ≈ 5 分钟/take
- 存储 ~90MB/输出帧 @1080p → 试点先 1 take × 200 帧（≈18GB）跑通全链路，通道裁剪（深度/法线/粗糙度打包、HiZ 存 3~4 层、radiance f16）后再量产
- 抽帧时域噪声（1~3 帧 stride）在数据组装层做，渲染器不管

---

## 5. 场景与素材清单

`Baseline-FidelityFX-SDK/media/`：9 场景 + 3 资源库。主力 **Hangar**（大平面地板 + 50 动画节点 + 极端光比）；备选 SponzaNew（露天中庭，天空盒回退失真）、Brutalism（自带 SSSR 机位）、BistroInterior（密集镜面物件）。`IBL/` 有户外全景图可供自定义水面场景使用。当前 demo 配置已指向 Hangar（`hybridreflectionsconfig.json`，相机 `Hangar_Camera_000`）。

---

## 6. 环境与构建备忘

- 本机仅 VS2026（`D:\MicrosoftIDES\VisualStudio`），CMake 用其自带的 4.3.1（PATH 无 cmake）；.slnx 新格式
- VS2026 兼容补丁（已进 12eb658）：`<experimental/filesystem>`→`<filesystem>`（10 文件）、`using namespace std::experimental`→`std`（16 文件）、`common.cmake` C++ 标准 11→17
- AMD 分发二进制已入 git 跟踪（335bb46 白名单）：`sdk/libs/pix/bin/`、`framework/.../libs/{agilitysdk,dxc,pix}/bin/` 共 19 个文件；root `bin/`、`build/` 保持忽略
- 构建：`sdk/build` 三配置 → 根 `build/` 生成 → `cmake --build --config ReleaseDX12 --target FFX_HYBRIDREFLECTIONS`；产物 `bin/FFX_HYBRIDREFLECTIONS_DX12.exe`，工作目录 `bin/`
- Git Bash 沙箱坑：管道输出被转 CRLF，文件列表过管道喂 sed/xargs 带 `\r` 报"can't read"，用 `find -exec` 替代；MSBuild 参数用 `-p:` 不用 `/p:`（路径转换）

---

## 7. 里程碑状态

| 里程碑 | 内容 | 状态 |
|---|---|---|
| 录制端 | AutoCamera + PoseRecorder + 端到端验证 | ✅ 6d4d96f |
| M0 | 蒙皮/掩码门控/vsync 三项验证 | ✅ 2026-10-20 |
| M1 | 姿态回放（posebin → 覆写 + 运动矢量正确性可视化） | ⬜ 下一步 |
| M2 | SSR 采集 pass（顺序回放 + dump 全通道 + 差异图谱可算） | ⬜ |
| M3 | 参考采集 pass（FULL_HW + 累积 + 收敛性检验 + HW 列表完整性检查） | ⬜ |
| M4 | 批处理编排 + manifest | ⬜ |
| 之后 | 鉴别器训练 → 误差-预算曲线评测 → （视情况）离线 RL 微调 | ⬜ |
