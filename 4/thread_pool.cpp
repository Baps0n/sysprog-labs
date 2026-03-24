#include "thread_pool.h"

#include <pthread.h>
#include <queue>

enum struct TaskState {
	CREATED,
	QUEUED,
	RUNNING,
	FINISHED,
};

struct thread_task {
	thread_task_f function;

	/* PUT HERE OTHER MEMBERS */
	pthread_mutex_t mutex;
	pthread_cond_t cond;
	TaskState state = TaskState::CREATED;
	thread_pool *pool;
	bool is_detached = false;
};

struct thread_pool {
	std::vector<pthread_t> threads;

	/* PUT HERE OTHER MEMBERS */
	std::queue<thread_task*> tasks_queue;
	size_t max_threads = 0;
	size_t idle_threads = 0;
	pthread_mutex_t mutex;
	pthread_cond_t cond;
	int tasks_running = 0;
	bool is_stopping = false;
};

static void* worker_thread(void *arg) {
	thread_pool *pool = (thread_pool*) arg;

	while (true) {
		pthread_mutex_lock(&pool->mutex);
		while (pool->tasks_queue.empty() && !pool->is_stopping) {
			pool->idle_threads++;
			pthread_cond_wait(&pool->cond, &pool->mutex);
			pool->idle_threads--;
		}

		thread_task *cur_task = pool->tasks_queue.front();
		bool is_stopping = pool->is_stopping;
		if (!is_stopping) {
			pool->tasks_queue.pop();
			pool->tasks_running++;
		}
		pthread_mutex_unlock(&pool->mutex);

		if (is_stopping) {
			return NULL;
		}

		pthread_mutex_lock(&cur_task->mutex);
		cur_task->state = TaskState::RUNNING;
		pthread_mutex_unlock(&cur_task->mutex);

		cur_task->function();

		pthread_mutex_lock(&cur_task->mutex);
		bool is_detached = cur_task->is_detached;
		cur_task->state = TaskState::FINISHED;
		if (!is_detached) {
			pthread_cond_signal(&cur_task->cond); 
		}
		pthread_mutex_unlock(&cur_task->mutex);

		if (is_detached) {
			thread_task_delete(cur_task);
		}

		pthread_mutex_lock(&pool->mutex);
		pool->tasks_running--;
		if (pool->tasks_running == 0) {
			pthread_cond_broadcast(&pool->cond);
		}
		pthread_mutex_unlock(&pool->mutex);
	}
}

int
thread_pool_new(int thread_count, struct thread_pool **pool)
{
	if (thread_count > TPOOL_MAX_THREADS || thread_count <= 0) {
		return TPOOL_ERR_INVALID_ARGUMENT;
	}

	thread_pool *new_pool = new thread_pool();
	new_pool->max_threads = thread_count;
	pthread_mutex_init(&new_pool->mutex, NULL);
	pthread_cond_init(&new_pool->cond, NULL);
	*pool = new_pool;

	return 0;
}

int
thread_pool_delete(struct thread_pool *pool)
{
	pthread_mutex_lock(&pool->mutex);
	bool has_tasks = false;
	if (pool->tasks_running > 0 || !pool->tasks_queue.empty()) {
		has_tasks = true;
	}
	pthread_mutex_unlock(&pool->mutex);

	if (has_tasks) {
		return TPOOL_ERR_HAS_TASKS;
	}

	pthread_mutex_lock(&pool->mutex);
	pool->is_stopping = true;
	pthread_cond_broadcast(&pool->cond);
	pthread_mutex_unlock(&pool->mutex);

	for (pthread_t pthread : pool->threads) {
		pthread_join(pthread, NULL);
	}
	delete pool;

	return 0;
}

int
thread_pool_push_task(struct thread_pool *pool, struct thread_task *task)
{
	pthread_mutex_lock(&pool->mutex);
	bool has_too_many = false;
	if (pool->tasks_queue.size() + pool->tasks_running >= TPOOL_MAX_TASKS) {
		has_too_many = true;
	}
	pthread_mutex_unlock(&pool->mutex);

	if (has_too_many) {
		return TPOOL_ERR_TOO_MANY_TASKS;
	}

	pthread_mutex_lock(&pool->mutex);
	pthread_mutex_lock(&task->mutex);
	task->pool = pool;
	task->state = TaskState::QUEUED;
	pthread_mutex_unlock(&task->mutex);

	pool->tasks_queue.push(task);
	
	if (pool->threads.size() < pool->max_threads && pool->idle_threads == 0) {
		pthread_t new_pthread;
		pthread_create(&new_pthread, NULL, worker_thread, pool);
		pool->threads.push_back(new_pthread);
	}
	else {
		pthread_cond_signal(&pool->cond);
	}

	pthread_mutex_unlock(&pool->mutex);

	return 0;
}

int
thread_task_new(struct thread_task **task, const thread_task_f &function)
{
	thread_task *new_task = new thread_task();
	new_task->function = function;
	new_task->state = TaskState::CREATED;
	pthread_mutex_init(&new_task->mutex, NULL);
	pthread_cond_init(&new_task->cond, NULL);
	*task = new_task;

	return 0;
}

bool
thread_task_is_finished(const struct thread_task *task)
{
	pthread_mutex_lock(const_cast<pthread_mutex_t*>(&task->mutex));
	bool retval = false;
	if (task->state == TaskState::FINISHED) {
		retval = true;
	}
	pthread_mutex_unlock(const_cast<pthread_mutex_t*>(&task->mutex));

	return retval;
}

bool
thread_task_is_running(const struct thread_task *task)
{
	pthread_mutex_lock(const_cast<pthread_mutex_t*>(&task->mutex));
	bool retval = false;
	if (task->state == TaskState::RUNNING) {
		retval = true;
	}
	pthread_mutex_unlock(const_cast<pthread_mutex_t*>(&task->mutex));
	
	return retval;
}

int
thread_task_join(struct thread_task *task)
{
	pthread_mutex_lock(const_cast<pthread_mutex_t*>(&task->mutex));
	bool in_pool = false;
	if (task->state != TaskState::CREATED) {
		in_pool = true;
	}
	pthread_mutex_unlock(const_cast<pthread_mutex_t*>(&task->mutex));
	
	if (!in_pool) {
		return TPOOL_ERR_TASK_NOT_PUSHED;
	}

	pthread_mutex_lock(const_cast<pthread_mutex_t*>(&task->mutex));
	while (!(task->state == TaskState::FINISHED)) {
		pthread_cond_wait(&task->cond, &task->mutex);
	}
	task->pool = NULL;
	pthread_mutex_unlock(const_cast<pthread_mutex_t*>(&task->mutex));

	return 0;
}

#if NEED_TIMED_JOIN

int
thread_task_timed_join(struct thread_task *task, double timeout)
{
	pthread_mutex_lock(const_cast<pthread_mutex_t*>(&task->mutex));
	bool in_pool = false;
	if (task->state != TaskState::CREATED) {
		in_pool = true;
	}
	pthread_mutex_unlock(const_cast<pthread_mutex_t*>(&task->mutex));
	
	if (!in_pool) {
		return TPOOL_ERR_TASK_NOT_PUSHED;
	}

	timespec timespec_timeout;
	auto now = std::chrono::system_clock::now();
	auto chrono_timeout = now + std::chrono::duration<double>(timeout);
	long long chrono_timeout_nsec = std::chrono::duration_cast<std::chrono::nanoseconds>(chrono_timeout.time_since_epoch()).count();
	timespec_timeout.tv_sec = chrono_timeout_nsec / 1000000000;
	timespec_timeout.tv_nsec = chrono_timeout_nsec % 1000000000;

	pthread_mutex_lock(const_cast<pthread_mutex_t*>(&task->mutex));
	bool is_timeout = false;
	while (!(task->state == TaskState::FINISHED)) {
		if (pthread_cond_timedwait(&task->cond, &task->mutex, &timespec_timeout) == ETIMEDOUT) {
			is_timeout = true;
			break;
		}
	}
	task->pool = NULL;
	pthread_mutex_unlock(const_cast<pthread_mutex_t*>(&task->mutex));

	if (is_timeout) {
		return TPOOL_ERR_TIMEOUT;
	}

	return 0;
}

#endif

int
thread_task_delete(struct thread_task *task)
{
	pthread_mutex_lock(const_cast<pthread_mutex_t*>(&task->mutex));
	bool in_pool = false;
	if (task->pool != NULL) {
		in_pool = true;
	}
	pthread_mutex_unlock(const_cast<pthread_mutex_t*>(&task->mutex));


	if (in_pool) {
		return TPOOL_ERR_TASK_IN_POOL;
	}
	
	delete task;
	return 0;
}

#if NEED_DETACH

int
thread_task_detach(struct thread_task *task)
{
	pthread_mutex_lock(const_cast<pthread_mutex_t*>(&task->mutex));
	bool is_pushed = false;
	if (task->state != TaskState::CREATED) {
		is_pushed = true;
	}

	bool is_finished = false;
	if (task->state == TaskState::FINISHED) {
		is_finished = true;
	}

	if (is_pushed && !is_finished) {
		task->is_detached = true;
	}
	pthread_mutex_unlock(const_cast<pthread_mutex_t*>(&task->mutex));

	if (!is_pushed) {
		return TPOOL_ERR_TASK_NOT_PUSHED;
	}
	task->pool = NULL;
	if (is_finished) {
		thread_task_delete(task);
		return 0;
	}

	return 0;
}

#endif
