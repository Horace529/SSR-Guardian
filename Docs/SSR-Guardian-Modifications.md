# SSR-Guardian — Baseline 改造记录

> 基线：AMD FidelityFX SDK 1.1.4（git `c6efa6b`），HybridReflections sample，场景 `media/Hangar/Hangar_1105.gltf`
> 环境：本机 Visual Studio 2026 Community 18.10.1（v145 工具集）+ VS 自带 CMake 4.3.1，Windows x64
> 记录日期：2026-10-19

本文档记录对 `Baseline-FidelityFX-SDK` 的全部改造：构建系统适配、场景几何解析工具、JSON 驱动的自动运镜系统（含 5 条无穿模验证路径）。

---

## 1. 构建系统适配（VS2026）

基线从另一台机器打包而来（路径一致，同为 VS2026 生成），本机适配只需重新绑定 CMake 缓存并把配置固化为预设。

### 新增文件

| 文件 | 内容 |
|---|---|
| `CMakePresets.json` | 根目录预设：`samples-dx12`（生成器 `Visual Studio 18 2026`、x64、`BUILD_TYPE=SAMPLES_DX12`、`FFX_HYBRIDREFLECTIONS=ON`、`RUNTIME_SHADER_RECOMPILE=0`）及 DebugDX12 / ReleaseDX12 / RelWithDebInfoDX12 三个构建预设；另附 `samples-vk`（本机未装 Vulkan SDK，暂不可用） |
| `sdk/CMakePresets.json` | SDK 后端预设 `sdk-dx12-x64`（`FFX_API_BACKEND=DX12_X64`、`FFX_AUTO_COMPILE_SHADERS=1`、组件 CLASSIFIER+DENOISER+SPD），仅当修改 `sdk/` 源码后需要重建后端库 |

### 常用命令

```bash
# 生成解决方案（输出 build/*.slnx，启动项目 FFX_HYBRIDREFLECTIONS）
cmake --preset samples-dx12
# 编译（输出 bin/FFX_HYBRIDREFLECTIONS_DX12D.exe）
cmake --build build --config DebugDX12 --target FFX_HYBRIDREFLECTIONS
```

注意：预编译后端库在 `sdk/bin/ffx_sdk/`，**只包含 CLASSIFIER+DENOISER+SPD 三个组件**（HybridReflections 所需）。要构建其他 sample（FSR、CACAO 等），需先用 sdk 预设重建后端并补上对应 `FFX_*` 组件。

### 工作区已有改动（非本次改造内容，仅记录）

- 多个 framework 源文件：`std::experimental` → `std`（VS2026 兼容，用户手动修改）
- `common.cmake`：`CMAKE_CXX_STANDARD` 11 → 17

---

## 2. 场景几何解析工具

两套 Python 脚本（依赖 numpy/scipy），**不改引擎代码**，为路径设计提供几何依据。

### 2.1 glTF 场景图解析

脚本：`autocamera/analyze_geometry.py`（初版 `%TEMP%\ffx_gltf_analyze.py`）
解析 `.gltf` JSON 元数据（节点 TRS 变换 × POSITION accessor min/max 包围盒），不解压几何 buffer。

Hangar 场景结论：494 节点 / 396 网格 / 87 材质，218 个根节点（典型 DCC 扁平导出）；几乎无 glTF 实例化（仅 2 个网格被共享）；5 个相机（启动相机 `Hangar_Camera_000` 位于 (-15.3, 18.6, -95.8)，入口内侧）；1 段 84 通道动画（3 架无人机 + 舱口机构）；"ShipBody" 是包裹整个机库的外壳（子树 400 节点），并非独立飞船实体——SSR 失效区 = 外壳/顶棚遮挡方向 + 后廊低矮区 + 入口开放区。

### 2.2 体素几何解析（穿模验证的基础）

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

## 3. 自动运镜系统（JSON 驱动）

### 3.1 架构

路径完全外置为时间序列 JSON，运行时按名字加载，UI 按钮触发；引擎侧只保留通用播放器。

```
autocamera/                            （基线根目录，git 追踪）
├── *.json                             5 条路径（20Hz 关键帧时间序列，含验证元数据）
├── generate_paths.py                  路径生成器（航点 → 样条 → 速度剖面 → 穿模修复 → 验证 → JSON）
└── analyze_geometry.py                体素几何解析器

framework/cauldron/framework/
├── inc/core/autocamera.h              （新增）控制器声明：路径结构体、加载、播放、DrawUI
├── src/core/autocamera.cpp            （新增）JSON 加载、Catmull-Rom 时间插值、ImGui 窗口
│                                        加载目录按优先级尝试 bin/configs/autocamera → ../autocamera
├── inc/core/components/cameracomponent.h    （修改）新增 public SetAnimatedPose(eye, look)
├── src/core/components/cameracomponent.cpp  （修改）Update() 在 AMD 预留 "Support camera
│                                            animations" 占位处接入控制器；自动化播放时
│                                            旁路 WASD/鼠标输入，投影 jitter 逻辑不受影响
├── inc/core/components/animationcomponent.h （修改）累积时间从函数内 static 改为成员，
├── src/core/components/animationcomponent.cpp 新增 ResetAnimationTime()
└── src/core/uibackend.cpp             （修改）UIBackend::Update() 中调用 DrawUI() 绘制
                                             "Auto Camera Paths" 独立 ImGui 窗口
```

### 3.2 路径 JSON 格式

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

- 每条 25 秒闭环、501 帧（20Hz），C++ 侧对 `pos`/`look` 分别做 Catmull-Rom 时间插值（二分查找 + 端点钳制）
- `meta` 块由生成器写入的验证数据（线/角速度 min·max·比值），便于人工核对

### 3.3 运行时行为

- **启动默认自由相机**，不自动播放
- **UI 触发**：渲染器内常驻 "Auto Camera Paths" 窗口（每条路径一个按钮，播放中高亮；Stop 按钮恢复自由相机；显示当前路径/圈数/进度）
- **按键**：F7 = 停止恢复自由相机；F8 = 重启当前圈；F1 开关主 UI
- **确定性保证**：首次播放、每圈循环回绕、F8 重启、场景切换四个时机会调用 `AnimationComponentMgr::ResetAnimationTime()`，把无人机、舱口等所有动画物体复位到 glTF 初始状态 → 每圈画面完全一致（可作录制同步标记，日志有对应输出）
- 加载器按文件名排序、**按 name 去重**（手动复制的副本文件会被跳过）

### 3.4 五条路径（全部通过逐采样穿模验证，4 单位安全余量）

| # | 名称 | 内容 | 长度 | 线速度 (min~max) | 线速度比 | 角速度比 |
|---|---|---|---|---|---|---|
| 1 | `01_floor_lanes` | 三条地板航带低空扫掠 + 中央平台掠过（贴地 y6~10，覆盖主反射地面） | 729u | 13.8~59.4 | 4.30 | 62 |
| 2 | `02_platform_orbit` | 中央平台椭圆低空环绕，视线在平台/地板/顶棚间切换 | 353u | 6.9~28.7 | 4.15 | 103 |
| 3 | `03_dolly_and_corridor` | 入口缓推 → 中央高速冲刺 → 后廊低速窄通道（y≤14）→ 高空折返 | 787u | 4.8~64.1 | 13.3 | 54 |
| 4 | `04_high_structure_scan` | y46~64 高空环线，俯视地板反射 + 仰视顶梁/外壳遮挡 | 641u | 12.6~52.2 | 4.15 | 37 |
| 5 | `05_entrance_snake` | 入口蛇形穿越，开放大门天空反射 vs 侧墙/壁龛遮挡对比 | 453u | 8.9~36.9 | 4.15 | 44 |

要求"线速度与角速度 max ≥ 4×min"全部满足。角速度通过"POI 兴趣点轨迹 + 偏航漂移/振荡调制 + 俯仰扫描"构造，并做了每帧 5° 限幅（≤100°/s）消除甩头尖峰。

### 3.5 路径生成管线（改路径时使用）

```bash
# 1. 首次或场景变更后：生成体素内部自由空间图（产物 %TEMP%\ffx_hangar_voxel.npz）
python analyze_geometry.py
# 2. 修改 generate_paths.py 中的航点表（PathDef 列表）后重新生成：
python generate_paths.py     # 输出 5 个 JSON + 验证报告，全部 OK 才算通过
```

生成管线：闭环 Catmull-Rom 样条（弧长参数化）→ 速度剖面 `v(s) = vmin·(1+3.15w(s))`（比值 4.15，余量抵消折线弦长效应）→ 穿模自动修复（阻挡点沿 y 双向找最近自由高度，增量平滑迭代收敛 + 邻域余量直接修复）→ 逐采样验证（100% 落在内部自由体素）→ 速度统计（线/角比值 ≥4 才写盘）。

**设计新航点的正确姿势**：先用 `analyze_geometry.py` 的探测函数查目标走廊的柱体自由高度区间（脚本内有 `free_y_intervals` / `clutter` 探针），把航点 y 放在探测值之上，再跑生成器复验——纯靠自动修复对抗大障碍会震荡失败（v1→v2 迭代中验证过的教训）。

---

## 4. 已知事项

- 路径 3 线速度比 13.3（非 4.15）：后廊窄通道处修复台阶所致，仍满足 ≥4 要求
- 路尺度按 Hangar 场景标定；换场景需重跑 2.2 体素解析 + 3.5 生成管线
- VK 侧（`samples-vk` 预设）本机不可用（无 Vulkan SDK）
- 只做过编译验证（DebugDX12 通过），未做长时间运行验证

---

## 5. Git 集成与异机同步

基线已作为 vendored 目录收入 `SSR-Guardian` 仓库（原 AMD `.git` 已删除，基线版本 `c6efa6b`，上游 `GPUOpen-LibrariesAndSDKs/FidelityFX-SDK` 随时可重新 clone 对照）。基线自带的 `.gitignore` 对父仓库继续生效：`media/`（5.6G）、`media-cache/`、`build/`、`bin/`、`out/` 均不入库，入库内容约 165MB。

**新机器同步流程**：

1. `git clone https://github.com/Horace529/SSR-Guardian.git`
2. **media 需另行获取**（git 不追踪）：从原机器拷贝 `media/`（至少 `media/Hangar/`），或在基线根目录跑 `UpdateMedia.bat` 由 AMD 服务器下载
3. 需要 VS2026（v145 工具集）+ 其自带 CMake（≥3.23 且支持 "Visual Studio 18 2026" 生成器）
4. `cmake --preset samples-dx12`（在 `Baseline-FidelityFX-SDK/` 下执行，生成 build/*.slnx）
5. `cmake --build build --config DebugDX12 --target FFX_HYBRIDREFLECTIONS`（产物进 `bin/`，运镜路径 JSON 由加载器从 `autocamera/` 回退读取，无需手动拷贝）

**以后无法用 git merge AMD 更新**——需要时重新 clone 上游用 diff 工具对照移植。
