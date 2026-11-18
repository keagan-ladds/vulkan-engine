"""Blender add-on: generate vulkan-engine foliage (acacias, fever trees, baobabs,
bushes, jungle trees, palms, ferns) as editable objects, and export them for
the engine.

Install: Edit > Preferences > Add-ons > Install... > pick this file (it finds
treegen.py next to it; or set the path in the panel). Then View3D sidebar (N) >
"Foliage":
  - Species, seeds ("1, 2, 3": one variant each), spacing.
  - Generate: builds <Name>_LOD0/1/2 (+ _COL) objects per variant in a
    collection, with bark / leaf materials (alpha clip, double sided, normal
    map) that the glTF exporter turns into what the engine expects.
    Edit them as you like (move cards, sculpt the trunk, delete LODs...).
  - Export: the collection as .gltf (+ .bin, textures referenced in
    sandbox/assets/textures/foliage) into the models folder.

Headless (CI / batch):
    blender -b -P tools/foliage/blender_foliage.py -- --species acacia --seeds 1,2,3 --out sandbox/assets/models/foliage [--blend out.blend]

Names: the engine reads `<variant>_LOD<n>` levels and `<variant>_COL` collision
hulls (ve/world/LodModel.hpp); Blender's ".001" suffixes are ignored there.
"""

bl_info = {
    "name": "vulkan-engine Foliage",
    "author": "vulkan-engine",
    "version": (1, 0, 0),
    "blender": (3, 6, 0),
    "location": "View3D > Sidebar > Foliage",
    "description": "Procedural savanna / jungle trees and plants with LODs, exported for vulkan-engine",
    "category": "Add Mesh",
}

import os
import sys

import bpy

HERE = os.path.dirname(os.path.abspath(__file__))
if HERE not in sys.path:
    sys.path.insert(0, HERE)


def _treegen():
    import importlib
    import treegen
    return importlib.reload(treegen) if "treegen" in sys.modules and getattr(treegen, "_reload", False) else treegen


def default_texture_dir():
    return os.path.normpath(os.path.join(HERE, "..", "..", "sandbox", "assets", "textures", "foliage"))


def default_model_dir():
    return os.path.normpath(os.path.join(HERE, "..", "..", "sandbox", "assets", "models", "foliage"))


# -----------------------------------------------------------------------------
# Materials
# -----------------------------------------------------------------------------


def _image(path):
    for im in bpy.data.images:
        if os.path.normpath(bpy.path.abspath(im.filepath)) == os.path.normpath(path):
            return im
    return bpy.data.images.load(path, check_existing=True)


def make_material(kind, texture, tex_dir):
    name = f"{texture}"
    if name in bpy.data.materials:
        return bpy.data.materials[name]
    m = bpy.data.materials.new(name)
    m.use_nodes = True
    nt = m.node_tree
    bsdf = nt.nodes["Principled BSDF"]
    tex = nt.nodes.new("ShaderNodeTexImage")
    tex.location = (-600, 200)
    vcol = nt.nodes.new("ShaderNodeVertexColor")  # the generator's ambient occlusion / tint
    vcol.location = (-600, -100)
    mul = nt.nodes.new("ShaderNodeMixRGB")
    mul.blend_type = "MULTIPLY"
    mul.inputs[0].default_value = 1.0
    mul.location = (-300, 150)
    if kind == "bark":
        tex.image = _image(os.path.join(tex_dir, texture + "_albedo.png"))
        nrm = nt.nodes.new("ShaderNodeTexImage")
        nrm.image = _image(os.path.join(tex_dir, texture + "_normal.png"))
        nrm.image.colorspace_settings.name = "Non-Color"
        nrm.location = (-600, -350)
        nmap = nt.nodes.new("ShaderNodeNormalMap")
        nmap.location = (-300, -350)
        nt.links.new(nrm.outputs["Color"], nmap.inputs["Color"])
        nt.links.new(nmap.outputs["Normal"], bsdf.inputs["Normal"])
        bsdf.inputs["Roughness"].default_value = 0.92
        m.use_backface_culling = True  # (exported single-sided)
    else:
        tex.image = _image(os.path.join(tex_dir, texture + ".png"))
        nt.links.new(tex.outputs["Alpha"], bsdf.inputs["Alpha"])
        bsdf.inputs["Roughness"].default_value = 0.78
        m.blend_method = "CLIP"
        m.alpha_threshold = 0.45
        m.use_backface_culling = False
        try:
            m.shadow_method = "CLIP"
        except AttributeError:
            pass
    nt.links.new(tex.outputs["Color"], mul.inputs[1])
    nt.links.new(vcol.outputs["Color"], mul.inputs[2])
    nt.links.new(mul.outputs["Color"], bsdf.inputs["Base Color"])
    # Roughness from the metallic-roughness map (glTF: G).
    mr = nt.nodes.new("ShaderNodeTexImage")
    mr.image = _image(os.path.join(tex_dir, texture + "_mr.png"))
    mr.image.colorspace_settings.name = "Non-Color"
    mr.location = (-900, -650)
    sep = nt.nodes.new("ShaderNodeSeparateColor")
    sep.location = (-600, -650)
    nt.links.new(mr.outputs["Color"], sep.inputs["Color"])
    nt.links.new(sep.outputs["Green"], bsdf.inputs["Roughness"])
    if kind != "bark":
        # Leaf normal map, and light shining through (a Translucent BSDF mixed in;
        # exported as KHR_materials_diffuse_transmission by link_textures()).
        nrm = nt.nodes.new("ShaderNodeTexImage")
        nrm.image = _image(os.path.join(tex_dir, texture + "_normal.png"))
        nrm.image.colorspace_settings.name = "Non-Color"
        nrm.location = (-900, -350)
        nmap = nt.nodes.new("ShaderNodeNormalMap")
        nmap.location = (-600, -350)
        nt.links.new(nrm.outputs["Color"], nmap.inputs["Color"])
        nt.links.new(nmap.outputs["Normal"], bsdf.inputs["Normal"])
        strength, tint = _treegen().LEAF_TRANSLUCENCY.get(texture, (0.5, (1.0, 1.0, 0.8)))
        trans = nt.nodes.new("ShaderNodeBsdfTranslucent")
        trans.location = (0, -300)
        nt.links.new(mul.outputs["Color"], trans.inputs["Color"])
        mixs = nt.nodes.new("ShaderNodeMixShader")
        mixs.location = (300, 0)
        mixs.inputs["Fac"].default_value = strength * 0.4
        out = nt.nodes["Material Output"]
        nt.links.new(bsdf.outputs["BSDF"], mixs.inputs[1])
        nt.links.new(trans.outputs["BSDF"], mixs.inputs[2])
        nt.links.new(mixs.outputs["Shader"], out.inputs["Surface"])
    return m


# -----------------------------------------------------------------------------
# Meshes -> objects
# -----------------------------------------------------------------------------


def mesh_object(name, parts, materials, collection, location):
    """parts: {material key: treegen.MeshBuf} -> one object with a material slot per part."""
    verts, faces, uvs, cols, norms, mat_idx = [], [], [], [], [], []
    slots = []
    for key, buf in parts.items():
        if buf is None or buf.empty():
            continue
        o = len(verts)
        slot = len(slots)
        slots.append(key)
        verts += buf.p
        norms += buf.n
        for k in range(0, len(buf.i), 3):
            faces.append((buf.i[k] + o, buf.i[k + 1] + o, buf.i[k + 2] + o))
            mat_idx.append(slot)
        uvs += buf.uv
        cols += buf.c
    if not faces:
        return None
    # treegen is Y-up (glTF); Blender is Z-up: (x, y, z) -> (x, -z, y).
    conv = lambda v: (v[0], -v[2], v[1])
    me = bpy.data.meshes.new(name)
    me.from_pydata([conv(v) for v in verts], [], faces)
    me.update()
    # Create every attribute first, then look them up again: adding an attribute
    # can reallocate the others (stale references would write into the wrong one).
    me.uv_layers.new(name="UVMap")
    if materials is not None:
        me.color_attributes.new(name="Color", type="FLOAT_COLOR", domain="CORNER")
    uv = me.uv_layers["UVMap"]
    loop_vi = [0] * len(me.loops)
    me.loops.foreach_get("vertex_index", loop_vi)
    uv.data.foreach_set("uv", [c for vi in loop_vi for c in (uvs[vi][0], 1.0 - uvs[vi][1])])  # (glTF v down, Blender v up)
    if materials is not None:
        me.color_attributes["Color"].data.foreach_set("color", [c for vi in loop_vi for c in cols[vi]])
    me.polygons.foreach_set("material_index", mat_idx)
    me.polygons.foreach_set("use_smooth", [True] * len(me.polygons))
    me.normals_split_custom_set_from_vertices([conv(n) for n in norms])
    if hasattr(me, "use_auto_smooth"):
        me.use_auto_smooth = True
    if materials is not None:
        for key in slots:
            me.materials.append(materials[key])
    ob = bpy.data.objects.new(name, me)
    ob.location = location
    collection.objects.link(ob)
    if materials is None:
        ob.display_type = "WIRE"
        ob.hide_render = True
    return ob


def generate(species, seeds, spacing=15.0, tex_dir=None):
    tg = _treegen()
    tex_dir = tex_dir or default_texture_dir()
    spec = tg.SPECIES[species]
    col = bpy.data.collections.get(species) or bpy.data.collections.new(species)
    if col.name not in bpy.context.scene.collection.children:
        bpy.context.scene.collection.children.link(col)
    for ob in list(col.objects):
        bpy.data.objects.remove(ob, do_unlink=True)
    mats = {"bark": make_material("bark", spec["bark"], tex_dir), "leaves": make_material("leaves", spec["leaves"], tex_dir)}
    for k, seed in enumerate(seeds):
        p = tg.generate(species, seed)
        loc = (spacing * k, 0.0, 0.0)
        for lvl, parts in enumerate(p.lods):
            ob = mesh_object(f"{p.name}_LOD{lvl}", parts, mats, col, loc)
            if ob and lvl > 0:
                ob.hide_set(True) if bpy.context.view_layer.objects.get(ob.name) else None
        if p.collider is not None and not p.collider.empty():
            mesh_object(f"{p.name}_COL", {"bark": p.collider}, None, col, loc)
    return col


def link_textures(path, tex_rel="../../textures/foliage"):
    """Points the exported materials at the shared foliage textures (named after the
    materials: <bark>_albedo / _normal.png, <leaves>.png) instead of copies."""
    import json
    with open(path) as f:
        doc = json.load(f)
    doc["images"], doc["textures"] = [], []
    doc["samplers"] = [{"magFilter": 9729, "minFilter": 9987, "wrapS": 10497, "wrapT": 10497}]

    def tex(uri):
        doc["images"].append({"uri": uri})
        doc["textures"].append({"source": len(doc["images"]) - 1, "sampler": 0})
        return {"index": len(doc["textures"]) - 1}

    used = set(doc.get("extensionsUsed", []))
    for m in doc.get("materials", []):
        pbr = m.setdefault("pbrMetallicRoughness", {})
        name = m["name"].split(".")[0]
        if m.get("alphaMode") == "MASK":
            pbr["baseColorTexture"] = tex(f"{tex_rel}/{name}.png")
            strength, tint = _treegen().LEAF_TRANSLUCENCY.get(name, (0.5, (1.0, 1.0, 0.8)))
            m.setdefault("extensions", {})["KHR_materials_diffuse_transmission"] = {
                "diffuseTransmissionFactor": strength, "diffuseTransmissionColorFactor": list(tint)}
            used.add("KHR_materials_diffuse_transmission")
        else:
            pbr["baseColorTexture"] = tex(f"{tex_rel}/{name}_albedo.png")
            m.pop("doubleSided", None)
        m["normalTexture"] = tex(f"{tex_rel}/{name}_normal.png")
        pbr["metallicRoughnessTexture"] = tex(f"{tex_rel}/{name}_mr.png")
        pbr["roughnessFactor"] = 1.0
        pbr["metallicFactor"] = 0.0
        pbr.pop("baseColorFactor", None)
    if used:
        doc["extensionsUsed"] = sorted(used)
    with open(path, "w") as f:
        json.dump(doc, f, indent=1)


def export(collection, path):
    """The collection's objects as glTF (separate .bin, textures referenced, not copied)."""
    bpy.ops.object.select_all(action="DESELECT")
    for ob in collection.objects:
        ob.hide_set(False)
        ob.select_set(True)
    os.makedirs(os.path.dirname(path), exist_ok=True)
    kw = dict(filepath=path, export_format="GLTF_SEPARATE", use_selection=True, export_yup=True, export_apply=True,
              export_texcoords=True, export_normals=True, export_materials="EXPORT", export_image_format="NONE")
    try:
        bpy.ops.export_scene.gltf(export_vertex_color="ACTIVE", **kw)  # Blender 4.2+
    except TypeError:
        bpy.ops.export_scene.gltf(export_colors=True, **kw)
    link_textures(path)


# -----------------------------------------------------------------------------
# UI
# -----------------------------------------------------------------------------


def _species_items(self, context):
    try:
        return [(k, k.replace("_", " ").title(), "") for k in _treegen().SPECIES]
    except Exception as e:  # (treegen missing)
        return [("acacia", "Acacia", str(e))]


class FoliageProps(bpy.types.PropertyGroup):
    species: bpy.props.EnumProperty(name="Species", items=_species_items)
    seeds: bpy.props.StringProperty(name="Seeds", default="1, 2, 3", description="One variant per seed")
    spacing: bpy.props.FloatProperty(name="Spacing", default=15.0, min=0.0)
    texture_dir: bpy.props.StringProperty(name="Textures", subtype="DIR_PATH", default="")
    model_dir: bpy.props.StringProperty(name="Export to", subtype="DIR_PATH", default="")


class FOLIAGE_OT_generate(bpy.types.Operator):
    bl_idname = "foliage.generate"
    bl_label = "Generate"
    bl_description = "Build the species' variants (LOD0..2 + collision) as objects"

    def execute(self, context):
        p = context.scene.foliage
        seeds = [int(s) for s in p.seeds.replace(",", " ").split() if s.strip().lstrip("-").isdigit()]
        generate(p.species, seeds or [1], p.spacing, bpy.path.abspath(p.texture_dir) if p.texture_dir else None)
        return {"FINISHED"}


class FOLIAGE_OT_export(bpy.types.Operator):
    bl_idname = "foliage.export"
    bl_label = "Export for the engine"
    bl_description = "Write the species' collection as <species>.gltf into the models folder"

    def execute(self, context):
        p = context.scene.foliage
        col = bpy.data.collections.get(p.species)
        if not col:
            self.report({"ERROR"}, "Generate it first")
            return {"CANCELLED"}
        out = bpy.path.abspath(p.model_dir) if p.model_dir else default_model_dir()
        export(col, os.path.join(out, p.species + ".gltf"))
        self.report({"INFO"}, f"Exported {p.species}.gltf")
        return {"FINISHED"}


class FOLIAGE_PT_panel(bpy.types.Panel):
    bl_space_type = "VIEW_3D"
    bl_region_type = "UI"
    bl_category = "Foliage"
    bl_label = "vulkan-engine Foliage"

    def draw(self, context):
        p = context.scene.foliage
        col = self.layout.column()
        col.prop(p, "species")
        col.prop(p, "seeds")
        col.prop(p, "spacing")
        col.prop(p, "texture_dir")
        col.operator("foliage.generate", icon="OUTLINER_OB_FORCE_FIELD")
        col.separator()
        col.prop(p, "model_dir")
        col.operator("foliage.export", icon="EXPORT")


CLASSES = (FoliageProps, FOLIAGE_OT_generate, FOLIAGE_OT_export, FOLIAGE_PT_panel)


def register():
    for c in CLASSES:
        bpy.utils.register_class(c)
    bpy.types.Scene.foliage = bpy.props.PointerProperty(type=FoliageProps)


def unregister():
    del bpy.types.Scene.foliage
    for c in reversed(CLASSES):
        bpy.utils.unregister_class(c)


# -----------------------------------------------------------------------------
# Headless: blender -b -P blender_foliage.py -- --species acacia --seeds 1,2 --out dir [--blend file]
# -----------------------------------------------------------------------------

if __name__ == "__main__" and "--" in sys.argv:
    import argparse
    ap = argparse.ArgumentParser()
    ap.add_argument("--species", default="acacia")
    ap.add_argument("--seeds", default="1,2,3")
    ap.add_argument("--out", default=default_model_dir())
    ap.add_argument("--textures", default=None)
    ap.add_argument("--blend", default=None)
    a = ap.parse_args(sys.argv[sys.argv.index("--") + 1:])
    bpy.ops.wm.read_factory_settings(use_empty=True)
    c = generate(a.species, [int(s) for s in a.seeds.split(",")], tex_dir=a.textures)
    export(c, os.path.join(a.out, a.species + ".gltf"))
    if a.blend:
        bpy.ops.wm.save_as_mainfile(filepath=os.path.abspath(a.blend))
    print(f"exported {a.species} ({len(c.objects)} objects)")
elif __name__ == "__main__":
    register()
