"""Build a Lyra Shooter Core arena from a Doom map exported by DoomUE.

1. In DoomUE:  UnrealEditor-Cmd DoomUE.uproject -run=DoomContent -exportlyra=E1M1 -out=<Lyra>/Saved/DoomExport/E1M1
2. In Lyra:    UnrealEditor LyraStarterGame.uproject -ExecutePythonScript="<this file>"
               (or Tools > Execute Python Script in the Lyra editor; it needs a real editor level, so not the
               pythonscript commandlet. DOOM_EXPORT=<dir> selects another export, DOOM_QUIT=1 closes the editor.)

The level (/Game/Maps/L_Doom<MAP>) uses Lyra's own pieces:
  * the Shooter Core Elimination experience (heroes, abilities, teams, bots, HUD) in its World Settings,
    and a playlist entry (/Game/System/Playlists/DA_Doom<MAP>) so it appears in Lyra's Play menu;
  * LyraPlayerStarts at Doom's co-op and deathmatch starts;
  * B_WeaponSpawner pads where Doom has weapons (shotgun, rifle, pistol) and health (heal pickups);
  * B_Launcher_Up pads where Doom has lifts, B_Teleport pairs where it has teleporters;
  * Lyra's example-content materials by surface role (walls, tile floors, metal trim, orange doors...),
    an atmosphere sun, sky light, fog and Lumen, point lights at Doom's lamps and bright rooms;
  * invisible collision ramps over Doom's steps and a nav mesh for the bots.
"""
import json
import math
import os
import re

import unreal

EXPORT = os.environ.get("DOOM_EXPORT", r"E:/Work/LyraStarterGame/Saved/DoomExport/E1M1")

MATERIALS = {
    "Wall": "/LyraExampleContent/Materials/MI_MS_Surface_Wall_1",
    "Floor": "/LyraExampleContent/Materials/MI_Tile_1",
    "Ceiling": "/LyraExampleContent/Materials/MI_MS_Surface_2",
    "Metal": "/LyraExampleContent/Materials/MI_MS_Basic_Metal",
    "Door": "/LyraExampleContent/Materials/MI_MS_Orange_1",
    "Accent": "/LyraExampleContent/Materials/MI_MS_Red_1",
    "Light": "/LyraExampleContent/Materials/MI_Glass_Tile",
    "Liquid": "/LyraExampleContent/Materials/MI_MS_Blue_2",
    "Ramp": "/Engine/EngineMaterials/WorldGridMaterial",
}
PICKUPS = {
    "shotgun": "/ShooterCore/Weapons/Shotgun/WeaponPickupData_Shotgun",
    "rifle": "/ShooterCore/Weapons/Rifle/WeaponPickupData_Rifle",
    "pistol": "/ShooterCore/Weapons/Pistol/WeaponPickupData_Pistol",
    "heal_big": "/ShooterCore/Items/HealthPickup/WeaponPickupData_Heal_Big",
    "heal_small": "/ShooterCore/Items/HealthPickup/WeaponPickupData_Heal_Small",
}
EXPERIENCE_CLASS = "/ShooterCore/Experiences/B_ShooterGame_Elimination.B_ShooterGame_Elimination_C"
PLAYLIST_TEMPLATE = "/ShooterCore/System/Playlists/DA_ShooterGame_ShooterGym"


def log(msg):
    unreal.log_warning("DoomLyra: " + msg)


def lyra_obj(src, dst):
    """Copies an exported OBJ for import: without groups (one mesh, a slot per role) and pre-mirrored
    in Y, because the OBJ importer mirrors Y (OBJ is right-handed) and keeps the face winding, so
    mirroring the positions back keeps every face pointing the way DoomUE built it."""
    out = []
    for line in open(src).read().splitlines():
        if line.startswith("g "):
            continue
        if line.startswith("v ") or line.startswith("vn "):
            tag, x, y, z = line.split()
            out.append("%s %s %s %s" % (tag, x, -float(y), z))
        else:
            out.append(line)
    os.makedirs(os.path.dirname(dst), exist_ok=True)
    open(dst, "w").write("\n".join(out) + "\n")


def import_meshes(files, dest):
    tasks = []
    for f in files:
        staged = os.path.join(EXPORT, "_lyra", os.path.basename(f))
        lyra_obj(os.path.join(EXPORT, f), staged)
        t = unreal.AssetImportTask()
        t.filename = staged
        t.destination_path = dest
        t.automated = True
        t.replace_existing = True
        t.save = False
        tasks.append(t)
    unreal.AssetToolsHelpers.get_asset_tools().import_asset_tasks(tasks)
    meshes = []
    for t in tasks:
        for p in t.imported_object_paths:
            a = unreal.load_asset(p)
            if isinstance(a, unreal.StaticMesh):
                meshes.append(a)
    return meshes


def dress_mesh(mesh):
    """Lyra materials by slot (surface role) and per-polygon collision for the level geometry."""
    slots = mesh.static_materials
    for i, slot in enumerate(slots):
        path = MATERIALS.get(str(slot.material_slot_name), MATERIALS["Wall"])
        mat = unreal.load_asset(path)
        if mat:
            mesh.set_material(i, mat)
    body = mesh.get_editor_property("body_setup")
    if body:
        body.set_editor_property("collision_trace_flag", unreal.CollisionTraceFlag.CTF_USE_COMPLEX_AS_SIMPLE)
    # the OBJ importer leaves section collision off: switch it on so the polygons collide
    sub = unreal.get_editor_subsystem(unreal.StaticMeshEditorSubsystem)
    if sub:
        before = sub.is_section_collision_enabled(mesh, 0, 0)
        for i in range(len(slots)):
            sub.enable_section_collision(mesh, True, 0, i)
        log("%s: section collision %s -> %s, complexity %s" % (mesh.get_name(), before, sub.is_section_collision_enabled(mesh, 0, 0), sub.get_collision_complexity(mesh)))
    unreal.EditorAssetLibrary.save_loaded_asset(mesh)


def set_any(actor, names, value):
    """Sets the first Blueprint variable of these names that the actor has; False keeps Lyra's default."""
    for n in names:
        try:
            actor.set_editor_property(n, value)
            log("%s.%s set" % (actor.get_class().get_name(), n))
            return True
        except Exception:
            pass
    log("%s: none of %s, keeping the Blueprint's default" % (actor.get_class().get_name(), names))
    return False


def tweak(component, **values):
    """Sets component properties, through their setter functions when they are not editable properties."""
    for name, value in values.items():
        try:
            component.set_editor_property(name, value)
            continue
        except Exception:
            pass
        setter = getattr(component, "set_" + name, None)
        try:
            if setter:
                setter(value)
                continue
        except Exception as e:
            log("%s.%s: %s" % (component.get_class().get_name(), name, e))
            continue
        log("%s has no %s" % (component.get_class().get_name(), name))


def vec(d, dz=0.0):
    return unreal.Vector(d["x"], d["y"], d["z"] + dz)


def spawn(cls_path, location, yaw=0.0, label=None, folder=None):
    eas = unreal.get_editor_subsystem(unreal.EditorActorSubsystem)
    cls = unreal.load_class(None, cls_path)
    if cls is None:
        log("missing class " + cls_path)
        return None
    actor = eas.spawn_actor_from_class(cls, location, unreal.Rotator(0, 0, yaw))
    if actor:
        if label:
            actor.set_actor_label(label)
        if folder:
            actor.set_folder_path(folder)
    return actor


def main():
    info = json.load(open(os.path.join(EXPORT, info_file())))
    map_name = info["map"]
    level_path = "/Game/Maps/L_Doom%s" % map_name
    mesh_dir = "/Game/DoomArena/%s/Meshes" % map_name

    meshes = import_meshes(info["tiles"], mesh_dir)
    ramps = import_meshes([info["ramps"]], mesh_dir)
    for m in meshes + ramps:
        dress_mesh(m)
    log("imported %d tiles and %d ramp meshes" % (len(meshes), len(ramps)))

    les = unreal.get_editor_subsystem(unreal.LevelEditorSubsystem)
    if unreal.EditorAssetLibrary.does_asset_exist(level_path):
        unreal.EditorAssetLibrary.delete_asset(level_path)
    if not les.new_level(level_path):
        log("could not create " + level_path)
        return
    world = unreal.get_editor_subsystem(unreal.UnrealEditorSubsystem).get_editor_world()
    eas = unreal.get_editor_subsystem(unreal.EditorActorSubsystem)

    # geometry (the meshes are in world space)
    for m in meshes:
        a = eas.spawn_actor_from_object(m, unreal.Vector(0, 0, 0))
        a.set_actor_label(m.get_name())
        a.set_folder_path("Doom/Geometry")
    for m in ramps:
        a = eas.spawn_actor_from_object(m, unreal.Vector(0, 0, 0))
        a.set_actor_label("StepRamps")
        a.set_folder_path("Doom/Geometry")
        a.set_actor_hidden_in_game(True)  # collision only: Doom's 24-unit steps are taller than a Lyra step

    # player starts (the capsule centre sits half a mannequin above the floor)
    n = 0
    for s in info["playerStarts"]:
        n += 1
        spawn("/Script/LyraGame.LyraPlayerStart", vec(s, 100), s["yaw"], "PlayerStart_%s_%d" % (s["kind"], n), "Doom/Starts")

    # pickups on Lyra's weapon spawner pads
    for i, item in enumerate(info["items"]):
        data = unreal.load_asset(PICKUPS[item["kind"]])
        a = spawn("/ShooterCore/Blueprint/B_WeaponSpawner.B_WeaponSpawner_C", vec(item), item["yaw"], "Pickup_%s_%d" % (item["kind"], i), "Doom/Pickups")
        if a and data:
            a.set_editor_property("WeaponDefinition", data)

    # lifts become launch pads strong enough for the old lift height
    for i, lift in enumerate(info["lifts"]):
        a = spawn("/ShooterCore/Blueprint/B_Launcher_Up.B_Launcher_Up_C", vec(lift), 0, "LaunchPad_%d" % i, "Doom/Lifts")
        if a:
            up = math.sqrt(2.0 * 980.0 * 1.6 * (lift["rise"] + 150.0))
            if not set_any(a, ["LaunchVelocity"], unreal.Vector(0, 0, up)):
                ok = unreal.DoomLyraToolsLibrary.set_property_from_text(a, "Launch Velocity", "(X=0,Y=0,Z=%.0f)" % up)
                log("LaunchPad_%d Launch Velocity Z %.0f: %s" % (i, up, ok))

    # teleporters: a sender on the line, a receiver at the destination
    for i, tp in enumerate(info["teleporters"]):
        dest = spawn("/ShooterCore/Blueprint/B_Teleport.B_Teleport_C", vec(tp["to"]), tp["toYaw"], "TeleportExit_%d" % i, "Doom/Teleporters")
        src = spawn("/ShooterCore/Blueprint/B_Teleport.B_Teleport_C", vec(tp["from"]), tp["fromYaw"], "Teleport_%d" % i, "Doom/Teleporters")
        if dest and src:
            if not set_any(dest, ["ReceiverOnly"], True):
                unreal.DoomLyraToolsLibrary.set_property_from_text(dest, "Receiver Only", "True")
            set_any(src, ["TeleporterDestination", "Teleporter Destination"], [dest])

    # Lyra-style lighting: atmosphere sun, real-time sky light, height fog, clouds
    sun = spawn("/Script/Engine.DirectionalLight", unreal.Vector(0, 0, 4000), 0, "Sun", "Lighting")
    if sun:
        sun.set_actor_rotation(unreal.Rotator(0, -50, 35), False)
        c = sun.get_component_by_class(unreal.DirectionalLightComponent)
        tweak(c, atmosphere_sun_light=True, intensity=6.0, light_color=unreal.Color(255, 236, 214, 255))
    spawn("/Script/Engine.SkyAtmosphere", unreal.Vector(0, 0, 0), 0, "SkyAtmosphere", "Lighting")
    sky = spawn("/Script/Engine.SkyLight", unreal.Vector(0, 0, 3000), 0, "SkyLight", "Lighting")
    if sky:
        sc = sky.get_component_by_class(unreal.SkyLightComponent)
        tweak(sc, mobility=unreal.ComponentMobility.MOVABLE, real_time_capture=True, intensity=1.2)
    fog = spawn("/Script/Engine.ExponentialHeightFog", unreal.Vector(0, 0, -600), 0, "HeightFog", "Lighting")
    if fog:
        fc = fog.get_component_by_class(unreal.ExponentialHeightFogComponent)
        tweak(fc, fog_density=0.008, volumetric_fog=True)
    spawn("/Script/Engine.VolumetricCloud", unreal.Vector(0, 0, 0), 0, "Clouds", "Lighting")
    pp = spawn("/Script/Engine.PostProcessVolume", unreal.Vector(0, 0, 0), 0, "PostProcess", "Lighting")
    if pp:
        pp.set_editor_property("unbound", True)

    for i, l in enumerate(info["lights"]):
        a = spawn("/Script/Engine.PointLight", vec(l), 0, "Light_%d" % i, "Lighting/Doom")
        if a:
            c = a.get_component_by_class(unreal.PointLightComponent)
            room = "radius" in l
            tweak(c, light_color=unreal.Color(int(255 * min(1, l["r"])), int(255 * min(1, l["g"])), int(255 * min(1, l["b"])), 255),
                  intensity=30.0 * l["intensity"] if room else 60.0,
                  attenuation_radius=min(2500.0, l["radius"]) if room else 900.0,
                  cast_shadows=not room)

    # navigation for the bots, also rebuilt at runtime
    bmin, bmax = info["boundsMin"], info["boundsMax"]
    centre = unreal.Vector((bmin["x"] + bmax["x"]) / 2, (bmin["y"] + bmax["y"]) / 2, (bmin["z"] + bmax["z"]) / 2)
    nav = spawn("/Script/NavigationSystem.NavMeshBoundsVolume", centre, 0, "NavBounds", "Navigation")
    if nav:
        nav.set_actor_scale3d(unreal.Vector((bmax["x"] - bmin["x"]) / 200 + 2, (bmax["y"] - bmin["y"]) / 200 + 2, (bmax["z"] - bmin["z"]) / 200 + 2))
    unreal.SystemLibrary.execute_console_command(world, "RebuildNavigation")
    for a in eas.get_all_level_actors():
        if a.get_class().get_name() == "RecastNavMesh":
            a.set_editor_property("runtime_generation", unreal.RuntimeGenerationType.DYNAMIC)

    # the Lyra experience this level plays
    ws = world.get_world_settings()
    exp = unreal.load_class(None, EXPERIENCE_CLASS)
    # EditDefaultsOnly: the details panel edits it on the level's World Settings; scripts go through
    # the DoomLyraTools plugin (Plugins/DoomLyraTools in the Lyra project)
    done = False
    try:
        done = unreal.DoomLyraToolsLibrary.set_property_from_text(ws, "DefaultGameplayExperience", EXPERIENCE_CLASS)
    except Exception as e:
        log("experience: %s (is the DoomLyraTools plugin built?)" % e)
    log("world settings %s experience set %s: %s" % (ws.get_class().get_name(), done, ws.get_editor_property("default_gameplay_experience")))

    unreal.EditorLoadingAndSavingUtils.save_map(world, level_path)
    log("saved %s" % level_path)

    # a tile in Lyra's Play menu
    playlist = "/Game/System/Playlists/DA_Doom%s" % map_name
    if unreal.EditorAssetLibrary.does_asset_exist(playlist):
        unreal.EditorAssetLibrary.delete_asset(playlist)
    da = unreal.EditorAssetLibrary.duplicate_asset(PLAYLIST_TEMPLATE, playlist)
    map_id = da.get_editor_property("map_id")
    map_id.set_editor_property("primary_asset_name", level_path)
    da.set_editor_property("map_id", map_id)
    exp_id = da.get_editor_property("experience_id")
    exp_id.set_editor_property("primary_asset_name", "B_ShooterGame_Elimination")
    da.set_editor_property("experience_id", exp_id)
    da.set_editor_property("tile_title", "Elimination")
    da.set_editor_property("tile_sub_title", "Doom %s" % map_name)
    da.set_editor_property("tile_description", "Doom's %s rebuilt as a Lyra arena: launch pads for lifts, weapon pads for Doom's weapons and medikits" % map_name)
    da.set_editor_property("max_player_count", 8)
    unreal.EditorAssetLibrary.save_loaded_asset(da)
    log("playlist %s -> %s" % (playlist, da.get_editor_property("map_id")))


def info_file():
    for f in os.listdir(EXPORT):
        if re.match(r"^[A-Z0-9]+\.json$", f):
            return f
    raise RuntimeError("no export JSON in " + EXPORT)


try:
    main()
finally:
    if os.environ.get("DOOM_QUIT") == "1":
        unreal.SystemLibrary.quit_editor()
