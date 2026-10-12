#pragma once
// Section timers for the offline test (built with NVSMP_PROFILE): where a physics step's time goes.
// In the plugin they compile to nothing.
#ifdef NVSMP_PROFILE
#	include <windows.h>
namespace nvsmp::prof
{
	enum Slot { ReadTransform, BonesAndBounds, Broadphase, Narrowphase, Solve, Integrate, WriteTransform, Count };
	inline const char* kNames[Count] = { "read transforms", "bones + bounds", "broadphase", "narrowphase (collision)", "constraint solve", "predict + integrate", "write transforms" };
	inline double gTotal[Count] = {};
	struct Scope
	{
		Slot slot;
		LARGE_INTEGER t0;
		explicit Scope(Slot s) : slot(s) { QueryPerformanceCounter(&t0); }
		~Scope()
		{
			LARGE_INTEGER t1, f;
			QueryPerformanceCounter(&t1);
			QueryPerformanceFrequency(&f);
			gTotal[slot] += (double)(t1.QuadPart - t0.QuadPart) * 1000.0 / (double)f.QuadPart;
		}
	};
}
#	define NVSMP_PROF(slot) nvsmp::prof::Scope nvsmpProfScope##__LINE__(nvsmp::prof::slot)
#else
#	define NVSMP_PROF(slot)
#endif
