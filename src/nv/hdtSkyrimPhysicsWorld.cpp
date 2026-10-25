// Ported from FSMP (hdtSMP64, src/hdtSkyrimPhysicsWorld.cpp), GPL-3.0 with FSMP's modding
// exception. The solver set-up and the substepping are FSMP's; the frame, pause, shutdown and
// camera events become one update per game frame, its step optionally on a physics thread.
#include "hdtSkyrimPhysicsWorld.h"
#include "hdtSkyrimBone.h"

#include <LinearMath/btQuickprof.h>
#include <thread>

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

	static float MsSince(const LARGE_INTEGER& t0)
	{
		LARGE_INTEGER t1, freq;
		QueryPerformanceCounter(&t1);
		QueryPerformanceFrequency(&freq);
		return (float)((t1.QuadPart - t0.QuadPart) * 1000.0 / freq.QuadPart);
	}

	void SkyrimPhysicsWorld::beginUpdate(float interval, bool async)
	{
		waitForStep();   // finishUpdate normally did, unless the frame skipped it
		m_pending = Pending::None;
		if (disabled || m_systems.empty())
			return;

		_MM_SET_FLUSH_ZERO_MODE(_MM_FLUSH_ZERO_ON);
		QueryPerformanceCounter(&m_beginTick);
		std::unique_lock<std::mutex> l(m_lock);
		recentre();

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
			m_accumulatedInterval -= remainingTimeStep;
			m_pendingTime = remainingTimeStep;
			m_pending = Pending::Stepped;
			m_pendingAsync = async;
			l.unlock();
			m_readMs = MsSince(m_beginTick);

			if (async) {
				// The physics thread takes m_lock for the step: the game thread's changes to the world (systems added or
				// removed) wait for it there, and finishUpdate waits for it before writing.
				std::unique_lock<std::mutex> s(m_stepMutex);
				if (!m_threadStarted) {
					m_threadStarted = true;
					std::thread([this] { stepThread(); }).detach();   // never stopped: it waits for the next step
				}
				m_stepRunning = true;
				s.unlock();
				m_stepStart.notify_one();
			} else
				runStepGuarded();
		} else {
			m_pending = Pending::Followed;
			m_pendingAsync = false;
			m_readMs = MsSince(m_beginTick);
		}
	}

	void SkyrimPhysicsWorld::runStep()
	{
		LARGE_INTEGER t0;
		QueryPerformanceCounter(&t0);
		// The bodies stay in the physics' space (SkyrimBone::s_origin, see recentre): no offset there and back every step,
		// which in world coordinates rounded every body by up to about 0.004 units each time.
		stepSimulation(m_pendingTime, 0, m_timeTick * 1.0001f);   // a hair over: n - 1 whole steps and a last one of the same size
		m_stepThreadMs = MsSince(t0);
	}

	static int KeepStepFault(EXCEPTION_POINTERS* info, char* out, size_t size)
	{
		snprintf(out, size, "code %08X at %p", (unsigned)info->ExceptionRecord->ExceptionCode, info->ExceptionRecord->ExceptionAddress);
		return EXCEPTION_EXECUTE_HANDLER;
	}

	void SkyrimPhysicsWorld::runStepGuarded()
	{
		// No RAII lock: a fault (SEH) does not unwind C++ objects here, and m_lock must be let go of after one.
		m_lock.lock();
		__try {
			runStep();
		} __except (KeepStepFault(GetExceptionInformation(), m_stepFailure, sizeof(m_stepFailure))) {
			disabled = true;
			m_pending = Pending::None;
		}
		m_lock.unlock();
	}

	void SkyrimPhysicsWorld::stepThread()
	{
		_MM_SET_FLUSH_ZERO_MODE(_MM_FLUSH_ZERO_ON);
		for (;;) {
			std::unique_lock<std::mutex> s(m_stepMutex);
			m_stepStart.wait(s, [this] { return m_stepRunning; });
			s.unlock();
			runStepGuarded();
			s.lock();
			m_stepRunning = false;
			s.unlock();
			m_stepDone.notify_all();
		}
	}

	void SkyrimPhysicsWorld::waitForStep()
	{
		std::unique_lock<std::mutex> s(m_stepMutex);
		m_stepDone.wait(s, [this] { return !m_stepRunning; });
	}

	std::string SkyrimPhysicsWorld::takeStepFailure()
	{
		std::string out = m_stepFailure;
		m_stepFailure[0] = 0;
		return out;
	}

	void SkyrimPhysicsWorld::finishUpdate()
	{
		if (m_pending == Pending::None)
			return;
		LARGE_INTEGER t0, t1;
		QueryPerformanceCounter(&t0);
		waitForStep();
		const float waitMs = MsSince(t0);
		if (m_pending == Pending::None)   // the step faulted
			return;
		QueryPerformanceCounter(&t1);
		std::lock_guard<std::mutex> l(m_lock);
		const bool stepped = m_pending == Pending::Stepped;
		m_pending = Pending::None;
		if (m_systems.empty())
			return;

		if (stepped) {
			NVSMP_PROF(WriteTransform);
			SkyrimBone::s_stepTime = m_pendingTime;   // the jitter filter's step
			SkyrimBone::s_boneByNode.clear();
			for (auto& s : m_systems)
				for (auto& b : s->getBones()) {
					auto* sb = static_cast<SkyrimBone*>(b.get());
					SkyrimBone::s_boneByNode[sb->m_node.get()] = sb;
					// A step from the physics thread is written a frame after its read, which was the last to put the
					// animated extra bones (a chain's anchors) on their anchors: put them there as drawn now, or the chain
					// hangs from where the body was a frame ago on every frame with a step (bones go root first).
					if (m_pendingAsync && sb->m_anchor && sb->m_rig.isStaticOrKinematicObject())
						sb->m_node->world = sb->m_anchor->world * sb->m_anchorToNode;
				}
			writeTransform();
		} else {
			NVSMP_PROF(WriteTransform);
			followTransform();
		}

		// The whole work, and the game thread's share of it: with the step on the physics thread, only what it waited
		// for the step (none if the step was done before the game came back) instead of the step itself.
		const float writeMs = MsSince(t1);
		const float stepMs = stepped ? m_stepThreadMs : 0.0f;
		m_averageStepMs += (m_readMs + stepMs + writeMs - m_averageStepMs) * 0.1f;
		m_averageMainMs += (m_readMs + (m_pendingAsync ? waitMs : stepMs) + writeMs - m_averageMainMs) * 0.1f;
	}

	// Bullet works relative to the centre of everything simulated: far from the world origin, single precision would make
	// the bones jitter. The bodies live in the physics' space (SkyrimBone::s_origin) all the time, not just during a step
	// (moved there and back every step, they were rounded at world coordinates every step: a charm shimmered in place).
	// Once their centre is over 1000 units from its origin (the player walked on, another cell), every body moves back by
	// that much and the origin forward: one rounding then, and still again.
	void SkyrimPhysicsWorld::recentre()
	{
		btVector3 center(0, 0, 0);
		int count = 0;
		for (int i = 0; i < m_collisionObjects.size(); ++i)
			if (auto rig = btRigidBody::upcast(m_collisionObjects[i])) {
				center += rig->getWorldTransform().getOrigin();
				++count;
			}
		if (count == 0)
			return;
		center /= static_cast<btScalar>(count);
		if (center.length2() < 1000.0f * 1000.0f)
			return;
		for (int i = 0; i < m_collisionObjects.size(); ++i)
			if (auto rig = btRigidBody::upcast(m_collisionObjects[i])) {
				rig->getWorldTransform().getOrigin() -= center;
				btTransform t = rig->getInterpolationWorldTransform();
				t.getOrigin() -= center;
				rig->setInterpolationWorldTransform(t);
			}
		SkyrimBone::s_origin += center;
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

namespace hdt
{
	// Impact sounds. Each step, before its solve, every contact between two simulated bones' shapes (Bullet's own collision of
	// bone shapes: a bottle's box against another's) where either bone names impact sounds: a pair that was not touching the
	// step before, striking at its bone's impact speed or faster, sounds once (then not again for kImpactQuiet seconds, so
	// two bottles settling against each other do not buzz). Not against the body's animated bones, which pieces brush all the
	// time. How fast they struck is their bodies' speed toward each other along the contact normal, taken before the solver
	// takes it out (after it they read 0, and it writes no impulses back to these contacts). A softer touch is queued too,
	// without a file (the previewer logs it, to tune <impact-speed>). Runs on the physics step (its thread, when async):
	// queued for takeImpacts.
	static constexpr double kImpactQuiet = 0.12;
	static constexpr size_t kMaxQueuedImpacts = 32;

	void SkyrimPhysicsWorld::contactsFound()
	{
		m_impactClock += m_timeTick;
		std::set<std::pair<const void*, const void*>> touching;
		btDispatcher* dispatcher = getCollisionWorld()->getDispatcher();
		const int n = dispatcher->getNumManifolds();
		for (int i = 0; i < n; ++i) {
			btPersistentManifold* manifold = dispatcher->getManifoldByIndexInternal(i);
			if (manifold->getNumContacts() <= 0) continue;
			const auto* a = btRigidBody::upcast(manifold->getBody0());
			const auto* b = btRigidBody::upcast(manifold->getBody1());
			if (!a || !b || !a->getUserPointer() || !b->getUserPointer()) continue;   // a skinned mesh (cloth, body), not a bone
			auto* boneA = static_cast<SkyrimBone*>(static_cast<SkinnedMeshBone*>(a->getUserPointer()));
			auto* boneB = static_cast<SkyrimBone*>(static_cast<SkinnedMeshBone*>(b->getUserPointer()));
			if (&boneA->m_rig != a || &boneB->m_rig != b) continue;
			if (a->isStaticOrKinematicObject() || b->isStaticOrKinematicObject()) continue;   // the body's animated bones
			SkyrimBone* sounding = !boneA->m_impactSounds.empty() ? boneA : !boneB->m_impactSounds.empty() ? boneB : nullptr;
			if (!sounding) continue;
			const void* pa = boneA; const void* pb = boneB;
			const auto key = pa < pb ? std::make_pair(pa, pb) : std::make_pair(pb, pa);
			touching.insert(key);
			if (m_touching.count(key)) continue;   // still touching: it sounded when they met
			// The contact where they come together fastest, and how fast: A's speed toward B along the normal (B to A).
			int best = -1;
			btScalar closing = 0;
			for (int k = 0; k < manifold->getNumContacts(); ++k) {
				const btManifoldPoint& pt = manifold->getContactPoint(k);
				const btVector3 va = a->getVelocityInLocalPoint(pt.getPositionWorldOnA() - a->getCenterOfMassPosition());
				const btVector3 vb = b->getVelocityInLocalPoint(pt.getPositionWorldOnB() - b->getCenterOfMassPosition());
				const btScalar c = -(va - vb).dot(pt.m_normalWorldOnB);
				if (best < 0 || c > closing) { closing = c; best = k; }
			}
			if (best < 0) continue;
			const float speed = std::max(0.0f, (float)closing);
			const bool soft = speed < sounding->m_impactSpeed;   // queued without a file: the previewer logs it, for tuning
			// Soft touches keep their own quiet time: one must not silence a real strike just after it.
			auto& quiet = soft ? m_softAt : m_impactAt;
			auto last = quiet.find(key);
			if (last != quiet.end() && m_impactClock - last->second < kImpactQuiet) continue;
			quiet[key] = m_impactClock;

			const bool hard = !soft && speed >= sounding->m_impactHardSpeed && !sounding->m_impactSoundsHard.empty();
			const auto& files = hard ? sounding->m_impactSoundsHard : sounding->m_impactSounds;
			m_impactRandom = m_impactRandom * 1664525u + 1013904223u;
			const btVector3 p = manifold->getContactPoint(best).getPositionWorldOnA() + SkyrimBone::s_origin;
			const float span = std::max(sounding->m_impactHardSpeed - sounding->m_impactSpeed, 1.0f);
			Impact hit;
			if (!soft) hit.file = files[(m_impactRandom >> 8) % files.size()];
			hit.threshold = sounding->m_impactSpeed;
			hit.at = { p.x(), p.y(), p.z() };
			hit.volume = std::clamp(0.35f + 0.65f * (speed - sounding->m_impactSpeed) / span, 0.35f, 1.0f) * sounding->m_impactVolume;
			hit.speed = speed;
			hit.boneA = boneA->m_name.c_str();
			hit.boneB = boneB->m_name.c_str();
			std::lock_guard l(m_impactLock);
			if (m_impacts.size() < kMaxQueuedImpacts) m_impacts.push_back(std::move(hit));
		}
		m_touching.swap(touching);
		// Pairs quiet for a while are forgotten (their bones may be gone).
		for (auto it = m_impactAt.begin(); it != m_impactAt.end();)
			it = m_impactClock - it->second > 1.0 ? m_impactAt.erase(it) : std::next(it);
		for (auto it = m_softAt.begin(); it != m_softAt.end();)
			it = m_impactClock - it->second > 1.0 ? m_softAt.erase(it) : std::next(it);
	}

	std::vector<SkyrimPhysicsWorld::Impact> SkyrimPhysicsWorld::takeImpacts()
	{
		std::lock_guard l(m_impactLock);
		std::vector<Impact> out;
		out.swap(m_impacts);
		return out;
	}
}
