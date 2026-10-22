#pragma once
// The few oneTBB calls FSMP's physics core makes. parallel_for_each (the collision tests: skinning the colliders, the
// body pairs and the tree pairs inside each) runs on a small worker pool, with the calling thread taking items too;
// SetWorkers(0) runs everything on the calling thread. parallel_for (readTransform, which reads game nodes) stays
// serial.
//
// A thread that starts a parallel loop only ever works on that loop's items until it is done, never on another
// loop's: FSMP's collision code keeps per-thread scratch (thread_local) that an unrelated item would overwrite (what
// FSMP's tbb::this_task_arena::isolate is for), and it also rules out deadlock (every loop can be finished by its
// own caller alone).
#include <atomic>
#include <condition_variable>
#include <cstddef>
#include <immintrin.h>
#include <iterator>
#include <mutex>
#include <thread>
#include <type_traits>
#include <utility>
#include <vector>

namespace tbb
{
	namespace detail
	{
		struct Job
		{
			void (*run)(void* ctx, size_t i);
			void* ctx;
			size_t count;
			std::atomic<size_t> next{ 0 };
			std::atomic<size_t> done{ 0 };
			std::atomic<int> helpers{ 0 };   // workers still holding this job (it lives on its caller's stack)
		};

		struct Pool
		{
			std::mutex lock;
			std::condition_variable wake;
			std::vector<Job*> jobs;          // loops still posted (under lock)
			std::vector<std::thread> threads;
			std::atomic<unsigned> posted{ 0 };
			bool stopping = false;

			static void work(Job* job)
			{
				for (size_t i; (i = job->next.fetch_add(1, std::memory_order_relaxed)) < job->count;) {
					job->run(job->ctx, i);
					job->done.fetch_add(1, std::memory_order_release);
				}
			}

			Job* open() const   // under lock
			{
				for (Job* j : jobs)
					if (j->next.load(std::memory_order_relaxed) < j->count) return j;
				return nullptr;
			}

			void loop()
			{
				for (;;) {
					// a step posts several short loops in a row: spin a little before sleeping
					const unsigned seen = posted.load(std::memory_order_acquire);
					for (int spin = 0; spin < 4000 && posted.load(std::memory_order_acquire) == seen; spin++) _mm_pause();
					Job* job = nullptr;
					{
						// one look: the callers take items without the lock, so a second open() can come back empty
						std::unique_lock<std::mutex> l(lock);
						wake.wait(l, [&] { return stopping || (job = open()) != nullptr; });
						if (stopping) return;
						job->helpers.fetch_add(1, std::memory_order_relaxed);
					}
					work(job);
					job->helpers.fetch_sub(1, std::memory_order_release);   // the last touch of the job
				}
			}

			void resize(int n)
			{
				stop();
				stopping = false;
				for (int i = 0; i < n; i++) threads.emplace_back([this] { loop(); });
			}

			void stop()
			{
				{
					std::lock_guard<std::mutex> l(lock);
					stopping = true;
				}
				wake.notify_all();
				for (auto& t : threads) t.join();
				threads.clear();
			}

			// At process exit the workers are already gone (joining them from DLL unload can hang): let them be.
			~Pool() { for (auto& t : threads) t.detach(); }

			void run(Job& job)
			{
				{
					std::lock_guard<std::mutex> l(lock);
					jobs.push_back(&job);
				}
				posted.fetch_add(1, std::memory_order_release);
				wake.notify_all();
				work(&job);
				{
					std::lock_guard<std::mutex> l(lock);   // no worker picks it up after this
					for (size_t i = 0; i < jobs.size(); i++)
						if (jobs[i] == &job) { jobs.erase(jobs.begin() + i); break; }
				}
				while (job.done.load(std::memory_order_acquire) < job.count || job.helpers.load(std::memory_order_acquire) != 0) _mm_pause();
			}
		};

		inline Pool gPool;
	}

	// Worker threads for parallel_for_each (the calling thread works too). 0: serial. Not while a loop is running.
	inline void SetWorkers(int n)
	{
		if (n < 0) n = 0;
		if ((size_t)n != detail::gPool.threads.size()) detail::gPool.resize(n);
	}

	// About half the cores minus the game's own busy threads, at most 4.
	inline int AutoWorkers()
	{
		const int n = (int)std::thread::hardware_concurrency() / 2 - 1;
		return n < 0 ? 0 : (n > 4 ? 4 : n);
	}

	inline int Workers() { return (int)detail::gPool.threads.size(); }

	template <class It, class F>
	void parallel_for_each(It first, It last, F&& f)
	{
		const size_t count = (size_t)std::distance(first, last);
		if (count < 2 || detail::gPool.threads.empty()) {
			for (; first != last; ++first) f(*first);
			return;
		}
		struct Ctx { It first; std::remove_reference_t<F>* f; } ctx{ first, &f };
		detail::Job job;
		job.run = [](void* c, size_t i) { auto* x = (Ctx*)c; (*x->f)(*std::next(x->first, (std::ptrdiff_t)i)); };
		job.ctx = &ctx;
		job.count = count;
		detail::gPool.run(job);
	}

	template <class Range, class F>
	void parallel_for_each(Range& range, F&& f)
	{
		parallel_for_each(std::begin(range), std::end(range), std::forward<F>(f));
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
