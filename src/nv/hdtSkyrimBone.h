#pragma once
// FSMP's SkyrimBone for New Vegas: a physics bone on one scene graph node. Kept under FSMP's name
// so the physics core and the system builder use it unchanged.
#include "hdtConvertNi.h"
#include "hdtSkinnedMesh/hdtSkinnedMeshBone.h"
#include <unordered_map>

namespace hdt
{
	class SkyrimBone : public SkinnedMeshBone
	{
	public:
		SkyrimBone(const RE::BSFixedString& name, RE::NiNode* node, RE::NiNode* skeleton, btRigidBody::btRigidBodyConstructionInfo& ci);

		void readTransform(float timeStep) override;
		void writeTransform() override;
		void followTransform() override;

		int m_depth;
		RE::NiPointer<RE::NiNode> m_node;
		RE::NiPointer<RE::NiNode> m_skeleton;

		// New Vegas: a bone the garment brings that the skeleton does not have. The game points such a
		// bone's skin slot at the skeleton root, so the system creates the node itself (see
		// SkyrimSystemCreator::createExtraBoneNode) and carries it on `m_anchor` at its bind offset:
		// it follows the anchor while kinematic, and the physics moves it otherwise.
		RE::NiPointer<RE::NiNode> m_anchor;
		RE::NiTransform m_anchorToNode;

		// New Vegas: the jitter filter. A simulated bone's drawn place, relative to the node it hangs from, goes through
		// a One Euro filter (Casiez et al. 2012): an adaptive low-pass whose cutoff rises with the bone's speed, so a
		// bone barely moving (where jitter shows) is smoothed hard and a swinging one passes through with little lag.
		// Only the drawn node is filtered: the physics (and the collisions, which use the rigid body) are not.
		struct FilterSettings
		{
			bool enabled = true;
			float minCutoff = 2.0f;   // Hz, at rest: lower is smoother (and slower to follow a slow drift)
			float beta = 0.03f;       // how fast the cutoff rises with speed (Hz per unit/s)
			float dCutoff = 1.0f;     // Hz, the speed estimate's own smoothing
		};
		static inline FilterSettings s_filter;
		static inline float s_stepTime = 0;   // the step the world just took (seconds), set before writeTransform
		static inline float s_kinematicFraction = 1;   // how far the animated bones go toward this frame's pose (see readTransform), set before readTransform
		static inline std::unordered_map<RE::NiAVObject*, SkyrimBone*> s_boneByNode;   // every simulated system's bones by node, set before writeTransform
		// The plugin's and the previewer's one knob: smoothing 0 (light) .. 1 (strong) sets the cutoff at rest from 8 Hz
		// down to 0.5 Hz and the speed term from 0.12 down to 0.0075 (both evenly on a log scale; 0.5 = 2 Hz and 0.03,
		// which with 180 Hz physics kept every chain of the Sunset poncho under 0.005 jitter at 60, 144 and 180 fps, for
		// about 1 unit of lag while running).
		static void SetJitterFilter(bool enabled, float smoothing, float beta = -1)
		{
			s_filter.enabled = enabled;
			s_filter.minCutoff = 8.0f * std::pow(0.5f / 8.0f, std::clamp(smoothing, 0.0f, 1.0f));
			s_filter.beta = 0.12f * std::pow(0.0625f, std::clamp(smoothing, 0.0f, 1.0f));
			if (beta >= 0) s_filter.beta = beta;
		}

		bool m_filterReady = false;
		btVector3 m_filterPos, m_filterSpeed;
		btQuaternion m_filterRot;
		// Jitter, measured on what is drawn and on the raw physics: per step, how much of the bone's acceleration
		// (relative to the node it hangs from) turns back against the last step's, a zig-zag (a smooth swing turns
		// back rarely), averaged over about a second at 60 Hz. Units per step.
		float m_jitter = 0, m_rawJitter = 0;
		float m_filterLag = 0;   // how far what is drawn trails the physics (units, averaged like the jitter)
		btVector3 m_jitterHist[2], m_rawHist[2], m_jitterD2, m_rawD2;
		int m_jitterSamples = 0;
		float m_lastStepTime = 0;
		btTransform m_drawnRel;          // the last drawn place relative to the parent (followTransform puts it back)
		bool m_drawnRelValid = false;
	};
}
