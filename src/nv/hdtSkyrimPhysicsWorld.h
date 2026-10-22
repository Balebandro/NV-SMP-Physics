#pragma once
// FSMP's SkyrimPhysicsWorld for New Vegas: the one Bullet world every simulated actor's systems
// live in. FSMP's solver set-up and time stepping, driven once a frame from the plugin's hook
// (main.cpp) instead of Skyrim's frame events. The pose is read and the bones written on the game
// thread; the step itself can run on a physics thread of its own (beginUpdate / finishUpdate).
#include "hdtSkinnedMesh/hdtSkinnedMeshWorld.h"
#include "hdtSkyrimSystem.h"
#include <condition_variable>
#include <string>

namespace hdt
{
	constexpr float RESET_PHYSICS = -10.0f;

	class SkyrimPhysicsWorld : protected SkinnedMeshWorld
	{
	public:
		static SkyrimPhysicsWorld* get();

		// One game frame: reads the animated pose, steps the simulation, writes the bones back.
		void doUpdate(float interval) { beginUpdate(interval, false); finishUpdate(); }

		// The same frame in two halves, for stepping on the physics thread while the game draws: beginUpdate reads the
		// animated pose (game thread) and starts the step, on the physics thread when async; finishUpdate waits for it
		// and writes the bones back (game thread). Only the step runs on the physics thread: it touches no game node.
		void beginUpdate(float interval, bool async);
		void finishUpdate();
		// Waits for a step on the physics thread (anything changing the world or its settings from the game thread first).
		void waitForStep();
		// A fault in the step on the physics thread (empty if none), taken once: the world is disabled after one.
		std::string takeStepFailure();

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

		float m_averageStepMs = 0.0f;   // the physics' whole work a frame: reading the pose, the step, writing the bones
		float m_averageMainMs = 0.0f;   // what of it the game thread spends (all of it unless the step runs on the physics thread)

	private:
		SkyrimPhysicsWorld();
		~SkyrimPhysicsWorld() noexcept;

		void runStep();          // the step itself (m_lock held): no game node is read or written
		void runStepGuarded();   // runStep under m_lock, a fault caught (SEH) and kept for takeStepFailure
		void stepThread();

		std::mutex m_lock;
		float m_accumulatedInterval = 0;

		// What beginUpdate left for finishUpdate: nothing, a step to write back, or no step this frame (the bones follow).
		enum class Pending { None, Stepped, Followed } m_pending = Pending::None;
		float m_pendingTime = 0;        // the time the pending step covers
		bool m_pendingAsync = false;    // it runs on the physics thread
		LARGE_INTEGER m_beginTick{};    // when beginUpdate started
		float m_readMs = 0;             // beginUpdate's own time on the game thread (the pose read)

		std::mutex m_stepMutex;
		std::condition_variable m_stepStart, m_stepDone;
		bool m_stepRunning = false;
		bool m_threadStarted = false;
		float m_stepThreadMs = 0;       // the step's own time on the physics thread
		char m_stepFailure[128] = {};
	};
}
