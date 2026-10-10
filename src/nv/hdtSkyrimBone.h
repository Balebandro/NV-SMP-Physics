#pragma once
// FSMP's SkyrimBone for New Vegas: a physics bone on one scene graph node. Kept under FSMP's name
// so the physics core and the system builder use it unchanged.
#include "hdtConvertNi.h"
#include "hdtSkinnedMesh/hdtSkinnedMeshBone.h"

namespace hdt
{
	class SkyrimBone : public SkinnedMeshBone
	{
	public:
		SkyrimBone(const RE::BSFixedString& name, RE::NiNode* node, RE::NiNode* skeleton, btRigidBody::btRigidBodyConstructionInfo& ci);

		void readTransform(float timeStep) override;
		void writeTransform() override;

		int m_depth;
		RE::NiPointer<RE::NiNode> m_node;
		RE::NiPointer<RE::NiNode> m_skeleton;

		// New Vegas: a bone the garment brings that the skeleton does not have. The game points such a
		// bone's skin slot at the skeleton root, so the system creates the node itself (see
		// SkyrimSystemCreator::createExtraBoneNode) and carries it on `m_anchor` at its bind offset:
		// it follows the anchor while kinematic, and the physics moves it otherwise.
		RE::NiPointer<RE::NiNode> m_anchor;
		RE::NiTransform m_anchorToNode;
	};
}
