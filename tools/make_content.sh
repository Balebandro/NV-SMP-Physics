#!/bin/sh
# Rebuilds the Ranger coat content (build/content) from Physically Based Rangers' meshes.
set -e
ROOT=$(cd "$(dirname "$0")/.." && pwd)
SRC="${SRC:-/c/Users/Alejandro/AppData/Local/ModOrganizer/New Vegas/mods/Physically Based Rangers/Meshes/armor/combatranger}"
mkdir -p "$ROOT/build/content"
cd "$ROOT/build/content"
# Forward slashes in the XML path: in double quotes "\$" would escape the variable. The plugin reads either.
"$ROOT/build/smpify.exe" "$SRC/CombatRanger.nif" "CombatRanger.nif" "Upperbody:Coat" "Meshes/armor/combatranger/CombatRanger_smp.xml" "CombatRanger_smp.xml" --collide Legs | tail -1
"$ROOT/build/smpify.exe" "$SRC/combatranger_f.nif" "combatranger_f.nif" "Upperbody:Coat" "Meshes/armor/combatranger/combatranger_f_smp.xml" "combatranger_f_smp.xml" --collide Legs | tail -1
