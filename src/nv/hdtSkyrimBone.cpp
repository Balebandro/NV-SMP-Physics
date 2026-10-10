// Ported from FSMP (hdtSMP64, src/hdtSkyrimBone.cpp), GPL-3.0 with FSMP's modding exception: the
// same reading and writing of a bone's world transform, with Skyrim's forced weapon-node updates
// left out and the New Vegas extra-bone anchoring added.
#include "hdtSkyrimBone.h"
#include "hdtSkyrimPhysicsWorld.h"

namespace hdt
{
	SkyrimBone::SkyrimBone(const RE::BSFixedString& name, RE::NiNode* node, RE::NiNode* skeleton, btRigidBody::btRigidBodyConstructionInfo& ci) :
		SkinnedMeshBone(name, ci), m_node(node), m_skeleton(skeleton)
	{
		if (ci.m_mass)
			m_rig.setCollisionFlags(0);
		else
			m_rig.setCollisionFlags(btCollisionObject::CF_KINEMATIC_OBJECT);

		m_depth = 0;
		for (RE::NiAVObject* i = node; i; i = i->parent)
			++m_depth;
	}

	void SkyrimBone::readTransform(float timeStep)
	{
		// An extra bone rides on its anchor: where it would be if it were the anchor's child.
		if (m_anchor && (timeStep <= RESET_PHYSICS || m_rig.isStaticOrKinematicObject()))
			m_node->world = m_anchor->world * m_anchorToNode;

		auto oldScale = m_currentTransform.getScale();

		m_currentTransform = convertNi(m_node->world);

		auto newScale = m_currentTransform.getScale();
		auto current = m_rig.getWorldTransform();
		auto isStaticOrKinematic = m_rig.isStaticOrKinematicObject();
		auto scaleChanged = !btFuzzyZero(newScale - oldScale);

		if (scaleChanged) {
			auto factor = oldScale / newScale;
			if (!isStaticOrKinematic) {
				auto factor2 = factor * factor;
				auto factor3 = factor2 * factor;
				auto factor5 = factor3 * factor2;
				auto inertia = m_rig.getInvInertiaDiagLocal();
				m_rig.setMassProps(1.0f / (m_rig.getInvMass() * factor3), btVector3(1, 1, 1));
				m_rig.setInvInertiaDiagLocal(inertia * factor5);
				m_rig.updateInertiaTensor();
			}
			auto invFactor = 1.0f / factor;
			m_localToRig.getOrigin() *= invFactor;
			m_rigToLocal.getOrigin() *= invFactor;
			m_rig.getCollisionShape()->setLocalScaling(setAll(newScale));
		}

		auto dest = m_currentTransform.asTransform() * m_localToRig;
		if (timeStep <= RESET_PHYSICS) {
			static const btVector3 zero(0, 0, 0);
			m_rig.setWorldTransform(dest);
			m_rig.setInterpolationWorldTransform(dest);
			m_rig.setLinearVelocity(zero);
			m_rig.setAngularVelocity(zero);
			m_rig.setInterpolationLinearVelocity(zero);
			m_rig.setInterpolationAngularVelocity(zero);
			m_rig.updateInertiaTensor();
		} else if (isStaticOrKinematic) {
			btVector3 linVel, angVel;
			btTransformUtil::calculateVelocity(current, dest, timeStep, linVel, angVel);
			m_rig.setLinearVelocity(linVel);
			m_rig.setAngularVelocity(angVel);
			m_rig.setInterpolationLinearVelocity(linVel);
			m_rig.setInterpolationAngularVelocity(angVel);
		}
	}

	void SkyrimBone::writeTransform()
	{
		auto transform = m_rig.getWorldTransform() * m_rigToLocal;

		m_currentTransform.setBasis(transform.getBasis());
		m_currentTransform.setOrigin(transform.getOrigin());

		m_node->world.rotate = convertBt(transform.getBasis());
		m_node->world.translate = convertBt(transform.getOrigin());

		// Nodes under a simulated bone that are not simulated themselves (attached props, a chain's
		// loose tip) follow it. Simulated children are written after this one (bones go root first).
		for (uint16_t i = 0; i < m_node->ChildEnd(); ++i) {
			if (auto* child = m_node->children[i])
				updateTransformUpDown(child);
		}
	}
}
