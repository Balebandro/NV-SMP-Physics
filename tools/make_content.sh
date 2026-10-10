#!/bin/sh
# Rebuilds the Ranger coat content (build/content) from Physically Based Rangers' meshes.
set -e
ROOT=$(cd "$(dirname "$0")/.." && pwd)
SRC="${SRC:-/c/Users/Alejandro/AppData/Local/ModOrganizer/New Vegas/mods/Physically Based Rangers/Meshes/armor/combatranger}"
mkdir -p "$ROOT/build/content"
cd "$ROOT/build/content"
for pair in "CombatRanger.nif:CombatRanger_smp.xml" "combatranger_f.nif:combatranger_f_smp.xml"; do
	nif=${pair%%:*}; xml=${pair#*:}
	"$ROOT/build/smpify.exe" "$SRC/$nif" "$nif" "Upperbody:Coat" "Meshes\armor\combatranger\$xml" "$xml" --collide Legs --collide Holster | tail -1
done
