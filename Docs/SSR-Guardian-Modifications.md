# SSR-Guardian — Baseline 改造总记录

> 基线：AMD FidelityFX SDK 1.1.4（上游 git `c6efa6b`），HybridReflections sample，场景 `media/Hangar/Hangar_1105.gltf`
> 环境：Visual Studio 2026 Community 18.10.1（v145 工具集）+ VS 自带 CMake 4.3.1，Windows x64
> 仓库：`D:\Projects\SSR-Guardian`（分支 main）；基线 vendored 于 commit `12eb658`
> 记录日期：2026-10-19 初稿 ｜ 2026-10-20 增补录制/回放 ｜ 2026-10-21 合并整理
> 姊妹文档：`SSR-Guardian技术路线.md`（技术决策与依据）；本文只记录"改了什么、在哪、怎么用"

本文档合并整理对 `Baseline-FidelityFX-SDK` 的全部改造：构建系统适配、AMD 二进制入库修复、场景几何解析工具、JSON 驱动的自动运镜系统（含 5 条无穿模验证路径）、姿态序列录制器与回放器。

---

## 0. 变更总览

| 分类 | 内容 | 引入位置 |
|---|---|---|
| 构建预设 | 根目录 `CMakePresets.json`（samples-dx12 等）+ `sdk/CMakePresets.json`（后端） | `12eb658` |
| 构建兼容层 | VS2026 工具集适配（filesystem/C++17） | `12eb658` 内 |
| 二进制资产 | AMD 分发运行库纳入 git 跟踪（修同步丢文件问题） | `335bb46` |
| 几何解析 | glTF 场景图解析 + 体素化内部自由空间分析脚本（路径设计依据） | `12eb658` |
| 自动镜头系统 | AutoCameraController + 5 条轨迹 + 生成脚本 | `12eb658` 内 |
| 姿态序列录制器 | PoseRecorder（.posebin 数据采集） | `6d4d96f` |
| 姿态回放器 | PosePlayback（.posebin 确定性重放，M1） | `9043784` |
| 示例配置 | HybridReflections 指向 Hangar 场景 | `12eb658` 内 |
| 文档 | 技术路线 + 本文档 | `ecf4f79` / 本次 |

---

## 1. 构建系统适配（VS2026）

基线从另一台机器打包而来（路径一致，同为 VS2026 生成），本机适配 = CMake 预设固化 + 源码兼容层修改。

### 1.1 CMake 预设与常用命令

| 文件 | 内容 |
|---|---|
| `CMakePresets.json` | 根目录预设：`samples-dx12`（生成器 `Visual Studio 18 2026`、x64、`BUILD_TYPE=SAMPLES_DX12`、`FFX_HYBRIDREFLECTIONS=ON`、`RUNTIME_SHADER_RECOMPILE=0`）及 DebugDX12 / ReleaseDX12 / RelWithDebInfoDX12 三个构建预设；另附 `samples-vk`（本机未装 Vulkan SDK，暂不可用） |
| `sdk/CMakePresets.json` | SDK 后端预设 `sdk-dx12-x64`（`FFX_API_BACKEND=DX12_X64`、`FFX_AUTO_COMPILE_SHADERS=1`、组件 CLASSIFIER+DENOISER+SPD），仅当修改 `sdk/` 源码后需要重建后端库 |

```bash
# 生成解决方案（输出 build/*.slnx，启动项目 FFX_HYBRIDREFLECTIONS）
cmake --preset samples-dx12
# 编译（输出 bin/FFX_HYBRIDREFLECTIONS_DX12D.exe）
cmake --build build --config DebugDX12 --target FFX_HYBRIDREFLECTIONS
```

预编译后端库在 `sdk/bin/ffx_sdk/`，**只包含 CLASSIFIER+DENOISER+SPD 三个组件**（HybridReflections 所需）。要构建其他 sample（FSR、CACAO 等），需先用 sdk 预设重建后端并补上对应 `FFX_*` 组件。

### 1.2 VS2026 兼容层（filesystem / C++17）

**背景**：SDK 1.1.4 官方支持到 VS2019，代码使用已废弃的 `<experimental/filesystem>`。VS2026 新工具集移除了该头文件，且项目原设定 `CMAKE_CXX_STANDARD 11`（实际按 C++14 编译）下 `<filesystem>` 头只提供 experimental 命名空间，形成双重不兼容。

**修改一：filesystem 现代化（10 个文件）**

`#include <experimental/filesystem>` → `<filesystem>`；`std::experimental::filesystem` → `std::filesystem`：

```
framework/cauldron/framework/inc/core/framework.h
framework/cauldron/framework/inc/core/contentmanager.h
framework/cauldron/framework/inc/core/loaders/textureloader.h
framework/cauldron/framework/inc/render/swapchain.h
framework/cauldron/framework/inc/render/particle.h
framework/cauldron/framework/src/render/backend_shader_reloader/backend_shader_reloader_common.cpp
framework/cauldron/framework/src/render/texture.cpp
framework/cauldron/framework/src/render/dx12/swapchain_dx12.h
framework/cauldron/framework/src/render/win/shaderbuilder_win.cpp
framework/cauldron/framework/src/render/vk/swapchain_vk.h
```

**修改二：using 声明（16 个文件）**

`using namespace std::experimental;` → `using namespace std;`（使 `filesystem::path` 等非限定名解析到 std 版本）：

```
framework/cauldron/framework/src/core/contentmanager.cpp
framework/cauldron/framework/src/core/framework.cpp
framework/cauldron/framework/src/core/scene.cpp
framework/cauldron/framework/src/core/loaders/gltfloader.cpp
framework/cauldron/framework/src/core/loaders/textureloader.cpp
framework/cauldron/framework/src/core/win/framework_win.cpp
framework/cauldron/framework/src/render/texture.cpp
framework/cauldron/framework/src/render/dx12/swapchain_dx12.cpp
framework/cauldron/framework/src/render/vk/swapchain_vk.cpp
framework/cauldron/framework/src/render/win/shaderbuilder_win.cpp
framework/rendermodules/lighting/lightingrendermodule.cpp
framework/rendermodules/skydome/skydomerendermodule.cpp
samples/brixelizergi/brixelizergirendermodule.cpp
samples/lpm/lpmrendermodule.cpp
samples/parallelsort/parallelsortrendermodule.cpp
samples/spd/spdrendermodule.cpp
```

**修改三：C++ 标准**

`common.cmake:27`：`CMAKE_CXX_STANDARD` 11 → **17**（根 CMakeLists 经 `include(common)` 全局生效；framework 与 samples 同时受控。不提标准，新版 MSVC 的 `<filesystem>` 头在 C++14 模式下不提供 `std::filesystem`）。

**刻意不动**：`framework/cauldron/framework/libs/json/json.h`——自带 `__has_include(<filesystem>)` 特性检测守卫，无需修改。

---

## 2. AMD 分发二进制入库（修 git 同步丢文件，`335bb46`）

**背景**：`.gitignore`（VisualStudio 模板）的全局 `[Bb]in/`、`x64/` 规则误伤了 AMD 随源码分发的运行库（路径恰好含 bin/x64 字样），git 同步即丢，CMake generate 报 `Cannot find source file`。

**修改**：`.gitignore` 末尾追加白名单（先 `!` 目录再 `!` 内容，`x64/`、`arm64/` 子目录单独反掉），并把以下 19 个文件纳入跟踪：

```
sdk/libs/pix/bin/{WinPixGpuCapturer.dll, arm64/*, x64/WinPixEventRuntime.dll+lib}
framework/cauldron/framework/libs/agilitysdk/bin/x64/{D3D12Core.dll+pdb, d3d12SDKLayers.dll+pdb, d3dconfig.exe+pdb}
framework/cauldron/framework/libs/dxc/bin/x64/{dxc.exe, dxcompiler.dll, dxil.dll}
framework/cauldron/framework/libs/pix/bin/{WinPixGpuCapturer.dll, arm64/*, x64/WinPixEventRuntime.dll+lib}
```

root `bin/`、`build/`、`sdk/build/` 仍为忽略（纯构建产物）。今后同步/clone 不再缺文件。

---

## 3. 场景几何解析工具

两套 Python 脚本（依赖 numpy/scipy），**不改引擎代码**，为路径设计提供几何依据。

### 3.1 glTF 场景图解析

脚本：`autocamera/analyze_geometry.py`（初版 `%TEMP%\ffx_gltf_analyze.py`）
解析 `.gltf` JSON 元数据（节点 TRS 变换 × POSITION accessor min/max 包围盒），不解压几何 buffer。

Hangar 场景结论：494 节点 / 396 网格 / 87 材质，218 个根节点（典型 DCC 扁平导出）；几乎无 glTF 实例化（仅 2 个网格被共享）；5 个相机（启动相机 `Hangar_Camera_000` 位于 (-15.3, 18.6, -95.8)，入口内侧）；1 段 84 通道动画（3 架无人机 + 舱口机构）；"ShipBody" 是包裹整个机库的外壳（子树 400 节点），并非独立飞船实体——SSR 失效区 = 外壳/顶棚遮挡方向 + 后廊低矮区 + 入口开放区。

### 3.2 体素几何解析（穿模验证的基础）

脚本：`autocamera/analyze_geometry.py`，产物 `%TEMP%\ffx_hangar_voxel.npz`

流程：读取 56MB `Hangar_1105.bin` → 131 万三角形变换到世界空间 → 重心采样栅格化进 2 单位体素格（ROI x∈[-160,160], y∈[-20,170], z∈[-190,400]）→ 膨胀 4 单位安全余量 → 从启动相机位置洪泛出**内部自由空间**连通域。

关键测量结果（路径设计依据）：

| 区域 | 结论 |
|---|---|
| 机库主厅 | 内部自由范围 x∈[-142,124]，z∈[-136,200]，净高到 y≈164 |
| 后廊（z>200） | 通道收窄至 x∈[-36,18]，**天花板降至 y≈14** |
| 立柱 ×2 | (x≈62, z≈120) 下方到 y28 实心；(x≈66~74, z≈0) 分段阻挡 |
| 中央检修平台 | 顶部结构 + 栏杆使周边 y6~16 分段阻挡；西北角有 y10~16 悬空结构带 |
| 入口区道具群 | 道具顶 10~16 不等；入口两侧 y54~82 有高结构（限制高空路径） |

> 教训：v1 硬编码路径的侧翼航点在 x=±232~248（墙外 100+ 单位），导致 19 秒后镜头一直在机库外飞行——这就是必须做体素解析的原因。

---

## 4. AutoCamera 自动镜头系统（JSON 驱动）

> 引入于 `12eb658`。用途：确定性自动镜头轨迹，替代人工操作，保证每次运行可重复。路径完全外置为时间序列 JSON，运行时按名字加载，UI 按钮触发；引擎侧只保留通用播放器。

### 4.1 架构

```
autocamera/                            （基线根目录，git 追踪）
├── *.json                             5 条路径（20Hz 关键帧时间序列，含验证元数据）
├── generate_paths.py                  路径生成器（航点 → 样条 → 速度剖面 → 穿模修复 → 验证 → JSON）
└── analyze_geometry.py                体素几何解析器

framework/cauldron/framework/
├── inc/core/autocamera.h              （新增）控制器声明：路径结构体、加载、播放、录制、回放、DrawUI
├── src/core/autocamera.cpp            （新增）JSON 加载、Catmull-Rom 时间插值、ImGui 窗口、
│                                        录制/回放实现；加载目录按优先级尝试
│                                        bin/configs/autocamera → ../autocamera
├── inc/core/components/cameracomponent.h    （修改）新增 public SetAnimatedPose(eye, look)
├── src/core/components/cameracomponent.cpp  （修改）Update() 在 AMD 预留 "Support camera
│                                            animations" 占位处（约 :250）接入控制器
│                                            AutoCameraController::Get().Update(deltaTime, this)；
│                                            自动化播放时旁路 WASD/鼠标输入，投影 jitter 逻辑不受影响
├── inc/core/components/animationcomponent.h （修改）累积时间从函数内 static 改为成员，
├── src/core/components/animationcomponent.cpp 新增 ResetAnimationTime()（每圈重置场景动画时钟，
│                                            保证无人机等动画实体每圈运动完全一致）
└── src/core/uibackend.cpp             （修改）UIBackend::Update()（:77 附近）中调用 DrawUI()
                                             绘制 "Auto Camera Paths" 独立 ImGui 窗口
```

### 4.2 路径 JSON 格式

```json
{
  "name": "02_platform_orbit",
  "description": "Low elliptical orbit of the central service platform...",
  "duration": 25.0,
  "loop": true,
  "meta": {
    "path_length": 352.9,
    "linear_speed_units": [5.59, 23.2],
    "linear_ratio": 4.15,
    "angular_speed_deg_s": [0.97, 100.0],
    "angular_ratio": 102.817,
    "keyframe_rate_hz": 20
  },
  "keyframes": [
    { "t": 0.0,  "pos": [48.0, 26.0, 111.0], "look": [15.86, 2.20, 111.92] },
    ...
  ]
}
```

- 每条 25 秒闭环、501 帧（20Hz），C++ 侧对 `pos`/`look` 分别做 Catmull-Rom 时间插值（二分查找 + 端点钳制，速度连续）
- `duration` 到达后无缝 loop（lap 计数）；每圈开始重置场景动画时钟
- `meta` 块由生成器写入的验证数据（线/角速度 min·max·比值），便于人工核对

### 4.3 运行时行为与操作

- **启动默认自由相机**，不自动播放
- **UI 触发**：渲染器内常驻 "Auto Camera Paths" 窗口（每条路径一个按钮，播放中高亮；Stop 按钮恢复自由相机；显示当前路径/圈数/进度）
- **按键**：F7 = 停止恢复自由相机；F8 = 重启当前圈；F1 开关主 UI
- **确定性保证**：首次播放、每圈循环回绕、F8 重启、场景切换四个时机会调用 `AnimationComponentMgr::ResetAnimationTime()`，把无人机、舱口等所有动画物体复位到 glTF 初始状态 → 每圈画面完全一致（可作录制同步标记，日志有对应输出）
- 加载器按文件名排序、**按 name 去重**（手动复制的副本文件会被跳过）

### 4.4 五条路径（全部通过逐采样穿模验证，4 单位安全余量）

| # | 名称 | 内容 | 长度 | 线速度 (min~max) | 线速度比 | 角速度比 |
|---|---|---|---|---|---|---|
| 1 | `01_floor_lanes` | 三条地板航带低空扫掠 + 中央平台掠过（贴地 y6~10，覆盖主反射地面） | 729u | 13.8~59.4 | 4.30 | 62 |
| 2 | `02_platform_orbit` | 中央平台椭圆低空环绕，视线在平台/地板/顶棚间切换 | 353u | 6.9~28.7 | 4.15 | 103 |
| 3 | `03_dolly_and_corridor` | 入口缓推 → 中央高速冲刺 → 后廊低速窄通道（y≤14）→ 高空折返 | 787u | 4.8~64.1 | 13.3 | 54 |
| 4 | `04_high_structure_scan` | y46~64 高空环线，俯视地板反射 + 仰视顶梁/外壳遮挡 | 641u | 12.6~52.2 | 4.15 | 37 |
| 5 | `05_entrance_snake` | 入口蛇形穿越，开放大门天空反射 vs 侧墙/壁龛遮挡对比 | 453u | 8.9~36.9 | 4.15 | 44 |

要求"线速度与角速度 max ≥ 4×min"全部满足。角速度通过"POI 兴趣点轨迹 + 偏航漂移/振荡调制 + 俯仰扫描"构造，并做了每帧 5° 限幅（≤100°/s）消除甩头尖峰。

### 4.5 路径生成管线（改路径时使用）

```bash
# 1. 首次或场景变更后：生成体素内部自由空间图（产物 %TEMP%\ffx_hangar_voxel.npz）
python analyze_geometry.py
# 2. 修改 generate_paths.py 中的航点表（PathDef 列表）后重新生成：
python generate_paths.py     # 输出 5 个 JSON + 验证报告，全部 OK 才算通过
```

生成管线：闭环 Catmull-Rom 样条（弧长参数化）→ 速度剖面 `v(s) = vmin·(1+3.15w(s))`（比值 4.15，余量抵消折线弦长效应）→ 穿模自动修复（阻挡点沿 y 双向找最近自由高度，增量平滑迭代收敛 + 邻域余量直接修复）→ 逐采样验证（100% 落在内部自由体素）→ 速度统计（线/角比值 ≥4 才写盘）。

**设计新航点的正确姿势**：先用 `analyze_geometry.py` 的探测函数查目标走廊的柱体自由高度区间（脚本内有 `free_y_intervals` / `clutter` 探针），把航点 y 放在探测值之上，再跑生成器复验——纯靠自动修复对抗大障碍会震荡失败（v1→v2 迭代中验证过的教训）。

---

## 5. PoseRecorder 姿态序列录制器（commit `6d4d96f`）

> 用途：SSR-Guardian 训练数据采集第一步——把自动镜头运行时的全部位姿状态录成可离线重放的序列。与镜头控制器同文件同 UI，语义为**先武装、后播放，才产生数据**。

**修改文件**：

| 文件 | 改动 |
|---|---|
| `framework/cauldron/framework/inc/core/autocamera.h` | 录制状态成员、`RecordFrame()` 公共接口、会话管理方法声明、Record 布局注释 |
| `framework/cauldron/framework/src/core/autocamera.cpp` | 会话生命周期、二进制写入器、UI 复选框、F9 热键、环境变量、每圈切文件逻辑（+265 行） |
| `framework/cauldron/framework/inc/core/components/animationcomponent.h` | 新增 `AnimationComponentMgr::GetElapsedTime()`（动画时钟只读访问器） |
| `framework/cauldron/framework/src/core/framework.cpp` | 组件管理器更新循环**之后**挂接 `AutoCameraController::Get().RecordFrame(m_DeltaTime)`（+include） |

**使用方式**：

| 方式 | 操作 |
|---|---|
| UI | "Auto Camera Paths" 窗口勾选 **Record Pose Sequence (F9)**，再点轨迹按钮 |
| 热键 | **F9** 随时切换武装状态（不播放时也可按） |
| 无头批量 | 启动前设环境变量 `CAULDRON_AUTO_RECORD=1`（自动武装）、`CAULDRON_AUTO_PLAY=<索引或轨迹名子串>`（自动开播） |

**当前库存**（2026-10-20 重录，全部通过完整性校验）：五条轨迹各一条完整圈 take（`*_lap{2,4,6,8,10}_*.posebin`，4672–4949 记录，零残尾/零帧间隙/时钟零回退，无人机运动正常），已纳入 git 跟踪，作为 M2/M3 离线采集的输入。

**会话语义**：录制会话存在当且仅当 `武装 && 播放中`。圈（lap）边界（自然 loop 回绕 / F8 / 场景切换）自动关闭当前文件、下一帧开新文件——**一圈一个 take**。停止播放（F7）或取消武装即收尾。

**输出格式**（`autocamera/recordings/<轨迹名>_lap<N>_<时间戳>.posebin`）：

- 首行：JSON 头（`magic: CAULDRON_POSE_V1`、轨迹名、圈次、时长、实体名字表、二进制布局说明），以 `\n` 结束
- 之后定长二进制记录流（小端）：

```
frame_index u64 | lap u32 | anim_clock f64 | delta_time f64 | path_time f64
cam_pos f32[3] | cam_look f32[3] | cam_inv_view f32[16]
entity_count u32 | 各动画实体世界矩阵 f32[16] × N（顺序 == 头内实体表）
```

- 矩阵为行主序列出（`m[r][c] = Matrix4::getElem(col=c, row=r)`，世界空间）；平移量在 `m[0..2][3]` 即线性索引 `[3],[7],[11]`
- 崩溃安全：每 600 帧强制 flush，硬杀进程最多损失最后一条记录（解析时容忍 <stride 的残尾）

**记录内容的最小充分集**（设计要点）：

- 相机：控制器求值的 `pos/look` + 相机最终 `inv_view` 矩阵（消除朝向约定歧义）
- 实体：`AnimationComponentMgr` 管理的**全部动画实体**的世界矩阵（Hangar 当前为 50 个：3 无人机及灯/旋翼子节点、闸门、舱门、飞船、液压臂等），名字取 glTF 节点名（会话内顺序稳定）
- 时域元数据：`anim_clock`（动画系统时钟快照）、`delta_time`（真实帧间隔）——使每一帧的动画状态自描述，不受变步长影响

**离线消费契约（M1 铁律，已由回放器实现，见第 6 节）**：回放端直接 `SetTransform(本帧) + SetPrevTransform(序列前帧)` 覆写实体、跳过动画系统求值。PrevTransform 是引擎运动矢量的来源，错误 = 时域特征与降噪行为静默失真。

**实测**：2756 条完整记录 @ ~201fps 捕获；动画时钟零回退、帧号零跳变、全部数值有限；无人机运动真实（Drone_01 峰值 88 u/s）。

---

## 6. PosePlayback 姿态回放器（commit `9043784`，M1）

> 用途：把 `.posebin` take 确定性重放进渲染管线——相机与全部动画实体由录制序列驱动，替代实时路径求值。这是 M2/M3（SSR 采集 pass / 参考采集 pass）的对齐基础：两张图必须逐帧对应同一姿态序列。

**修改文件**：

| 文件 | 改动 |
|---|---|
| `framework/cauldron/framework/inc/core/autocamera.h` | 回放状态成员、`LoadPosePlayback()` / `ApplyPlaybackTransforms()` 公共接口 |
| `framework/cauldron/framework/src/core/autocamera.cpp` | `LoadPosePlayback()`（文件解析）/ `UpdatePlayback()`（相机驱动 + 帧推进）/ `ApplyPlaybackTransforms()`（实体覆写）三个函数（+~280 行）；`RecordFrame()` 会话条件放宽以支持回放中再录制 |
| `framework/cauldron/framework/src/core/framework.cpp` | 组件更新循环后、`RecordFrame()` 之前挂接 `ApplyPlaybackTransforms()` |

**使用方式**：

| 方式 | 操作 |
|---|---|
| UI | "Auto Camera Paths" 窗口的 **Playback Takes** 区块：自动扫描录制目录按新到旧列出全部 take，点按钮即回放（Refresh 重新扫描） |
| 无头批量 | 启动前设环境变量 `CAULDRON_AUTO_PLAYBACK=<posebin 路径>` |

回放无缝循环；**F8** 从头重放；**F7** 停止并恢复自由相机；UI 显示蓝色 PLAYBACK 状态行（源名 / 帧进度 / 圈次）。可与录制同时开启（`CAULDRON_AUTO_RECORD=1`）——用于往返校验或派生 take。模式互斥：点轨迹按钮会干净退出回放，回放激活时轨迹按钮高亮被抑制。

**机制**（与数据公理一一对应）：

1. **相机**：逐帧读记录的 `pos/look` 走 `SetAnimatedPose()`——与录制时完全相同的代码路径，视图矩阵逐位一致
2. **实体覆写**：按录制时的组件顺序（`GetComponentList()` 索引）逐实体 `SetPrevTransform(前帧矩阵) + SetTransform(本帧矩阵)`——PrevTransform 链保证引擎运动矢量正确（G-buffer 运动矢量 = 前帧到本帧的变换差，是鉴别器的输入特征之一）
3. **名字校验**：绑定时逐实体比对 glTF 节点名，场景与录制时不符即拒绝覆写并告警（防错绑）
4. **动画系统求值被跳过**（姿态是数据，不是过程）；动画时钟照常流逝但无消费者

**加载期对齐守卫（验证环节抓出的两个 bug 的修复）**：

- 回放帧计数在场景加载期间就会推进（默认相机先于场景实体就绪），导致 take 与源错位约 56 帧 → 修复：实体绑定完成前回放**停在帧 0** 不推进
- 录制会话早于实体绑定开启，take 前几十帧混入未覆写状态 → 修复：回放模式下录制会话等待绑定完成后才开始

**验收（往返差分测试）**：回放轨迹 1 take 的同时再录制一遍，两个 posebin 逐帧比对（相机 pos/look + inv_view + 全部 50 实体矩阵）：**5243 / 5243 帧，字节级零差异，帧 0 对齐**。该测试同时验证了回放正确性、PrevTransform 链、录制/回放共存三个点。

**回放步长系统（2026-10-21 新增）**：

- **语义**：步长 X = 下一渲染帧相对上一渲染帧跳过 X-1 个记录帧。`m_PlaybackApplied`（PrevTransform 来源）每个渲染帧只更新一次，步长回放下自动保持"上一渲染帧"的变换——运动矢量与降噪器历史看到的帧距 == 真实帧距，低配掉帧采集的特征/标签物理自洽
- **两个互斥模式**：均匀步长 1~5（1 = 完全连续）；随机 X~Y（每个渲染帧独立抽取，UI 强制 X<Y）
- **UI**："Auto Camera Paths" 窗口 Playback Takes 区块的 **Stride** 折叠区，即时生效；PLAYBACK 状态行显示 last stride
- **无头**：`CAULDRON_AUTO_PLAYBACK_STRIDE=<S>`（1~5）或 `CAULDRON_AUTO_PLAYBACK_STRIDE_RAND=<X:Y>`（同时设置时随机模式优先）
- **可复现**：随机调度固定种子（`kStrideSeed`），F8 / 重新加载同一 take 重放完全相同的 pacing 计划；步长回放中再录制时，take 内 `frame_index` 按步长跳变，解析端可据此识别 pacing

---

## 7. 示例配置修改

`samples/hybridreflections/config/hybridreflectionsconfig.json`（构建时同步拷贝到 `bin/configs/`）：

```json
"Content": {
    "Scenes": [ "../media/Hangar/Hangar_1105.gltf" ],   // 原 HybridReflections/scene.gltf
    "Camera": "Hangar_Camera_000",                       // 原 "Camera"（Hangar 中不存在该名）
    "SceneExposure": 2.0
}
```

---

## 8. 已知事项与边界

1. 路径 3 线速度比 13.3（非 4.15）：后廊窄通道处修复台阶所致，仍满足 ≥4 要求
2. 路尺度按 Hangar 场景标定；换场景需重跑 3.2 体素解析 + 4.5 生成管线
3. VK 侧（`samples-vk` 预设）本机不可用（无 Vulkan SDK）
4. 自动运镜只做过编译验证（DebugDX12 通过），未做长时间运行验证
5. `json.h` 的 filesystem 检测守卫是刻意的——升级该 vendored 库时保留其自带逻辑
5a. vendored imgui 1.87 打过防御补丁（`imgui_widgets.cpp` ButtonBehavior，2026-10-21）：`ActiveIdMouseButton == -1` 的边缘时序会触发 `mouse_button >= 0` 断言崩溃（调整步长 UI 时踩中），已按上游 1.88+ 的方式改为守卫判断走释放路径，同函数 Repeat 分支的负索引一并修掉。升级 imgui ≥1.88 时此补丁可移除（上游已加固）
6. 录制器输出目录候选 `../autocamera/recordings` → `recordings`（相对 exe 工作目录 `bin/`）
7. posebin 的 JSON 头按行解析（单行、`\n` 结束），实体名经 UTF-8 转换（`misc/helpers.h` 的 `WStringToString`）；回放端用 `json >> header` 解析后逐字符消费到 `\n` 定位数据区起点
8. posebin take 已纳入 git 跟踪（当前 5 条约 78MB，单文件 15–16MB，低于 GitHub 100MB 红线）；`01_floor_lanes_playback_lap1_145019`（M1 往返校验副产物）已随重录清理。将来量产时单文件若逼近 100MB 或总量失控，再调整（如 LFS 或不入库）
9. MSBuild 参数在 Git Bash 下用 `-p:` 形式（`/p:` 会被路径转换破坏）；构建命令模板见技术路线文档第 6 节
10. 回放模式当前要求场景与录制时完全一致（实体数量 + 名字逐一校验）——将来多场景数据集需要 take 内记录场景标识并校验

---

## 9. Git 集成与异机同步

基线已作为 vendored 目录收入 `SSR-Guardian` 仓库（原 AMD `.git` 已删除，基线版本上游 `c6efa6b`，上游仓库 `GPUOpen-LibrariesAndSDKs/FidelityFX-SDK` 随时可重新 clone 对照）。基线自带的 `.gitignore` 对父仓库继续生效：`media/`（5.6G）、`media-cache/`、`build/`、`bin/`、`out/` 均不入库；另经第 2 节白名单修复后，AMD 分发运行库不再被误伤丢文件。入库内容约 165MB + posebin takes 约 78MB。

**新机器同步流程**：

1. `git clone https://github.com/Horace529/SSR-Guardian.git`
2. **media 需另行获取**（git 不追踪）：从原机器拷贝 `media/`（至少 `media/Hangar/`），或在基线根目录跑 `UpdateMedia.bat` 由 AMD 服务器下载
3. 需要 VS2026（v145 工具集）+ 其自带 CMake（≥3.23 且支持 "Visual Studio 18 2026" 生成器）
4. `cmake --preset samples-dx12`（在 `Baseline-FidelityFX-SDK/` 下执行，生成 build/*.slnx）
5. `cmake --build build --config DebugDX12 --target FFX_HYBRIDREFLECTIONS`（产物进 `bin/`，运镜路径 JSON 由加载器从 `autocamera/` 回退读取，无需手动拷贝）

**以后无法用 git merge AMD 更新**——需要时重新 clone 上游用 diff 工具对照移植。

---

## 10. 文档资产

| 文件 | 内容 |
|---|---|
| `SSR-Guardian技术路线.md` | 技术决策总览：管线解剖、鉴别器设计、数据工厂、预算、里程碑状态 |
| `SSR-Guardian-网络与数据规格.md` | 输入/输出/数据格式/数据流的规格单一事实源（网络任务、34 通道张量、posebin/F/v/HC/A/E 格式、推理与采集数据流） |
| `Docs/SSR-Guardian-Modifications.md` | 本文档（两份改造记录的合并版） |
| `Baseline-FidelityFX-SDK/autocamera/recordings/` | 录制产出目录（.posebin takes，已入 git；五条轨迹各一条完整圈，M2/M3 采集输入） |
