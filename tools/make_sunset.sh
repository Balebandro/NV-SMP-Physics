#!/bin/sh
# Rebuilds the SMP version of the user's MetalArmorSunset.nif (cape TornCho + rope chains with rigid bottles). The NIF names
# its physics file as installed in the game: Data\Meshes\Armor\Uniques\SunsetArmor\ (Physically Based Apparel in MO2).
set -e
ROOT=$(cd "$(dirname "$0")/.." && pwd)
SRC="${SRC:-/c/Users/Alejandro/Downloads/screws_obj/MetalArmorSunset.nif}"
OUT="${OUT:-/c/Users/Alejandro/Downloads/screws_obj/smp/Meshes/armor/metalarmorsunset}"
mkdir -p "$OUT"
cd "$OUT"
"$ROOT/build/smpdress.exe" "$SRC" MetalArmorSunset.nif 'Meshes\Armor\Uniques\SunsetArmor\MetalArmorSunset_smp.xml' MetalArmorSunset_smp.xml \
	--cloth "Upperbody:TornCho" --anchor "Bip01 Spine2" --radial --collar 11 --angles -135 210 --grid 11 7 --fixed-rows 2 \
		--detach "Bip01 R UpperArm=Bip01 R Clavicle" --detach "Bip01 RUpArmTwistBone=Bip01 R Clavicle" \
		--detach "Bip01 R Forearm=Bip01 R Clavicle" --detach "Bip01 LUpArmTwistBone=Bip01 L Clavicle" \
		--keep "Bip01 L Clavicle" 0.05 0.2 \
	--chains "Upperbody:Rope" --anchor "Bip01 Pelvis" --links 4 --keep-top 1 1.75 \
	--collide "Upperbody:MetalArmorSunset" --collide "Upperbody:ReinforcedPants" --collide "PipboyOff:MetalArmorSunset" --collide "PipboyOn:MetalArmorSunset" \
	--collide-skip "Bip01 L Calf" --collide-skip "Bip01 R Calf" --collide-skip "Bip01 L Foot" --collide-skip "Bip01 R Foot" \
	--collide-skip "Bip01 L Toe0" --collide-skip "Bip01 R Toe0" --collide-skip "Bip01 L ForeTwist" --collide-skip "Bip01 R ForeTwist" \
	--collide-skip "Bip01 L Forearm" --collide-skip "Bip01 R Forearm" --collide-skip "Bip01 L Hand" --collide-skip "Bip01 R Hand" \
	--collide-skip "Bip01 L Thumb1" --collide-skip "Bip01 R Thumb1" \
	--cloth-margin 1 --body-margin 0.3 --penetration 0.3 --bend 3 --stiffness 0.1 --no-weft --cloth-friction 0.3 --cloth-damping 0.95 0.97 --cloth-mass 0.0552 --cloth-restitution 0.62 \
	--proxy 3
