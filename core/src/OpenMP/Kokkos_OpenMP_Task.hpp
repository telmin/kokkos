/*
//@HEADER
// ************************************************************************
//
//                        Kokkos v. 2.0
//              Copyright (2014) Sandia Corporation
//
// Under the terms of Contract DE-AC04-94AL85000 with Sandia Corporation,
// the U.S. Government retains certain rights in this software.
//
// Redistribution and use in source and binary forms, with or without
// modification, are permitted provided that the following conditions are
// met:
//
// 1. Redistributions of source code must retain the above copyright
// notice, this list of conditions and the following disclaimer.
//
// 2. Redistributions in binary form must reproduce the above copyright
// notice, this list of conditions and the following disclaimer in the
// documentation and/or other materials provided with the distribution.
//
// 3. Neither the name of the Corporation nor the names of the
// contributors may be used to endorse or promote products derived from
// this software without specific prior written permission.
//
// THIS SOFTWARE IS PROVIDED BY SANDIA CORPORATION "AS IS" AND ANY
// EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
// IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR
// PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL SANDIA CORPORATION OR THE
// CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL,
// EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO,
// PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR
// PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF
// LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING
// NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS
// SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
//
// Questions? Contact Christian R. Trott (crtrott@sandia.gov)
//
// ************************************************************************
//@HEADER
*/

#ifndef KOKKOS_IMPL_OPENMP_TASK_HPP
#define KOKKOS_IMPL_OPENMP_TASK_HPP

#include <Kokkos_Macros.hpp>
#if defined( KOKKOS_ENABLE_OPENMP ) && defined( KOKKOS_ENABLE_TASKDAG )

#include <Kokkos_TaskScheduler_fwd.hpp>

#include <impl/Kokkos_HostThreadTeam.hpp>
#include <Kokkos_OpenMP.hpp>

#include <type_traits>
#include <cassert>

//----------------------------------------------------------------------------
//----------------------------------------------------------------------------

namespace Kokkos {
namespace Impl {

class HostThreadTeamDataSingleton : private HostThreadTeamData {
private:

  HostThreadTeamDataSingleton();
  ~HostThreadTeamDataSingleton();

public:

  static HostThreadTeamData & singleton();

};


template<class Scheduler>
class TaskQueueSpecializationConstrained<
  Scheduler,
  typename std::enable_if<
    std::is_same<typename Scheduler::execution_space, Kokkos::OpenMP>::value
  >::type
>
{
public:

  using execution_space = Kokkos::OpenMP;
  using scheduler_type = Scheduler;
  using member_type = TaskTeamMemberAdapter<
    Kokkos::Impl::HostThreadTeamMember<execution_space>,
    scheduler_type
  >;
  using memory_space = Kokkos::HostSpace ;

  enum : int { max_league_size = HostThreadTeamData::max_pool_members };

  static
  void iff_single_thread_recursive_execute( scheduler_type const& scheduler ) {
    using task_base_type = typename scheduler_type::task_base;
    using queue_type = typename scheduler_type::queue_type;

#ifdef KOKKOS_ENABLE_DEPRECATED_CODE
    if ( 1 == OpenMP::thread_pool_size() )
#else
    if ( 1 == OpenMP::impl_thread_pool_size() )
#endif
    {

      task_base_type * const end = (task_base_type *) task_base_type::EndTag ;

      HostThreadTeamData & team_data_single =
        HostThreadTeamDataSingleton::singleton();

      member_type single_exec( scheduler, team_data_single );

      task_base_type * task = end ;

      do {

        task = end ;

        // Loop by priority and then type
        for ( int i = 0 ; i < queue_type::NumQueue && end == task ; ++i ) {
          for ( int j = 0 ; j < 2 && end == task ; ++j ) {
            task = queue_type::pop_ready_task( & scheduler.m_queue->m_ready[i][j] );
          }
        }

        if ( end == task ) break ;

        (*task->m_apply)( task , & single_exec );

        scheduler.m_queue->complete( task );

      } while(1);
    }

  }

  // Must provide task queue execution function
  static void execute(scheduler_type const& scheduler)
  {
    using task_base_type = typename scheduler_type::task_base;
    using queue_type = typename scheduler_type::queue_type;

    static task_base_type * const end =
      (task_base_type *) task_base_type::EndTag ;

    constexpr task_base_type* no_more_tasks_sentinel = nullptr;


    HostThreadTeamData & team_data_single =
      HostThreadTeamDataSingleton::singleton();

    Impl::OpenMPExec * instance = t_openmp_instance;
#ifdef KOKKOS_ENABLE_DEPRECATED_CODE
    const int pool_size = OpenMP::thread_pool_size();
#else
    const int pool_size = OpenMP::impl_thread_pool_size();
#endif

    const int team_size = 1;  // Threads per core
    instance->resize_thread_data( 0 /* global reduce buffer */
      , 512 * team_size /* team reduce buffer */
      , 0 /* team shared buffer */
      , 0 /* thread local buffer */
    );
    assert(pool_size % team_size == 0);
    auto& queue = scheduler.queue();
    queue.initialize_team_queues(pool_size / team_size);

#pragma omp parallel num_threads(pool_size)
    {
      Impl::HostThreadTeamData & self = *(instance->get_thread_data());

      // Organizing threads into a team performs a barrier across the
      // entire pool to insure proper initialization of the team
      // rendezvous mechanism before a team rendezvous can be performed.

      // organize_team() returns true if this is an active team member
      if ( self.organize_team( team_size ) ) {

        member_type single_exec(scheduler, team_data_single);
        member_type team_exec(scheduler, self);

        auto& team_queue = team_exec.scheduler().queue();

        // Loop until all queues are empty and no tasks in flight

        task_base_type * task = no_more_tasks_sentinel;


        do {
          // Each team lead attempts to acquire either a thread team task
          // or a single thread task for the team.

          if ( 0 == team_exec.team_rank() ) {

            bool leader_loop = false ;

            do {

              if ( task != no_more_tasks_sentinel && task != end ) {
                // team member #0 completes the previously executed task,
                // completion may delete the task
                team_queue.complete( task );
              }

              // If 0 == m_ready_count then set task = 0

              // TODO shouldn't this be just an atomic load acquire?  This isn't potentially device code...
              if( *((volatile int *) & team_queue.m_ready_count) > 0 ) {
                task = end;
                // Attempt to acquire a task
                // Loop by priority and then type
                for ( int i = 0 ; i < queue_type::NumQueue && end == task ; ++i ) {
                  for ( int j = 0 ; j < 2 && end == task ; ++j ) {
                    task = queue_type::pop_ready_task( & team_queue.m_ready[i][j] );
                  }
                }
              }
              else {
                // returns nullptr if and only if all other queues have a ready
                // count of 0 also. Otherwise, returns a task from another queue
                // or `end` if one couldn't be popped
                task = team_queue.attempt_to_steal_task();
                if(task != no_more_tasks_sentinel && task != end) {
                  std::printf("task stolen on rank %d\n", team_exec.league_rank());
                }
              }

              // If still tasks are still executing
              // and no task could be acquired
              // then continue this leader loop
              if(task == end) {
                // this means that the ready task count was not zero, but we
                // couldn't pop a task (because, for instance, someone else
                // got there before us
                leader_loop = true;
              }
              else if ( ( task != no_more_tasks_sentinel ) &&
                ( task_base_type::TaskSingle == task->m_task_type ) ) {

                // if a single thread task then execute now

                (*task->m_apply)(task, &single_exec);

                leader_loop = true;
              }
              else {
                leader_loop = false;
              }
            } while ( leader_loop );
          }

          // Team lead either found 0 == m_ready_count or a team task
          // Team lead broadcast acquired task:

          team_exec.team_broadcast( task , 0);

          if ( task != no_more_tasks_sentinel ) { // Thread Team Task

            (*task->m_apply)( task , & team_exec );

            // The m_apply function performs a barrier
          }
        } while( task != no_more_tasks_sentinel );
      }
      self.disband_team();
    } // end pragma omp parallel
  }

  template< typename TaskType >
  static
  typename TaskType::function_type
  get_function_pointer() { return TaskType::apply ; }
};

extern template class TaskQueue< Kokkos::OpenMP > ;

}} /* namespace Kokkos::Impl */

//----------------------------------------------------------------------------
//----------------------------------------------------------------------------

#endif /* #if defined( KOKKOS_ENABLE_TASKDAG ) */
#endif /* #ifndef KOKKOS_IMPL_OPENMP_TASK_HPP */

