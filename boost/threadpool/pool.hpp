/*! \file
* \brief Thread pool core.
*
* This file contains the threadpool's core class: pool<Task, SchedulingPolicy>.
*
* Thread pools are a mechanism for asynchronous and parallel processing
* within the same process. The pool class provides a convenient way
* for dispatching asynchronous tasks as functions objects. The scheduling
* of these tasks can be easily controlled by using customized schedulers.
*
* Copyright (c) 2005-2007 Philipp Henkel
* Copyright (c) 2016 Mikhail Komarov (nemo1369@gmail.com)
*
* Use, modification, and distribution are  subject to the
* Boost Software License, Version 1.0. (See accompanying  file
* LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
*
* http://threadpool.sourceforge.net
*
*/


#ifndef THREADPOOL_POOL_HPP_INCLUDED
#define THREADPOOL_POOL_HPP_INCLUDED

#define BOOST_THREAD_PROVIDES_FUTURE
#define BOOST_THREAD_PROVIDES_FUTURE_CONTINUATION
#define BOOST_THREAD_PROVIDES_SIGNATURE_PACKAGED_TASK

#include <boost/thread.hpp>
#include <boost/ref.hpp>

#include "./detail/pool_core.hpp"

#include "task_adaptors.hpp"

#include "./detail/locking_ptr.hpp"

#include "scheduling_policies.hpp"
#include "size_policies.hpp"
#include "shutdown_policies.hpp"


/// The namespace threadpool contains a thread pool and related utility classes.
namespace boost {
    namespace threadpool {

        /*! \brief Thread pool.
        *
        * Thread pools are a mechanism for asynchronous and parallel processing
        * within the same process. The pool class provides a convenient way
        * for dispatching asynchronous tasks as functions objects. The scheduling
        * of these tasks can be easily controlled by using customized schedulers.
        * A task must not throw an exception.
        *
        * A pool is DefaultConstructible, CopyConstructible and Assignable.
        * It has reference semantics; all copies of the same pool are equivalent and interchangeable.
        * All operations on a pool except assignment are strongly thread safe or sequentially consistent;
        * that is, the behavior of concurrent calls is as if the calls have been issued sequentially in an unspecified order.
        *
        * \param Task A function object which implements the operator 'void operator() (void) const'. The operator () is called by the pool to execute the task. Exceptions are ignored.
        * \param SchedulingPolicy A task container which determines how tasks are scheduled. It is guaranteed that this container is accessed only by one thread at a time. The scheduler shall not throw exceptions.
        *
        * \remarks The pool class is thread-safe.
        *
        * \see Tasks: task_functor, priority_task_functor
        * \see Scheduling policies: fifo_scheduler, lifo_scheduler, priority_scheduler
        */
        template<
                template<template<typename> class Task = task_functor, typename TaskReturnType = void> class SchedulingPolicy = fifo_scheduler,
                template<typename> class SizePolicy = static_size,
                template<typename> class SizePolicyController = resize_controller,
                template<typename> class ShutdownPolicy = wait_for_all_tasks,
                template<typename> class Task = task_functor
        >
        class thread_pool {
            using pool_core_type = detail::pool_core<
                    SchedulingPolicy,
                    SizePolicy,
                    SizePolicyController,
                    ShutdownPolicy,
                    Task>;
            shared_ptr<pool_core_type> m_core; // pimpl idiom
            shared_ptr<void> m_shutdown_controller; // If the last pool holding a pointer to the core is deleted the controller shuts the pool down.

        public: // Type definitions
            template<typename TaskReturnType>
            using task_type = Task<TaskReturnType>; //!< Indicates the task's type.

            using scheduler_type = SchedulingPolicy<Task>;       //!< Indicates the scheduler's type.
            /*   typedef thread_pool<Task,
                                   SchedulingPolicy,
                                   SizePolicy,
                                   ShutdownPolicy > pool_type;          //!< Indicates the thread pool's type.
            */
            typedef SizePolicy<pool_core_type> size_policy_type;
            typedef SizePolicyController<pool_core_type> size_controller_type;


        public:
            /*! Constructor.
             * \param initial_threads The pool is immediately resized to set the specified number of threads. The pool's actual number threads depends on the SizePolicy.
             */
            thread_pool(size_t initial_threads = 0)
                    : m_core(new pool_core_type),
                      m_shutdown_controller(static_cast<void *>(0), bind(&pool_core_type::shutdown, m_core)) {
                size_policy_type::init(*m_core, initial_threads);
            }


            /*! Gets the size controller which manages the number of threads in the pool.
            * \return The size controller.
            * \see SizePolicy
            */
            size_controller_type size_controller() {
                return m_core->size_controller();
            }

            /*! Gets the number of threads in the pool.
            * \return The number of threads.
            */
            size_t size() const {
                return m_core->size();
            }

            /*! Schedules a task for asynchronous execution. The task will be executed once only.
            * \param task The task function object. It should not throw execeptions.
            * \return true, if the task could be scheduled and false otherwise.
            */

            template<typename ResultType = void>
            future<ResultType> schedule(task_type<ResultType> const &task) {
                shared_ptr<packaged_task<ResultType()>>
                        packaged(::boost::make_shared<packaged_task<ResultType()>>
                        (task.task_function));
                future<ResultType> ret(packaged->get_future());

                Task<void> modified_task = task;
                modified_task.task_function = bind<void>([](shared_ptr<
                        packaged_task<ResultType()>> const &wrapper) {
                    (*wrapper)();
                }, packaged);

                if (this->m_core->schedule(modified_task)) {
                    return ::boost::move(ret);
                } else {
                    throw std::invalid_argument("Invalid function passed to be executed");
                }
            };


            /*! Returns the number of tasks which are currently executed.
            * \return The number of active tasks.
            */
            size_t active() const {
                return m_core->active();
            }


            /*! Returns the number of tasks which are ready for execution.
            * \return The number of pending tasks.
            */
            size_t pending() const {
                return m_core->pending();
            }


            /*! Removes all pending tasks from the pool's scheduler.
            */
            void clear() {
                m_core->clear();
            }


            /*! Indicates that there are no tasks pending.
            * \return true if there are no tasks ready for execution.
            * \remarks This function is more efficient that the check 'pending() == 0'.
            */
            bool empty() const {
                return m_core->empty();
            }


            /*! The current thread of execution is blocked until the sum of all active
            *  and pending tasks is equal or less than a given threshold.
            * \param task_threshold The maximum number of tasks in pool and scheduler.
            */
            void wait(size_t task_threshold = 0) const {
                m_core->wait(task_threshold);
            }


            /*! The current thread of execution is blocked until the timestamp is met
            * or the sum of all active and pending tasks is equal or less
            * than a given threshold.
            * \param timestamp The time when function returns at the latest.
            * \param task_threshold The maximum number of tasks in pool and scheduler.
            * \return true if the task sum is equal or less than the threshold, false otherwise.
            */
            bool wait(xtime const &timestamp, size_t task_threshold = 0) const {
                return m_core->wait(timestamp, task_threshold);
            }
        };


        /*! \brief Fifo pool.
        *
        * The pool's tasks are fifo scheduled task_functor functors.
        *
        */

        using fifo_pool = thread_pool<fifo_scheduler, static_size, resize_controller, wait_for_all_tasks, task_functor>;

        /*! \brief Lifo pool.
        *
        * The pool's tasks are lifo scheduled task_functor functors.
        *
        */

        using lifo_pool = thread_pool<lifo_scheduler, static_size, resize_controller, wait_for_all_tasks, task_functor>;


        /*! \brief Pool for prioritized task.
        *
        * The pool's tasks are prioritized priority_task_functor functors.
        *
        */

        using priority_pool = thread_pool<priority_scheduler, static_size, resize_controller, wait_for_all_tasks, priority_task_functor>;


        /*! \brief A standard pool.
        *
        * The pool's tasks are fifo scheduled task_functor functors.
        *
        */
        using pool = fifo_pool;


    }
} // namespace boost::threadpool

#endif // THREADPOOL_POOL_HPP_INCLUDED
