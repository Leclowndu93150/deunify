#pragma once

#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <mutex>
#include <thread>
#include <vector>

namespace deunify::detail {

/// Threads that run the numbered jobs of one batch at a time, together with the caller.
class WorkerPool {
public:
	/// `threads` counts the caller: `threads - 1` helper threads are started.
	explicit WorkerPool(uint32_t threads);
	~WorkerPool();
	WorkerPool(const WorkerPool&)            = delete;
	WorkerPool& operator=(const WorkerPool&) = delete;

	/// Calls `job` with every number below `count`, on the helpers and this thread, and returns
	/// when every call has. Batches from several callers run one after the other.
	void each(size_t count, const std::function<void(size_t)>& job);

private:
	void help();
	bool hasWork() const;

	std::mutex                         m_turn;
	std::mutex                         m_mutex;
	std::condition_variable            m_more;
	std::condition_variable            m_idle;
	const std::function<void(size_t)>* m_job      = nullptr;
	size_t                             m_next     = 0;
	size_t                             m_count    = 0;
	size_t                             m_running  = 0;
	bool                               m_stopping = false;
	std::vector<std::thread>           m_threads;
};

} // namespace deunify::detail
