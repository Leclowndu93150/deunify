#pragma once

#include <cstdint>
#include <map>
#include <memory>
#include <mutex>
#include <unordered_map>
#include <vector>

namespace deunify::detail {

using Decoded = std::shared_ptr<const std::vector<uint8_t>>;

/// Decoded results kept by key up to a byte cap, oldest (by last ask) dropped first. Each key is
/// decoded once however many threads ask at the same time: the first asker decodes without the
/// lock held, the others wait for its result. A failed decode (null) is given to those waiting
/// for it but not kept, so the next asker decodes again.
template <typename Key, typename Hash>
class DecodedStore {
public:
	/// The result for `key`, made by `make` (returns Decoded, null on failure) unless kept or
	/// being made.
	template <typename Make>
	Decoded kept(const Key& key, uint64_t cap, Make&& make) {
		std::shared_ptr<Once> made  = entry(key);
		bool                  fresh = false;
		std::call_once(made->once, [&] {
			fresh       = true;
			made->bytes = make();
		});
		if (fresh) {
			done(key, made, cap);
		}
		return made->bytes;
	}

	bool has(const Key& key) const {
		std::lock_guard lock(m_mutex);
		return m_kept.contains(key);
	}

	/// Bytes of the results kept now.
	uint64_t bytes() const {
		std::lock_guard lock(m_mutex);
		return m_bytes;
	}

private:
	struct Once {
		std::once_flag once;
		Decoded        bytes;
	};

	struct Kept {
		std::shared_ptr<Once> made;
		uint64_t              bytes = 0;
		uint64_t              age   = 0;
	};

	std::shared_ptr<Once> entry(const Key& key) {
		std::lock_guard lock(m_mutex);
		uint64_t        age = ++m_next;
		auto [at, now]      = m_kept.try_emplace(key);
		Kept& kept          = at->second;
		if (now) {
			kept.made = std::make_shared<Once>();
		} else {
			m_ages.erase(kept.age);
		}
		kept.age = age;
		m_ages.emplace(age, key);
		return kept.made;
	}

	void done(const Key& key, const std::shared_ptr<Once>& made, uint64_t cap) {
		std::lock_guard lock(m_mutex);
		auto            at = m_kept.find(key);
		if (at == m_kept.end() || at->second.made != made) {
			return;
		}
		if (made->bytes == nullptr) {
			m_ages.erase(at->second.age);
			m_kept.erase(at);
			return;
		}
		at->second.bytes = made->bytes->size();
		m_bytes += at->second.bytes;
		while (m_bytes > cap && !m_ages.empty()) {
			auto oldest = m_ages.begin();
			auto gone   = m_kept.find(oldest->second);
			m_bytes -= gone->second.bytes;
			m_kept.erase(gone);
			m_ages.erase(oldest);
		}
	}

	mutable std::mutex                  m_mutex;
	std::unordered_map<Key, Kept, Hash> m_kept;
	std::map<uint64_t, Key>             m_ages;
	uint64_t                            m_next  = 0;
	uint64_t                            m_bytes = 0;
};

} // namespace deunify::detail
