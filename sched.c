#include <errno.h>

#include <sys/semaphore.h>
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

#include <unistd.h>
#include <ucontext.h>

#define STACK_SIZE                        (128 * 1024)
#define STACK_ALIGNMENT_BYTES             (8)
#define SCHEDULING_TASKS_NUMBER           (10)
#define SCHEDULING_TIME_SLICE_MS          (1)
#define _err(fmt, ...) fprintf(stderr, "[ERROR] "fmt, __VA_ARGS__)

/****************************************************************************
 * Private Data Types
 ****************************************************************************/

typedef enum state_e {
  TASK_INVALID_STATE = 0,
  TASK_START,
  TASK_RESUME,
  TASK_RUNNING,
  TASK_NUM_STATES
} state_t;

typedef struct tcb_s {
  ucontext_t context;
  state_t task_state;
  uint8_t *unaligned_stack_ptr;
} tcb_t;

/****************************************************************************
 * Private Data
 ****************************************************************************/

static pid_t g_parent_pid;
static volatile bool is_scheduler_active;
static tcb_t g_context_array[SCHEDULING_TASKS_NUMBER];
static volatile uint8_t g_active_task;
static sem_t g_console_sema;

/****************************************************************************
 * Private Functions
 ****************************************************************************/

static void print_me(char *fmt, ...)
{
  va_list arg_list;

  sem_wait(&g_console_sema);

  va_start(arg_list, fmt);
  vprintf(fmt, arg_list);
  va_end(arg_list);

  sem_post(&g_console_sema);
}

/* The registered task counter starts from 1 as the first one is the IDLE task
 */
static volatile uint8_t g_registered_task_num = 1;

/* Create a new task and specify the entry point
 */
static int task_create(uint32_t stack_size, void (*entry_point)(void))
{
  uint8_t *aligned_stack_addr;

  ucontext_t *task_context = &g_context_array[g_registered_task_num].context;
  int ret = getcontext(task_context);
  if (ret < 0) {
    _err("%d get current context\n", ret);
    goto errout_with_ucontext;
  }

  /* Allocate a new stack for this task and align the address */

  uint8_t *stack_ptr = calloc(1, stack_size);
  if (stack_ptr == NULL) {
    _err("no memory to allocate %d bytes of stack\n", stack_size);
    ret = -ENOMEM;
    goto errout_with_ucontext;
  }

  aligned_stack_addr = stack_ptr +
    (STACK_ALIGNMENT_BYTES - ((uint64_t)stack_ptr) % STACK_ALIGNMENT_BYTES);

  task_context->uc_stack.ss_sp   = aligned_stack_addr;
  task_context->uc_stack.ss_size = stack_size -
    (STACK_ALIGNMENT_BYTES - ((uint64_t)stack_ptr) % STACK_ALIGNMENT_BYTES);
  task_context->uc_stack.ss_flags = 0;
  task_context->uc_link           = &g_context_array[g_registered_task_num - 1].context;
  makecontext(task_context, (void *)entry_point, 1);

  g_context_array[g_registered_task_num].task_state          = TASK_START;
  g_context_array[g_registered_task_num].unaligned_stack_ptr = stack_ptr;

  printf("[PARENT] Created task num: %d\n", g_registered_task_num);
  g_registered_task_num++;

  return 0;

errout_with_new_stack:
  free(stack_ptr);

errout_with_ucontext:
  return ret;
}

/* Parent signal handler that does the context switch
 */
static void parent_signal_handler(int sig, siginfo_t *si, void *old_ucontext)
{
  printf("[PARENT] Signal handler: signo %d context %p\n", sig, old_ucontext);
  int old_active_task = g_active_task;

  /* Check for exit request */
  if (sig == SIGINT) {
    is_scheduler_active = false;
  }

  /* Save the context in the global context array list */

  g_context_array[g_active_task].task_state = TASK_RESUME;

  do {
    /* Look at the next task to execute */

    g_active_task = (g_active_task + 1) % SCHEDULING_TASKS_NUMBER;
  } while (g_context_array[g_active_task].task_state == TASK_INVALID_STATE);

  //printf("[PARENT] Next task to run : %d state %d\n", g_active_task,
  //       g_context_array[g_active_task].task_state);

  g_context_array[g_active_task].task_state = TASK_RUNNING;
  swapcontext(&g_context_array[old_active_task].context, &g_context_array[g_active_task].context);
//  setcontext(&g_context_array[g_active_task].context);
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
  act.sa_flags = SA_RESTART | SA_SIGINFO;

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

void task(void)
{
  uint32_t index = 0;
  for (;;) {
    print_me("[TASK %d] Running cycle %d\n", g_active_task, index++);
  }
}

static void setup_scheduler(void)
{
  is_scheduler_active = true;

  for (int i = 1; i < SCHEDULING_TASKS_NUMBER; i++)
    task_create(STACK_SIZE, task);

  sem_init(&g_console_sema, 0, 1);
}

/*
 */
int main(int argc, char **argv)
{
  int wstatus, ret;

  setup_scheduler();
  setup_signals(parent_signal_handler);
  setup_timer();

  /* Run the idle loop */

  while (is_scheduler_active) {
    ;;
  }

  printf("[PARENT] Scheduler stopped\n");

  /* Cancel timer */

  struct itimerval it;
  memset(&it, 0, sizeof(struct itimerval));
  setitimer(ITIMER_REAL, &it, NULL);

  /* Free task resources */

  for (int i = 1; i < SCHEDULING_TASKS_NUMBER; i++)
    free(g_context_array[i].unaligned_stack_ptr);

  return 0;
}
