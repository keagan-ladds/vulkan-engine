"""
VE materials - Blender node groups that look like the engine's materials, and
the export of their .material files:

  * "VE Standard": standard.frag (PBR, emission, translucency): any model.
  * "VE Painted": standard_painted.frag, so vertex-painted dirt / wear /
    wetness can be judged in Blender while painting.

(Material Preview or Rendered view, Eevee or Cycles.)

Run once per .blend (Blender 4.0 or newer): open the Scripting workspace, open
this file in the Text Editor and press Run Script (Alt+P). It creates (or
rebuilds, so it's safe to run again after updating the script):

  * node group "VE Standard" and material "VE Standard Example" (image nodes
    for albedo, ORM, normal, emission and the translucency map);
  * node group "VE Painted": one node with the shader's inputs, outputting a
    BSDF (plus the three masks, handy to look at while painting);
  * material "VE Painted Example": the group wired up the way the engine
    samples things (see "Wiring" below). Copy it, or copy its nodes.

VE Standard inputs = the material file's values
-----------------------------------------------
  Base Color (+ image), Alpha        "albedo", "baseColor" (alpha: the albedo image's A; an image
                                     wired from another image's output is merged into
                                     <albedo>_rgba.png on export: Poly Haven's *_alpha maps)
  Roughness / Metallic / ORM         "roughness", "metallic", "metallicRoughness" (+ "occlusion")
  Normal Map / Normal Strength       "normal", "normalStrength"
  Vertex Color Weight                "vertexColorWeight" (0 = ignore the mesh's colours)
  Emission Color (+ image), Strength "emissive" / "emissiveTexture", "emissiveIntensity"
  Translucency, Translucency Color   "translucency", "translucencyColor": thin surfaces
                                     (leaves): the front reflects albedo x (1 - t), light from
                                     behind comes through as albedo x colour x t
  Translucency Map (Non-Color, R)    "translucencyTexture": where light gets through
The panel's Mode (Auto / Opaque / Cutout / Transparent) and Cutoff write
"mode" and "alphaCutoff"; Auto = Cutout when Alpha is used (Transparent with
the Blended render method). VE Painted has the same translucency inputs.

An imported model (glTF, FBX, a Poly Haven download) arrives with Principled
BSDFs (leaves: mixed with a Translucent BSDF): the panel's "Convert to VE
Standard" (this material / every material of the selected objects) rewires
the same images and values into a VE Standard node and reports anything that
doesn't carry over (roughness not in an ORM's G channel, subsurface, ...).

Exporting the engine's material files
-------------------------------------
Every material using a VE Standard or VE Painted node becomes
assets/materials/<material name>.material (values, images, the overlay scale
and the UV Mapping scale), so the engine replaces the exported glTF material
with the same look (it looks material files up by the glTF material's name).
  * Automatically with the model: File > Export > glTF 2.0 writes the
    material files of the materials it exports (add-on installed, "Write
    with glTF export" on in the panel). The assets folder is the one the
    .gltf is saved in or under, else the panel's / the one above the .blend.
  * By hand: Properties editor > Material tab > "VE Engine" panel >
    Export VE material (this material) or Export all VE materials (every
    VE Painted material in the file). The assets folder is found above the
    .blend (or set it in the panel). Export the
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
  * Grunge, dirt and wear images: world-space triplanar, as the shader
    samples them (the overlays continue across modular pieces and don't
    depend on UVs or the object's rotation): "VE Triplanar Coords" (world
    position x Overlay Scale) -> three Image Texture nodes with the same image
    (pick it in the first; the copies follow) -> "VE Triplanar Blend" -> the
    input. Materials wired the older way (one Box-projected image node on
    world position x scale) still export; the panel's "Convert overlays to
    triplanar" rewires them (Box projection is only right for unrotated,
    evenly scaled objects: Blender picks its sides from the object's normals).
  * An older copy of the VE Painted group in a .blend (made by an older
    script) still exports: inputs it lacks are written with their defaults
    and the export says so. Press "Create / update VE Painted" to update it.
  * Colour images as sRGB, everything else Non-Color (the engine decides by
    the image's .texture file / name; keep the two in agreement).
  * Grunge Breakup only applies when a grunge image is connected (the engine
    uses no breakup without one): leave it at 0 otherwise.

Not reproduced (small differences)
----------------------------------
  * The 15% ambient-occlusion darkening under dirt (Principled has no AO input).
  * The material's occlusion texture (Principled has no AO input); VE
    Painted: emission.
  * Lighting: the engine's sky, ambient, exposure and grading differ from your
    Blender scene. Use the AgX view transform, one sun and a plain world colour
    to get close; judge final looks in the engine (material files reload live).
"""

bl_info = {
    "name": "VE materials (vulkan-engine)",
    "author": "vulkan-engine",
    "version": (2, 0, 0),
    "blender": (4, 0, 0),
    "location": "Properties > Material > VE Engine",
    "description": "The engine's standard and painted materials as node groups, and export of their .material files",
    "category": "Material",
}

import bpy

GROUP_NAME = "VE Painted"
EXAMPLE_MATERIAL = "VE Painted Example"
MARK_AS_ASSET = True
TRIPLANAR_COORDS = "VE Triplanar Coords"
TRIPLANAR_BLEND = "VE Triplanar Blend"

# The VE Painted inputs' defaults (also what the export writes for an input an
# older copy of the group in a .blend doesn't have yet).
INPUT_DEFAULTS = {
    "Base Color": (0.8, 0.8, 0.8, 1.0),
    "Alpha": 1.0,
    "Roughness": 0.8,
    "Metallic": 0.0,
    "ORM": (1.0, 1.0, 1.0, 1.0),
    "Normal Map": (0.5, 0.5, 1.0, 1.0),
    "Normal Strength": 1.0,
    "Baked Normal": (0.5, 0.5, 1.0, 1.0),
    "Baked Normal Strength": 1.0,
    "Dirt Strength": 1.0,
    "Wear Strength": 1.0,
    "Wet Strength": 1.0,
    "Grunge Breakup": 0.0,
    "Edge Sharpness": 3.0,
    "Dirt Color": (0.16, 0.12, 0.09, 1.0),
    "Dirt Roughness": 0.95,
    "Dirt Texture": (1.0, 1.0, 1.0, 1.0),
    "Wear Color": (0.3, 0.2, 0.13, 1.0),
    "Wear Roughness": 0.85,
    "Wear Texture": (1.0, 1.0, 1.0, 1.0),
    "Wet Darkening": 0.65,
    "Wet Roughness": 0.2,
    "Grunge": 0.5,
    "Invert Dirt": 0.0,
    "Invert Wear": 0.0,
    "Invert Wet": 0.0,
    # (VE Standard; translucency: both groups)
    "Vertex Color Weight": 0.0,
    "Emission Color": (0.0, 0.0, 0.0, 1.0),
    "Emission Strength": 1.0,
    "Translucency": 0.0,
    "Translucency Color": (1.0, 1.0, 1.0, 1.0),
    "Translucency Map": (1.0, 1.0, 1.0, 1.0),
}


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
    keep their values and links when the script is run again), else created.
    `default` None = INPUT_DEFAULTS[name] (if listed)."""
    if default is None:
        default = INPUT_DEFAULTS.get(name)
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
    add_input(group, "Base Color", "NodeSocketColor")
    add_input(group, "Alpha", "NodeSocketFloat", lo=0.0, hi=1.0, factor=True)
    add_input(group, "Roughness", "NodeSocketFloat", lo=0.0, hi=1.0, factor=True)
    add_input(group, "Metallic", "NodeSocketFloat", lo=0.0, hi=1.0, factor=True)
    add_input(group, "ORM", "NodeSocketColor")
    add_input(group, "Normal Map", "NodeSocketColor")
    add_input(group, "Normal Strength", "NodeSocketFloat", lo=0.0, hi=4.0)
    add_input(group, "Baked Normal", "NodeSocketColor")
    add_input(group, "Baked Normal Strength", "NodeSocketFloat", lo=0.0, hi=4.0)
    paint = ["Dirt Strength", "Wear Strength", "Wet Strength", "Grunge Breakup", "Edge Sharpness"]
    add_input(group, "Dirt Strength", "NodeSocketFloat", lo=0.0, hi=1.0, factor=True)
    add_input(group, "Wear Strength", "NodeSocketFloat", lo=0.0, hi=1.0, factor=True)
    add_input(group, "Wet Strength", "NodeSocketFloat", lo=0.0, hi=1.0, factor=True)
    add_input(group, "Grunge Breakup", "NodeSocketFloat", lo=0.0, hi=1.0, factor=True)
    add_input(group, "Edge Sharpness", "NodeSocketFloat", lo=1.0, hi=20.0)
    dirt = ["Dirt Color", "Dirt Roughness", "Dirt Texture"]
    add_input(group, "Dirt Color", "NodeSocketColor")
    add_input(group, "Dirt Roughness", "NodeSocketFloat", lo=0.0, hi=1.0, factor=True)
    add_input(group, "Dirt Texture", "NodeSocketColor")
    wear = ["Wear Color", "Wear Roughness", "Wear Texture"]
    add_input(group, "Wear Color", "NodeSocketColor")
    add_input(group, "Wear Roughness", "NodeSocketFloat", lo=0.0, hi=1.0, factor=True)
    add_input(group, "Wear Texture", "NodeSocketColor")
    wet = ["Wet Darkening", "Wet Roughness"]
    add_input(group, "Wet Darkening", "NodeSocketFloat", lo=0.0, hi=1.0, factor=True)
    add_input(group, "Wet Roughness", "NodeSocketFloat", lo=0.0, hi=1.0, factor=True)
    overlay = ["Grunge", "Invert Dirt", "Invert Wear", "Invert Wet"]
    add_input(group, "Grunge", "NodeSocketFloat", lo=0.0, hi=1.0, factor=True)
    add_input(group, "Invert Dirt", "NodeSocketFloat", lo=0.0, hi=1.0, factor=True)
    add_input(group, "Invert Wear", "NodeSocketFloat", lo=0.0, hi=1.0, factor=True)
    add_input(group, "Invert Wet", "NodeSocketFloat", lo=0.0, hi=1.0, factor=True)
    add_translucency_inputs(group)
    panels = (("Paint", paint), ("Dirt (R)", dirt), ("Wear (G)", wear), ("Wet (B)", wet), ("Grunge / invert", overlay),
              ("Translucency", TRANSLUCENCY))
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
        # Z eased towards 1 below strength 1, so strength 0 = no effect at all,
        # whatever colour an unconnected input holds (the engine skips a map
        # that isn't set; only X / Y are scaled there, a small difference
        # between strengths 0 and 1).
        t = b.math("MINIMUM", I(strength_name), 1.0, -800, y - 80)
        z = b.math("MULTIPLY_ADD", b.math("SUBTRACT", xyz.outputs[2], 1.0, -950, y - 80), t, -650, y - 80, c=1.0)
        return x, yy, z

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
    b.link(rough3, socket(bsdf.inputs, "Roughness"))
    b.link(metal2, socket(bsdf.inputs, "Metallic"))
    normal_vec = socket(n_norm.outputs, "Vector")
    b.link(normal_vec, socket(bsdf.inputs, "Normal"))
    # (Specular IOR Level stays at 0.5 = F0 0.04, the engine's dielectric reflectance.)

    # Base colour (minus what passes through), translucency and alpha.
    gout.location = (2700, 0)
    b.link(build_surface_output(b, I, alb3, metal2, normal_vec, bsdf, 1500, -100), socket(gout.inputs, "BSDF"))
    b.link(dirt_mask, socket(gout.inputs, "Dirt Mask"))
    b.link(wear_mask, socket(gout.inputs, "Wear Mask"))
    b.link(wet_mask, socket(gout.inputs, "Wet Mask"))

    if MARK_AS_ASSET and hasattr(group, "asset_mark"):
        group.asset_mark()
        if group.asset_data:
            group.asset_data.description = group.description
    return group


# --- Translucency and alpha (both groups) ------------------------------------
#
# The engine (ve_lighting.glsl veStandardTranslucency / veTranslucentLighting):
#   t       = translucency x map.R x (1 - metallic)
#   front   = albedo x (1 - t)            (Principled's Base Color: what is reflected)
#   through = albedo x colour x t         (Translucent BSDF: light from the back)
# Alpha: a Mix Shader with Transparent (Principled's own Alpha would leave the
# translucent part showing where the leaf is cut out).

TRANSLUCENCY = ["Translucency", "Translucency Color", "Translucency Map"]


def add_translucency_inputs(group):
    add_input(group, "Translucency", "NodeSocketFloat", lo=0.0, hi=1.0, factor=True)
    add_input(group, "Translucency Color", "NodeSocketColor")
    add_input(group, "Translucency Map", "NodeSocketColor")


def _rgb(b, value, x, y):
    """A colour with `value` in R, G and B."""
    n = b.node("ShaderNodeCombineColor", x, y)
    n.hide = True
    for k in range(3):
        b.link(value, n.inputs[k])
    return socket(n.outputs, "Color")


def build_surface_output(b, I, albedo, metallic, normal, bsdf, x, y):
    """Wires `albedo` into the Principled `bsdf` (minus the share that passes
    through), adds the Translucent BSDF and the alpha; returns the shader."""
    sep = b.node("ShaderNodeSeparateColor", x, y + 320, label="Translucency Map R")
    sep.hide = True
    b.link(I("Translucency Map"), sep.inputs[0])
    t = b.math("MULTIPLY", I("Translucency"), sep.outputs[0], x, y + 280)
    t = b.math("MULTIPLY", t, b.math("SUBTRACT", 1.0, metallic, x, y + 240), x + 150, y + 280, clamp=True, label="t")
    keep = b.math("SUBTRACT", 1.0, t, x + 300, y + 240)
    front = b.mix("RGBA", 1.0, albedo, _rgb(b, keep, x + 300, y + 200), x + 450, y + 200, label="front: albedo x (1 - t)", blend="MULTIPLY")
    b.link(front, socket(bsdf.inputs, "Base Color"))
    through = b.mix("RGBA", 1.0, albedo, I("Translucency Color"), x + 450, y + 320, label="albedo x colour", blend="MULTIPLY")
    through = b.mix("RGBA", 1.0, through, _rgb(b, t, x + 450, y + 280), x + 600, y + 320, label="through: x t", blend="MULTIPLY")
    tr = b.node("ShaderNodeBsdfTranslucent", x + 750, y + 320)
    b.link(through, socket(tr.inputs, "Color"))
    b.link(normal, socket(tr.inputs, "Normal"))
    add = b.node("ShaderNodeAddShader", x + 950, y)
    b.link(socket(bsdf.outputs, "BSDF"), add.inputs[0])
    b.link(socket(tr.outputs, "BSDF"), add.inputs[1])
    clear = b.node("ShaderNodeBsdfTransparent", x + 950, y + 150)
    out = b.node("ShaderNodeMixShader", x + 1100, y, label="alpha")
    b.link(I("Alpha"), out.inputs[0])
    b.link(socket(clear.outputs, "BSDF"), out.inputs[1])
    b.link(add.outputs[0], out.inputs[2])
    return out.outputs[0]


# --- VE Standard: the engine's standard material -----------------------------
#
# standard.frag as a node: the material file's fields as inputs (see the
# export below), the same maths inside. For models whose materials are
# ordinary PBR (Poly Haven, Sketchfab, ...): "Convert to VE Standard" in the
# panel rewires an imported Principled BSDF into it.

STANDARD_GROUP = "VE Standard"
STANDARD_EXAMPLE = "VE Standard Example"


def build_standard_group():
    group = _rebuild(STANDARD_GROUP, "The engine's standard material (standard.frag): PBR, emission, translucency")
    add_input(group, "Base Color", "NodeSocketColor")
    add_input(group, "Alpha", "NodeSocketFloat", lo=0.0, hi=1.0, factor=True)
    add_input(group, "Roughness", "NodeSocketFloat", lo=0.0, hi=1.0, factor=True)
    add_input(group, "Metallic", "NodeSocketFloat", lo=0.0, hi=1.0, factor=True)
    add_input(group, "ORM", "NodeSocketColor")
    add_input(group, "Normal Map", "NodeSocketColor")
    add_input(group, "Normal Strength", "NodeSocketFloat", lo=0.0, hi=4.0)
    add_input(group, "Vertex Color Weight", "NodeSocketFloat", lo=0.0, hi=1.0, factor=True)
    emission = ["Emission Color", "Emission Strength"]
    add_input(group, "Emission Color", "NodeSocketColor")
    add_input(group, "Emission Strength", "NodeSocketFloat", lo=0.0, hi=100.0)
    add_translucency_inputs(group)
    panels = (("Emission", emission), ("Translucency", TRANSLUCENCY))
    for title, names in panels:
        move_into(group, add_panel(group, title), names)
    add_output(group, "BSDF", "NodeSocketShader")
    keep = {("INPUT", n) for n in ("Base Color", "Alpha", "Roughness", "Metallic", "ORM", "Normal Map", "Normal Strength",
                                   "Vertex Color Weight")}
    keep |= {("INPUT", n) for _, names in panels for n in names}
    keep |= {("OUTPUT", "BSDF")} | {("PANEL", title) for title, _ in panels}
    remove_unused(group, keep)

    b = Builder(group)
    gin = b.node("NodeGroupInput", -1200, 0)
    gout = b.node("NodeGroupOutput", 1700, 0)
    I = lambda name: socket(gin.outputs, name)

    # Albedo x vertex colour (weight 0 = ignored; a mesh without colours reads white in the engine).
    attr = b.node("ShaderNodeVertexColor", -1000, 350, label="Vertex colour (active Color Attribute)")
    vc = b.mix("RGBA", I("Vertex Color Weight"), (1.0, 1.0, 1.0, 1.0), socket(attr.outputs, "Color"), -800, 350, label="vertex colour weight")
    albedo = b.mix("RGBA", 1.0, I("Base Color"), vc, -600, 300, label="albedo", blend="MULTIPLY")

    # Roughness / metallic x ORM (glTF: G, B). (R = occlusion: the engine uses it, Principled has no AO input.)
    orm = b.node("ShaderNodeSeparateColor", -1000, 0)
    b.link(I("ORM"), orm.inputs[0])
    rough = b.math("MULTIPLY", I("Roughness"), orm.outputs[1], -800, 0)
    metal = b.math("MULTIPLY", I("Metallic"), orm.outputs[2], -800, -60)

    # Normal map as the engine decodes it: X / Y tilt x strength, normalised.
    v = b.node("ShaderNodeVectorMath", -1000, -300, operation="MULTIPLY_ADD", label="Normal Map * 2 - 1")
    v.hide = True
    b.link(I("Normal Map"), v.inputs[0])
    v.inputs[1].default_value = (2.0, 2.0, 2.0)
    v.inputs[2].default_value = (-1.0, -1.0, -1.0)
    xyz = b.node("ShaderNodeSeparateXYZ", -850, -300)
    xyz.hide = True
    b.link(v.outputs[0], xyz.inputs[0])
    tilt = b.node("ShaderNodeCombineXYZ", -650, -300, label="x, y x strength")
    tilt.hide = True
    b.link(b.math("MULTIPLY", xyz.outputs[0], I("Normal Strength"), -750, -280), tilt.inputs[0])
    b.link(b.math("MULTIPLY", xyz.outputs[1], I("Normal Strength"), -750, -320), tilt.inputs[1])
    # (Z eased towards 1 below strength 1: strength 0 = no effect whatever an unconnected input holds.)
    zt = b.math("MINIMUM", I("Normal Strength"), 1.0, -850, -360)
    b.link(b.math("MULTIPLY_ADD", b.math("SUBTRACT", xyz.outputs[2], 1.0, -850, -400), zt, -750, -360, c=1.0), tilt.inputs[2])
    unit = b.node("ShaderNodeVectorMath", -550, -300, operation="NORMALIZE")
    unit.hide = True
    b.link(tilt.outputs[0], unit.inputs[0])
    enc = b.node("ShaderNodeVectorMath", -450, -300, operation="MULTIPLY_ADD", label="* 0.5 + 0.5")
    enc.hide = True
    b.link(socket(unit.outputs, "Vector"), enc.inputs[0])
    enc.inputs[1].default_value = (0.5, 0.5, 0.5)
    enc.inputs[2].default_value = (0.5, 0.5, 0.5)
    nmap = b.node("ShaderNodeNormalMap", -300, -300)
    b.link(enc.outputs[0], socket(nmap.inputs, "Color"))
    normal = socket(nmap.outputs, "Normal")

    bsdf = b.node("ShaderNodeBsdfPrincipled", 0, -100)
    b.link(rough, socket(bsdf.inputs, "Roughness"))
    b.link(metal, socket(bsdf.inputs, "Metallic"))
    b.link(normal, socket(bsdf.inputs, "Normal"))
    b.link(I("Emission Color"), socket(bsdf.inputs, "Emission Color"))
    b.link(I("Emission Strength"), socket(bsdf.inputs, "Emission Strength"))
    b.link(build_surface_output(b, I, albedo, metal, normal, bsdf, 300, -100), socket(gout.inputs, "BSDF"))

    if MARK_AS_ASSET and hasattr(group, "asset_mark"):
        group.asset_mark()
        if group.asset_data:
            group.asset_data.description = group.description
    return group


def build_standard_example(group):
    mat = bpy.data.materials.get(STANDARD_EXAMPLE) or bpy.data.materials.new(STANDARD_EXAMPLE)
    if bpy.app.version < (5, 0, 0):
        mat.use_nodes = True
    mat.use_fake_user = True
    tree = mat.node_tree
    tree.nodes.clear()
    b = Builder(tree)
    g = b.node("ShaderNodeGroup", 300, 0)
    g.node_tree = group
    g.width = 240
    out = b.node("ShaderNodeOutputMaterial", 650, 0)
    b.link(socket(g.outputs, "BSDF"), socket(out.inputs, "Surface"))
    uv = b.node("ShaderNodeTexCoord", -900, 300)
    mapping = b.node("ShaderNodeMapping", -700, 300, label="uvScale (params[3].xy)")
    b.link(socket(uv.outputs, "UV"), socket(mapping.inputs, "Vector"))
    y = 450
    for label, target, non_color in (("Albedo (sRGB; alpha in A)", "Base Color", False), ("ORM (Non-Color)", "ORM", True),
                                     ("Normal (Non-Color)", "Normal Map", True), ("Emission (sRGB)", "Emission Color", False),
                                     ("Translucency map (Non-Color, R)", "Translucency Map", True)):
        img = b.node("ShaderNodeTexImage", -400, y, label=label + " -> " + target)
        b.link(socket(mapping.outputs, "Vector"), socket(img.inputs, "Vector"))
        img["ve_connect_to"] = target
        img["ve_non_color"] = non_color
        y -= 280
    note = tree.nodes.new("NodeFrame")
    note.label = "Pick images, then press Create / update VE nodes (or connect them by hand)"
    note.location = (-450, 650)
    note.label_size = 14
    connect_example_images(mat)
    return mat


# --- World-space triplanar (the shader's overlay sampling) -------------------
#
# The engine samples the grunge / dirt / wear images three times, projected
# along the world axes, blended by the surface normal (weights |n|^4,
# normalised): standard_painted.frag's triplanar(). An Image Texture node set
# to Box projection picks its sides from the OBJECT's normals, so it only
# matches for unrotated, evenly scaled objects. Instead, per image:
#   "VE Triplanar Coords" (once per material: world position x Overlay Scale
#   -> one coordinate per axis, plus the weights) -> three Image Texture nodes
#   with the same image -> "VE Triplanar Blend" -> the VE Painted input.
# Pick the image in the first node; the two copies follow it by themselves.
# The coordinates also follow the glTF export's axis change (Blender Z up =
# engine Y up) and the engine's top-down V, so the pattern lands exactly where
# it does in the game.

def _rebuild(name, description):
    group = bpy.data.node_groups.get(name) or bpy.data.node_groups.new(name, "ShaderNodeTree")
    group.nodes.clear()
    group.use_fake_user = True
    group.description = description
    return group


def build_triplanar_groups():
    """Creates / updates the two helper groups (see above)."""
    # Coordinates and weights.
    g = _rebuild(TRIPLANAR_COORDS, "World position x scale projected along each axis (engine axes and V), and the |n|^4 blend weights")
    add_input(g, "Scale", "NodeSocketFloat", 0.5, lo=0.0)
    for face in ("X Face", "Y Face", "Z Face", "Weights"):
        add_output(g, face, "NodeSocketVector")
    remove_unused(g, {("INPUT", "Scale")} | {("OUTPUT", f) for f in ("X Face", "Y Face", "Z Face", "Weights")})
    b = Builder(g)
    gin = b.node("NodeGroupInput", -900, 0)
    gout = b.node("NodeGroupOutput", 700, 0)
    geo = b.node("ShaderNodeNewGeometry", -900, -250)
    pos = b.node("ShaderNodeVectorMath", -650, 0, operation="SCALE", label="world position x scale")
    b.link(socket(geo.outputs, "Position"), pos.inputs[0])
    b.link(socket(gin.outputs, "Scale"), socket(pos.inputs, "Scale"))
    sep = b.node("ShaderNodeSeparateXYZ", -450, 0)
    b.link(socket(pos.outputs, "Vector"), sep.inputs[0])
    X, Y, Z = (socket(sep.outputs, a) for a in "XYZ")
    negY = b.math("MULTIPLY", Y, -1.0, -250, -60)
    negZ = b.math("MULTIPLY", Z, -1.0, -250, -100)
    # Engine sample (u, v) = Blender (u, -v); engine position = (x, z, -y).
    for k, (face, u, v) in enumerate((("X Face", negY, negZ), ("Y Face", X, negZ), ("Z Face", X, Y))):
        c = b.node("ShaderNodeCombineXYZ", 0, 200 - 150 * k, label=face)
        b.link(u, socket(c.inputs, "X"))
        b.link(v, socket(c.inputs, "Y"))
        b.link(socket(c.outputs, "Vector"), socket(gout.inputs, face))
    nsep = b.node("ShaderNodeSeparateXYZ", -650, -300)
    b.link(socket(geo.outputs, "Normal"), nsep.inputs[0])
    w = []
    for k, a in enumerate("XYZ"):
        ab = b.math("ABSOLUTE", socket(nsep.outputs, a), None, -450, -280 - 40 * k)
        w.append(b.math("POWER", ab, 4.0, -300, -280 - 40 * k))
    total = b.math("ADD", b.math("ADD", w[0], w[1], -150, -300), w[2], 0, -320)
    wc = b.node("ShaderNodeCombineXYZ", 400, -300, label="weights")
    for a, wi in zip("XYZ", w):
        b.link(b.math("DIVIDE", wi, total, 200, -300 - 40 * "XYZ".index(a)), socket(wc.inputs, a))
    b.link(socket(wc.outputs, "Vector"), socket(gout.inputs, "Weights"))

    # Blend of the three samples.
    g = _rebuild(TRIPLANAR_BLEND, "The three samples of an image, weighted by the surface normal")
    for face in ("X Face", "Y Face", "Z Face"):
        add_input(g, face, "NodeSocketColor", (1.0, 1.0, 1.0, 1.0))
    add_input(g, "Weights", "NodeSocketVector", (0.0, 0.0, 1.0))
    add_output(g, "Color", "NodeSocketColor")
    remove_unused(g, {("INPUT", f) for f in ("X Face", "Y Face", "Z Face", "Weights")} | {("OUTPUT", "Color")})
    b = Builder(g)
    gin = b.node("NodeGroupInput", -600, 0)
    gout = b.node("NodeGroupOutput", 500, 0)
    wsep = b.node("ShaderNodeSeparateXYZ", -400, -250)
    b.link(socket(gin.outputs, "Weights"), wsep.inputs[0])
    terms = []
    for k, (face, a) in enumerate(zip(("X Face", "Y Face", "Z Face"), "XYZ")):
        m = b.node("ShaderNodeVectorMath", -150, 150 - 150 * k, operation="SCALE")
        b.link(socket(gin.outputs, face), m.inputs[0])
        b.link(socket(wsep.outputs, a), socket(m.inputs, "Scale"))
        terms.append(socket(m.outputs, "Vector"))
    add1 = b.node("ShaderNodeVectorMath", 100, 50, operation="ADD")
    b.link(terms[0], add1.inputs[0])
    b.link(terms[1], add1.inputs[1])
    add2 = b.node("ShaderNodeVectorMath", 300, 0, operation="ADD")
    b.link(socket(add1.outputs, "Vector"), add2.inputs[0])
    b.link(terms[2], add2.inputs[1])
    b.link(socket(add2.outputs, "Vector"), socket(gout.inputs, "Color"))


def _group_node(tree, group_name, x, y, label=None):
    n = tree.nodes.new("ShaderNodeGroup")
    n.node_tree = bpy.data.node_groups[group_name]
    n.location = (x, y)
    if label:
        n.label = label
    return n


def add_triplanar(tree, img, coords):
    """Turns image node `img` into a triplanar sample: two hidden copies of it
    and a VE Triplanar Blend node (returned; its Color replaces img's Color)."""
    img.projection = "FLAT"
    x, y = img.location
    blend = _group_node(tree, TRIPLANAR_BLEND, x + 320, y, label=(img.label or img.name).split(" -> ")[0] + " (triplanar)")
    tree.links.new(socket(coords.outputs, "X Face"), socket(img.inputs, "Vector"))
    tree.links.new(socket(img.outputs, "Color"), socket(blend.inputs, "X Face"))
    for k, face in enumerate(("Y Face", "Z Face")):
        c = tree.nodes.new("ShaderNodeTexImage")
        c.location = (x + 30, y - 270 - 40 * k)
        c.hide = True
        c.label = f"{face}: copy of the image above (follows it)"
        c.image, c.interpolation, c.extension = img.image, img.interpolation, img.extension
        c["ve_triplanar_copy"] = True
        tree.links.new(socket(coords.outputs, face), socket(c.inputs, "Vector"))
        tree.links.new(socket(c.outputs, "Color"), socket(blend.inputs, face))
    tree.links.new(socket(coords.outputs, "Weights"), socket(blend.inputs, "Weights"))
    return blend


def _is_group(node, name):
    return node is not None and node.type == "GROUP" and node.node_tree is not None and node.node_tree.name.startswith(name)


def sync_triplanar(mat):
    """Copies each triplanar image's image (and filtering) to its two copies."""
    if mat is None or mat.node_tree is None:
        return
    for blend in mat.node_tree.nodes:
        if not _is_group(blend, TRIPLANAR_BLEND):
            continue
        main, _ = _upstream(socket(blend.inputs, "X Face"))
        if main is None or main.type != "TEX_IMAGE":
            continue
        for face in ("Y Face", "Z Face"):
            c, _ = _upstream(socket(blend.inputs, face))
            if c is None or c.type != "TEX_IMAGE":
                continue
            if c.image != main.image:
                c.image = main.image
            if c.interpolation != main.interpolation:
                c.interpolation = main.interpolation
            if c.extension != main.extension:
                c.extension = main.extension


@bpy.app.handlers.persistent
def _ve_sync_triplanar(scene, depsgraph=None):
    # Keeps the copies in step when an image is picked in the first node.
    for mat in bpy.data.materials:
        if mat.get("ve_triplanar") and mat.node_tree is not None:
            try:
                sync_triplanar(mat)
            except Exception as e:  # (never break the depsgraph update)
                print("VE Painted: triplanar sync:", e)


def convert_to_triplanar(mat):
    """Rewires a material's Box-projected grunge / dirt / wear images (world
    position x scale) to the world-space triplanar setup, keeping their images
    and the Overlay Scale value. Returns how many images were converted."""
    g = painted_node(mat)
    if g is None:
        raise RuntimeError(f"'{mat.name}' has no '{GROUP_NAME}' node")
    build_triplanar_groups()
    tree = mat.node_tree
    coords = next((n for n in tree.nodes if _is_group(n, TRIPLANAR_COORDS)), None)
    converted, old = 0, []
    for target in ("Grunge", "Dirt Texture", "Wear Texture"):
        sock = next((s for s in g.inputs if s.name == target), None)
        if sock is None:
            continue
        img, _ = _upstream(sock)
        if img is None or img.type != "TEX_IMAGE":
            continue  # (not connected, or already triplanar)
        vm, _ = _upstream(socket(img.inputs, "Vector"))
        if coords is None:
            x, y = img.location
            coords = _group_node(tree, TRIPLANAR_COORDS, x - 300, y, label="Overlay coordinates (params[7].x = Scale)")
            src = None
            if vm is not None and vm.type == "VECT_MATH" and vm.operation == "SCALE":
                src, src_out = _upstream(socket(vm.inputs, "Scale"))
                if src is not None:
                    tree.links.new(src_out, socket(coords.inputs, "Scale"))
                else:
                    socket(coords.inputs, "Scale").default_value = socket(vm.inputs, "Scale").default_value
        if vm is not None and vm.type == "VECT_MATH":  # (only nodes the old wiring used are cleaned up)
            old.append(vm)
            geo, _ = _upstream(vm.inputs[0])
            if geo is not None and geo.type == "NEW_GEOMETRY":
                old.append(geo)
        blend = add_triplanar(tree, img, coords)
        tree.links.new(socket(blend.outputs, "Color"), sock)
        converted += 1
    for n in old:  # the old coordinate nodes, once nothing uses them
        if n.name in tree.nodes and not any(o.is_linked for o in n.outputs):
            tree.nodes.remove(n)
    if converted or coords is not None:
        mat["ve_triplanar"] = True
    return converted


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

    # World-space triplanar overlays (the shader's sampling; see above).
    build_triplanar_groups()
    scale = b.node("ShaderNodeValue", -1100, -750, label="Overlay Scale (params[7].x)")
    scale.outputs[0].default_value = 0.5
    coords = _group_node(tree, TRIPLANAR_COORDS, -800, -600, label="Overlay coordinates")
    b.link(scale.outputs[0], socket(coords.inputs, "Scale"))
    y = -350
    for label, target in (("Grunge (Non-Color, R)", "Grunge"), ("Dirt texture (sRGB)", "Dirt Texture"), ("Wear texture (sRGB)", "Wear Texture")):
        img = b.node("ShaderNodeTexImage", -500, y, label=label + " -> " + target)
        img["ve_non_color"] = target == "Grunge"
        blend = add_triplanar(tree, img, coords)
        blend["ve_connect_to"] = target
        y -= 380
    mat["ve_triplanar"] = True

    note = tree.nodes.new("NodeFrame")
    note.label = "Pick images, then run the script again (or connect the image nodes by hand)"
    note.location = (-450, 600)
    note.label_size = 14
    connect_example_images(mat)
    return mat


def connect_example_images(mat):
    """Connects every image node that has an image to the group input it is labelled for."""
    tree = mat.node_tree
    group_node = ve_node(mat)
    if group_node is None:
        return
    sync_triplanar(mat)
    for node in tree.nodes:
        target = node.get("ve_connect_to")
        n = node
        if target and _is_group(node, TRIPLANAR_BLEND):
            n, _ = _upstream(socket(node.inputs, "X Face"))  # (the image is picked in the first node)
        if not target or n is None or n.type != "TEX_IMAGE" or n.image is None:
            continue
        if n.get("ve_non_color"):
            n.image.colorspace_settings.name = "Non-Color"
        # Game textures keep other data in alpha (terrain albedo: height): the
        # engine never multiplies colour by alpha, and neither does Channel Packed.
        n.image.alpha_mode = "CHANNEL_PACKED"
        out = socket(node.outputs, "Color")
        tree.links.new(out, socket(group_node.inputs, target))


# --- Exporting the engine's material file -----------------------------------
#
# Properties editor > Material tab > "VE Engine" panel:
#   Assets folder   the game's assets/ folder (found by itself when the .blend
#                   lives inside the project; saved in the .blend)
#   Export VE material / Export all VE materials / (automatically) glTF export
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
    """The image node feeding `sock` (through a VE Triplanar Blend: its first image)."""
    node, _ = _upstream(sock)
    if _is_group(node, TRIPLANAR_BLEND):
        node, _ = _upstream(socket(node.inputs, "X Face"))
    return node if node is not None and node.type == "TEX_IMAGE" else None


_NORMAL_WORDS = {"normal", "nrm", "nor", "norm", "n"}
_DATA_WORDS = {"rough", "roughness", "metal", "metallic", "metalness", "ao", "occlusion", "orm", "arm", "rma", "mr", "mask",
               "height", "disp", "displacement", "detail", "noise", "data", "lut", "opacity", "gloss", "spec", "specular"}


def texture_type_from_name(path):
    """What the engine loads an image as from its name (TextureSettings::typeFromName):
    the last word of the file name that isn't a size tag decides."""
    import re
    for word in reversed([w for w in re.split(r"[_\-. ]", os.path.splitext(os.path.basename(path))[0].lower()) if w]):
        if word.isdigit() or (len(word) >= 2 and word.endswith("k") and word[:-1].isdigit()):
            continue
        return "Normal" if word in _NORMAL_WORDS else "Data" if word in _DATA_WORDS else "Color"
    return "Color"


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


def material_mode(mat, alpha_used):
    """The engine mode for `mat`: the panel's choice, or (Auto) Cutout when the
    alpha is used (Transparent with Blender's Blended render method), else None
    (an existing file's mode is kept; a new file is Opaque)."""
    choice = getattr(mat, "ve_mode", "AUTO")
    if choice != "AUTO":
        return {"OPAQUE": "Opaque", "CUTOUT": "Cutout", "TRANSPARENT": "Transparent"}[choice]
    if not alpha_used:
        return None
    blended = getattr(mat, "surface_render_method", "") == "BLENDED" or getattr(mat, "blend_method", "") == "BLEND"
    return "Transparent" if blended else "Cutout"


def merge_alpha(albedo_node, alpha_node, alpha_from_alpha_output):
    """Writes <albedo>_rgba.png next to the albedo image: its RGB plus the alpha
    image's A (wired from its Alpha output) or R (from Color: a grey alpha
    map). The engine reads alpha from the albedo only. Rewritten only when a
    source is newer. Returns the written file's absolute path."""
    import numpy as np
    a_img, m_img = albedo_node.image, alpha_node.image
    src = os.path.normpath(bpy.path.abspath(a_img.filepath, library=a_img.library))
    msrc = os.path.normpath(bpy.path.abspath(m_img.filepath, library=m_img.library))
    out = os.path.splitext(src)[0] + "_rgba.png"
    if os.path.isfile(out) and all(os.path.getmtime(out) >= os.path.getmtime(s) for s in (src, msrc) if os.path.isfile(s)):
        return out
    w, h = a_img.size
    rgba = np.empty(w * h * 4, np.float32)
    a_img.pixels.foreach_get(rgba)
    m = m_img
    if tuple(m_img.size) != (w, h):
        m = m_img.copy()
        m.scale(w, h)
    mp = np.empty(w * h * 4, np.float32)
    m.pixels.foreach_get(mp)
    rgba[3::4] = mp[(3 if alpha_from_alpha_output else 0)::4]
    if m is not m_img:
        bpy.data.images.remove(m)
    tmp = bpy.data.images.new("ve_rgba_merge", w, h, alpha=True)
    try:
        tmp.colorspace_settings.name = a_img.colorspace_settings.name
        tmp.alpha_mode = "STRAIGHT"
        tmp.pixels.foreach_set(rgba)
        tmp.filepath_raw = out
        tmp.file_format = "PNG"
        tmp.save()
    finally:
        bpy.data.images.remove(tmp)
    return out


def export_material(mat, assets_root):
    """Writes the material file of `mat`'s VE Standard / VE Painted node;
    returns (path, warnings)."""
    g = ve_node(mat)
    if g is None:
        raise RuntimeError(f"'{mat.name}' has no '{STANDARD_GROUP}' or '{GROUP_NAME}' node")
    painted = node_kind(g) == "painted"
    if painted:
        sync_triplanar(mat)
    warnings, missing = [], []

    def inp(name):
        """The group node's input, or None if this file's copy of the group predates it."""
        s = next((x for x in g.inputs if x.name == name), None)
        if s is None and name not in missing:
            missing.append(name)
        return s

    def linked(name):
        s = inp(name)
        return s is not None and s.is_linked

    def relative(path):
        rel = os.path.relpath(path, assets_root) if assets_root else path
        if rel.startswith(".."):
            return None
        return rel.replace("\\", "/")

    def image_path(sock, what):
        if sock is None:
            return None, None
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
        rel = relative(path)
        if rel is None:
            warnings.append(f"{what}: {path} is outside the assets folder (absolute path written: not portable)")
            return path.replace("\\", "/"), node
        return rel, node

    def value(name):
        s = inp(name)
        v = s.default_value if s is not None else INPUT_DEFAULTS[name]
        try:
            return [round(float(x), 4) for x in v]
        except TypeError:
            return round(float(v), 4)

    def color3(name):
        return value(name)[:3]

    def check_type(rel, what, wanted, advice):
        # The engine decides how to load an image by its name or <image>.texture,
        # not by the slot (TextureSettings): a map it takes for colour is wrong.
        if not rel:
            return
        full = rel if os.path.isabs(rel) else os.path.join(assets_root, rel)
        settings = full + ".texture"
        if os.path.isfile(settings):
            try:
                with open(settings, encoding="utf-8") as f:
                    if str(json.load(f).get("type", "")).lower() == wanted.lower():
                        return
            except (OSError, ValueError, AttributeError):
                pass
        elif texture_type_from_name(rel) == wanted:
            return
        warnings.append(f"{what}: the engine won't load {os.path.basename(rel)} as {wanted.lower()}: {advice} "
                        f"or add {os.path.basename(rel)}.texture with {{\"type\": \"{wanted}\"}}")

    data = {"version": 1}
    if painted:
        data["fragmentShader"] = "standard_painted.frag"
        data["vertexColorWeight"] = 0
    else:
        data["fragmentShader"] = ""
        data["vertexColorWeight"] = value("Vertex Color Weight")

    # Base colour: an image (+ white), or the colour itself. Alpha: a constant
    # (baseColor's), or wired from an image: the albedo's own alpha, or another
    # image (Poly Haven's *_alpha) merged into <albedo>_rgba.png.
    albedo, albedo_node = image_path(inp("Base Color"), "Base Color")
    alpha_sock = inp("Alpha")
    alpha_linked = alpha_sock is not None and alpha_sock.is_linked
    if alpha_linked:
        src, out_sock = _upstream(alpha_sock)
        if src is None or src.type != "TEX_IMAGE" or src.image is None:
            warnings.append("Alpha: not wired straight from an image's Alpha / Color output (the engine reads alpha from the albedo image)")
        elif albedo_node is None or albedo_node.image is None or not albedo:
            warnings.append("Alpha: comes from an image but Base Color doesn't: the engine reads alpha from the albedo image")
        elif src.image == albedo_node.image and out_sock.name == "Alpha":
            if os.path.splitext(albedo)[1].lower() in (".jpg", ".jpeg"):
                warnings.append(f"Alpha: {os.path.basename(albedo)} is a JPEG: it has no alpha channel (use a PNG)")
        else:
            try:
                merged = merge_alpha(albedo_node, src, out_sock.name == "Alpha")
                albedo = relative(merged) or merged.replace("\\", "/")
                warnings.append(f"Alpha: from {src.image.name}: merged into {os.path.basename(merged)} (the engine's albedo)")
            except (RuntimeError, OSError, ValueError) as e:
                warnings.append(f"Alpha: couldn't merge {src.image.name} into the albedo: {e}")
    if albedo:
        data["albedo"] = albedo
        data["baseColor"] = [1.0, 1.0, 1.0, value("Alpha") if not alpha_linked else 1.0]
    else:
        data["albedo"] = ""
        data["baseColor"] = color3("Base Color") + [value("Alpha") if not alpha_linked else 1.0]
    mode = material_mode(mat, alpha_linked or value("Alpha") < 1.0)
    if mode is not None:
        data["mode"] = mode
    if mode == "Cutout":
        data["alphaCutoff"] = round(float(getattr(mat, "ve_alpha_cutoff", 0.5)), 4)

    orm, _ = image_path(inp("ORM"), "ORM")
    data["metallicRoughness"] = orm or ""
    data["occlusion"] = orm or ""  # glTF ORM: R = ambient occlusion (the engine uses it; Blender's preview doesn't)
    normal, _ = image_path(inp("Normal Map"), "Normal Map")
    check_type(normal, "Normal Map", "Normal", "name it *_normal (or *_n, *_nrm, *_nor)")
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

    if not painted:
        # Emission: an image (x white), or the colour; times the strength.
        emissive, _ = image_path(inp("Emission Color"), "Emission Color")
        data["emissiveTexture"] = emissive or ""
        data["emissive"] = [1.0, 1.0, 1.0] if emissive else color3("Emission Color")
        data["emissiveIntensity"] = value("Emission Strength")

    # Translucency (params[11] / textures[15], both shaders).
    data["translucency"] = value("Translucency")
    if linked("Translucency") or linked("Translucency Color"):
        warnings.append("Translucency / Translucency Color: only their values are exported (use Translucency Map for a texture)")
    data["translucencyColor"] = color3("Translucency Color")
    tmap, _ = image_path(inp("Translucency Map"), "Translucency Map")
    if tmap and data["translucency"] > 0:
        check_type(tmap, "Translucency Map", "Data", "name it *_mask (or *_opacity)")
    data["translucencyTexture"] = tmap or ""

    if painted:
        # Overlay images (slots 5-7) and their world scale (the Vector Math Scale
        # node, or its Value node, feeding their coordinates).
        textures, overlay_scale = [], None
        for slot, name in ((5, "Dirt Texture"), (6, "Grunge"), (7, "Wear Texture")):
            path, node = image_path(inp(name), name)
            if path:
                textures.append({"slot": slot, "path": path})
            if node is not None and overlay_scale is None:
                vm, _ = _upstream(socket(node.inputs, "Vector"))
                if _is_group(vm, TRIPLANAR_COORDS):
                    src, _ = _upstream(socket(vm.inputs, "Scale"))
                    overlay_scale = src.outputs[0].default_value if src is not None and src.type == "VALUE" else socket(vm.inputs, "Scale").default_value
                elif vm is not None and vm.type == "VECT_MATH" and vm.operation == "SCALE":
                    src, _ = _upstream(socket(vm.inputs, "Scale"))
                    overlay_scale = src.outputs[0].default_value if src is not None and src.type == "VALUE" else socket(vm.inputs, "Scale").default_value
                elif node.projection != "BOX":
                    warnings.append(f"{name}: not on world position x scale (Convert to triplanar): the engine samples it in world space")
        if not linked("Grunge") and value("Grunge Breakup") > 0:
            warnings.append("Grunge Breakup > 0 without a grunge image: the engine uses no breakup then")
        # Baked normal (slot 8) and params[9]: strength, its UV scale (a Mapping node on its UVs).
        baked, baked_node = image_path(inp("Baked Normal"), "Baked Normal")
        baked_scale = [1.0, 1.0]
        if baked and value("Baked Normal Strength") <= 0:
            # (The engine reads strength 0 as 1: a material without the value.)
            warnings.append("Baked Normal: strength 0: left out of the material file")
            baked = None
        check_type(baked, "Baked Normal", "Normal", "name it *_normal (or *_n, *_nrm, *_nor)")
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
    else:
        # (The standard shader reads no extra slots: a file switched from VE
        # Painted to VE Standard drops them.)
        data["textures"] = []
        data["params"] = []

    if missing:
        warnings.append(f"the '{g.node_tree.name}' group in this file is older than the add-on (no {', '.join(missing)}): "
                        "their defaults were exported. Press Create / update VE nodes to update it")

    path = os.path.join(assets_root, "materials", material_base_name(mat.name) + ".material")
    existing = {}
    if os.path.isfile(path):
        try:
            with open(path, encoding="utf-8") as f:
                existing = json.load(f)
        except (OSError, ValueError):
            warnings.append(f"{path} wasn't valid JSON: replaced")
    existing.update(data)  # (keeps fields this export doesn't manage: receiveDecals, ...)
    os.makedirs(os.path.dirname(path), exist_ok=True)
    with open(path, "w", encoding="utf-8", newline="\n") as f:
        json.dump(existing, f, indent=2)
        f.write("\n")
    return path, warnings


def node_kind(node):
    """"standard" / "painted" for a VE group node (also an older ".001" copy), else None."""
    name = node.node_tree.name if node is not None and node.type == "GROUP" and node.node_tree else ""
    return "standard" if name.startswith(STANDARD_GROUP) else "painted" if name.startswith(GROUP_NAME) else None


def ve_node(mat):
    """The material's VE Standard or VE Painted group node, or None."""
    if mat is None or mat.node_tree is None or (bpy.app.version < (5, 0, 0) and not mat.use_nodes):
        return None
    return next((n for n in mat.node_tree.nodes if node_kind(n) is not None), None)


def painted_node(mat):
    """The material's VE Painted group node, or None."""
    n = ve_node(mat)
    return n if node_kind(n) == "painted" else None


def export_materials(materials, assets_root):
    """Exports every VE Standard / VE Painted material in `materials` (once per file name:
    "Rust" and "Rust.001" share materials/Rust.material, the first one wins).
    Returns (written paths, warnings)."""
    written, warnings, done = [], [], set()
    for mat in materials:
        if ve_node(mat) is None:
            continue
        base = material_base_name(mat.name)
        if base in done:
            continue
        done.add(base)
        try:
            path, w = export_material(mat, assets_root)
            written.append(path)
            warnings += [f"{mat.name}: {x}" for x in w]
        except (RuntimeError, OSError) as e:
            warnings.append(f"{mat.name}: {e}")
    return written, warnings


# --- Converting imported PBR materials to VE Standard ------------------------
#
# A glTF / FBX import (or a Poly Haven / Sketchfab download) arrives as a
# Principled BSDF, sometimes mixed with a Translucent BSDF (leaves). This
# rewires the same images and values into a VE Standard node, so the export
# writes a material file that looks like it. The old nodes stay (disconnected)
# in case something didn't carry over: the notes say what.

def _surface_shader(tree):
    """(output node, the shader node feeding its Surface)."""
    outs = [n for n in tree.nodes if n.type == "OUTPUT_MATERIAL"]
    out = next((n for n in outs if n.is_active_output), outs[0] if outs else None)
    if out is None:
        return None, None
    node, _ = _upstream(socket(out.inputs, "Surface"))
    return out, node


def _channel_source(sock):
    """(image node, channel index) when `sock` is fed by a Separate Color /
    RGB node on an image's Color, else (None, None)."""
    node, out = _upstream(sock)
    if node is None or node.type not in ("SEPARATE_COLOR", "SEPRGB"):
        return None, None
    img, _ = _upstream(node.inputs[0])
    if img is None or img.type != "TEX_IMAGE":
        return None, None
    return img, list(node.outputs).index(out)


def convert_to_standard(mat):
    """Replaces `mat`'s Principled BSDF (+ Translucent BSDF) with a VE Standard
    node wired to the same images and values. Returns notes (what didn't carry over)."""
    if mat is None or mat.node_tree is None:
        raise RuntimeError("no node material")
    if ve_node(mat) is not None:
        raise RuntimeError(f"'{mat.name}' already has a VE node")
    tree = mat.node_tree
    out, shader = _surface_shader(tree)
    if shader is None:
        raise RuntimeError(f"'{mat.name}': nothing is connected to the material output's Surface")
    principled, translucent, amount, notes = None, None, 0.0, []
    if shader.type == "BSDF_PRINCIPLED":
        principled = shader
    elif shader.type in ("MIX_SHADER", "ADD_SHADER"):
        first, second = (1, 2) if shader.type == "MIX_SHADER" else (0, 1)
        a, _ = _upstream(shader.inputs[first])
        b, _ = _upstream(shader.inputs[second])
        principled = next((n for n in (a, b) if n is not None and n.type == "BSDF_PRINCIPLED"), None)
        translucent = next((n for n in (a, b) if n is not None and n.type == "BSDF_TRANSLUCENT"), None)
        if translucent is not None:
            if shader.type == "MIX_SHADER":
                if shader.inputs[0].is_linked:
                    notes.append("the Translucent mix factor is wired: used 0.5")
                    fac = 0.5
                else:
                    fac = shader.inputs[0].default_value
                amount = fac if translucent is b else 1.0 - fac
            else:
                amount = 0.5
                notes.append("Principled + Translucent added (not mixed): translucency 0.5 (the engine splits the light, Add doesn't)")
    if principled is None:
        raise RuntimeError(f"'{mat.name}': no Principled BSDF on the Surface (found {shader.type})")

    if STANDARD_GROUP not in bpy.data.node_groups:
        build_standard_group()
    g = tree.nodes.new("ShaderNodeGroup")
    g.node_tree = bpy.data.node_groups[STANDARD_GROUP]
    g.location = (principled.location.x, principled.location.y + 250)
    g.width = 240
    P = lambda name: socket(principled.inputs, name)
    G = lambda name: socket(g.inputs, name)

    def carry(src_name, dst_name):
        """Links what feeds the Principled input to the group's, else copies its value."""
        src = P(src_name)
        if src.is_linked:
            tree.links.new(src.links[0].from_socket, G(dst_name))
        else:
            v = src.default_value
            G(dst_name).default_value = v if not hasattr(v, "__len__") or len(v) == len(G(dst_name).default_value) else (*v[:3], 1.0)

    carry("Base Color", "Base Color")
    carry("Alpha", "Alpha")
    # Roughness / metallic: the engine reads them from one ORM image (G, B).
    orm_img = None
    for name, channel in (("Roughness", 1), ("Metallic", 2)):
        src = P(name)
        if not src.is_linked:
            G(name).default_value = src.default_value
            continue
        img, ch = _channel_source(src)
        if img is not None and ch == channel and (orm_img is None or orm_img == img):
            orm_img = img
            G(name).default_value = 1.0
        else:
            from_node, _ = _upstream(src)
            notes.append(f"{name}: wired from {from_node.name if from_node else '?'}, not an ORM image's "
                         f"{'GB'[channel - 1]} channel: kept the value {round(src.default_value, 3)} (pack it into an ORM)")
            G(name).default_value = src.default_value
    if orm_img is not None:
        tree.links.new(socket(orm_img.outputs, "Color"), G("ORM"))
        if orm_img.image is not None:
            orm_img.image.colorspace_settings.name = "Non-Color"
    # Normal: Principled Normal <- Normal Map node <- image.
    nm, _ = _upstream(P("Normal"))
    if nm is not None and nm.type == "NORMAL_MAP":
        img_sock = socket(nm.inputs, "Color")
        if img_sock.is_linked:
            tree.links.new(img_sock.links[0].from_socket, G("Normal Map"))
        G("Normal Strength").default_value = socket(nm.inputs, "Strength").default_value
        if nm.space != "TANGENT":
            notes.append(f"Normal Map node in {nm.space} space: the engine uses tangent space")
    elif nm is not None:
        notes.append(f"Normal: wired from {nm.name} (not a Normal Map node): left flat")
    # Emission.
    try:
        carry("Emission Color", "Emission Color")
        carry("Emission Strength", "Emission Strength")
    except KeyError:
        pass
    # Translucency: the mix factor; its colour relative to the albedo.
    if translucent is not None:
        G("Translucency").default_value = max(0.0, min(1.0, amount))
        tc = socket(translucent.inputs, "Color")
        base = P("Base Color")
        if tc.is_linked and base.is_linked and tc.links[0].from_node == base.links[0].from_node:
            G("Translucency Color").default_value = (1.0, 1.0, 1.0, 1.0)
        elif not tc.is_linked and not base.is_linked:
            G("Translucency Color").default_value = tuple(min(1.0, c / max(b, 1e-3)) for c, b in zip(tc.default_value[:3], base.default_value[:3])) + (1.0,)
        else:
            notes.append("Translucent BSDF colour isn't the base colour: Translucency Color left white (tint it by hand)")
    for name in ("Subsurface Weight", "Transmission Weight", "Coat Weight", "Sheen Weight"):
        s = next((x for x in principled.inputs if x.name == name), None)
        if s is not None and (s.is_linked or s.default_value > 0):
            notes.append(f"{name}: the engine has no {name.split()[0].lower()} (ignored)")
    tree.links.new(socket(g.outputs, "BSDF"), socket(out.inputs, "Surface"))
    return notes


class VE_OT_convert_standard(bpy.types.Operator):
    """Rewire Principled BSDF materials (+ Translucent BSDF) into VE Standard nodes, keeping their images and values"""
    bl_idname = "ve.convert_to_standard"
    bl_label = "Convert to VE Standard"
    bl_options = {"REGISTER", "UNDO"}

    selected: bpy.props.BoolProperty(name="Selected objects", description="Every material of the selected objects, not just this one")

    def execute(self, context):
        if self.selected:
            mats = {s.material for o in context.selected_objects for s in getattr(o, "material_slots", []) if s.material}
        else:
            mats = {getattr(context, "material", None)} - {None}
        done = 0
        for mat in sorted(mats, key=lambda m: m.name):
            if ve_node(mat) is not None:
                continue
            try:
                for n in convert_to_standard(mat):
                    self.report({"WARNING"}, f"{mat.name}: {n}")
                done += 1
            except RuntimeError as e:
                self.report({"WARNING"}, str(e))
        self.report({"INFO"}, f"{done} material(s) converted to {STANDARD_GROUP}")
        return {"FINISHED"}


# Shown in the panel: what the last automatic export (with a glTF) did.
_last_gltf_export = ""


class glTF2ExportUserExtension:
    """Hook of Blender's glTF exporter (it calls this class of every enabled
    add-on): writes the .material file of each VE Standard / VE Painted material the glTF
    export includes. Only active when this file is installed as an add-on."""

    def __init__(self):
        self.enabled = bool(getattr(bpy.context.scene, "ve_export_with_gltf", True))

    def gather_material_hook(self, gltf2_material, blender_material, *args):
        global _last_gltf_export
        export_settings = args[-1] if args and isinstance(args[-1], dict) else {}
        if not self.enabled or ve_node(blender_material) is None:
            return
        state = export_settings.setdefault("ve_painted_export", {"done": set(), "written": 0, "warnings": 0})
        base = material_base_name(blender_material.name)
        if base in state["done"]:  # (the hook runs once per mesh using the material)
            return
        state["done"].add(base)
        root = _assets_root(bpy.context)
        gltf_path = export_settings.get("gltf_filepath", "")
        if gltf_path and not bpy.context.scene.ve_assets_root:
            root = find_assets_root(os.path.dirname(os.path.abspath(gltf_path))) or root
        if not root or not os.path.isdir(root):
            print(f"VE materials: {blender_material.name}: no assets folder found (set it in the VE Engine panel): material file not written")
            state["warnings"] += 1
        else:
            written, warnings = export_materials([blender_material], root)
            for w in warnings:
                print("VE materials: warning:", w)
            for path in written:
                print("VE materials: wrote", path)
            state["written"] += len(written)
            state["warnings"] += len(warnings)
        _last_gltf_export = f"Last glTF export: {state['written']} material file(s), {state['warnings']} warning(s) (see the console)"


class VE_OT_export_material(bpy.types.Operator):
    """Write the engine's .material file for this material (its VE Standard / VE Painted node's values and images)"""
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


class VE_OT_export_all_materials(bpy.types.Operator):
    """Write the .material file of every material in this file that uses a VE Standard / VE Painted node"""
    bl_idname = "ve.export_all_materials"
    bl_label = "Export all VE materials"
    bl_options = {"REGISTER"}

    def execute(self, context):
        root = _assets_root(context)
        if not root or not os.path.isdir(root):
            self.report({"ERROR"}, "Set the assets folder (VE Engine panel) first")
            return {"CANCELLED"}
        written, warnings = export_materials(bpy.data.materials, root)
        for w in warnings:
            self.report({"WARNING"}, w)
        if not written and not warnings:
            self.report({"WARNING"}, f"No material in this file uses a '{STANDARD_GROUP}' or '{GROUP_NAME}' node")
            return {"CANCELLED"}
        self.report({"INFO"}, f"Wrote {len(written)} material file(s) to {os.path.join(root, 'materials')}")
        return {"FINISHED"}


class VE_OT_convert_triplanar(bpy.types.Operator):
    """Rewire this material's Box-projected grunge / dirt / wear images to world-space triplanar sampling (exactly as the engine samples them, whatever the object's rotation)"""
    bl_idname = "ve.convert_triplanar"
    bl_label = "Convert overlays to triplanar"
    bl_options = {"REGISTER", "UNDO"}

    @classmethod
    def poll(cls, context):
        return painted_node(getattr(context, "material", None)) is not None

    def execute(self, context):
        n = convert_to_triplanar(context.material)
        self.report({"INFO"}, f"{n} image(s) now sampled triplanar" if n else "Nothing to convert (already triplanar, or no overlay images)")
        return {"FINISHED"}


class VE_OT_build_group(bpy.types.Operator):
    """Create / update the VE Standard and VE Painted node groups and their example materials in this file"""
    bl_idname = "ve.build_painted_group"
    bl_label = "Create / update VE nodes"
    bl_options = {"REGISTER", "UNDO"}

    def execute(self, context):
        main()
        self.report({"INFO"}, f"{STANDARD_GROUP} and {GROUP_NAME} node groups ready")
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
            if ve_node(mat) is not None:
                layout.label(text="-> materials/" + material_base_name(mat.name) + ".material")
                row = layout.row(align=True)
                row.prop(mat, "ve_mode")
                cut = row.row(align=True)
                cut.active = mat.ve_mode in ("AUTO", "CUTOUT")
                cut.prop(mat, "ve_alpha_cutoff")
            else:
                layout.label(text=f"No '{STANDARD_GROUP}' / '{GROUP_NAME}' node in this material", icon="ERROR")
                row = layout.row(align=True)
                row.operator(VE_OT_convert_standard.bl_idname, icon="NODETREE").selected = False
                row.operator(VE_OT_convert_standard.bl_idname, text="Convert selected objects", icon="NODETREE").selected = True
        row = layout.row(align=True)
        row.operator(VE_OT_export_material.bl_idname, icon="EXPORT")
        row.operator(VE_OT_export_all_materials.bl_idname, icon="EXPORT")
        layout.prop(context.scene, "ve_export_with_gltf")
        g = painted_node(mat)
        if g is not None and any(n.type == "TEX_IMAGE" for n in (_upstream(s)[0] for s in g.inputs
                                                                 if s.name in ("Grunge", "Dirt Texture", "Wear Texture")) if n is not None):
            layout.label(text="Overlays use Box projection: wrong on rotated objects", icon="ERROR")
            layout.operator(VE_OT_convert_triplanar.bl_idname, icon="MOD_UVPROJECT")
        if __name__ == "__main__":
            layout.label(text="Install this file as an add-on for the glTF export hook", icon="INFO")
        elif _last_gltf_export:
            layout.label(text=_last_gltf_export, icon="CHECKMARK")
        layout.operator(VE_OT_build_group.bl_idname, icon="NODETREE")


_classes = (VE_OT_export_material, VE_OT_export_all_materials, VE_OT_convert_triplanar, VE_OT_convert_standard, VE_OT_build_group,
            VE_PT_material)


def _set_handler(on):
    handlers = bpy.app.handlers.depsgraph_update_post
    for h in list(handlers):
        if getattr(h, "__name__", "") == "_ve_sync_triplanar":  # (also an older run's copy)
            handlers.remove(h)
    if on:
        handlers.append(_ve_sync_triplanar)


def register():
    bpy.types.Scene.ve_assets_root = bpy.props.StringProperty(
        name="Assets folder", subtype="DIR_PATH",
        description="The game's assets/ folder (empty: found above the exported .gltf, else above this .blend)")
    bpy.types.Scene.ve_export_with_gltf = bpy.props.BoolProperty(
        name="Write with glTF export", default=True,
        description="When exporting glTF (File > Export > glTF 2.0), also write the .material file of every VE material it exports")
    bpy.types.Material.ve_mode = bpy.props.EnumProperty(
        name="Mode", default="AUTO",
        items=(("AUTO", "Auto", "Cutout when the Alpha input is used (Transparent with the Blended render method), else the file's / Opaque"),
               ("OPAQUE", "Opaque", "Solid, one sided"),
               ("CUTOUT", "Cutout", "Alpha tested at the cutoff, double sided, casts cut-out shadows (leaves, fences)"),
               ("TRANSPARENT", "Transparent", "Blended, no shadows (glass, water)")),
        description="The engine material mode written to the .material file")
    bpy.types.Material.ve_alpha_cutoff = bpy.props.FloatProperty(
        name="Cutoff", default=0.5, min=0.0, max=1.0, description="Cutout: alpha below this is cut away")
    for c in _classes:
        try:
            bpy.utils.unregister_class(c)  # (running the script again)
        except RuntimeError:
            pass
        bpy.utils.register_class(c)
    _set_handler(True)


def unregister():
    _set_handler(False)
    for c in reversed(_classes):
        bpy.utils.unregister_class(c)
    del bpy.types.Scene.ve_assets_root
    del bpy.types.Scene.ve_export_with_gltf
    del bpy.types.Material.ve_mode
    del bpy.types.Material.ve_alpha_cutoff


def flatten_unconnected_normals():
    """Unconnected Normal Map / Baked Normal inputs of every VE node
    back to flat (0.5, 0.5, 1): a constant there is never meant (the engine
    ignores it), and a socket added by an update can start out black or grey."""
    flat = (0.5, 0.5, 1.0, 1.0)
    for mat in bpy.data.materials:
        g = ve_node(mat)
        if g is None:
            continue
        for s in g.inputs:
            if s.name in ("Normal Map", "Baked Normal") and not s.is_linked and tuple(round(v, 3) for v in s.default_value) != flat:
                s.default_value = flat


def main():
    if bpy.app.version < (4, 0, 0):
        raise RuntimeError("The VE nodes need Blender 4.0 or newer (node group interface API)")
    group = build_group()
    standard = build_standard_group()
    build_triplanar_groups()
    flatten_unconnected_normals()
    for name, build, g in ((EXAMPLE_MATERIAL, build_example, group), (STANDARD_EXAMPLE, build_standard_example, standard)):
        mat = bpy.data.materials.get(name)
        if mat is not None and mat.node_tree and any(n.get("ve_connect_to") for n in mat.node_tree.nodes):
            connect_example_images(mat)  # re-run: keep the user's images, just wire the newly picked ones
        else:
            build(g)
    print(f"{STANDARD_GROUP} ({len(standard.nodes)} nodes), {GROUP_NAME} ({len(group.nodes)} nodes): node groups ready; "
          f"example materials '{STANDARD_EXAMPLE}', '{EXAMPLE_MATERIAL}'")


if __name__ == "__main__":
    # Run from the Text Editor: build the group and add the VE Engine panel
    # (for this session; install the file as an add-on to always have it).
    main()
    register()
