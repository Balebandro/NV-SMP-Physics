#pragma once
// FSMP's SkyrimPhysicsWorld for New Vegas: the one Bullet world every simulated actor's systems
// live in. FSMP's solver set-up and time stepping, driven once a frame from the plugin's hook
// (main.cpp) instead of Skyrim's frame events, and run on the game thread.
#include "hdtSkinnedMesh/hdtSkinnedMeshWorld.h"
#include "hdtSkyrimSystem.h"

namespace hdt
{
	constexpr float RESET_PHYSICS = -10.0f;

	class SkyrimPhysicsWorld : protected SkinnedMeshWorld
	{
	public:
		static SkyrimPhysicsWorld* get();

		// One game frame: reads the animated pose, steps the simulation, writes the bones back.
		void doUpdate(float interval);

		void addSkinnedMeshSystem(SkinnedMeshSystem* system) override;
		void removeSkinnedMeshSystem(SkinnedMeshSystem* system) override;
		void removeSystemByNode(void* root);
		void removeAllSystems();
		void resetSystems();
		size_t systemCount() const { return m_systems.size(); }
		int bodyCount() const { return getNumCollisionObjects(); }
		using SkinnedMeshWorld::updateConstraintsForBone;
		using SkinnedMeshWorld::m_collisionsEnabled;

		std::unique_lock<std::mutex> lockSimulation() { return std::unique_lock(m_lock); }

		btVector3 applyTranslationOffset();
		void restoreTranslationOffset(const btVector3&);

		btContactSolverInfo& getSolverInfo() { return btDiscreteDynamicsWorld::getSolverInfo(); }
		void setGravity(const btVector3& g) { btDiscreteDynamicsWorld::setGravity(g); }

		// Wind in game units per second: smoothed toward the target over a_smoothingSamples frames.
		void setWind(const btVector3& target, uint32_t a_smoothingSamples = 8);

		// FSMP's settings, defaults from its configs.xml; the plugin's INI and menu set them.
		float m_timeTick = 1 / 60.f;
		int m_maxSubSteps = 4;
		bool m_clampRotations = true;
		float m_rotationSpeedLimit = 10.f;   // radians per second the root may turn before the cloth is held back
		bool m_unclampedResets = true;
		float m_unclampedResetAngle = 120.0f;
		bool m_enableWind = true;
		float m_windStrength = 2.0f;
		bool disabled = false;
		uint8_t m_resetPc = 0;

		float m_averageStepMs = 0.0f;

	private:
		SkyrimPhysicsWorld();
		~SkyrimPhysicsWorld() noexcept;

		std::mutex m_lock;
		float m_accumulatedInterval = 0;
	};
}
