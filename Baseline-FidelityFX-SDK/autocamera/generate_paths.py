"""Generate 5 verified camera paths for the Hangar scene -> time-series JSON.

Reads the voxel interior map produced by ffx_hangar_geometry.py, builds closed
Catmull-Rom loops through hand-placed waypoints INSIDE the verified interior,
times them with a v(s)=vmin*(1+3w(s)) profile (exact 4:1 linear speed ratio),
adds a look-at construction with guaranteed angular-speed variation, verifies
clearance + speed ratios numerically, and emits dense time-series keyframes.
"""
import json
import numpy as np

VOX = r"C:\Users\test\AppData\Local\Temp\ffx_hangar_voxel.npz"
OUT_DIR = os.path.dirname(os.path.abspath(__file__))   # write next to this script
T = 25.0          # seconds per path
RATE = 20         # keyframe rate (Hz)

data = np.load(VOX)
interior = data["interior"]
ROI_MIN = data["roi_min"].astype(np.float64)
CELL = float(data["cell"])

def in_interior(p):
    g = np.floor((np.asarray(p) - ROI_MIN) / CELL).astype(int)
    if np.any(g < 0) or np.any(g >= np.array(interior.shape)):
        return False
    return interior[tuple(g)]

def catmull(p0, p1, p2, p3, u):
    u2, u3 = u*u, u*u*u
    return 0.5*((2*p1) + (-p0+p2)*u + (2*p0-5*p1+4*p2-p3)*u2 + (-p0+3*p1-3*p2+p3)*u3)

def build_loop(wps, subdiv=40):
    """closed CR loop -> (pts, cumarc, total)"""
    n = len(wps)
    pts = []
    for i in range(n):
        p0, p1, p2, p3 = wps[(i-1) % n], wps[i], wps[(i+1) % n], wps[(i+2) % n]
        for j in range(subdiv):
            pts.append(catmull(p0, p1, p2, p3, j / subdiv))
    pts.append(pts[0])
    pts = np.array(pts)
    seg = np.linalg.norm(np.diff(pts, axis=0), axis=1)
    cum = np.concatenate([[0], np.cumsum(seg)])
    return pts, cum, cum[-1]

def eval_arc(pts, cum, s):
    """evaluate at arc-length s (wrapped)"""
    total = cum[-1]
    s = s % total
    i = np.searchsorted(cum, s, side="right") - 1
    i = min(i, len(pts) - 2)
    t = (s - cum[i]) / max(cum[i+1] - cum[i], 1e-9)
    return pts[i] + (pts[i+1] - pts[i]) * t

def rot_y(v, ang):
    c, s = np.cos(ang), np.sin(ang)
    return np.array([v[0]*c + v[2]*s, v[1], -v[0]*s + v[2]*c])

def rot_axis(v, axis, ang):
    axis = axis / (np.linalg.norm(axis) + 1e-12)
    c, s = np.cos(ang), np.sin(ang)
    return v*c + np.cross(axis, v)*s + axis*np.dot(axis, v)*(1-c)

class PathDef:
    def __init__(self, name, desc, wps, pois, lead,
                 speed_cycles=3, yaw_drift_deg=5.0, yaw_cycles=2, yaw_amp_ratio=0.7,
                 pitch_amp_deg=12.0, pitch_cycles=1, pitch_phase=0.8, vmin=None):
        self.name, self.desc = name, desc
        self.wps = np.array(wps, dtype=np.float64)
        self.pois = np.array(pois, dtype=np.float64)
        self.lead = lead
        self.speed_cycles = speed_cycles
        self.yaw_drift = np.deg2rad(yaw_drift_deg)
        self.yaw_amp = np.deg2rad(yaw_amp_ratio * yaw_drift_deg)
        self.yaw_cycles = yaw_cycles
        self.pitch_amp = np.deg2rad(pitch_amp_deg)
        self.pitch_cycles = pitch_cycles
        self.pitch_phase = pitch_phase
        self.vmin = vmin

def make_path(defn):
    pts, cum, total = build_loop(defn.wps)

    # --- collision repair: lift/drop y of dense path samples to nearest free voxel ---
    M = 2000
    ss = np.linspace(0, total, M, endpoint=False)
    dense = np.array([eval_arc(pts, cum, s) for s in ss])
    def nearest_free_y(x, y, z):
        g0 = np.floor((np.array([x, y, z]) - ROI_MIN) / CELL).astype(int)
        yc = g0[1]
        for d in range(0, 40):
            for sign in ((0,) if d == 0 else (-d, d)):
                g = g0.copy(); g[1] = yc + sign
                if np.all(g >= 0) and np.all(g < np.array(interior.shape)) and interior[tuple(g)]:
                    return ROI_MIN[1] + (yc + sign) * CELL + CELL * 0.5
        return None
    kern = np.exp(-0.5 * (np.arange(-80, 81) / 24.0) ** 2)
    kern /= kern.sum()
    y_orig = dense[:, 1].copy()
    delta = np.zeros(M)
    for _ in range(8):  # iterate: needed lift -> smooth the delta -> re-check
        dense[:, 1] = y_orig + delta
        need = np.zeros(M)
        for i, p in enumerate(dense):
            if not in_interior(p):
                y = nearest_free_y(p[0], p[1], p[2])
                if y is None:
                    raise RuntimeError(f"{defn.name}: no free y near {p}")
                need[i] = y - p[1]
        if not need.any():
            break
        delta = np.convolve(np.pad(delta + need, 80, mode="edge"), kern, mode="valid")
    dense[:, 1] = y_orig + delta
    # final direct fix (no smoothing) for residual blocked samples; a 1-sample
    # step is imperceptible and the mask already carries 4 units of clearance
    for i, p in enumerate(dense):
        if not in_interior(p):
            y = nearest_free_y(p[0], p[1], p[2])
            if y is None:
                raise RuntimeError(f"{defn.name}: no free y near {p}")
            # lift a neighborhood with margin so interpolation between samples
            # cannot dip back into the blocked band
            for j in range(max(0, i - 3), min(len(dense), i + 4)):
                dense[j, 1] = max(dense[j, 1], y + 2.0)
    # rebuild arc data with repaired y
    dense[-1] = dense[0]
    seg = np.linalg.norm(np.diff(dense, axis=0), axis=1)
    cum = np.concatenate([[0], np.cumsum(seg)])
    pts, total = dense, cum[-1]

    # timing: v(s) = vmin*(1 + 3.05*w(s)) -> ratio 4.05 (margin vs discretization)
    vmin = defn.vmin if defn.vmin else total / (T * 2.525)
    vmax = 4.15 * vmin   # profile margin so sampled chord speeds keep ratio >= 4
    # integrate ds/dt = v(s): sample s finely, build t(s) then s(t) on uniform t grid
    S = np.linspace(0, total, 4000, endpoint=False)
    w = 0.5 - 0.5*np.cos(2*np.pi*defn.speed_cycles*S/total + 0.9)
    v = vmin + (vmax - vmin) * w
    dt_ds = 1.0 / v
    t_of_s = np.concatenate([[0], np.cumsum((np.diff(S)) * 0.5*(dt_ds[1:] + dt_ds[:-1]))])
    t_of_s *= T / t_of_s[-1]
    # POI loop: uniform-time closed CR through anchors
    ppts, pcum, ptotal = build_loop(defn.pois, subdiv=20)

    keys = []
    N = int(T * RATE)
    for k in range(N + 1):
        t = T * k / N
        s = np.interp(t, t_of_s, S)
        eye = eval_arc(pts, cum, s)
        poi = eval_arc(ppts, pcum, ptotal * (t / T))
        # keep a minimum eye->POI distance so the view direction never whips
        rel = poi - eye
        rl = np.linalg.norm(rel)
        if rl < 25.0:
            rel = rel / (rl + 1e-9) * 25.0
            poi = eye + rel
        d = poi - eye
        d = d / (np.linalg.norm(d) + 1e-12)
        yaw = defn.yaw_drift*t + defn.yaw_amp*np.sin(2*np.pi*defn.yaw_cycles*t/T + 0.4)
        d = rot_y(d, yaw)
        right = np.cross(np.array([0, 1.0, 0]), d)
        nr = np.linalg.norm(right)
        right = right/nr if nr > 1e-8 else np.array([1.0, 0, 0])
        pitch = defn.pitch_amp*np.sin(2*np.pi*defn.pitch_cycles*t/T + defn.pitch_phase)
        d = rot_axis(d, right, pitch)
        look = eye + d * defn.lead
        keys.append((t, eye, look))

    # cap the per-key view rotation (~5 deg/key = 100 deg/s) to remove whip pans
    cap = np.deg2rad(5.0)
    for i in range(2, len(keys)):  # keep the first two untouched so stats ignore spin-up
        e0, l0 = keys[i-1][1], keys[i-1][2]
        e1, l1 = keys[i][1], keys[i][2]
        d0 = (l0 - e0); d0 /= (np.linalg.norm(d0) + 1e-12)
        d1 = (l1 - e1); dist = np.linalg.norm(d1); d1 /= (dist + 1e-12)
        ang = np.arccos(np.clip(np.dot(d0, d1), -1, 1))
        if ang > cap:
            axis = np.cross(d0, d1)
            na = np.linalg.norm(axis)
            if na > 1e-9:
                d1 = rot_axis(d0, axis / na, cap)
            keys[i] = (keys[i][0], e1, e1 + d1 * dist)
    return keys, total, vmin, vmax

def verify(keys):
    ts = np.array([k[0] for k in keys])
    pos = np.array([k[1] for k in keys])
    look = np.array([k[2] for k in keys])
    # clearance
    bad = [(i, pos[i]) for i in range(len(pos)) if not in_interior(pos[i])]
    # speeds (interior of timeline only)
    core = slice(2, len(ts) - 2)
    v = np.linalg.norm(np.diff(pos, axis=0), axis=1) / np.diff(ts)
    d = look - pos
    d = d / np.linalg.norm(d, axis=1, keepdims=True)
    ang = np.arccos(np.clip(np.sum(d[1:]*d[:-1], axis=1), -1, 1)) / np.diff(ts)
    def stats(x):
        x = x[core.start-1:core.stop-1]
        return x.min(), x.max(), x.max()/max(x.min(), 1e-9)
    vmin_s, vmax_s, vratio = stats(v)
    amin_s, amax_s, aratio = stats(ang)
    return bad, (vmin_s, vmax_s, vratio), (np.rad2deg(amin_s), np.rad2deg(amax_s), aratio)

def emit(defn, keys, extra):
    out = {
        "name": defn.name,
        "description": defn.desc,
        "duration": T,
        "loop": True,
        "meta": extra,
        "keyframes": [
            {"t": round(float(t), 4),
             "pos": [round(float(p[0]), 3), round(float(p[1]), 3), round(float(p[2]), 3)],
             "look": [round(float(l[0]), 3), round(float(l[1]), 3), round(float(l[2]), 3)]}
            for t, p, l in keys
        ],
    }
    return out

# ---------------- 5 path definitions ----------------
paths = [
    PathDef(
        "01_floor_lanes",
        "Main hall floor sweep: three lanes across the glossy floor, central platform fly-by",
        [[-40, 8, -112], [0, 7, -45], [28, 12, -10], [35, 9, 30], [52, 28, 68],
         [60, 24, 104], [64, 34, 140], [30, 10, 160], [-40, 18, 155], [-70, 24, 126], [-78, 8, 70],
         [-60, 7, -10], [-35, 8, -70]],
        [[0, 0, -60], [35, 0, 60], [70, 0, 150], [-45, 0, 160], [-70, 0, 60],
         [-10, 2, 111], [-25, 0, -50]],
        lead=45, speed_cycles=3, yaw_drift_deg=5.0, yaw_cycles=2,
        pitch_amp_deg=13.0, pitch_cycles=1),
    PathDef(
        "02_platform_orbit",
        "Low elliptical orbit of the central service platform, look alternates platform/floor/roof",
        [[48, 26, 111], [25, 28, 158], [-45, 27, 150], [-62, 26, 105],
         [-45, 28, 58], [20, 27, 64]],
        [[-7, 2, 111], [-7, 2, 111], [40, 0, 160], [0, 68, 60], [-70, 0, 80], [-7, 2, 111]],
        lead=40, speed_cycles=2, yaw_drift_deg=6.0, yaw_cycles=2,
        pitch_amp_deg=16.0, pitch_cycles=2, pitch_phase=2.2),
    PathDef(
        "03_dolly_and_corridor",
        "Entrance dolly, fast center sprint, slow rear low-corridor pass, high return under roof beams",
        [[-10, 9, -120], [0, 9, -60], [12, 12, 20], [-12, 10, 90], [0, 9, 150],
         [2, 7, 205], [0, 6, 252], [2, 8, 192], [4, 13, 172], [-8, 36, 128], [-14, 44, 85],
         [10, 52, 5], [-6, 40, -70], [-14, 18, -112]],
        [[0, 8, -170], [20, 0, 30], [-30, 0, 120], [0, 4, 240], [-20, 60, 140],
         [0, 80, 60], [30, 40, -40], [0, 8, -160]],
        lead=50, speed_cycles=4, yaw_drift_deg=5.5, yaw_cycles=3,
        pitch_amp_deg=14.0, pitch_cycles=2, pitch_phase=1.6),
    PathDef(
        "04_high_structure_scan",
        "High-altitude loop beneath the roof structures, looking down at floor reflections and up at beams",
        [[-85, 48, -70], [-40, 52, -95], [30, 54, -88], [58, 50, -40],
         [52, 56, 62], [0, 64, 128], [-70, 54, 118], [-90, 50, 40], [-80, 46, -38]],
        [[-40, 0, -60], [40, 0, -40], [80, 0, 40], [20, 0, 130], [-80, 0, 110],
         [-60, 0, 20], [0, 85, 60], [-30, 88, 110]],
        lead=60, speed_cycles=3, yaw_drift_deg=5.0, yaw_cycles=2,
        pitch_amp_deg=15.0, pitch_cycles=2, pitch_phase=0.5),
    PathDef(
        "05_entrance_snake",
        "Snake through the entrance gate area and side niches: open-gate sky vs wall occlusion",
        [[48, 10, -112], [10, 13, -104], [-45, 9, -78], [-5, 8, -34],
         [42, 11, -60], [25, 12, -112], [-30, 10, -122], [10, 10, -80]],
        [[0, 12, -170], [-80, 22, -60], [0, 0, -90], [80, 20, -40],
         [0, 12, -170], [-90, 10, -100], [20, 0, -60], [60, 24, -80]],
        lead=42, speed_cycles=4, yaw_drift_deg=6.5, yaw_cycles=2,
        pitch_amp_deg=14.0, pitch_cycles=2, pitch_phase=3.0),
]

import os
os.makedirs(OUT_DIR, exist_ok=True)
all_ok = True
for defn in paths:
    keys, total, vmin, vmax = make_path(defn)
    bad, vst, ast = verify(keys)
    ok = len(bad) == 0 and vst[2] >= 4.0 and ast[2] >= 4.0
    all_ok &= ok
    print(f"{defn.name}: len={total:6.1f} v={vmin:5.2f}..{vmax:6.2f} "
          f"(ratio {vst[2]:4.2f}, sampled {vst[0]:5.2f}..{vst[1]:6.2f})  "
          f"w={ast[0]:5.2f}..{ast[1]:6.2f} deg/s (ratio {ast[2]:4.2f})  "
          f"clearance {'OK' if not bad else 'FAIL ' + str(bad[:4])}")
    doc = emit(defn, keys, {
        "path_length": round(total, 1),
        "linear_speed_units": [round(vmin, 2), round(vmax, 2)],
        "linear_ratio": round(float(vst[2]), 3),
        "angular_speed_deg_s": [round(float(ast[0]), 2), round(float(ast[1]), 2)],
        "angular_ratio": round(float(ast[2]), 3),
        "keyframe_rate_hz": RATE,
    })
    with open(os.path.join(OUT_DIR, defn.name + ".json"), "w") as f:
        json.dump(doc, f, indent=1)
print("ALL OK" if all_ok else "SOME FAILED - adjust and re-run")
print("written to", OUT_DIR)
