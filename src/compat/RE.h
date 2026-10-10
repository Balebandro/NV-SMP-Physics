#pragma once
// Stand-ins for the CommonLibSSE types FSMP's physics core (src/fsmp/hdtSkinnedMesh) uses, so that
// code compiles unchanged for New Vegas: an interned string (BSFixedString), an intrusive smart
// pointer (BSTSmartPointer) and its ref count (BSIntrusiveRefCounted), with the same semantics.
#include <atomic>
#include <cstdint>
#include <cstring>
#include <functional>
#include <mutex>
#include <string>
#include <string_view>
#include <unordered_set>
#include <utility>

namespace RE
{
	namespace detail
	{
		// Every distinct string once, for the life of the process, as the game's string pool keeps
		// them: equal strings share one pointer, so comparing and hashing go by pointer.
		inline const char* Intern(std::string_view s)
		{
			static std::mutex lock;
			static std::unordered_set<std::string> pool;
			std::lock_guard<std::mutex> guard(lock);
			return pool.emplace(s).first->c_str();
		}
	}

	class BSFixedString
	{
	public:
		BSFixedString() : m_data(detail::Intern("")) {}
		BSFixedString(const char* s) : m_data(detail::Intern(s ? s : "")) {}
		BSFixedString(const std::string& s) : m_data(detail::Intern(s)) {}
		BSFixedString(std::string_view s) : m_data(detail::Intern(s)) {}

		const char* c_str() const noexcept { return m_data; }
		const char* data() const noexcept { return m_data; }
		std::size_t size() const noexcept { return std::strlen(m_data); }
		std::size_t length() const noexcept { return size(); }
		bool empty() const noexcept { return !m_data[0]; }
		operator std::string_view() const noexcept { return m_data; }

		friend bool operator==(const BSFixedString& a, const BSFixedString& b) noexcept { return a.m_data == b.m_data; }
		friend bool operator!=(const BSFixedString& a, const BSFixedString& b) noexcept { return a.m_data != b.m_data; }
		friend bool operator==(const BSFixedString& a, const char* b) noexcept { return std::strcmp(a.m_data, b ? b : "") == 0; }
		friend bool operator!=(const BSFixedString& a, const char* b) noexcept { return !(a == b); }
		friend bool operator<(const BSFixedString& a, const BSFixedString& b) noexcept { return std::strcmp(a.m_data, b.m_data) < 0; }

	private:
		const char* m_data;
	};

	class BSIntrusiveRefCounted
	{
	public:
		std::uint32_t IncRef() const { return ++m_refCount; }
		std::uint32_t DecRef() const { return --m_refCount; }
		std::uint32_t QRefCount() const { return m_refCount; }

	protected:
		BSIntrusiveRefCounted() = default;
		BSIntrusiveRefCounted(const BSIntrusiveRefCounted&) {}
		BSIntrusiveRefCounted& operator=(const BSIntrusiveRefCounted&) { return *this; }

	private:
		mutable std::atomic<std::uint32_t> m_refCount{ 0 };
	};

	// Intrusive pointer: IncRef on take, DecRef on release, delete at zero.
	template <class T>
	class BSTSmartPointer
	{
	public:
		using element_type = T;

		BSTSmartPointer() noexcept = default;
		BSTSmartPointer(std::nullptr_t) noexcept {}
		explicit BSTSmartPointer(T* p) : m_ptr(p) { Acquire(); }
		BSTSmartPointer(const BSTSmartPointer& o) : m_ptr(o.m_ptr) { Acquire(); }
		BSTSmartPointer(BSTSmartPointer&& o) noexcept : m_ptr(std::exchange(o.m_ptr, nullptr)) {}
		template <class U>
		BSTSmartPointer(const BSTSmartPointer<U>& o) : m_ptr(o.get()) { Acquire(); }
		template <class U>
		BSTSmartPointer(BSTSmartPointer<U>&& o) noexcept : m_ptr(o.release_unsafe()) {}
		~BSTSmartPointer() { Release(); }

		BSTSmartPointer& operator=(const BSTSmartPointer& o)
		{
			if (this != &o) {
				o.Acquire();
				Release();
				m_ptr = o.m_ptr;
			}
			return *this;
		}
		BSTSmartPointer& operator=(BSTSmartPointer&& o) noexcept
		{
			if (this != &o) {
				Release();
				m_ptr = std::exchange(o.m_ptr, nullptr);
			}
			return *this;
		}
		template <class U>
		BSTSmartPointer& operator=(const BSTSmartPointer<U>& o)
		{
			return *this = BSTSmartPointer(o);
		}

		void reset(T* p = nullptr)
		{
			if (p) p->IncRef();
			Release();
			m_ptr = p;
		}

		T* get() const noexcept { return m_ptr; }
		T& operator*() const noexcept { return *m_ptr; }
		T* operator->() const noexcept { return m_ptr; }
		explicit operator bool() const noexcept { return m_ptr != nullptr; }

		// Hands the pointer over without touching its count (move between pointer types).
		T* release_unsafe() noexcept { return std::exchange(m_ptr, nullptr); }

		template <class U>
		friend bool operator==(const BSTSmartPointer& a, const BSTSmartPointer<U>& b) noexcept { return a.get() == b.get(); }
		friend bool operator==(const BSTSmartPointer& a, std::nullptr_t) noexcept { return !a.m_ptr; }
		template <class U>
		friend bool operator==(const BSTSmartPointer& a, U* b) noexcept { return a.get() == b; }
		template <class U>
		friend bool operator!=(const BSTSmartPointer& a, U* b) noexcept { return a.get() != b; }

	private:
		void Acquire() const
		{
			if (m_ptr) m_ptr->IncRef();
		}
		void Release()
		{
			if (m_ptr && m_ptr->DecRef() == 0) delete m_ptr;
			m_ptr = nullptr;
		}

		T* m_ptr = nullptr;
	};

	template <class T, class... Args>
	BSTSmartPointer<T> make_smart(Args&&... args)
	{
		return BSTSmartPointer<T>(new T(std::forward<Args>(args)...));
	}
}

namespace std
{
	template <>
	struct hash<RE::BSFixedString>
	{
		std::size_t operator()(const RE::BSFixedString& s) const noexcept { return std::hash<const char*>{}(s.data()); }
	};

	template <class T>
	struct hash<RE::BSTSmartPointer<T>>
	{
		std::size_t operator()(const RE::BSTSmartPointer<T>& p) const noexcept { return std::hash<T*>{}(p.get()); }
	};
}

namespace hdt
{
	template <class T>
	[[nodiscard]] RE::BSTSmartPointer<T> make_smart(T* a_ptr)
	{
		RE::BSTSmartPointer<T> result;
		result.reset(a_ptr);
		return result;
	}
}
