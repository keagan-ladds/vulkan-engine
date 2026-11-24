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
    keep = {("INPUT", n) for n in ("Base Color", "Alpha", "Roughness", "Metallic", "ORM", "Normal Map", "Normal Strength")}
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

    nmap = b.node("ShaderNodeNormalMap", -800, -450)  # tangent space, the mesh's UVs (OpenGL convention, like the engine)
    b.link(I("Normal Map"), socket(nmap.inputs, "Color"))
    b.link(I("Normal Strength"), socket(nmap.inputs, "Strength"))
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
    main()
