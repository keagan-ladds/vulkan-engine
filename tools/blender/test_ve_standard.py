"""
Checks the "VE Standard" node group, its .material export and "Convert to VE
Standard" (run after changing ve_painted_node_group.py or the engine's
standard material / translucency):

    blender -b --factory-startup --python tools/blender/test_ve_standard.py
    (or: pip install bpy; python3 tools/blender/test_ve_standard.py)

1. Bakes the group's diffuse colour (Cycles, CPU) with translucency 0.4: the
   front reflects albedo x (1 - t) (ve_lighting.glsl veStandardTranslucency).
2. Exports a material wired like a Poly Haven leaf (albedo + a separate grey
   alpha map, ORM, normal map, translucency map, uv scale) into a temporary
   assets folder and checks the file, and the merged <albedo>_rgba.png.
3. Converts a Principled BSDF mixed with a Translucent BSDF to VE Standard and
   checks what carried over.
Exit code 1 on any failure.
"""
import json, os, runpy, sys, tempfile
import bpy

ns = runpy.run_path(os.path.join(os.path.dirname(os.path.abspath(__file__)), "ve_painted_node_group.py"), run_name="__main__")
failures = []


def check(what, ok, detail=""):
    print(("ok   " if ok else "FAIL ") + what + (f": {detail}" if detail else ""))
    if not ok:
        failures.append(what)


def new_material(name):
    mat = bpy.data.materials.new(name)
    if bpy.app.version < (5, 0, 0):
        mat.use_nodes = True
    mat.node_tree.nodes.clear()
    return mat


# --- 1. diffuse colour with translucency --------------------------------------
BASE, T = (0.6, 0.5, 0.4), 0.4
mesh = bpy.data.meshes.new("quad")
mesh.from_pydata([(0, 0, 0), (1, 0, 0), (1, 1, 0), (0, 1, 0)], [], [(0, 1, 2, 3)])
uv = mesh.uv_layers.new(name="UVMap")
for li, loop in enumerate(mesh.loops):
    v = mesh.vertices[loop.vertex_index].co
    uv.data[li].uv = (v.x, v.y)
obj = bpy.data.objects.new("quad", mesh)
bpy.context.scene.collection.objects.link(obj)
bpy.context.view_layer.objects.active = obj
obj.select_set(True)
mat = new_material("bake")
tree = mat.node_tree
g = tree.nodes.new("ShaderNodeGroup")
g.node_tree = bpy.data.node_groups["VE Standard"]
g.inputs["Base Color"].default_value = (*BASE, 1)
g.inputs["Translucency"].default_value = T
out = tree.nodes.new("ShaderNodeOutputMaterial")
tree.links.new(g.outputs["BSDF"], out.inputs["Surface"])
img = tree.nodes.new("ShaderNodeTexImage")
target = bpy.data.images.new("bake", 8, 8, float_buffer=True)
target.colorspace_settings.name = "Non-Color"
img.image = target
tree.nodes.active = img
obj.data.materials.append(mat)
scene = bpy.context.scene
scene.render.engine = "CYCLES"
scene.cycles.samples = 1
scene.cycles.device = "CPU"
scene.render.bake.use_pass_direct = False
scene.render.bake.use_pass_indirect = False
scene.render.bake.use_pass_color = True


def bake_diffuse(translucency_color):
    # Cycles' diffuse colour pass sums the diffuse closures: Principled's
    # albedo x (1 - t) plus the Translucent BSDF's albedo x colour x t.
    g.inputs["Translucency Color"].default_value = (*translucency_color, 1)
    bpy.ops.object.bake(type="DIFFUSE")
    return list(target.pixels)[(4 * 8 + 4) * 4:(4 * 8 + 4) * 4 + 3]


px = bake_diffuse((0.0, 0.0, 0.0))  # nothing comes through: the front alone
front = [b * (1 - T) for b in BASE]
check("front reflects albedo x (1 - t)", max(abs(a - b) for a, b in zip(px, front)) < 0.03,
      f"baked {[round(v, 3) for v in px]}, expected {[round(v, 3) for v in front]}")
px = bake_diffuse((1.0, 1.0, 1.0))
check("front + through (white) = albedo", max(abs(a - b) for a, b in zip(px, BASE)) < 0.03,
      f"baked {[round(v, 3) for v in px]}, expected {list(BASE)}")

# --- 2. export ---------------------------------------------------------------
root = tempfile.mkdtemp(prefix="ve_assets_")
os.makedirs(os.path.join(root, "textures"))


def save_image(name, w, h, fill, non_color):
    im = bpy.data.images.new(name, w, h, alpha=True)
    if non_color:
        im.colorspace_settings.name = "Non-Color"
    im.pixels.foreach_set([c for i in range(w * h) for c in fill(i % w, i // w)])
    im.filepath_raw = os.path.join(root, "textures", name)
    im.file_format = "PNG"
    im.save()
    return im


W = 16
albedo_img = save_image("leaf_albedo.png", W, W, lambda x, y: (0.2, 0.5, 0.1, 1.0), False)
alpha_img = save_image("leaf_alpha.png", W, W, lambda x, y: ((1.0 if x >= W // 2 else 0.0),) * 3 + (1.0,), True)
orm_img = save_image("leaf_orm.png", W, W, lambda x, y: (1.0, 0.7, 0.0, 1.0), True)
nrm_img = save_image("leaf_normal.png", W, W, lambda x, y: (0.5, 0.5, 1.0, 1.0), True)
tmap_img = save_image("leaf_translucency_mask.png", W, W, lambda x, y: (0.8, 0.8, 0.8, 1.0), True)

mat = new_material("Leaf Test")
tree = mat.node_tree
g = tree.nodes.new("ShaderNodeGroup")
g.node_tree = bpy.data.node_groups["VE Standard"]
o = tree.nodes.new("ShaderNodeOutputMaterial")
tree.links.new(g.outputs["BSDF"], o.inputs["Surface"])
coord = tree.nodes.new("ShaderNodeTexCoord")
mapping = tree.nodes.new("ShaderNodeMapping")
mapping.inputs["Scale"].default_value = (2.0, 3.0, 1.0)
tree.links.new(coord.outputs["UV"], mapping.inputs["Vector"])


def image_node(im, to, output="Color"):
    n = tree.nodes.new("ShaderNodeTexImage")
    n.image = im
    tree.links.new(mapping.outputs["Vector"], n.inputs["Vector"])
    tree.links.new(n.outputs[output], g.inputs[to])
    return n


image_node(albedo_img, "Base Color")
image_node(alpha_img, "Alpha")  # a separate grey alpha map, from its Color output
image_node(orm_img, "ORM")
image_node(nrm_img, "Normal Map")
image_node(tmap_img, "Translucency Map")
g.inputs["Translucency"].default_value = 0.35
g.inputs["Translucency Color"].default_value = (1.0, 1.0, 0.7, 1.0)
g.inputs["Roughness"].default_value = 0.9
mat.ve_alpha_cutoff = 0.4

path, warnings = ns["export_material"](mat, root)
for w in warnings:
    print("     export warning:", w)
data = json.load(open(path, encoding="utf-8"))
check("file name from the material", os.path.basename(path) == "Leaf Test.material", path)
check("albedo = the merged RGBA", data.get("albedo") == "textures/leaf_albedo_rgba.png", data.get("albedo"))
check("mode Cutout (alpha used) at the panel's cutoff", data.get("mode") == "Cutout" and data.get("alphaCutoff") == 0.4,
      f"{data.get('mode')} {data.get('alphaCutoff')}")
check("baseColor white, opaque", data.get("baseColor") == [1.0, 1.0, 1.0, 1.0], data.get("baseColor"))
check("ORM + occlusion", data.get("metallicRoughness") == "textures/leaf_orm.png" and data.get("occlusion") == "textures/leaf_orm.png")
check("normal map", data.get("normal") == "textures/leaf_normal.png" and not any("Normal Map" in w for w in warnings))
check("uvScale from the Mapping node", data.get("uvScale") == [2.0, 3.0], data.get("uvScale"))
check("translucency", data.get("translucency") == 0.35 and data.get("translucencyColor") == [1.0, 1.0, 0.7],
      f"{data.get('translucency')} {data.get('translucencyColor')}")
check("translucency map (a *_mask: no warning)", data.get("translucencyTexture") == "textures/leaf_translucency_mask.png"
      and not any("Translucency Map" in w for w in warnings))
check("standard shader, no extra slots", data.get("fragmentShader") == "" and data.get("textures") == [] and data.get("params") == [])
merged = os.path.join(root, "textures", "leaf_albedo_rgba.png")
if os.path.isfile(merged):
    m = bpy.data.images.load(merged)
    p = list(m.pixels)
    left, right = p[(8 * W + 2) * 4: (8 * W + 2) * 4 + 4], p[(8 * W + W - 3) * 4: (8 * W + W - 3) * 4 + 4]
    check("merged alpha: 0 left, 1 right; RGB kept", left[3] < 0.01 and right[3] > 0.99 and abs(right[1] - 0.5) < 0.01,
          f"left {[round(v, 3) for v in left]}, right {[round(v, 3) for v in right]}")
else:
    check("merged RGBA written", False, merged)

# --- 3. convert a Principled + Translucent leaf -------------------------------
mat = new_material("Imported Leaf")
tree = mat.node_tree
p = tree.nodes.new("ShaderNodeBsdfPrincipled")
tr = tree.nodes.new("ShaderNodeBsdfTranslucent")
mix = tree.nodes.new("ShaderNodeMixShader")
o = tree.nodes.new("ShaderNodeOutputMaterial")
mix.inputs[0].default_value = 0.3
tree.links.new(p.outputs["BSDF"], mix.inputs[1])
tree.links.new(tr.outputs["BSDF"], mix.inputs[2])
tree.links.new(mix.outputs[0], o.inputs["Surface"])
alb = tree.nodes.new("ShaderNodeTexImage")
alb.image = albedo_img
tree.links.new(alb.outputs["Color"], p.inputs["Base Color"])
tree.links.new(alb.outputs["Color"], tr.inputs["Color"])
alpha = tree.nodes.new("ShaderNodeTexImage")
alpha.image = alpha_img
tree.links.new(alpha.outputs["Color"], p.inputs["Alpha"])
arm = tree.nodes.new("ShaderNodeTexImage")
arm.image = orm_img
sep = tree.nodes.new("ShaderNodeSeparateColor")
tree.links.new(arm.outputs["Color"], sep.inputs[0])
tree.links.new(sep.outputs[1], p.inputs["Roughness"])
nimg = tree.nodes.new("ShaderNodeTexImage")
nimg.image = nrm_img
nm = tree.nodes.new("ShaderNodeNormalMap")
nm.inputs["Strength"].default_value = 0.8
tree.links.new(nimg.outputs["Color"], nm.inputs["Color"])
tree.links.new(nm.outputs["Normal"], p.inputs["Normal"])
p.inputs["Metallic"].default_value = 0.0

notes = ns["convert_to_standard"](mat)
for n in notes:
    print("     convert note:", n)
g = ns["ve_node"](mat)
up = lambda name: ns["_upstream"](g.inputs[name])[0]
check("converted: a VE Standard node on the output", g is not None and ns["_upstream"](o.inputs["Surface"])[0] == g)
check("converted: base colour / alpha / ORM / normal images", up("Base Color") == alb and up("Alpha") == alpha and up("ORM") == arm
      and up("Normal Map") == nimg)
check("converted: normal strength, translucency 0.3, colour white",
      abs(g.inputs["Normal Strength"].default_value - 0.8) < 1e-4 and abs(g.inputs["Translucency"].default_value - 0.3) < 1e-4
      and tuple(g.inputs["Translucency Color"].default_value)[:3] == (1.0, 1.0, 1.0))
path, _ = ns["export_material"](mat, root)
data = json.load(open(path, encoding="utf-8"))
check("converted material exports", data.get("translucency") == 0.3 and data.get("mode") == "Cutout"
      and data.get("albedo") == "textures/leaf_albedo_rgba.png", f"{data.get('translucency')} {data.get('mode')} {data.get('albedo')}")

print("FAILURES:", len(failures))
sys.exit(1 if failures else 0)
