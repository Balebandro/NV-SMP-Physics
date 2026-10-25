# NV SMP Physics

HDT-SMP physics for Fallout: New Vegas: [FSMP](https://github.com/DaymareOn/hdtSMP64)'s (Faster HDT-SMP)
physics core and Bullet, built as a 32-bit xNVSE plugin, reading the same SMP physics XML Skyrim mods use.
Cloth, capes, coats, hair and anything else rigged with physics bones move with the character, collide
with the body under them, and blow in the weather's wind.

## How it works

Once a frame, right after New Vegas has animated every actor and before it draws (TESMain's call to
`DrawWorld_WaitForParallelUpdate`), the plugin:

1. finds items with SMP physics on the player and the nearest high-process actors: the
   `HDT Skinned Mesh Physics Object` string extra data naming their physics XML, as in Skyrim;
2. builds a physics system for each new one from its XML (FSMP's reader, templates and constraints,
   unchanged) and drops the systems of unequipped items and actors that left range;
3. reads the animated pose, steps Bullet (FSMP's solver set-up and time stepping) and writes the simulated
   bones back, so the bones it writes are the ones rendered this frame.

### What New Vegas needs that Skyrim does not

- **The item's own bones.** Skyrim merges an item's physics bones into the actor's skeleton when it is
  equipped. New Vegas points the skin slots of any bone the skeleton lacks at the skeleton root, and the
  item's own bone nodes are dropped. So an item lists its skin slots in a `SMP Bones` string extra data on
  each skinned shape (`name|parent;name|parent;...`, in skin-instance order), and the plugin creates
  those bones, places them at their bind pose against the body and carries them on their parent.
  `smpify` writes this list.
- **Gamebryo 2.x skinning.** Vertex positions come from `NiGeometryData`, weights and triangles from the
  `NiSkinPartition` the renderer uses (partition-local vertex indices through a vertex map, lists or
  strips).

### Additions to SMP

- **Ground.** Every system with simulated cloth gets a ground plane at the actor's feet (a 300-unit square
  on an animation-driven bone carried on the actor's root, colliding as a per-triangle body tagged `ground`
  with that actor's own cloth), so cloth rests on the floor when crouching or sitting. An item opts out with
  `<no-collide-with-tag>ground</no-collide-with-tag>`; `bGround` in the INI (and the menu) turns it off.
  Flat at the feet: on slopes and stairs it is approximate.
- **Names match case-insensitively**, as Skyrim's `BSFixedString` does, so ported XMLs keep working.
- **Virtual legs.** `<nv-virtual-legs fit="Legs" scale="0.9">` (with a per-triangle shape's margin,
  penetration and tags) collides against smooth tapered capsules on the thighs and shins instead of a leg
  mesh, each end's radius fitted to the named shape(s). Without the trousers' creases and tight hips, cloth
  can collide up to the waist. `smpify --virtual-legs Legs` writes it (and lets the coat's top rows collide
  partly, threshold 0.5); on the game's run that ended the coat clipping into the thighs at the hips.
- **Impact sounds.** A bone's `<impact-sound>` (a file under Data, repeatable: one is picked at random each time)
  plays where its shape strikes another simulated bone's, as in game: two bottles hanging side by side clink.
  `<impact-speed>` (default 20 units/s) is how fast they must come together, below that they touch in silence;
  `<impact-sound-hard>` files play instead from `<impact-hard-speed>` (default 150); the volume rises with the
  speed, scaled by `<impact-volume>` (0 to 1, default 1). A pair sounds when it meets, not while it stays
  touching, and not again for 0.12 s. Contacts with the body's animated bones make no sound. In game only within
  1500 units of the player, at most 4 a frame. The previewer plays them too and logs every touch with its speed,
  so the thresholds can be set by watching it; its Bone values > Impact sounds picks each bone's files from the
  game's own sounds (and plays them) and sets the speeds and volume.
  For example, the game's own bottle sounds:
  `<impact-sound>Sound\fx\phy\glass\bottle_l\phy_bottle_l_01.wav</impact-sound>` (and `_02` to `_05`),
  `<impact-sound-hard>Sound\fx\phy\glass\bottle_h\phy_bottle_h_01.wav</impact-sound-hard>` (and `_02` to `_04`).

## Layout

| Path | What |
|---|---|
| `src/fsmp/hdtSkinnedMesh` | FSMP's physics core, unchanged except two 32-bit fixes (below) |
| `src/fsmp/XmlReader.*`, `XmlInspector` | FSMP's XML reader |
| `src/nv` | New Vegas glue: FSMP's system builder, bone, body and world ported to the game's scene graph (keeping FSMP's class names so the core links against it unchanged), the actor manager, the frame hook, the in-game menu |
| `src/compat` | Stand-ins for the CommonLibSSE types (`BSFixedString`, `BSTSmartPointer`) and oneTBB calls the core uses |
| `extern/bullet` | Bullet 3.25 with FSMP's patches (`tools/patch_bullet.pl`: non-Hookean springs, profiler gate) |
| `extern/imgui` | Dear ImGui, for the menu |
| `tools/smpify.cpp` | Makes a skinned coat or skirt an SMP item: cloth bone grid, reweighting, partitions, `SMP Bones`, the XML |
| `tools/smpdress.cpp` | `smpify` for whole outfits: several cloth pieces (grid or radial), bone chains with rigid pieces, low-poly collision proxies, one XML |
| `tools/smprepair.cpp` | Puts back the SMP bones Outfit Studio drops on export (unweighted bones and their nodes) and rewrites `SMP Bones` |
| `tools/smpprepare.cpp` | NV SMP Prepare: pick SMP NIFs made elsewhere (a Skyrim body or outfit); it adds the SMP Bones lists, the physics path and the vertex keep flags they need (`tools/smplist.h`, shared with `smprepair --list`) |
| `tools/nifdump.cpp` | Lists a NIF's shapes, their bones and connected pieces |
| `tools/systemtest.cpp` | Runs the plugin's builder and world on a NIF outside the game, laid out as the game has it in memory, and walks it |
| `tools/scene.cpp` | That in-memory NIF, shared by the test and the previewer |
| `tools/previewer` | NV SMP Previewer: the item on a moving body in a window, with the plugin's own physics; `maker.h` its Make SMP window (the NV SMP Maker) |
| `tools/nifcheck.cpp` | What a tool changed in a NIF, block by block, and whether every skin partition is consistent |
| `tools/kf.cpp` | Reads and samples `.kf` animations (keyframed and B-spline) |
| `tools/rigpreviewer` | NV Rig Previewer: weights and rigging checks on posed bodies |

### Changes to FSMP's core

- `hdtSkinnedMeshWorld.cpp`: falls back to Bullet's sequential task scheduler (this build has no TBB).
- `hdtSkinnedMeshAlgorithm.h`: the collision merge buffer is allocated 16-byte aligned. `malloc` is only
  8-byte aligned on 32-bit Windows, and the SSE adds into it faulted on the first contact.

## Building

Visual Studio 2022 C++ build tools (x86).

```
build.bat                  the plugin: build\NVSMPPhysics.dll
tools\build_tools.bat         smpify, smpdress, smprepair, nifcheck, nifdump, NVSMPPrepare and systemtest
tools\build_previewer.bat     build\NVSMPPreviewer.exe and build\NVSMPMaker.exe
tools\build_rigpreviewer.bat  build\NVRigPreviewer.exe
```

## Installing

Beta builds are on the [Releases](https://github.com/Balebandro/NV-SMP-Physics/releases) page.

- **The plugin** (`NVSMPPhysics-<version>.zip`): install it with your mod manager, or copy its `nvse` folder into
  the game's `Data` folder (`Data\nvse\Plugins\NVSMPPhysics.dll` and `.ini`). Needs xNVSE and the game's 1.4.0.525
  runtime. The plugin does nothing by itself: it simulates items that come with SMP physics (a NIF naming an SMP
  physics XML, with its `SMP Bones` list). The menu is on F7. The log is `NVSMPPhysics.log` in the game folder.
- **The previewer** (`NVSMPPreviewer-<version>.zip`): unzip anywhere and run `NVSMPPreviewer.exe` (Windows 10 or
  later, any Direct3D 9 GPU). It keeps its settings in `NVSMPPreviewer.ini` beside it.

## Previewing an item

`NVSMPPreviewer.exe [item.nif]` (or drop a NIF on its window) shows an SMP item on a body that walks, runs,
turns, jumps or sways, or that you drive with WASD, simulated by the plugin's own system builder and physics
world: what it shows is what the game does. It finds the physics XML, the skeleton, the animations and the
textures as the game would: loose files in the NIF's own Data folder, Mod Organizer 2's enabled mods (in
priority order), extra folders and the game's Data folder, then the BSA archives (the mods', then the game's;
`tools/previewer/bsa.cpp`, `inflate.cpp`). The MO2 instance is the one the NIF is inside (portable or global,
any name, MO2's own path settings honoured), else MO2's New Vegas instance; the game folder comes from MO2,
else the GOG, Bethesda or Steam registry entries. All of it can be set by hand under Data sources. With the game's `skeleton.nif` found, the body is that skeleton and
the presets play the game's locomotion animations (`tools/kf.cpp`: keyframed and compressed B-spline
`.kf` files), in the stance and gender chosen, as the game would pick them (kNVSE's gender and
AnimGroupOverride folders first), crossfaded, the body carried by each clip's root motion; "Play animation
file..." plays any `.kf`. Save the XML in any editor and it rebuilds. Overlays show the bones
(orange simulated, grey animated), the constraints and the collision shapes; the physics settings are the
in-game menu's. Ctrl + drag moves the body by hand; Screenshot saves the view to `screenshots\`.

## Making an item

### NV SMP Maker

`NVSMPMaker.exe` (the previewer built with `NVSMP_MAKER`; the previewer's **Make SMP...** button opens the same
window) makes a plain armour an SMP item without the command line. Open the armour; the body holds still.

- **Paint chains**: paint on the mesh where something should hang and swing, starting where it is tied on and
  dragging to its end. Each stroke becomes a chain of bones laid along it (`smpdress --painted`): the bone it hangs
  from is picked from what the start of the stroke was weighted to, the links from its length, and its rigging
  (swing limits, springs, mass, damping, wind) from two sliders, Stiffness and Weight. The brush's soft edge blends
  into the mesh's own weights. Tick what the chains collide with, then **Build and preview**.
- **Outfit**: cloth grids, rope chains and collision options, `smpdress` with every option as a form.
- **Coat or skirt**: `smpify`. **Fix after Outfit Studio**: `smprepair`.

The output is a folder laid out as `Data` (point it at an empty MO2 mod to install it directly); files it replaces
are kept as `.bak`. Each build saves its settings beside the XML (`*_smp.xml.smprecipe`, with the painted chains
in `*.smppaint`); **Load recipe...** brings them back to rebuild or change. **Copy command** gives the command line.
The Maker runs `smpdress.exe`, `smpify.exe` and `smprepair.exe` from its own folder.

### The command-line tools

```
smpify in.nif out.nif "Coat Shape" "Meshes\path\coat_smp.xml" coat_smp.xml --collide "Legs"
```

lays a cloth bone grid over the coat's skirt (waist anchors on the pelvis, simulated rows below), reweights
it, and writes the XML with FSMP-style warp and weft constraints and collision against the shapes given.
Any SMP XML works too, as long as the item's own bones are listed in `SMP Bones`.

## Licence

GPL-3.0 with FSMP's modding exception (`LICENSE`, `EXCEPTIONS`), as FSMP. Bullet is zlib, Dear ImGui and
XmlInspector are MIT.

Credits: FSMP by DaymareOn and contributors, from HydrogensaysHDT's HDT-SMP; Bullet by Erwin Coumans.
