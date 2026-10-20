// Ported from FSMP (hdtSMP64, src/hdtSkyrimPhysicsWorld.cpp), GPL-3.0 with FSMP's modding
// exception. The solver set-up and the substepping are FSMP's; the frame, pause, shutdown and
// camera events become one synchronous doUpdate per game frame.
#include "hdtSkyrimPhysicsWorld.h"
#include "hdtSkyrimBone.h"

#include <LinearMath/btQuickprof.h>

namespace hdt
{
	SkyrimPhysicsWorld::SkyrimPhysicsWorld()
	{
		gDisableDeactivation = true;
		btDiscreteDynamicsWorld::setGravity(btVector3(0, 0, -9.8f * scaleSkyrim));

		// FSMP's solver settings (see its notes in hdtSkyrimPhysicsWorld.cpp).
		getSolverInfo().m_friction = 0;
		getSolverInfo().m_splitImpulse = true;
		getSolverInfo().m_splitImpulsePenetrationThreshold = -0.01f;
		getSolverInfo().m_erp2 = 0.15f;
		getSolverInfo().m_globalCfm = 0.001f;
		getSolverInfo().m_restitutionVelocityThreshold = 0.2f;
		getSolverInfo().m_solverMode = SOLVER_SIMD;
		getSolverInfo().m_leastSquaresResidualThreshold = 0.0001f;
	}

	SkyrimPhysicsWorld::~SkyrimPhysicsWorld() noexcept
	{
	}

	SkyrimPhysicsWorld* SkyrimPhysicsWorld::get()
	{
		static SkyrimPhysicsWorld g_World;
		return &g_World;
	}

	void SkyrimPhysicsWorld::doUpdate(float interval)
	{
		if (disabled || m_systems.empty())
			return;

		_MM_SET_FLUSH_ZERO_MODE(_MM_FLUSH_ZERO_ON);
		std::lock_guard<std::mutex> l(m_lock);

		LARGE_INTEGER t0, t1, freq;
		QueryPerformanceCounter(&t0);

		// New Vegas: a fixed step, m_timeTick (1 / the rate setting), whatever the frame rate. FSMP stepped the frame
		// time itself above its rate (and whole ticks plus a remainder below it), so the cloth behaved differently at
		// every frame rate, and a remainder of a sliver of a millisecond kicked it (the joints' error correction scales
		// with 1 / step). Here the frame's time is banked and spent in whole steps; what is left waits for the next
		// frame, and a frame with no step puts the bones back on the moving body as last simulated (followTransform).
		// A frame longer than m_maxSubSteps 60 Hz frames drops the rest rather than catching up.
		m_accumulatedInterval += interval;
		const float tick = m_timeTick;
		const int maxSteps = std::max(1, (int)(m_maxSubSteps * (1.0f / 60.0f) / tick + 0.5f));
		int steps = (int)(m_accumulatedInterval / tick);
		if (steps > maxSteps) {
			steps = maxSteps;
			m_accumulatedInterval = steps * tick;
		}

		if (steps > 0) {
			const float remainingTimeStep = steps * tick;

			{
				NVSMP_PROF(ReadTransform);
				// The animated bones go as far as the physics advances: it ends this frame behind it by what stays banked.
				SkyrimBone::s_kinematicFraction = remainingTimeStep / m_accumulatedInterval;
				readTransform(remainingTimeStep);
				SkyrimBone::s_kinematicFraction = 1;
			}
			m_resetPc -= m_resetPc > 0;

			auto offset = applyTranslationOffset();
			stepSimulation(remainingTimeStep, 0, tick * 1.0001f);   // a hair over: n - 1 whole steps and a last one of the same size
			restoreTranslationOffset(offset);
			m_accumulatedInterval -= remainingTimeStep;

			{
				NVSMP_PROF(WriteTransform);
				SkyrimBone::s_stepTime = remainingTimeStep;   // the jitter filter's step
				SkyrimBone::s_boneByNode.clear();
				for (auto& s : m_systems)
					for (auto& b : s->getBones()) { auto* sb = static_cast<SkyrimBone*>(b.get()); SkyrimBone::s_boneByNode[sb->m_node.get()] = sb; }
				writeTransform();
			}
		} else {
			NVSMP_PROF(WriteTransform);
			followTransform();
		}

		QueryPerformanceCounter(&t1);
		QueryPerformanceFrequency(&freq);
		float ms = (float)((t1.QuadPart - t0.QuadPart) * 1000.0 / freq.QuadPart);
		m_averageStepMs += (ms - m_averageStepMs) * 0.1f;
	}

	// Bullet works relative to the centre of everything simulated: far from the world origin, single
	// precision would make the bones jitter.
	btVector3 SkyrimPhysicsWorld::applyTranslationOffset()
	{
		btVector3 center;
		center.setZero();
		int count = 0;
		for (int i = 0; i < m_collisionObjects.size(); ++i) {
			auto rig = btRigidBody::upcast(m_collisionObjects[i]);
			if (rig) {
				center += rig->getWorldTransform().getOrigin();
				++count;
			}
		}

		if (count > 0) {
			center /= static_cast<btScalar>(count);
			for (int i = 0; i < m_collisionObjects.size(); ++i) {
				auto rig = btRigidBody::upcast(m_collisionObjects[i]);
				if (rig)
					rig->getWorldTransform().getOrigin() -= center;
			}
		}
		return center;
	}

	void SkyrimPhysicsWorld::restoreTranslationOffset(const btVector3& offset)
	{
		for (int i = 0; i < m_collisionObjects.size(); ++i) {
			auto rig = btRigidBody::upcast(m_collisionObjects[i]);
			if (rig)
				rig->getWorldTransform().getOrigin() += offset;
		}
	}

	void SkyrimPhysicsWorld::setWind(const btVector3& target, uint32_t a_smoothingSamples)
	{
		if (a_smoothingSamples == 0)
			a_smoothingSamples = 1;
		const float old = static_cast<float>(a_smoothingSamples - 1);
		m_windSpeed = (m_windSpeed * old + target) / static_cast<float>(a_smoothingSamples);
	}

	void SkyrimPhysicsWorld::addSkinnedMeshSystem(SkinnedMeshSystem* system)
	{
		std::lock_guard<std::mutex> l(m_lock);
		auto s = dynamic_cast<SkyrimSystem*>(system);
		if (!s)
			return;

		s->m_initialized = false;
		SkinnedMeshWorld::addSkinnedMeshSystem(system);
	}

	void SkyrimPhysicsWorld::removeSkinnedMeshSystem(SkinnedMeshSystem* system)
	{
		std::lock_guard<std::mutex> l(m_lock);
		SkinnedMeshWorld::removeSkinnedMeshSystem(system);
	}

	void SkyrimPhysicsWorld::removeSystemByNode(void* root)
	{
		std::lock_guard<std::mutex> l(m_lock);
		for (size_t i = 0; i < m_systems.size();) {
			auto* s = dynamic_cast<SkyrimSystem*>(m_systems[i].get());
			if (s && s->m_skeleton == root)
				SkinnedMeshWorld::removeSkinnedMeshSystem(s);
			else
				++i;
		}
	}

	void SkyrimPhysicsWorld::removeAllSystems()
	{
		std::lock_guard<std::mutex> l(m_lock);
		while (!m_systems.empty())
			SkinnedMeshWorld::removeSkinnedMeshSystem(m_systems.back().get());
	}

	void SkyrimPhysicsWorld::resetSystems()
	{
		std::lock_guard<std::mutex> l(m_lock);
		for (auto& i : m_systems)
			i->readTransform(i->prepareForRead(RESET_PHYSICS));
	}
}
