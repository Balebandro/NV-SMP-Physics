#pragma once
// The few oneTBB calls FSMP's physics core makes, run on the calling thread. FSMP spreads a
// frame's collision tests over cores with them; one New Vegas frame has a handful of simulated
// actors, so serial is the starting point (a pool can replace these without touching the core).
#include <cstddef>
#include <iterator>
#include <utility>

namespace tbb
{
	template <class It, class F>
	void parallel_for_each(It first, It last, F&& f)
	{
		for (; first != last; ++first) f(*first);
	}

	template <class Range, class F>
	void parallel_for_each(Range& range, F&& f)
	{
		for (auto& item : range) f(item);
	}

	template <class Index, class F>
	void parallel_for(Index first, Index last, F&& f)
	{
		for (Index i = first; i < last; ++i) f(i);
	}

	class task_group
	{
	public:
		template <class F>
		void run(F&& f) { f(); }
		void wait() {}
	};

	namespace this_task_arena
	{
		template <class F>
		auto isolate(F&& f) -> decltype(f()) { return f(); }
	}
}
