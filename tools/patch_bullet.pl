#!/usr/bin/perl
# Applies FSMP's Bullet 3.25 modifications (hdtSMP64 cmake/ports/bullet3/portfile.cmake) to the
# vendored copy in extern/bullet: the non-Hookean spring terms of btGeneric6DofSpring2Constraint,
# which SMP configs use, and the runtime profiler gate. Run from the repo root; a file already
# patched is skipped, and any replacement that does not apply stops the script. Bullet's 3.25 tag
# has no blank line before "btScalar cfm" in the spring constraint, which FSMP's vcpkg copy has.
use strict; use warnings;

sub patch {
	my ($file, @pairs) = @_;
	local $/; open(my $in, '<', $file) or die "$file: $!"; my $text = <$in>; close $in;
	my $marker = ref($pairs[0]) eq 'Regexp' ? shift @pairs : qr/m_nonHookeanDamping|gBtProfileEnabled/;
	if ($text =~ $marker) { print "already patched $file\n"; return; }
	while (@pairs) {
		my ($from, $to) = (shift @pairs, shift @pairs);
		my $count = () = $text =~ /\Q$from\E/g;
		die "$file: expected 1 match, found $count for:\n$from\n" if $count < 1;
		$text =~ s/\Q$from\E/$to/g;
	}
	open(my $out, '>', $file) or die "$file: $!"; binmode $out; print $out $text; close $out;
	print "patched $file\n";
}

my $h = 'extern/bullet/BulletDynamics/ConstraintSolver/btGeneric6DofSpring2Constraint.h';
patch($h,
	"btScalar m_currentPosition;\n\tint m_currentLimit;",
	"btScalar m_currentPosition;\n\tint m_currentLimit;\n\n\t// BM: Definitely a better way to do this, but yay dirty hacks for Skyrims.\n\tbtScalar m_nonHookeanDamping;\n\tbtScalar m_nonHookeanStiffness;",
	"m_currentPosition = 0;\n\t\tm_currentLimit = 0;",
	"m_currentPosition = 0;\n\t\tm_currentLimit = 0;\n\n\t\tm_nonHookeanDamping = 0.f;\n\t\tm_nonHookeanStiffness = 0.f;",
	"m_currentPosition = limot.m_currentPosition;\n\t\tm_currentLimit = limot.m_currentLimit;",
	"m_currentPosition = limot.m_currentPosition;\n\t\tm_currentLimit = limot.m_currentLimit;\n\n\t\tm_nonHookeanDamping = limot.m_nonHookeanDamping;\n\t\tm_nonHookeanStiffness = limot.m_nonHookeanStiffness;",
	"btVector3 m_currentLinearDiff;\n\tint m_currentLimit[3];",
	"btVector3 m_currentLinearDiff;\n\tint m_currentLimit[3];\n\n\t// BM: Definitely a better way to do this, but yay dirty hacks for Skyrims.\n\tbtVector3 m_nonHookeanDamping;\n\tbtVector3 m_nonHookeanStiffness;",
	"m_currentLimit[i] = 0;",
	"m_currentLimit[i] = 0;\n\n\t\t\tm_nonHookeanDamping[i] = btScalar(0.f);\n\t\t\tm_nonHookeanStiffness[i] = btScalar(0.f);",
	"m_currentLimit[i] = other.m_currentLimit[i];",
	"m_currentLimit[i] = other.m_currentLimit[i];\n\n\t\t\tm_nonHookeanDamping[i] = other.m_nonHookeanDamping[i];\n\t\t\tm_nonHookeanStiffness[i] = other.m_nonHookeanStiffness[i];",
	"void setStiffness(int index, btScalar stiffness, bool limitIfNeeded = true);  // if limitIfNeeded is true the system will automatically limit the stiffness in necessary situations where otherwise the spring would move unrealistically too widely\n\tvoid setDamping",
	"void setStiffness(int index, btScalar stiffness, bool limitIfNeeded = true);  // if limitIfNeeded is true the system will automatically limit the stiffness in necessary situations where otherwise the spring would move unrealistically too widely\n\tvoid setNonHookeanDamping(int index, btScalar factor);                        // Increases spring damping by factor (using a power of 2 curve), the closer the current position is to equilibrium.\n\tvoid setNonHookeanStiffness(int index, btScalar factor);                      // Increases spring stiffness the further position is from equilibrium, by this factor (power curve).\n\tvoid setDamping",
);

my $c = 'extern/bullet/BulletDynamics/ConstraintSolver/btGeneric6DofSpring2Constraint.cpp';
patch($c,
	"limot.m_motorERP = (flags & BT_6DOF_FLAGS_ERP_MOTO2) ? m_linearLimits.m_motorERP[i] : info->erp;\n\n\t\t\t//rotAllowed",
	"limot.m_motorERP = (flags & BT_6DOF_FLAGS_ERP_MOTO2) ? m_linearLimits.m_motorERP[i] : info->erp;\n\n\t\t\tlimot.m_nonHookeanDamping = m_linearLimits.m_nonHookeanDamping[i];\n\t\t\tlimot.m_nonHookeanStiffness = m_linearLimits.m_nonHookeanStiffness[i];\n\n\t\t\t//rotAllowed",
	"vel = (linVelA + tanVelA).dot(ax1) - (linVelB + tanVelB).dot(ax1);
		}
		btScalar cfm = BT_ZERO;",
	"vel = (linVelA + tanVelA).dot(ax1) - (linVelB + tanVelB).dot(ax1);
		}

		btScalar dampingFactor = limot->m_nonHookeanDamping;\n\t\tbtScalar stiffnessFactor = limot->m_nonHookeanStiffness;\n\t\tif (!btFuzzyZero(error) && (!btFuzzyZero(dampingFactor) || !btFuzzyZero(stiffnessFactor)))\n\t\t{\n\t\t\tbtScalar range = pos < ep ? ep - limot->m_loLimit : limot->m_hiLimit - ep;\n\t\t\tbtScalar rf = !btFuzzyZero(range) ? btFabs(error) / range : BT_ZERO;\n\t\t\t// Avoid blowing shit up.\n\t\t\tbtScalar t = btClamped(rf, BT_ZERO, BT_ONE);\n\n\t\t\tkd *= BT_ONE - (dampingFactor * t);\n\t\t\tks *= BT_ONE - (stiffnessFactor * t);\n\t\t}\n\n\t\tbtScalar cfm = BT_ZERO;",
	"}\n}\n\nvoid btGeneric6DofSpring2Constraint::setDamping(int index, btScalar damping, bool limitIfNeeded)",
	"}\n}\n\n// Elastic, uneven damping used to fake non-Hookean springs for a specific axis.\n// Default: 0 (disabled).  Other values below 1 will likely blow your spring up.. individual speed and hilarity may vary.\nvoid btGeneric6DofSpring2Constraint::setNonHookeanDamping(int index, btScalar damping)\n{\n\tbtAssert((index >= 0) && (index < 6));\n\tif (index < 3)\n\t{\n\t\tm_linearLimits.m_nonHookeanDamping[index] = damping;\n\t}\n\telse\n\t{\n\t\tm_angularLimits[index - 3].m_nonHookeanDamping = damping;\n\t}\n}\n\nvoid btGeneric6DofSpring2Constraint::setNonHookeanStiffness(int index, btScalar damping)\n{\n\tbtAssert((index >= 0) && (index < 6));\n\tif (index < 3)\n\t{\n\t\tm_linearLimits.m_nonHookeanStiffness[index] = damping;\n\t}\n\telse\n\t{\n\t\tm_angularLimits[index - 3].m_nonHookeanStiffness = damping;\n\t}\n}\n\nvoid btGeneric6DofSpring2Constraint::setDamping(int index, btScalar damping, bool limitIfNeeded)",
);

# The non-Hookean block above uses pos and ep, which FSMP's Bullet names in the spring section and
# the 3.25 tag does not.
patch($c, qr/btScalar ep = limot->m_equilibriumPoint;/,
	"btScalar error = limot->m_currentPosition - limot->m_equilibriumPoint;\n\t\tcalculateJacobi",
	"btScalar pos = limot->m_currentPosition;\n\t\tbtScalar ep = limot->m_equilibriumPoint;\n\t\tbtScalar error = pos - ep;\n\t\tcalculateJacobi",
);

patch('extern/bullet/LinearMath/btQuickprof.h',
	"void btSetCustomLeaveProfileZoneFunc(btLeaveProfileZoneFunc* leaveFunc);\n\n#ifndef BT_ENABLE_PROFILE",
	"void btSetCustomLeaveProfileZoneFunc(btLeaveProfileZoneFunc* leaveFunc);\n\nvoid btEnterProfileZone(const char* name);\nvoid btLeaveProfileZone();\n\nextern bool gBtProfileEnabled;\nvoid btSetProfileEnabled(bool enabled);\nbool btGetProfileEnabled();\nSIMD_FORCE_INLINE bool btIsProfileEnabled()\n{\n\treturn gBtProfileEnabled;\n}\n\n#ifndef BT_ENABLE_PROFILE",
	"class CProfileSample\n{\npublic:\n\tCProfileSample(const char* name);\n\n\t~CProfileSample(void);\n};",
	"class CProfileSample\n{\npublic:\n\tCProfileSample(const char* name) :\n\t\tm_enabled(btIsProfileEnabled())\n\t{\n\t\tif (m_enabled)\n\t\t{\n\t\t\tbtEnterProfileZone(name);\n\t\t}\n\t}\n\n\t~CProfileSample(void)\n\t{\n\t\tif (m_enabled)\n\t\t{\n\t\t\tbtLeaveProfileZone();\n\t\t}\n\t}\n\nprivate:\n\tbool m_enabled;\n};",
);

patch('extern/bullet/LinearMath/btQuickprof.cpp',
	"static btEnterProfileZoneFunc* bts_enterFunc = btEnterProfileZoneDefault;\nstatic btLeaveProfileZoneFunc* bts_leaveFunc = btLeaveProfileZoneDefault;",
	"bool gBtProfileEnabled = false;\n\nstatic btEnterProfileZoneFunc* bts_enterFunc = btEnterProfileZoneDefault;\nstatic btLeaveProfileZoneFunc* bts_leaveFunc = btLeaveProfileZoneDefault;",
	"void btSetCustomLeaveProfileZoneFunc(btLeaveProfileZoneFunc* leaveFunc)\n{\n\tbts_leaveFunc = leaveFunc;\n}\n\nCProfileSample::CProfileSample(const char* name)\n{\n\tbtEnterProfileZone(name);\n}\n\nCProfileSample::~CProfileSample(void)\n{\n\tbtLeaveProfileZone();\n}\n",
	"void btSetCustomLeaveProfileZoneFunc(btLeaveProfileZoneFunc* leaveFunc)\n{\n\tbts_leaveFunc = leaveFunc;\n}\n\nvoid btSetProfileEnabled(bool enabled)\n{\n\tgBtProfileEnabled = enabled;\n}\n\nbool btGetProfileEnabled()\n{\n\treturn gBtProfileEnabled;\n}\n",
);
