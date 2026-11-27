#include "workerPool.hpp"

namespace deunify::detail {

WorkerPool::WorkerPool(uint32_t threads) {
	for (uint32_t i = 1; i < threads; ++i) {
		m_threads.emplace_back([this] { help(); });
	}
}

WorkerPool::~WorkerPool() {
	{
		std::lock_guard lock(m_mutex);
		m_stopping = true;
	}
	m_more.notify_all();
	for (std::thread& thread : m_threads) {
		thread.join();
	}
}

void WorkerPool::each(size_t count, const std::function<void(size_t)>& job) {
	if (count < 2 || m_threads.empty()) {
		for (size_t i = 0; i < count; ++i) {
			job(i);
		}
		return;
	}
	std::lock_guard turn(m_turn);
	{
		std::lock_guard lock(m_mutex);
		m_job   = &job;
		m_next  = 0;
		m_count = count;
	}
	m_more.notify_all();
	for (;;) {
		size_t index = 0;
		{
			std::lock_guard lock(m_mutex);
			if (m_next >= m_count) {
				break;
			}
			index = m_next++;
		}
		job(index);
	}
	std::unique_lock lock(m_mutex);
	m_job = nullptr;
	m_idle.wait(lock, [this] { return m_running == 0; });
}

void WorkerPool::help() {
	std::unique_lock lock(m_mutex);
	for (;;) {
		m_more.wait(lock, [this] { return m_stopping || hasWork(); });
		if (m_stopping) {
			return;
		}
		size_t                             index = m_next++;
		const std::function<void(size_t)>* job   = m_job;
		++m_running;
		lock.unlock();
		(*job)(index);
		lock.lock();
		if (--m_running == 0) {
			m_idle.notify_all();
		}
	}
}

bool WorkerPool::hasWork() const {
	return m_job != nullptr && m_next < m_count;
}

} // namespace deunify::detail
