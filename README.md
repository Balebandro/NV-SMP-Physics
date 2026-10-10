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
| `tools/systemtest.cpp` | Runs the plugin's builder and world on a NIF outside the game, laid out as the game has it in memory, and walks it |

### Changes to FSMP's core

- `hdtSkinnedMeshWorld.cpp`: falls back to Bullet's sequential task scheduler (this build has no TBB).
- `hdtSkinnedMeshAlgorithm.h`: the collision merge buffer is allocated 16-byte aligned. `malloc` is only
  8-byte aligned on 32-bit Windows, and the SSE adds into it faulted on the first contact.

## Building

Visual Studio 2022 C++ build tools (x86).

```
build.bat                  the plugin: build\NVSMPPhysics.dll
tools\build_tools.bat      smpify.exe and systemtest.exe
tools\make_content.sh      the Ranger coat (build\content)
```

## Installing

Copy `NVSMPPhysics.dll` and `NVSMPPhysics.ini` to `Data\nvse\Plugins`, with the items' meshes and XML.
Needs xNVSE. The menu is on F7. The log is `NVSMPPhysics.log` in the game folder.

## Making an item

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
