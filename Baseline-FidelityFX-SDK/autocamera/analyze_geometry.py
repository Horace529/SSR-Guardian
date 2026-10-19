"""Hangar scene geometry analysis: voxel occupancy + interior free space.

Reads media/Hangar/Hangar_1105.gltf + .bin, transforms all mesh vertices to
world space, rasterizes triangles into a voxel grid, flood-fills the interior
air volume from the start camera position, and reports free-space
cross-sections used to design collision-free camera paths.
"""
import json, sys
import numpy as np
from collections import defaultdict

GLTF = r"D:\Projects\SSR-Guardian\Baseline-FidelityFX-SDK\media\Hangar\Hangar_1105.gltf"
BIN  = GLTF.replace(".gltf", ".bin")

CELL = 2.0          # voxel size (world units)
CLEARANCE = 4.0     # required clearance for the camera from occupied voxels
START_CAM = np.array([-15.3, 18.6, -95.8])   # Hangar_Camera_000 position

d = json.load(open(GLTF, encoding="utf-8-sig"))
nodes, meshes, accessors, views = d["nodes"], d["meshes"], d["accessors"], d["bufferViews"]
binbuf = np.fromfile(BIN, dtype=np.uint8)
print(f"bin: {len(binbuf)/1e6:.1f} MB, nodes={len(nodes)}, meshes={len(meshes)}")

# ---- world transforms (same math as ffx_gltf_analyze.py) ----
def mul(a, b):
    return np.einsum('ij,jk->ik', a, b)
def trs(t, q, s):
    x,y,z,w = q
    R = np.array([
        [1-2*(y*y+z*z), 2*(x*y-z*w),   2*(x*z+y*w)],
        [2*(x*y+z*w),   1-2*(x*x+z*z), 2*(y*z-x*w)],
        [2*(x*z-y*w),   2*(y*z+x*w),   1-2*(x*x+y*y)]])
    M = np.eye(4); M[:3,:3] = R * np.array(s)[None,:]; M[:3,3] = t
    return M
def from_node(n):
    if "matrix" in n:
        m = np.array(n["matrix"], dtype=np.float64).reshape(4,4).T  # column-major
        return m
    return trs(n.get("translation",[0,0,0]), n.get("rotation",[0,0,0,1]), n.get("scale",[1,1,1]))

has_parent = set()
for n in nodes: has_parent.update(n.get("children", []))
children = defaultdict(list)
for i, n in enumerate(nodes):
    for c in n.get("children", []): children[i].append(c)
roots = [i for i in range(len(nodes)) if i not in has_parent]
world = {}
def visit(i, pw):
    w = mul(pw, from_node(nodes[i])); world[i] = w
    for c in children[i]: visit(c, w)
for r in roots: visit(r, np.eye(4))

def accessor_pos(ai):
    a = accessors[ai]; v = views[a["bufferView"]]
    off = v.get("byteOffset",0) + a.get("byteOffset",0)
    stride = v.get("byteStride", 12) or 12
    cnt = a["count"]
    raw = binbuf[off: off + cnt*stride].reshape(cnt, stride)
    return raw[:, :12].copy().view(np.float32).reshape(cnt, 3).astype(np.float64)

def accessor_idx(ai):
    a = accessors[ai]; v = views[a["bufferView"]]
    off = v.get("byteOffset",0) + a.get("byteOffset",0)
    cnt = a["count"]
    ct = a["componentType"]
    if ct == 5125: return np.frombuffer(binbuf[off:off+cnt*4].tobytes(), np.uint32).astype(np.int64)
    if ct == 5123: return np.frombuffer(binbuf[off:off+cnt*2].tobytes(), np.uint16).astype(np.int64)
    return None

# ---- collect world-space triangles (skip tiny props below 0.5 unit scale for speed? keep all) ----
all_tris = []   # list of (N,3,3) arrays
for i, n in enumerate(nodes):
    mi = n.get("mesh")
    if mi is None: continue
    W = world[i]
    for prim in meshes[mi]["primitives"]:
        pai = prim.get("attributes", {}).get("POSITION")
        iai = prim.get("indices")
        if pai is None: continue
        P = accessor_pos(pai)
        Pw = P @ W[:3,:3].T + W[:3,3]
        if iai is not None:
            idx = accessor_idx(iai).reshape(-1, 3)
        else:
            idx = np.arange(len(P), dtype=np.int64).reshape(-1, 3)
        if mode_tri := (prim.get("mode", 4) != 4):  # non-triangle-list: skip
            continue
        all_tris.append(Pw[idx])
tris = np.concatenate(all_tris)
print(f"triangles: {len(tris)/1e6:.2f} M")

# ---- grid bounds: focus on hangar interior ROI (exclude far exterior wings) ----
ROI_MIN = np.array([-160.0, -20.0, -190.0])
ROI_MAX = np.array([ 160.0, 170.0,  400.0])
shape = np.ceil((ROI_MAX - ROI_MIN) / CELL).astype(int) + 1
print(f"grid {shape} = {np.prod(shape)/1e6:.1f} M voxels")

occ = np.zeros(shape, dtype=bool)
def vox_index(points):
    g = np.floor((points - ROI_MIN) / CELL).astype(int)
    ok = np.all((g >= 0) & (g < shape[None,:]), axis=1)
    return g[ok]

# rasterize triangles: barycentric sampling at ~CELL density
ab = tris[:,1] - tris[:,0]; ac = tris[:,2] - tris[:,0]
elen = np.maximum(np.linalg.norm(ab, axis=1), np.linalg.norm(ac, axis=1))
nsub = np.ceil(np.maximum(elen, 1e-3) / CELL).astype(int)
nsub = np.minimum(nsub, 400)
counts = (nsub + 1)**2
print(f"total samples ~{counts.sum()/1e6:.1f} M (max/tri {counts.max()})")
gidx = []
# barycentric grid sampling: one pass per (i,j) over the triangles subdivided at least that deep
for i in range(int(nsub.max()) + 1):
    rowsel = np.nonzero(nsub >= i)[0]
    if len(rowsel) == 0: continue
    jmax = int(nsub[rowsel].max())
    for j in range(jmax + 1):
        colsel = rowsel[nsub[rowsel] >= j]
        if len(colsel) == 0: continue
        # skip degenerate weights > 1
        u = i / nsub[colsel]; v = j / nsub[colsel]
        m = (u + v) <= 1.0
        if not m.any(): continue
        c = colsel[m]
        pts = tris[c,0] + ab[c]*u[m,None] + ac[c]*v[m,None]
        gidx.append(vox_index(pts))
    # (double loop over max subdiv is fine: only large triangles go deep)
G = np.concatenate(gidx)
occ[G[:,0], G[:,1], G[:,2]] = True
del gidx, G
print(f"occupied voxels: {occ.sum()/1e6:.2f} M, fill {occ.mean()*100:.2f}%")

# ---- clearance dilation & interior flood fill ----
from scipy import ndimage
clear_cells = int(round(CLEARANCE / CELL))
struct = ndimage.generate_binary_structure(3, 1)
blocked = ndimage.binary_dilation(occ, structure=struct, iterations=clear_cells)
free = ~blocked
labels, nlab = ndimage.label(free, structure=struct)
start_v = np.floor((START_CAM - ROI_MIN) / CELL).astype(int)
start_v = np.clip(start_v, 0, np.array(shape)-1)
lab = labels[tuple(start_v)]
interior = labels == lab
print(f"interior label {lab}/{nlab}: {interior.sum()/1e6:.2f} M voxels ({interior.mean()*100:.1f}% of ROI)")

np.savez_compressed(r"C:\Users\test\AppData\Local\Temp\ffx_hangar_voxel.npz",
                    occ=occ, interior=interior, roi_min=ROI_MIN, cell=CELL)
print("saved voxel data")

# ---- free-space cross sections for path design ----
def free_bounds_x(z0, z1, y0, y1):
    """free x-interval(s) at height/depth band inside interior"""
    sub = interior[:, int((y0-ROI_MIN[1])/CELL):int((y1-ROI_MIN[1])/CELL)+1,
                        int((z0-ROI_MIN[2])/CELL):int((z1-ROI_MIN[2])/CELL)+1]
    cols = np.nonzero(sub.any(axis=(1,2)))[0]
    if len(cols) == 0: return None
    # find contiguous runs (report widest 2)
    runs, s = [], cols[0]
    for a, b in zip(cols, cols[1:]):
        if b - a > 1: runs.append((s, a)); s = b
    runs.append((s, cols[-1]))
    runs.sort(key=lambda r: r[1]-r[0], reverse=True)
    return [(ROI_MIN[0]+r[0]*CELL, ROI_MIN[0]+r[1]*CELL) for r in runs[:3]]

print("\n=== interior free x-extents (world units) by z-band at low altitude y=2..8 ===")
for z0, z1 in [(-150,-100),(-100,-50),(-50,0),(0,50),(50,100),(100,150),(150,200),(200,250),(250,300),(300,350)]:
    r = free_bounds_x(z0, z1, 2, 8)
    print(f"  z[{z0:4d},{z1:4d}]: {r}")

print("\n=== interior free y-extents by z-band, central aisle x[-60,60] ===")
for z0, z1 in [(-150,-100),(-100,-50),(-50,0),(0,50),(50,100),(100,150),(150,200),(200,250),(250,300)]:
    sub = interior[int((-60-ROI_MIN[0])/CELL):int((60-ROI_MIN[0])/CELL)+1,
                        :,
                        int((z0-ROI_MIN[2])/CELL):int((z1-ROI_MIN[2])/CELL)+1]
    ys = np.nonzero(sub.any(axis=(0,2)))[0]
    if len(ys):
        print(f"  z[{z0:4d},{z1:4d}]: y {ROI_MIN[1]+ys[0]*CELL:6.1f} .. {ROI_MIN[1]+ys[-1]*CELL:6.1f}")

print("\n=== central obstruction (occupied fraction in x[-40,40]) by y-band x z ===")
for y0, y1 in [(0,10),(10,30),(30,60),(60,90),(90,120),(120,150)]:
    sub = occ[int((-40-ROI_MIN[0])/CELL):int((40-ROI_MIN[0])/CELL)+1,
                   int((y0-ROI_MIN[1])/CELL):int((y1-ROI_MIN[1])/CELL)+1,
                   int((-150-ROI_MIN[2])/CELL):int((300-ROI_MIN[2])/CELL)+1]
    print(f"  y[{y0:3d},{y1:3d}]: occupied {sub.mean()*100:5.1f}%")

# where is the big central mass (the ship / central structure)?
print("\n=== occupancy centroid slices (x-center of mass of occ, central band) ===")
for z in range(-140, 340, 40):
    sub = occ[:, :, int((z-ROI_MIN[2])/CELL)]
    ys, xs = np.nonzero(sub.T)
    if len(xs):
        print(f"  z={z:4d}: occ x-center {ROI_MIN[0]+xs.mean()*CELL:7.1f}, spread {xs.std()*CELL:5.1f}, occ voxels {len(xs)}")
