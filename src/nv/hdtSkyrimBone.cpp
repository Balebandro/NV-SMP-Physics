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
			m_filterReady = false;   // the jitter filter starts again from the reset place
			m_jitterSamples = 0;
			static const btVector3 zero(0, 0, 0);
			m_rig.setWorldTransform(dest);
			m_rig.setInterpolationWorldTransform(dest);
			m_rig.setLinearVelocity(zero);
			m_rig.setAngularVelocity(zero);
			m_rig.setInterpolationLinearVelocity(zero);
			m_rig.setInterpolationAngularVelocity(zero);
			m_rig.updateInertiaTensor();
		} else if (isStaticOrKinematic) {
			// The animation is read at the frame's time, the physics runs behind it by what is left in the bank: the
			// body goes the part of the way that the physics advances (s_kinematicFraction), so it is where the
			// animation had it at the physics' time rather than ahead by a varying slice of a step.
			if (s_kinematicFraction < 1.0f) {
				btTransform to = dest;
				dest.setOrigin(current.getOrigin().lerp(to.getOrigin(), s_kinematicFraction));
				btQuaternion a = current.getRotation(), b = to.getRotation();
				if (a.dot(b) < 0) b = -b;
				dest.setRotation(a.slerp(b, s_kinematicFraction));
			}
			btVector3 linVel, angVel;
			btTransformUtil::calculateVelocity(current, dest, timeStep, linVel, angVel);
			m_rig.setLinearVelocity(linVel);
			m_rig.setAngularVelocity(angVel);
			m_rig.setInterpolationLinearVelocity(linVel);
			m_rig.setInterpolationAngularVelocity(angVel);
		}
	}

	// One step of the zig-zag jitter measure (see m_jitter) on a place relative to the parent. The second difference is
	// taken over the steps' real lengths (dt this step, lastDt the one before), scaled back to what it would be in even
	// steps of their mean: frame times that vary a little must not read as a zig-zag on a smooth path.
	static void MeasureJitter(const btVector3& p, btVector3 hist[2], btVector3& lastD2, float& jitter, int samples, float dt, float lastDt)
	{
		if (samples >= 2) {
			btVector3 d2 = p - hist[0] * 2.0f + hist[1];
			if (dt > 0 && lastDt > 0) {
				float h = 0.5f * (dt + lastDt);
				d2 = ((p - hist[0]) / dt - (hist[0] - hist[1]) / lastDt) * h;
			}
			float back = 0;
			if (samples >= 3) {
				float l = lastD2.length();
				if (l > 1e-6f) back = std::max(0.0f, -d2.dot(lastD2) / l);
			}
			jitter += (back - jitter) * 0.02f;
			lastD2 = d2;
		}
		hist[1] = hist[0];
		hist[0] = p;
	}

	void SkyrimBone::writeTransform()
	{
		auto transform = m_rig.getWorldTransform() * m_rigToLocal;

		m_currentTransform.setBasis(transform.getBasis());
		m_currentTransform.setOrigin(transform.getOrigin());

		btTransform drawn = transform;
		RE::NiAVObject* parent = m_anchor ? (RE::NiAVObject*)m_anchor.get() : (RE::NiAVObject*)m_node->parent;
		if (!m_rig.isStaticOrKinematicObject() && parent && s_stepTime > 0) {
			// The pose relative to the parent at the physics' time (the parent's own physics body when it has one), put
			// on the parent as drawn this frame (written before this bone: bones go root first). The physics runs
			// behind the frame by a varying slice of a step; relative to the parent that slice hardly shows.
			btTransform P(convertNi(parent->world.rotate), convertNi(parent->world.translate));
			btTransform Pphys = P;
			if (auto it = s_boneByNode.find(parent); it != s_boneByNode.end())
				Pphys = it->second->m_rig.getWorldTransform() * it->second->m_rigToLocal;
			btTransform toParent = P.inverse();
			btTransform rel = Pphys.inverse() * transform;
			btVector3 x = rel.getOrigin();
			btQuaternion q = rel.getRotation();
			if (s_filter.enabled) {
				const float dt = s_stepTime;
				auto alpha = [dt](float cutoff) { float tau = 1.0f / (6.2831853f * std::max(cutoff, 0.01f)); return 1.0f / (1.0f + tau / dt); };
				// A jump (a reset, a teleport) is not smoothed: the filter starts again there.
				if (!m_filterReady || (x - m_filterPos).length2() > 20.0f * 20.0f) {
					m_filterPos = x;
					m_filterRot = q;
					m_filterSpeed.setZero();
					m_filterReady = true;
				} else {
					btVector3 speed = (x - m_filterPos) / dt;
					m_filterSpeed += (speed - m_filterSpeed) * alpha(s_filter.dCutoff);
					float a = alpha(s_filter.minCutoff + s_filter.beta * m_filterSpeed.length());
					m_filterPos += (x - m_filterPos) * a;
					if (m_filterRot.dot(q) < 0) q = -q;
					m_filterRot = m_filterRot.slerp(q, a);
					m_filterRot.normalize();
				}
				drawn = P * btTransform(m_filterRot, m_filterPos);
			} else
				drawn = P * rel;
			MeasureJitter(x, m_rawHist, m_rawD2, m_rawJitter, m_jitterSamples, s_stepTime, m_lastStepTime);
			btVector3 drawnRel = (toParent * drawn).getOrigin();
			MeasureJitter(drawnRel, m_jitterHist, m_jitterD2, m_jitter, m_jitterSamples, s_stepTime, m_lastStepTime);
			m_filterLag += ((drawnRel - x).length() - m_filterLag) * 0.02f;
			m_jitterSamples++;
			m_lastStepTime = s_stepTime;
			m_drawnRel = toParent * drawn;
			m_drawnRelValid = true;
		} else {
			m_filterReady = false;
			m_drawnRelValid = false;
		}

		// A blown-up body (a non-finite transform) is never drawn: a skinned shape's bound comes from its bones, so one
		// such bone gives the item, and with it the whole actor, a broken bound, and the game stops drawing the actor
		// (an invisible player). The bone goes back to rest on its anchor (or parent) and its body starts again there.
		const btVector3& o = drawn.getOrigin();
		const btMatrix3x3& m = drawn.getBasis();
		bool finite = std::isfinite(o.x()) && std::isfinite(o.y()) && std::isfinite(o.z());
		for (int r = 0; r < 3 && finite; ++r)
			finite = std::isfinite(m[r].x()) && std::isfinite(m[r].y()) && std::isfinite(m[r].z());
		if (!finite) {
			if (m_anchor) m_node->world = m_anchor->world * m_anchorToNode;
			else if (m_node->parent) m_node->world = m_node->parent->world * m_node->local;
			const btTransform rest = convertNi(m_node->world).asTransform() * m_localToRig;
			static const btVector3 zero(0, 0, 0);
			m_rig.setWorldTransform(rest);
			m_rig.setInterpolationWorldTransform(rest);
			m_rig.setLinearVelocity(zero);
			m_rig.setAngularVelocity(zero);
			m_rig.setInterpolationLinearVelocity(zero);
			m_rig.setInterpolationAngularVelocity(zero);
			m_currentTransform = convertNi(m_node->world);
			m_filterReady = false;
			m_drawnRelValid = false;
			return;
		}

		m_node->world.rotate = convertBt(drawn.getBasis());
		m_node->world.translate = convertBt(drawn.getOrigin());

		// Nodes under a simulated bone that are not simulated themselves (attached props, a chain's
		// loose tip) follow it. Simulated children are written after this one (bones go root first).
		for (uint16_t i = 0; i < m_node->ChildEnd(); ++i) {
			if (auto* child = m_node->children[i])
				updateTransformUpDown(child);
		}
	}

	void SkyrimBone::followTransform()
	{
		// An animated extra bone rides on its anchor, as readTransform puts it.
		if (m_rig.isStaticOrKinematicObject()) {
			if (m_anchor) m_node->world = m_anchor->world * m_anchorToNode;
			return;
		}
		// A simulated bone: its last drawn place relative to its parent, on the parent as it is now.
		RE::NiAVObject* parent = m_anchor ? (RE::NiAVObject*)m_anchor.get() : (RE::NiAVObject*)m_node->parent;
		if (!parent || !m_drawnRelValid) return;
		btTransform drawn = btTransform(convertNi(parent->world.rotate), convertNi(parent->world.translate)) * m_drawnRel;
		m_node->world.rotate = convertBt(drawn.getBasis());
		m_node->world.translate = convertBt(drawn.getOrigin());
		for (uint16_t i = 0; i < m_node->ChildEnd(); ++i) {
			if (auto* child = m_node->children[i])
				updateTransformUpDown(child);
		}
	}
}
