"""Renders a preview of a generated foliage file with Blender (Cycles, CPU).

    blender -b -P tools/foliage/preview_blender.py -- <file.gltf> <out.png> [lod] [samples]

Imports the glTF, keeps one LOD level of every variant (default 0) side by
side, adds a sun + sky and a ground plane, and frames them from a three-quarter view.
"""

import math
import os
import sys

import bpy
from mathutils import Vector

argv = sys.argv[sys.argv.index("--") + 1:]
src, out = argv[0], argv[1]
lod = int(argv[2]) if len(argv) > 2 else 0
samples = int(argv[3]) if len(argv) > 3 else 24

bpy.ops.wm.read_factory_settings(use_empty=True)
bpy.ops.import_scene.gltf(filepath=os.path.abspath(src))

keep = []
for o in list(bpy.data.objects):
    base = o.name.split(".")[0]
    if base.endswith(f"_LOD{lod}"):
        keep.append(o)
    else:
        bpy.data.objects.remove(o, do_unlink=True)
# Lay the variants out in a row (they may be side by side already).
xmin, xmax, top = 1e9, -1e9, 0.0
for o in keep:
    for c in o.bound_box:
        w = o.matrix_world @ Vector(c)
        xmin, xmax, top = min(xmin, w.x), max(xmax, w.x), max(top, w.z)

scene = bpy.context.scene
scene.render.engine = "CYCLES"
scene.cycles.samples = samples
scene.cycles.device = "CPU"
scene.cycles.use_denoising = False
scene.render.resolution_x, scene.render.resolution_y = 1400, 700
scene.render.film_transparent = False
scene.view_settings.view_transform = "Filmic" if "Filmic" in [i.identifier for i in scene.view_settings.bl_rna.properties["view_transform"].enum_items] else "Standard"

world = bpy.data.worlds.new("Sky")
scene.world = world
world.use_nodes = True
nt = world.node_tree
sky = nt.nodes.new("ShaderNodeTexSky")
sky.sky_type = "NISHITA" if hasattr(sky, "sky_type") else sky.sky_type
try:
    sky.sun_elevation = math.radians(35)
    sky.sun_disc = False
    sky.sun_rotation = math.radians(140)
except AttributeError:
    pass
bg = nt.nodes["Background"]
bg.inputs["Strength"].default_value = 0.06
nt.links.new(sky.outputs[0], bg.inputs[0])

sun = bpy.data.objects.new("Sun", bpy.data.lights.new("Sun", "SUN"))
sun.data.energy = 3.0
sun.data.angle = math.radians(1.5)
sun.rotation_euler = (math.radians(50), math.radians(10), math.radians(140))
scene.collection.objects.link(sun)

bpy.ops.mesh.primitive_plane_add(size=400, location=((xmin + xmax) / 2, 0, 0))
ground = bpy.context.active_object
gm = bpy.data.materials.new("Ground")
gm.use_nodes = True
gm.node_tree.nodes["Principled BSDF"].inputs["Base Color"].default_value = (0.32, 0.25, 0.16, 1)
ground.data.materials.append(gm)

cam = bpy.data.objects.new("Cam", bpy.data.cameras.new("Cam"))
scene.collection.objects.link(cam)
scene.camera = cam
cam.data.lens = 35
width = max(xmax - xmin, top)
centre = Vector(((xmin + xmax) / 2, 0, top * 0.45))
dist = width * 0.95 + 6
cam.location = centre + Vector((0, -dist, top * 0.25 + 1.5))
cam.rotation_euler = (centre - cam.location).to_track_quat("-Z", "Y").to_euler()
scene.render.filepath = os.path.abspath(out)
bpy.ops.render.render(write_still=True)
