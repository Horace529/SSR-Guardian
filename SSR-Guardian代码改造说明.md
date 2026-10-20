# SSR-Guardian 代码改造与新增功能说明

> 更新：2026-10-20 ｜ 仓库：`D:\Projects\SSR-Guardian`（分支 main）
> 基线来源：commit `12eb658`（AMD FidelityFX SDK 1.1.4 整体 vendored + 早期 SSR-Guardian 修改）
> 姊妹文档：`SSR-Guardian技术路线.md`（技术决策与依据）；本文只记录"改了什么、在哪、怎么用"。

---

## 0. 变更总览

| 分类 | 内容 | 引入位置 |
|---|---|---|
| 构建兼容层 | VS2026 工具集适配（filesystem/C++17） | `12eb658` 内 |
| 二进制资产 | AMD 分发运行库纳入 git 跟踪（修同步丢文件问题） | `335bb46` |
| 自动镜头系统 | AutoCameraController + 5 条轨迹 + 生成脚本 | `12eb658` 内 |
| 姿态序列录制器 | PoseRecorder（.posebin 数据采集） | `6d4d96f` |
| 示例配置 | HybridReflections 指向 Hangar 场景 | `12eb658` 内 |
| 文档 | 技术路线 + 本文 | `ecf4f79` / 本次 |

---

## 1. VS2026 构建兼容层

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

## 2. AMD 分发二进制入库（修 git 同步丢文件）

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

## 3. AutoCamera 自动镜头系统

> 引入于 `12eb658`（另一台机器开发后同步）。用途：确定性自动镜头轨迹，替代人工操作，保证每次运行可重复。

**新增/修改文件**：

| 文件 | 内容 |
|---|---|
| `framework/cauldron/framework/src/core/autocamera.cpp` | `AutoCameraController` 单例实现（约 300 行） |
| `framework/cauldron/framework/inc/core/autocamera.h` | 类定义 |
| `framework/cauldron/framework/src/core/components/cameracomponent.cpp` | `CameraComponent::Update` 内挂接：`AutoCameraController::Get().Update(deltaTime, this)`（约 :250），播放中由控制器独占相机位姿、旁路自由相机输入 |
| `framework/cauldron/framework/src/core/uibackend.cpp` | UI 窗口绘制挂接（:77） |
| `framework/cauldron/framework/src/core/components/animationcomponent.cpp` | 配套 `ResetAnimationTime()`（每圈重置场景动画时钟，保证无人机等动画实体每圈运动完全一致） |
| `autocamera/*.json` ×5 | 轨迹文件（见下） |
| `autocamera/generate_paths.py`、`autocamera/analyze_geometry.py` | 轨迹生成/几何分析脚本 |

**轨迹文件格式**（`autocamera/01_floor_lanes.json` 等 5 条）：

```json
{
  "name": "01_floor_lanes", "description": "...", "duration": 25.0, "loop": true,
  "meta": { "keyframe_rate_hz": 20, "linear_speed_units": [...], "angular_speed_deg_s": [...] },
  "keyframes": [ { "t": 0.0, "pos": [x,y,z], "look": [x,y,z] }, ... ]
}
```

**行为**：20Hz 关键帧 + 运行时 Catmull-Rom 插值（速度连续）；`duration` 到达后无缝 loop（lap 计数）；每圈开始重置场景动画时钟。加载目录优先级 `bin/configs/autocamera` → `../autocamera`。

**操作**：UI 窗口 "Auto Camera Paths" 点按钮开播；**F8** 重开当前圈；**F7** 停止并恢复自由相机；F1 开关主 UI 窗口。

---

## 4. PoseRecorder 姿态序列录制器（commit `6d4d96f`）

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

**离线消费契约（M1 铁律）**：回放端直接 `SetTransform(本帧) + SetPrevTransform(序列前帧)` 覆写实体、跳过动画系统求值。PrevTransform 是引擎运动矢量的来源，错误 = 时域特征与降噪行为静默失真。

**实测**：2756 条完整记录 @ ~201fps 捕获；动画时钟零回退、帧号零跳变、全部数值有限；无人机运动真实（Drone_01 峰值 88 u/s）。

---

## 5. 示例配置修改

`samples/hybridreflections/config/hybridreflectionsconfig.json`（构建时同步拷贝到 `bin/configs/`）：

```json
"Content": {
    "Scenes": [ "../media/Hangar/Hangar_1105.gltf" ],   // 原 HybridReflections/scene.gltf
    "Camera": "Hangar_Camera_000",                       // 原 "Camera"（Hangar 中不存在该名）
    "SceneExposure": 2.0
}
```

---

## 6. 文档资产

| 文件 | 内容 |
|---|---|
| `SSR-Guardian技术路线.md` | 技术决策总览：管线解剖、鉴别器设计、数据工厂、预算、里程碑状态 |
| `SSR-Guardian代码改造说明.md` | 本文 |
| `Baseline-FidelityFX-SDK/autocamera/recordings/` | 录制产出目录（.posebin takes） |

---

## 7. 注意事项与已知边界

1. `json.h` 的 filesystem 检测守卫是刻意的——升级该 vendored 库时保留其自带逻辑
2. 录制器输出目录候选 `../autocamera/recordings` → `recordings`（相对 exe 工作目录 `bin/`）
3. posebin 的 JSON 头按行解析（单行、`\n` 结束），实体名经 UTF-8 转换（`misc/helpers.h` 的 `WStringToString`）
4. 录制文件建议入 git（当前 `.gitignore` 未排除 `autocamera/recordings`，注意单文件 GB 级时再调整策略）
5. MSBuild 参数在 Git Bash 下用 `-p:` 形式（`/p:` 会被路径转换破坏）；构建命令模板见技术路线文档第 6 节
