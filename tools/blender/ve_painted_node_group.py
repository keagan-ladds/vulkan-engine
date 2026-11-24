"""
VE Painted - a Blender node group that looks like the engine's
standard_painted.frag, so vertex-painted dirt / wear / wetness can be judged in
Blender while painting (Material Preview or Rendered view, Eevee or Cycles).

Run once per .blend (Blender 4.0 or newer): open the Scripting workspace, open
this file in the Text Editor and press Run Script (Alt+P). It creates (or
rebuilds, so it's safe to run again after updating the script):

  * node group "VE Painted": one node with the shader's inputs, outputting a
    BSDF (plus the three masks, handy to look at while painting);
  * material "VE Painted Example": the group wired up the way the engine
    samples things (see "Wiring" below). Copy it, or copy its nodes.

Exporting the engine's material file
------------------------------------
Properties editor > Material tab > "VE Engine" panel > Export VE material
writes assets/materials/<material name>.material from the material's VE
Painted node (values, images, the overlay scale and the UV Mapping scale), so
the engine replaces the exported glTF material with the same look. The
assets folder is found above the .blend (or set it in the panel). Export the
model itself as glTF Separate with Data > Mesh > Vertex Color = Active (the
paint is in the vertex colours; "Material" may drop it, the group hides the
Color Attribute node). Images must be files under assets/ (not packed).

Always available: install this file as an add-on (Preferences > Add-ons >
Install from Disk), then the panel is in every file; its "Create / update VE
Painted" button builds the node group in the current file.

To reuse it in every project: save this .blend somewhere permanent, right-click
the group in the Asset Browser (it is marked as an asset by this script) or add
the .blend's folder under Preferences > File Paths > Asset Libraries, then drag
"VE Painted" into any material. Linking (instead of appending) keeps all your
files on the latest version of the group.

The paint (exactly as in the engine)
------------------------------------
The mesh's active Color Attribute (Vertex Paint mode):
  R = dirt, G = wear (the material underneath shows through), B = wetness.
  0 = clean, 1 = full. Fill the attribute with BLACK before painting: in the
  engine a mesh without vertex colours counts as white (fully painted).
  Painting values are linear in both Blender and the engine (a brush value of
  0.5 in a Byte Color attribute stores ~0.21); use a float Color attribute if
  you want exact numbers.

Inputs = the material file's values
-----------------------------------
  Base Color / Roughness / Metallic          "baseColor", "roughness", "metallic" (x the textures)
  ORM                                        "metallicRoughness" texture (G roughness, B metallic; Non-Color)
  Normal Map / Normal Strength               "normal" texture (Non-Color), "normalStrength"
  Baked Normal / Baked Normal Strength       textures[8] (Non-Color), params[9].x: a per-asset normal
                                             map (damage baked in Blender) on the mesh's own UVs (not
                                             uvScale), whiteout-blended over the tiled Normal Map"
  Alpha                                      base colour alpha (Cutout / Transparent modes)
  Dirt / Wear / Wet Strength, Grunge Breakup params[4] x, y, z, w
  Dirt Color, Dirt Roughness                 params[5] rgb, w   (linear colour, as Blender stores it)
  Wear Color, Wear Roughness                 params[6] rgb, w
  Edge Sharpness, Wet Darkening, Wet Roughness  params[7] y, z, w
  Invert Dirt / Wear / Wet                   params[8] x, y, z
  Grunge                                     textures[5..7]: see Wiring
  Dirt Texture, Wear Texture                 textures[5], textures[7] (white = no texture)
The overlay scale, params[7].x, is the "Overlay Scale" value node in the
example material (it drives the texture coordinates outside the group).

Wiring (as in the example material)
-----------------------------------
  * Albedo, normal and ORM images: the mesh's UVs. The engine's uvScale
    (params[3].xy) = a Mapping node's scale on the UVs.
  * Grunge, dirt and wear images: WORLD position x Overlay Scale, image node
    projection set to Box (blend ~0.3). That is Blender's version of the
    shader's world-space triplanar sampling: the overlays continue across
    modular pieces and don't depend on UVs.
  * Colour images as sRGB, everything else Non-Color (the engine decides by
    the image's .texture file / name; keep the two in agreement).
  * Grunge Breakup only applies when a grunge image is connected (the engine
    uses no breakup without one): leave it at 0 otherwise.

Not reproduced (small differences)
----------------------------------
  * The 15% ambient-occlusion darkening under dirt (Principled has no AO input).
  * The material's occlusion texture and emission (add them to the Principled
    node inside a copy if you need them).
  * Lighting: the engine's sky, ambient, exposure and grading differ from your
    Blender scene. Use the AgX view transform, one sun and a plain world colour
    to get close; judge final looks in the engine (material files reload live).
"""

bl_info = {
    "name": "VE Painted (vulkan-engine materials)",
    "author": "vulkan-engine",
    "version": (1, 1, 0),
    "blender": (4, 0, 0),
    "location": "Properties > Material > VE Engine",
    "description": "The engine's painted shader as a node group, and export of its .material files",
    "category": "Material",
}

import bpy

GROUP_NAME = "VE Painted"
EXAMPLE_MATERIAL = "VE Painted Example"
MARK_AS_ASSET = True


# --- Helpers -----------------------------------------------------------------

def socket(sockets, identifier_or_name):
    """A node socket by identifier (Mix nodes have several 'A' sockets), else by name."""
    for s in sockets:
        if s.identifier == identifier_or_name:
            return s
    for s in sockets:
        if s.name == identifier_or_name and s.enabled:
            return s
    raise KeyError(identifier_or_name)


class Builder:
    """Places nodes left to right in columns and links them."""

    def __init__(self, tree):
        self.tree = tree
        self.nodes = tree.nodes
        self.links = tree.links

    def node(self, kind, x, y, label=None, **props):
        n = self.nodes.new(kind)
        n.location = (x, y)
        if label:
            n.label = label
        for key, value in props.items():
            setattr(n, key, value)
        return n

    def link(self, out, inp):
        self.links.new(out, inp)

    def math(self, op, a, b=None, x=0, y=0, clamp=False, label=None, c=None):
        n = self.node("ShaderNodeMath", x, y, label=label, operation=op, use_clamp=clamp)
        n.hide = True
        for i, v in enumerate((a, b, c)):
            if v is None:
                continue
            if isinstance(v, (int, float)):
                n.inputs[i].default_value = v
            else:
                self.link(v, n.inputs[i])
        return n.outputs[0]

    def mix(self, data_type, factor, a, b, x=0, y=0, label=None, blend="MIX"):
        n = self.node("ShaderNodeMix", x, y, label=label, data_type=data_type)
        if data_type == "RGBA":
            n.blend_type = blend
        suffix = {"FLOAT": "Float", "RGBA": "Color", "VECTOR": "Vector"}[data_type]
        n.hide = True
        for ident, v in (("Factor_Float", factor), ("A_" + suffix, a), ("B_" + suffix, b)):
            target = socket(n.inputs, ident)
            if isinstance(v, (int, float)):
                target.default_value = v
            elif isinstance(v, tuple):
                target.default_value = v
            else:
                self.link(v, target)
        return socket(n.outputs, "Result_" + suffix)


def _find(group, item_type, name, in_out=None):
    for item in group.interface.items_tree:
        if item.item_type == item_type and item.name == name and (in_out is None or item.in_out == in_out):
            return item
    return None


def add_input(group, name, socket_type, default=None, lo=None, hi=None, factor=False):
    """The input socket `name`: kept if it exists (so materials using the group
    keep their values and links when the script is run again), else created."""
    s = _find(group, "SOCKET", name, "INPUT")
    if s is not None and s.socket_type != socket_type:
        group.interface.remove(s)
        s = None
    if s is None:
        s = group.interface.new_socket(name=name, in_out="INPUT", socket_type=socket_type)
    if default is not None:
        s.default_value = default  # (the default for new nodes; existing nodes keep theirs)
    if lo is not None:
        s.min_value = lo
    if hi is not None:
        s.max_value = hi
    if factor and hasattr(s, "subtype"):
        try:
            s.subtype = "FACTOR"
        except (TypeError, AttributeError):
            pass
    return s


def add_output(group, name, socket_type):
    s = _find(group, "SOCKET", name, "OUTPUT")
    if s is None or s.socket_type != socket_type:
        if s is not None:
            group.interface.remove(s)
        s = group.interface.new_socket(name=name, in_out="OUTPUT", socket_type=socket_type)
    return s


def add_panel(group, name):
    # Panels (Blender 4.0+) fold the long input list into sections.
    if not hasattr(group.interface, "new_panel"):
        return None
    return _find(group, "PANEL", name) or group.interface.new_panel(name)


def move_into(group, panel, names):
    if panel is None:
        return
    for name in names:
        item = _find(group, "SOCKET", name, "INPUT")
        if item is not None and item.parent != panel:
            group.interface.move_to_parent(item, panel, len(panel.interface_items))


def remove_unused(group, keep):
    """Drops sockets and panels this version of the script no longer makes."""
    for item in list(group.interface.items_tree):
        if item.item_type == "SOCKET" and (item.in_out, item.name) not in keep:
            group.interface.remove(item)
    for item in list(group.interface.items_tree):
        if item.item_type == "PANEL" and ("PANEL", item.name) not in keep:
            group.interface.remove(item)


# --- The node group ----------------------------------------------------------

def build_group():
    group = bpy.data.node_groups.get(GROUP_NAME)
    if group is None:
        group = bpy.data.node_groups.new(GROUP_NAME, "ShaderNodeTree")
    group.nodes.clear()  # (the inside is rebuilt; the interface is updated in place, see add_input)
    group.use_fake_user = True
    group.description = "The engine's standard_painted.frag: vertex-painted dirt (R), wear (G), wetness (B)"

    # Inputs, named after the material file's fields.
    add_input(group, "Base Color", "NodeSocketColor", (0.8, 0.8, 0.8, 1.0))
    add_input(group, "Alpha", "NodeSocketFloat", 1.0, 0.0, 1.0, factor=True)
    add_input(group, "Roughness", "NodeSocketFloat", 0.8, 0.0, 1.0, factor=True)
    add_input(group, "Metallic", "NodeSocketFloat", 0.0, 0.0, 1.0, factor=True)
    add_input(group, "ORM", "NodeSocketColor", (1.0, 1.0, 1.0, 1.0))
    add_input(group, "Normal Map", "NodeSocketColor", (0.5, 0.5, 1.0, 1.0))
    add_input(group, "Normal Strength", "NodeSocketFloat", 1.0, 0.0, 4.0)
    add_input(group, "Baked Normal", "NodeSocketColor", (0.5, 0.5, 1.0, 1.0))
    add_input(group, "Baked Normal Strength", "NodeSocketFloat", 1.0, 0.0, 4.0)
    paint = ["Dirt Strength", "Wear Strength", "Wet Strength", "Grunge Breakup", "Edge Sharpness"]
    add_input(group, "Dirt Strength", "NodeSocketFloat", 1.0, 0.0, 1.0, factor=True)
    add_input(group, "Wear Strength", "NodeSocketFloat", 1.0, 0.0, 1.0, factor=True)
    add_input(group, "Wet Strength", "NodeSocketFloat", 1.0, 0.0, 1.0, factor=True)
    add_input(group, "Grunge Breakup", "NodeSocketFloat", 0.0, 0.0, 1.0, factor=True)
    add_input(group, "Edge Sharpness", "NodeSocketFloat", 3.0, 1.0, 20.0)
    dirt = ["Dirt Color", "Dirt Roughness", "Dirt Texture"]
    add_input(group, "Dirt Color", "NodeSocketColor", (0.16, 0.12, 0.09, 1.0))
    add_input(group, "Dirt Roughness", "NodeSocketFloat", 0.95, 0.0, 1.0, factor=True)
    add_input(group, "Dirt Texture", "NodeSocketColor", (1.0, 1.0, 1.0, 1.0))
    wear = ["Wear Color", "Wear Roughness", "Wear Texture"]
    add_input(group, "Wear Color", "NodeSocketColor", (0.3, 0.2, 0.13, 1.0))
    add_input(group, "Wear Roughness", "NodeSocketFloat", 0.85, 0.0, 1.0, factor=True)
    add_input(group, "Wear Texture", "NodeSocketColor", (1.0, 1.0, 1.0, 1.0))
    wet = ["Wet Darkening", "Wet Roughness"]
    add_input(group, "Wet Darkening", "NodeSocketFloat", 0.65, 0.0, 1.0, factor=True)
    add_input(group, "Wet Roughness", "NodeSocketFloat", 0.2, 0.0, 1.0, factor=True)
    overlay = ["Grunge", "Invert Dirt", "Invert Wear", "Invert Wet"]
    add_input(group, "Grunge", "NodeSocketFloat", 0.5, 0.0, 1.0, factor=True)
    add_input(group, "Invert Dirt", "NodeSocketFloat", 0.0, 0.0, 1.0, factor=True)
    add_input(group, "Invert Wear", "NodeSocketFloat", 0.0, 0.0, 1.0, factor=True)
    add_input(group, "Invert Wet", "NodeSocketFloat", 0.0, 0.0, 1.0, factor=True)
    panels = (("Paint", paint), ("Dirt (R)", dirt), ("Wear (G)", wear), ("Wet (B)", wet), ("Grunge / invert", overlay))
    for title, names in panels:
        move_into(group, add_panel(group, title), names)

    add_output(group, "BSDF", "NodeSocketShader")
    for name in ("Dirt Mask", "Wear Mask", "Wet Mask"):
        add_output(group, name, "NodeSocketFloat")
    keep = {("INPUT", n) for n in ("Base Color", "Alpha", "Roughness", "Metallic", "ORM", "Normal Map", "Normal Strength",
                                   "Baked Normal", "Baked Normal Strength")}
    keep |= {("INPUT", n) for _, names in panels for n in names}
    keep |= {("OUTPUT", n) for n in ("BSDF", "Dirt Mask", "Wear Mask", "Wet Mask")}
    keep |= {("PANEL", title) for title, _ in panels}
    remove_unused(group, keep)

    b = Builder(group)
    gin = b.node("NodeGroupInput", -1400, 0)
    gout = b.node("NodeGroupOutput", 1400, 0)
    I = lambda name: socket(gin.outputs, name)

    # The paint: the active colour attribute (black if the mesh has none).
    attr = b.node("ShaderNodeVertexColor", -1400, 500, label="Paint (active Color Attribute)")
    sep = b.node("ShaderNodeSeparateColor", -1200, 500)
    b.link(socket(attr.outputs, "Color"), sep.inputs[0])

    # painted = mix(painted, 1 - painted, invert >= 0.5)
    def channel(index, invert_name, y):
        p = sep.outputs[index]
        flip = b.math("GREATER_THAN", I(invert_name), 0.5, -1000, y)
        one_minus_2p = b.math("MULTIPLY_ADD", p, -2.0, -1000, y - 40, c=1.0)  # 1 - 2p
        return b.math("MULTIPLY_ADD", flip, one_minus_2p, -800, y, c=p, clamp=True)  # p + flip * (1 - 2p)

    dirt_p = channel(0, "Invert Dirt", 520)
    wear_p = channel(1, "Invert Wear", 440)
    wet_p = channel(2, "Invert Wet", 360)

    # paintMask(p, g, b, s) = clamp((p * (1 + b) - b * (1 - g) - 0.5) * s + 0.5)
    breakup = I("Grunge Breakup")
    one_plus_b = b.math("ADD", breakup, 1.0, -800, 250)

    def paint_mask(p, g, y, label):
        a = b.math("MULTIPLY", p, one_plus_b, -600, y)
        c = b.math("MULTIPLY", breakup, b.math("SUBTRACT", 1.0, g, -600, y - 40), -450, y - 40)
        t = b.math("SUBTRACT", a, c, -300, y)
        return b.math("MULTIPLY_ADD", b.math("SUBTRACT", t, 0.5, -150, y), I("Edge Sharpness"), 0, y, clamp=True, label=label, c=0.5)

    grunge = I("Grunge")
    wear_mask = b.math("MULTIPLY", paint_mask(wear_p, grunge, 440, "wear paint mask"), I("Wear Strength"), 150, 440, label="Wear")
    inv_grunge = b.math("SUBTRACT", 1.0, grunge, -800, 150)
    dirt_mask = b.math("MULTIPLY", paint_mask(dirt_p, inv_grunge, 560, "dirt paint mask"), I("Dirt Strength"), 150, 560, label="Dirt")
    # wet = clamp(p * (1 + b) - b * g) * strength
    wet_t = b.math("SUBTRACT", b.math("MULTIPLY", wet_p, one_plus_b, -600, 320), b.math("MULTIPLY", breakup, grunge, -600, 280), -300, 320, clamp=True)
    wet_mask = b.math("MULTIPLY", wet_t, I("Wet Strength"), 150, 320, label="Wet")

    # Standard surface: roughness / metallic x ORM (G, B).
    orm = b.node("ShaderNodeSeparateColor", -1000, -200)
    b.link(I("ORM"), orm.inputs[0])
    rough0 = b.math("MULTIPLY", I("Roughness"), orm.outputs[1], -800, -200)
    metal0 = b.math("MULTIPLY", I("Metallic"), orm.outputs[2], -800, -260)

    # Wear, then dirt on top.
    wear_col = b.mix("RGBA", 1.0, I("Wear Color"), I("Wear Texture"), 300, 100, label="wear colour x texture", blend="MULTIPLY")
    dirt_col = b.mix("RGBA", 1.0, I("Dirt Color"), I("Dirt Texture"), 300, 60, label="dirt colour x texture", blend="MULTIPLY")
    alb1 = b.mix("RGBA", wear_mask, I("Base Color"), wear_col, 500, 100, label="+ wear")
    alb2 = b.mix("RGBA", dirt_mask, alb1, dirt_col, 650, 100, label="+ dirt")
    rough1 = b.mix("FLOAT", wear_mask, rough0, I("Wear Roughness"), 500, -200)
    rough2 = b.mix("FLOAT", dirt_mask, rough1, I("Dirt Roughness"), 650, -200)
    metal1 = b.math("MULTIPLY", metal0, b.math("SUBTRACT", 1.0, wear_mask, 500, -300), 650, -300)
    metal2 = b.math("MULTIPLY", metal1, b.math("SUBTRACT", 1.0, dirt_mask, 650, -340), 800, -300)

    # Wet: darken (porous surfaces more), smoother, bumps filled in.
    porous = b.math("MULTIPLY_ADD", rough2, 0.6, 800, 0, c=0.4)  # mix(0.4, 1, roughness)
    darken = b.math("MULTIPLY", b.math("MINIMUM", I("Wet Darkening"), 1.0, 800, -40), porous, 950, 0)
    keep = b.math("SUBTRACT", 1.0, b.math("MULTIPLY", darken, wet_mask, 950, -40), 1100, 0)
    scale_rgb = b.node("ShaderNodeCombineColor", 1100, 60)
    for k in range(3):
        b.link(keep, scale_rgb.inputs[k])
    alb3 = b.mix("RGBA", 1.0, alb2, socket(scale_rgb.outputs, "Color"), 1250, 100, label="wet darkening", blend="MULTIPLY")
    rough3 = b.mix("FLOAT", wet_mask, rough2, I("Wet Roughness"), 1100, -200, label="wet roughness")

    # Tangent-space normals, as the shader does them: each map decoded
    # (texel * 2 - 1), its X/Y tilt scaled by its strength, the baked one
    # whiteout-blended over the tiled one (X, Y added; Z multiplied), then one
    # Normal Map node (tangent space, the mesh's UVs; OpenGL convention).
    def decoded(color_name, strength_name, y):
        v = b.node("ShaderNodeVectorMath", -1100, y, operation="MULTIPLY_ADD", label=color_name + " * 2 - 1")
        v.hide = True
        b.link(I(color_name), v.inputs[0])
        v.inputs[1].default_value = (2.0, 2.0, 2.0)
        v.inputs[2].default_value = (-1.0, -1.0, -1.0)
        xyz = b.node("ShaderNodeSeparateXYZ", -950, y)
        xyz.hide = True
        b.link(v.outputs[0], xyz.inputs[0])
        x = b.math("MULTIPLY", xyz.outputs[0], I(strength_name), -800, y)
        yy = b.math("MULTIPLY", xyz.outputs[1], I(strength_name), -800, y - 40)
        return x, yy, xyz.outputs[2]

    x1, y1, z1 = decoded("Normal Map", "Normal Strength", -450)
    x2, y2, z2 = decoded("Baked Normal", "Baked Normal Strength", -560)
    blended = b.node("ShaderNodeCombineXYZ", -650, -480, label="whiteout blend")
    blended.hide = True
    b.link(b.math("ADD", x1, x2, -700, -450), blended.inputs[0])
    b.link(b.math("ADD", y1, y2, -700, -490), blended.inputs[1])
    b.link(b.math("MULTIPLY", z1, z2, -700, -530), blended.inputs[2])
    unit = b.node("ShaderNodeVectorMath", -550, -480, operation="NORMALIZE")
    unit.hide = True
    b.link(blended.outputs[0], unit.inputs[0])
    encoded = b.node("ShaderNodeVectorMath", -450, -480, operation="MULTIPLY_ADD", label="* 0.5 + 0.5")
    encoded.hide = True
    b.link(socket(unit.outputs, "Vector"), encoded.inputs[0])
    encoded.inputs[1].default_value = (0.5, 0.5, 0.5)
    encoded.inputs[2].default_value = (0.5, 0.5, 0.5)
    nmap = b.node("ShaderNodeNormalMap", -300, -450)
    b.link(encoded.outputs[0], socket(nmap.inputs, "Color"))
    socket(nmap.inputs, "Strength").default_value = 1.0  # (strengths applied above, like the engine)
    geo = b.node("ShaderNodeNewGeometry", -800, -650)
    flatten = b.math("MULTIPLY", wet_mask, 0.8, 950, -450)
    n_mix = b.mix("VECTOR", flatten, socket(nmap.outputs, "Normal"), socket(geo.outputs, "Normal"), 1100, -450, label="water fills the bumps")
    n_norm = b.node("ShaderNodeVectorMath", 1250, -450, operation="NORMALIZE")
    n_norm.hide = True
    b.link(n_mix, n_norm.inputs[0])

    bsdf = b.node("ShaderNodeBsdfPrincipled", 1250, -100)
    b.link(alb3, socket(bsdf.inputs, "Base Color"))
    b.link(rough3, socket(bsdf.inputs, "Roughness"))
    b.link(metal2, socket(bsdf.inputs, "Metallic"))
    b.link(I("Alpha"), socket(bsdf.inputs, "Alpha"))
    b.link(socket(n_norm.outputs, "Vector"), socket(bsdf.inputs, "Normal"))
    # (Specular IOR Level stays at 0.5 = F0 0.04, the engine's dielectric reflectance.)

    b.link(socket(bsdf.outputs, "BSDF"), socket(gout.inputs, "BSDF"))
    b.link(dirt_mask, socket(gout.inputs, "Dirt Mask"))
    b.link(wear_mask, socket(gout.inputs, "Wear Mask"))
    b.link(wet_mask, socket(gout.inputs, "Wet Mask"))

    if MARK_AS_ASSET and hasattr(group, "asset_mark"):
        group.asset_mark()
        if group.asset_data:
            group.asset_data.description = group.description
    return group


# --- Example material --------------------------------------------------------

def build_example(group):
    mat = bpy.data.materials.get(EXAMPLE_MATERIAL) or bpy.data.materials.new(EXAMPLE_MATERIAL)
    if bpy.app.version < (5, 0, 0):
        mat.use_nodes = True  # (always on from Blender 5)
    mat.use_fake_user = True
    tree = mat.node_tree
    tree.nodes.clear()
    b = Builder(tree)

    g = b.node("ShaderNodeGroup", 300, 0)
    g.node_tree = group
    g.width = 240
    out = b.node("ShaderNodeOutputMaterial", 650, 0)
    b.link(socket(g.outputs, "BSDF"), socket(out.inputs, "Surface"))

    # UV-mapped images (the engine's uvScale = the Mapping node's scale).
    uv = b.node("ShaderNodeTexCoord", -900, 300)
    mapping = b.node("ShaderNodeMapping", -700, 300, label="uvScale (params[3].xy)")
    b.link(socket(uv.outputs, "UV"), socket(mapping.inputs, "Vector"))
    y = 400
    for label, target, non_color in (("Albedo (sRGB)", "Base Color", False), ("ORM (Non-Color)", "ORM", True),
                                     ("Normal (Non-Color)", "Normal Map", True)):
        img = b.node("ShaderNodeTexImage", -400, y, label=label + " -> " + target)
        b.link(socket(mapping.outputs, "Vector"), socket(img.inputs, "Vector"))
        # Left unconnected until you pick an image (an empty image node reads black).
        img["ve_connect_to"] = target
        img["ve_non_color"] = non_color
        y -= 280

    # The baked normal: the mesh's own UVs (not uvScale), with its own scale.
    baked_map = b.node("ShaderNodeMapping", -700, -80, label="baked normal UV scale (params[9].yz)")
    b.link(socket(uv.outputs, "UV"), socket(baked_map.inputs, "Vector"))
    img = b.node("ShaderNodeTexImage", -400, -100, label="Baked normal (Non-Color) -> Baked Normal")
    b.link(socket(baked_map.outputs, "Vector"), socket(img.inputs, "Vector"))
    img["ve_connect_to"] = "Baked Normal"
    img["ve_non_color"] = True

    # World-space, box-projected overlays (the shader's triplanar sampling).
    geo = b.node("ShaderNodeNewGeometry", -1100, -500)
    scale = b.node("ShaderNodeValue", -1100, -750, label="Overlay Scale (params[7].x)")
    scale.outputs[0].default_value = 0.5
    world = b.node("ShaderNodeVectorMath", -900, -550, operation="SCALE", label="world position x scale")
    b.link(socket(geo.outputs, "Position"), world.inputs[0])
    b.link(scale.outputs[0], socket(world.inputs, "Scale"))
    y = -350
    for label, target in (("Grunge (Non-Color, R)", "Grunge"), ("Dirt texture (sRGB)", "Dirt Texture"), ("Wear texture (sRGB)", "Wear Texture")):
        img = b.node("ShaderNodeTexImage", -400, y, label=label + " -> " + target, projection="BOX", projection_blend=0.3)
        b.link(socket(world.outputs, "Vector"), socket(img.inputs, "Vector"))
        img["ve_connect_to"] = target
        img["ve_non_color"] = target == "Grunge"
        y -= 280

    note = tree.nodes.new("NodeFrame")
    note.label = "Pick images, then run the script again (or connect the image nodes by hand)"
    note.location = (-450, 600)
    note.label_size = 14
    connect_example_images(mat)
    return mat


def connect_example_images(mat):
    """Connects every image node that has an image to the group input it is labelled for."""
    tree = mat.node_tree
    group_node = next((n for n in tree.nodes if n.type == "GROUP" and n.node_tree and n.node_tree.name == GROUP_NAME), None)
    if group_node is None:
        return
    for n in tree.nodes:
        target = n.get("ve_connect_to") if n.type == "TEX_IMAGE" else None
        if not target or n.image is None:
            continue
        if n.get("ve_non_color"):
            n.image.colorspace_settings.name = "Non-Color"
        # Game textures keep other data in alpha (terrain albedo: height): the
        # engine never multiplies colour by alpha, and neither does Channel Packed.
        n.image.alpha_mode = "CHANNEL_PACKED"
        out = socket(n.outputs, "Color")
        tree.links.new(out, socket(group_node.inputs, target))


# --- Exporting the engine's material file -----------------------------------
#
# Properties editor > Material tab > "VE Engine" panel:
#   Assets folder   the game's assets/ folder (found by itself when the .blend
#                   lives inside the project; saved in the .blend)
#   Export VE material
#       writes <assets>/materials/<material name>.material from the active
#       material's "VE Painted" node: its values and the images plugged into
#       it (paths relative to the assets folder). Fields the export doesn't
#       manage (mode, receiveDecals, ...) of an existing file are kept.

import json
import os


def _upstream(sock, depth=0):
    """The node (and its output socket) feeding `sock`, skipping reroutes."""
    if not sock.is_linked or depth > 16:
        return None, None
    link = sock.links[0]
    node = link.from_node
    if node.type == "REROUTE":
        return _upstream(node.inputs[0], depth + 1)
    return node, link.from_socket


def _image_node(sock):
    node, _ = _upstream(sock)
    return node if node is not None and node.type == "TEX_IMAGE" else None


def find_assets_root(start):
    """The nearest folder named "assets" at or above `start` (the .blend's folder)."""
    d = os.path.abspath(start) if start else ""
    while d and os.path.dirname(d) != d:
        if os.path.basename(d) == "assets":
            return d
        candidate = os.path.join(d, "assets")
        if os.path.isdir(candidate):
            return candidate
        d = os.path.dirname(d)
    return ""


def _assets_root(context):
    root = bpy.path.abspath(context.scene.ve_assets_root) if context.scene.ve_assets_root else ""
    if not root and bpy.data.filepath:
        root = find_assets_root(os.path.dirname(bpy.data.filepath))
    return os.path.normpath(root) if root else ""


def material_base_name(name):
    """Blender's "Concrete.001" uses Concrete's file, as the engine looks it up."""
    base, dot, suffix = name.rpartition(".")
    return base if dot and suffix.isdigit() else name


def export_material(mat, assets_root):
    """Writes the material file; returns (path, warnings)."""
    tree = mat.node_tree
    g = next((n for n in tree.nodes if n.type == "GROUP" and n.node_tree and n.node_tree.name.startswith(GROUP_NAME)), None)
    if g is None:
        raise RuntimeError(f"'{mat.name}' has no '{GROUP_NAME}' node")
    warnings = []

    def image_path(sock, what):
        node = _image_node(sock)
        if node is None:
            if sock.is_linked:
                warnings.append(f"{what}: not an Image Texture node directly (ignored)")
            return None, None
        if node.image is None:
            warnings.append(f"{what}: the image node has no image (ignored)")
            return None, node
        if node.image.packed_file or not node.image.filepath:
            warnings.append(f"{what}: '{node.image.name}' is packed / unsaved: save it as a file under assets/")
            return None, node
        path = os.path.normpath(bpy.path.abspath(node.image.filepath, library=node.image.library))
        rel = os.path.relpath(path, assets_root) if assets_root else path
        if rel.startswith(".."):
            warnings.append(f"{what}: {path} is outside the assets folder (absolute path written: not portable)")
            return path.replace("\\", "/"), node
        return rel.replace("\\", "/"), node

    def value(name):
        v = socket(g.inputs, name).default_value
        try:
            return [round(float(x), 4) for x in v]
        except TypeError:
            return round(float(v), 4)

    def color3(name):
        return value(name)[:3]

    data = {"version": 1, "fragmentShader": "standard_painted.frag", "vertexColorWeight": 0}

    # Base colour: an image (+ white), or the colour itself.
    albedo, albedo_node = image_path(socket(g.inputs, "Base Color"), "Base Color")
    if albedo:
        data["albedo"] = albedo
        data["baseColor"] = [1.0, 1.0, 1.0, value("Alpha") if not socket(g.inputs, "Alpha").is_linked else 1.0]
    else:
        data["albedo"] = ""
        data["baseColor"] = color3("Base Color") + [value("Alpha")]
    orm, _ = image_path(socket(g.inputs, "ORM"), "ORM")
    data["metallicRoughness"] = orm or ""
    data["occlusion"] = orm or ""  # glTF ORM: R = ambient occlusion (the engine uses it; Blender's preview doesn't)
    normal, _ = image_path(socket(g.inputs, "Normal Map"), "Normal Map")
    data["normal"] = normal or ""
    data["roughness"] = value("Roughness")
    data["metallic"] = value("Metallic")
    data["normalStrength"] = value("Normal Strength")

    # uvScale: a Mapping node's scale on the base colour image's UVs.
    uv_scale = [1.0, 1.0]
    if albedo_node is not None:
        mapping, _ = _upstream(socket(albedo_node.inputs, "Vector"))
        if mapping is not None and mapping.type == "MAPPING" and not socket(mapping.inputs, "Scale").is_linked:
            sc = socket(mapping.inputs, "Scale").default_value
            uv_scale = [round(sc[0], 4), round(sc[1], 4)]
    data["uvScale"] = uv_scale

    # Overlay images (slots 5-7) and their world scale (the Vector Math Scale
    # node, or its Value node, feeding their coordinates).
    textures, overlay_scale = [], None
    for slot, name in ((5, "Dirt Texture"), (6, "Grunge"), (7, "Wear Texture")):
        path, node = image_path(socket(g.inputs, name), name)
        if path:
            textures.append({"slot": slot, "path": path})
        if node is not None and overlay_scale is None:
            vm, _ = _upstream(socket(node.inputs, "Vector"))
            if vm is not None and vm.type == "VECT_MATH" and vm.operation == "SCALE":
                src, _ = _upstream(socket(vm.inputs, "Scale"))
                overlay_scale = src.outputs[0].default_value if src is not None and src.type == "VALUE" else socket(vm.inputs, "Scale").default_value
            elif node.projection != "BOX":
                warnings.append(f"{name}: not box-projected on world position x scale: the engine samples it that way")
    if socket(g.inputs, "Grunge").is_linked is False and value("Grunge Breakup") > 0:
        warnings.append("Grunge Breakup > 0 without a grunge image: the engine uses no breakup then")
    # Baked normal (slot 8) and params[9]: strength, its UV scale (a Mapping node on its UVs).
    baked, baked_node = image_path(socket(g.inputs, "Baked Normal"), "Baked Normal")
    baked_scale = [1.0, 1.0]
    if baked:
        textures.append({"slot": 8, "path": baked})
        mapping, _ = _upstream(socket(baked_node.inputs, "Vector"))
        if mapping is not None and mapping.type == "MAPPING" and not socket(mapping.inputs, "Scale").is_linked:
            sc = socket(mapping.inputs, "Scale").default_value
            baked_scale = [round(sc[0], 4), round(sc[1], 4)]
    data["textures"] = textures
    data["params"] = [
        {"slot": 4, "value": [value("Dirt Strength"), value("Wear Strength"), value("Wet Strength"), value("Grunge Breakup")]},
        {"slot": 5, "value": color3("Dirt Color") + [value("Dirt Roughness")]},
        {"slot": 6, "value": color3("Wear Color") + [value("Wear Roughness")]},
        {"slot": 7, "value": [round(float(overlay_scale if overlay_scale is not None else 0.5), 4), value("Edge Sharpness"),
                              value("Wet Darkening"), value("Wet Roughness")]},
        {"slot": 8, "value": [value("Invert Dirt"), value("Invert Wear"), value("Invert Wet"), 0.0]},
        {"slot": 9, "value": [value("Baked Normal Strength")] + baked_scale + [0.0]},
    ]

    path = os.path.join(assets_root, "materials", material_base_name(mat.name) + ".material")
    existing = {}
    if os.path.isfile(path):
        try:
            with open(path, encoding="utf-8") as f:
                existing = json.load(f)
        except (OSError, ValueError):
            warnings.append(f"{path} wasn't valid JSON: replaced")
    existing.update(data)  # (keeps fields this export doesn't manage: mode, receiveDecals, ...)
    os.makedirs(os.path.dirname(path), exist_ok=True)
    with open(path, "w", encoding="utf-8", newline="\n") as f:
        json.dump(existing, f, indent=2)
        f.write("\n")
    return path, warnings


class VE_OT_export_material(bpy.types.Operator):
    """Write the engine's .material file for this material (its VE Painted node's values and images)"""
    bl_idname = "ve.export_material"
    bl_label = "Export VE material"
    bl_options = {"REGISTER"}

    @classmethod
    def poll(cls, context):
        return getattr(context, "material", None) is not None

    def execute(self, context):
        root = _assets_root(context)
        if not root or not os.path.isdir(root):
            self.report({"ERROR"}, "Set the assets folder (VE Engine panel) first")
            return {"CANCELLED"}
        try:
            path, warnings = export_material(context.material, root)
        except RuntimeError as e:
            self.report({"ERROR"}, str(e))
            return {"CANCELLED"}
        for w in warnings:
            self.report({"WARNING"}, w)
        self.report({"INFO"}, f"Wrote {path}")
        return {"FINISHED"}


class VE_OT_build_group(bpy.types.Operator):
    """Create / update the VE Painted node group and example material in this file"""
    bl_idname = "ve.build_painted_group"
    bl_label = "Create / update VE Painted"
    bl_options = {"REGISTER", "UNDO"}

    def execute(self, context):
        main()
        self.report({"INFO"}, f"{GROUP_NAME} node group ready")
        return {"FINISHED"}


class VE_PT_material(bpy.types.Panel):
    bl_label = "VE Engine"
    bl_space_type = "PROPERTIES"
    bl_region_type = "WINDOW"
    bl_context = "material"

    def draw(self, context):
        layout = self.layout
        layout.prop(context.scene, "ve_assets_root")
        root = _assets_root(context)
        if root and not context.scene.ve_assets_root:
            layout.label(text=f"Found: {root}", icon="INFO")
        mat = getattr(context, "material", None)
        if mat is not None:
            layout.label(text="-> materials/" + material_base_name(mat.name) + ".material")
        layout.operator(VE_OT_export_material.bl_idname, icon="EXPORT")
        layout.operator(VE_OT_build_group.bl_idname, icon="NODETREE")


_classes = (VE_OT_export_material, VE_OT_build_group, VE_PT_material)


def register():
    bpy.types.Scene.ve_assets_root = bpy.props.StringProperty(
        name="Assets folder", subtype="DIR_PATH",
        description="The game's assets/ folder (empty: found above this .blend)")
    for c in _classes:
        try:
            bpy.utils.unregister_class(c)  # (running the script again)
        except RuntimeError:
            pass
        bpy.utils.register_class(c)


def unregister():
    for c in reversed(_classes):
        bpy.utils.unregister_class(c)
    del bpy.types.Scene.ve_assets_root


def main():
    if bpy.app.version < (4, 0, 0):
        raise RuntimeError("VE Painted needs Blender 4.0 or newer (node group interface API)")
    group = build_group()
    mat = bpy.data.materials.get(EXAMPLE_MATERIAL)
    if mat is not None and mat.node_tree and any(n.get("ve_connect_to") for n in mat.node_tree.nodes):
        connect_example_images(mat)  # re-run: keep the user's images, just wire the newly picked ones
    else:
        build_example(group)
    print(f"{GROUP_NAME}: node group ready ({len(group.nodes)} nodes); example material '{EXAMPLE_MATERIAL}'")


if __name__ == "__main__":
    # Run from the Text Editor: build the group and add the VE Engine panel
    # (for this session; install the file as an add-on to always have it).
    main()
    register()
