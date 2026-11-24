"""
Checks that the "VE Painted" node group computes the same albedo and roughness
as engine/shaders/standard_painted.frag (run after changing either one):

    pip install bpy pillow     # Blender as a Python module (matching Python version)
    python3 tools/blender/test_ve_painted.py

Bakes a strip of quads painted with known vertex colours (Cycles, CPU) and
compares against the shader's maths ported below. Prints the worst difference;
exit code 1 above 0.03. (The Principled BSDF's own diffuse colour pass is
0-4% below the base colour, depending on roughness, by its energy
conservation: that is the expected remainder, the same with no group.)
"""
import os, runpy, sys
import bpy
runpy.run_path(os.path.join(os.path.dirname(os.path.abspath(__file__)), "ve_painted_node_group.py"), run_name="__main__")

BASE, BASE_R = (0.6, 0.5, 0.4), 0.5
cases = [(0, 0, 0), (1, 0, 0), (0, 0.6, 0), (0, 0, 1), (0.7, 0.55, 0.4), (0.45, 1, 0.8)]

# --- the shader's maths (standard_painted.frag), in Python: keep in step with it ---
def clamp(v): return max(0.0, min(1.0, v))
def mix(a, b, t): return a + (b - a) * t
def mask(p, g, b, s): return clamp((p * (1 + b) - b * (1 - g) - 0.5) * s + 0.5)
def shade(rgb, breakup=0.0, grunge=0.5, sharp=3.0):
    dirtC, dirtR, wearC, wearR, wetD, wetR = (0.16, 0.12, 0.09), 0.95, (0.3, 0.2, 0.13), 0.85, 0.65, 0.2
    alb, r = list(BASE), BASE_R
    wear = mask(rgb[1], grunge, breakup, sharp)
    alb = [mix(a, c, wear) for a, c in zip(alb, wearC)]; r = mix(r, wearR, wear)
    dirt = mask(rgb[0], 1 - grunge, breakup, sharp)
    alb = [mix(a, c, dirt) for a, c in zip(alb, dirtC)]; r = mix(r, dirtR, dirt)
    wet = clamp(rgb[2] * (1 + breakup) - breakup * grunge)
    darken = wetD * mix(0.4, 1.0, r)
    alb = [a * (1 - darken * wet) for a in alb]; r = mix(r, wetR, wet)
    return alb, r

# --- a strip of quads, one per case, each painted one colour (float attribute: exact values) ---
n = len(cases)
verts, faces, uvs = [], [], []
for i in range(n):
    x0, x1 = i / n, (i + 1) / n
    k = len(verts)
    verts += [(x0, 0, 0), (x1, 0, 0), (x1, 1, 0), (x0, 1, 0)]
    faces.append((k, k + 1, k + 2, k + 3))
mesh = bpy.data.meshes.new("strip")
mesh.from_pydata(verts, [], faces)
uv = mesh.uv_layers.new(name="UVMap")
for li, loop in enumerate(mesh.loops):
    v = verts[loop.vertex_index]
    uv.data[li].uv = (v[0], v[1])
col = mesh.color_attributes.new("Paint", "FLOAT_COLOR", "CORNER")
for poly in mesh.polygons:
    for li in poly.loop_indices:
        c = cases[poly.index]
        col.data[li].color = (c[0], c[1], c[2], 1.0)
mesh.color_attributes.active_color = col
mesh.color_attributes.render_color_index = 0
obj = bpy.data.objects.new("strip", mesh)
bpy.context.scene.collection.objects.link(obj)
bpy.context.view_layer.objects.active = obj
obj.select_set(True)

mat = bpy.data.materials.new("test")
tree = mat.node_tree if bpy.app.version >= (5, 0, 0) else (setattr(mat, "use_nodes", True) or mat.node_tree)
tree.nodes.clear()
g = tree.nodes.new("ShaderNodeGroup"); g.node_tree = bpy.data.node_groups["VE Painted"]
g.inputs["Base Color"].default_value = (*BASE, 1); g.inputs["Roughness"].default_value = BASE_R
out = tree.nodes.new("ShaderNodeOutputMaterial")
tree.links.new(g.outputs["BSDF"], out.inputs["Surface"])
img = tree.nodes.new("ShaderNodeTexImage")
W = 8 * n
target = bpy.data.images.new("bake", W, 8, float_buffer=True)
target.colorspace_settings.name = "Non-Color"
img.image = target
tree.nodes.active = img
obj.data.materials.append(mat)

scene = bpy.context.scene
scene.render.engine = "CYCLES"
scene.cycles.samples = 1
scene.cycles.device = "CPU"

def bake(kind, breakup=0.0):
    g.inputs["Grunge Breakup"].default_value = breakup
    if kind == "DIFFUSE":
        scene.render.bake.use_pass_direct = False
        scene.render.bake.use_pass_indirect = False
        scene.render.bake.use_pass_color = True
    bpy.ops.object.bake(type=kind)
    px = list(target.pixels)
    at = lambda i: (4 * W + 8 * i + 4) * 4  # row 4, centre of quad i
    return [px[at(i): at(i) + 3] for i in range(n)]

worst = 0.0
for breakup in (0.0, 0.6):
    alb = bake("DIFFUSE", breakup)
    rough = bake("ROUGHNESS", breakup)
    for i, c in enumerate(cases):
        ea, er = shade(c, breakup)
        da = max(abs(a - b) for a, b in zip(alb[i], ea)); dr = abs(rough[i][0] - er)
        worst = max(worst, da, dr)
        print(f"breakup {breakup} paint {c}: albedo {[round(v,3) for v in alb[i]]} vs {[round(v,3) for v in ea]}, rough {rough[i][0]:.3f} vs {er:.3f}")
print("WORST DIFFERENCE", round(worst, 4))
sys.exit(0 if worst < 0.03 else 1)
