#include <errno.h>

#ifdef __APPLE__
#include <sys/semaphore.h>
#else
#include <semaphore.h>
#endif

#include <sched.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdbool.h>
#include <string.h>
#include <stdint.h>
#include <stdarg.h>

#include <sys/types.h>
#include <sys/time.h>
#include <sys/wait.h>
#include <time.h>

#include <unistd.h>
#include <ucontext.h>

/* The ucontext stack size */

#define STACK_SIZE                        (128 * 1024)

/* The stack alignment */

#define STACK_ALIGNMENT_BYTES             (8)

/* Number of simulated tasks */

#define SCHEDULING_TASKS_NUMBER           (3)

/* The preemption tick interval in miliseconds */

#define SCHEDULING_TIME_SLICE_MS          (1)

/* The number of physical CPUs - POSIX threads that consume these tasks */

#define SCHEDULING_HOST_CPUS              (1)

/* Max recursive deth */

#define TASK_RANDOM_RECURSIVE_MAX_DEPTH   (4000)

/* Helper macro for errors logging */

#define _err(fmt, ...)                    \
  fprintf(stderr, "[ERROR] "fmt, __VA_ARGS__)

/****************************************************************************
 * Private Data Types
 ****************************************************************************/

/* This is a possible task state */

typedef enum state_e {
  TASK_INVALID_STATE = 0,   /* The default state before a task is started */
  TASK_START,               /* Indicate that is ready to start */
  TASK_RESUME,              /* Resume when it's preempted from sig handler */
  TASK_RUNNING,             /* When it's the active task after preemption */
  TASK_NUM_STATES
} state_t;

/* Task control block */

typedef struct tcb_s {
  ucontext_t context;
  uint32_t t_pid;
  state_t task_state;
  uint8_t stack[STACK_SIZE];
} tcb_t;

/****************************************************************************
 * Private Data
 ****************************************************************************/

static pid_t g_parent_pid;

/* Flag to inform if scheduler needs to stop due to CTRL-C */

static volatile bool is_scheduler_active;

/* Task control block array */

static tcb_t g_context_array[SCHEDULING_TASKS_NUMBER];

/* The active task index */

static volatile uint8_t g_active_task;

/* The console semaphore - we don't want to have scrambled output or end up
 * with a bad stdout stream.
 */

static sem_t g_console_sema;

/* The main context is the point where we return when we are done with the 
 * simulation.
 */

static ucontext_t g_main_context;

/****************************************************************************
 * Private Functions
 ****************************************************************************/

/* Get the next task to run
 */
static int get_next_task_to_run()
{
  return rand() % SCHEDULING_TASKS_NUMBER;
}

/* A safe version of the printf to call from handler
 */
static void print_me(char *fmt, ...)
{
  va_list arg_list;

  va_start(arg_list, fmt);
  vprintf(fmt, arg_list);
  va_end(arg_list);
}

/* The registered task counter starts from 1 as the first one is the IDLE task
 */
static volatile uint8_t g_registered_task_num = 1;

/* Create a new task and specify the entry point
 */
static int task_create(uint32_t stack_size, void (*entry_point)(void),
    uint32_t pid)
{
  uint8_t *aligned_stack_addr;

  ucontext_t *task_context = &g_context_array[pid].context;
  int ret = getcontext(task_context);
  if (ret < 0) {
    _err("%d get current context\n", ret);
  }

  /* Allocate a new stack for this task and align the address */

  uint8_t *stack_ptr = g_context_array[pid].stack;

  aligned_stack_addr = stack_ptr +
    (STACK_ALIGNMENT_BYTES - ((uint64_t)stack_ptr) % STACK_ALIGNMENT_BYTES);

  g_context_array[pid].t_pid     = pid;
  task_context->uc_stack.ss_sp   = aligned_stack_addr;
  task_context->uc_stack.ss_size = stack_size -
    (STACK_ALIGNMENT_BYTES - ((uint64_t)stack_ptr) % STACK_ALIGNMENT_BYTES);
  task_context->uc_stack.ss_flags = 0;
  task_context->uc_link           = pid == 0 ? &g_main_context :
    &g_context_array[pid].context;

  makecontext(task_context, (void *)entry_point, 0);

  g_context_array[pid].task_state = TASK_START;

  printf("[MAIN] Created task num: %d\n", pid);
  g_registered_task_num++;

  return 0;
}

/* Verify is a pointer is on the stack of a task received as argument
 */
static bool is_sp_in_current_task(struct tcb_s *task, uintptr_t sp)
{
  return (uintptr_t)&task->stack[0] <= sp &&
         sp < (uintptr_t)&task->stack[STACK_SIZE];
}

/* Parent signal handler that does the context switch
 */
static void parent_signal_handler(int sig, siginfo_t *si, void *old_ucontext)
{
  int old_active_task = g_active_task;

  /* Check for exit request */
  if (sig == SIGINT) {
    is_scheduler_active = false;
  }

  /* Save the context in the global context array list */

  g_context_array[g_active_task].task_state = TASK_RESUME;

  if (is_sp_in_current_task(&g_context_array[g_active_task], (uintptr_t)&old_active_task))
    {
      printf("[Handler] is on the stack for task %d\n", g_active_task);
    }
  else
    {
      printf("[Handler] is NOT on the stack for task %d\n", g_active_task);
      printf("SP: %p task_top_stack: %p task_bottom_stack: %p\n",
             &old_active_task,
             &g_context_array[g_active_task].stack[STACK_SIZE],
             &g_context_array[g_active_task].stack[0]);
    }

  do {
    /* Look at the next task to execute */

    g_active_task = (g_active_task + get_next_task_to_run()) % SCHEDULING_TASKS_NUMBER;
  } while (g_context_array[g_active_task].task_state == TASK_INVALID_STATE);

  printf("[Handler] Next task to run : %d state %d\n", g_active_task,
         g_context_array[g_active_task].task_state);

  g_context_array[g_active_task].task_state = TASK_RUNNING;

  ucontext_t *uc = (ucontext_t *)old_ucontext;
  swapcontext(&g_context_array[old_active_task].context,
              &g_context_array[g_active_task].context);
}

/* Parent signal handler that does the context switch
 */

/* Set up SIGALRM signal handler
 */
static int setup_signals(void (*action)(int, siginfo_t *, void *))
{
  struct sigaction act;
  int ret;
  sigset_t set;

  act.sa_sigaction = action;
  sigemptyset(&act.sa_mask);
  act.sa_flags = SA_SIGINFO;

  sigemptyset(&set);
  sigaddset(&set, SIGALRM);

  if (action == parent_signal_handler) {
    sigaddset(&set, SIGTERM);
    sigaddset(&set, SIGINT);
    sigaddset(&set, SIGQUIT);

    if ((ret = sigaction(SIGINT, &act, NULL)) != 0) {
        _err("%d signal handler", ret);
    }
  }

  if ((ret = sigaction(SIGALRM, &act, NULL)) != 0) {
      _err("%d signal handler", ret);
  }

  return ret;
}

/* Set up a timer to send periodic signals
 *
 */
static int setup_timer(void)
{
  int ret;
  struct itimerval it;

  it.it_interval.tv_sec  = 0;
  it.it_interval.tv_usec = SCHEDULING_TIME_SLICE_MS * 1000;
  it.it_value            = it.it_interval;

  ret = setitimer(ITIMER_REAL, &it, NULL);
  if (ret < 0) {
    _err("%d settimer\n", ret);
    return ret;
  }

  return ret;
}

static int recursive_task(int recursive_depth)
{
  if (recursive_depth > 0)
    return recursive_task(recursive_depth - 1);
  return recursive_depth;
}

static void task(void)
{
  uint32_t index = 0;
  unsigned int p;
  for (;;) {
    int r_rec =  rand_r(&p) % TASK_RANDOM_RECURSIVE_MAX_DEPTH;
    recursive_task(r_rec);
    sched_yield();
  }
}

static void task_idle(void)
{
  printf("[IDLE] start runing\n");

  setup_timer();

  while (is_scheduler_active) {
    ;;
  }

  /* Cancel timer */

  struct itimerval it;
  memset(&it, 0, sizeof(struct itimerval));
  setitimer(ITIMER_REAL, &it, NULL);
}

/* Initialize the task context
 *
 */
static void setup_scheduler(void)
{
  is_scheduler_active = true;
 
  task_create(STACK_SIZE, task_idle, 0);

  for (int i = 1; i < SCHEDULING_TASKS_NUMBER; i++)
    task_create(STACK_SIZE, task, i);

  sem_init(&g_console_sema, 0, 1);
}

/*
 */
int main(int argc, char **argv)
{
  int wstatus, ret;

  /* Seed the randome number generator */

  srand(time(NULL));

  setup_scheduler();
  setup_signals(parent_signal_handler);

  /* Switch context to idle task and start running */

  if (getcontext(&g_main_context) == 0)
    setcontext(&g_context_array[0].context);
  
  printf("[MAIN] Simulation stopped\n");
  return 0;
}
