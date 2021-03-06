/*
 * Threads.cpp - Library for threading on the Teensy.
 * Created by Fernando Trias, January 2017.
 * Copyright 2017 by Fernando Trias. All rights reserved.
*/
#include "TeensyThreads.h"
#include <Arduino.h>

#include <IntervalTimer.h>
IntervalTimer context_timer;

Threads threads;

#define __flush_cpu() __asm__ volatile("DMB");

// These variables are used by the assembly context_switch() function.
// They are copies or pointers to data in Threads and ThreadInfo
// and put here seperately in order to simplify the code.
extern "C" {
  int currentUseSystick;
  int currentActive;
  int currentCount;
  ThreadInfo *currentThread;
  void *currentSave;
  int currentMSP;
  void *currentSP;
  void loadNextThread() {
    threads.getNextThread();
  }
}

Threads::Threads() : current_thread(0), thread_count(0), thread_error(0) {
  // initialize context_switch() globals from thread 0, which is MSP and always running
  currentThread = thread;        // thread 0 is active
  currentSave = &thread[0].save;
  currentMSP = 1;
  currentSP = 0;
  currentCount = Threads::DEFAULT_TICKS;
  currentActive = FIRST_RUN;
  thread[0].flags = RUNNING;
  thread[0].ticks = DEFAULT_TICKS;
  currentUseSystick = 1;
}

/*
 * start() - Begin threading
 */
int Threads::start(int prev_state) {
  __disable_irq();
  int old_state = currentActive;
  if (prev_state == -1) prev_state = STARTED;
  currentActive = prev_state;
  __enable_irq();
  return old_state;
}

/*
 * stop() - Stop threading, even if active.
 *
 * If threads have already started, this should be called sparingly
 * because it could destabalize the system if thread 0 is stopped.
 */
int Threads::stop() {
  __disable_irq();
  int old_state = currentActive;
  currentActive = STOPPED;
  __enable_irq();
  return old_state;
}

/*
 * getNextThread() - Find next running thread
 *
 * This will also set the context_switcher() state variables
 */
void Threads::getNextThread() {
  // First, save the currentSP set by context_switch
  thread[current_thread].sp = currentSP;

  // Find any priority threads
  int priority_thread = -1;
  for(int i=0; i < MAX_THREADS; i++) {
    if (thread[current_thread].flags == RUNNING) {
      if (thread[i].priority) {
        current_thread = i;
        priority_thread = i;
        currentCount = thread[i].ticks; // .priority
        thread[i].priority = 0;
        break;
      }
    }
  }

  // If no priority threads, find next active one
  if (priority_thread == -1) {
    // Find the next running thread
    while(1) {
      current_thread++;
      if (current_thread >= MAX_THREADS) {
        current_thread = 0; // thread 0 is MSP; always active so return
        break;
      }
      if (thread[current_thread].flags == RUNNING) break;
    }
    currentCount = thread[current_thread].ticks;
  }

  currentThread = &thread[current_thread];
  currentSave = &thread[current_thread].save;
  currentMSP = (current_thread==0?1:0);
  currentSP = thread[current_thread].sp;
}

/*
 * Empty placeholder for IntervalTimer class
 */
static void context_pit_empty() {}

/*
 * Store the PIT timer flag register for use in assembly
 */
volatile uint32_t *context_timer_flag;

/*
 * Defined in assembly code
 */
extern "C" void context_switch_pit_isr();

/*
 * Stop using the SysTick interrupt and start using
 * the IntervalTimer timer. The parameter is the number of microseconds
 * for each tick.
 *
 * Implementation strategy suggested by @tni in Teensy Forums; see
 * https://forum.pjrc.com/threads/41504-Teensy-3-x-multithreading-library-first-release
 */
int Threads::setMicroTimer(int tick_microseconds)
{
  // lowest priority so we don't interrupt other interrupts
  context_timer.priority(255);
  // start timer with dummy fuction
  if (context_timer.begin(context_pit_empty, tick_microseconds) == 0) {
    // failed to set the timer!
    return 0;
  }
  currentUseSystick = 0; // disable Systick calls
  // get the PIT number [0-3] (IntervalTimer overrides IRQ_NUMBER_t op)
  int number = (IRQ_NUMBER_t)context_timer - IRQ_PIT_CH0;
  // calculate number of uint32_t per PIT; should be 4.
  // Not hard-coded in case this changes in future CPUs.
  const int width = (PIT_TFLG1 - PIT_TFLG0) / 4;
  // get the right flag to ackowledge PIT interrupt
  context_timer_flag = &PIT_TFLG0 + (width * number);
  attachInterruptVector(context_timer, context_switch_pit_isr);
  return 1;
}

/*
 * Set each time slice to be 'microseconds' long
 */
int Threads::setSliceMicros(int microseconds)
{
  setMicroTimer(microseconds);
  setDefaultTimeSlice(1);
  return 1;
}

/*
 * Set each time slice to be 'milliseconds' long
 */
int Threads::setSliceMillis(int milliseconds)
{
  if (currentUseSystick) {
    setDefaultTimeSlice(milliseconds);
  }
  else {
    // if we're using the PIT, we should probably really disable it and
    // re-establish the systick timer; but this is easier for now
    setSliceMicros(milliseconds * 1000);
  }
  return 1;
}

/*
 * Replace the SysTick interrupt for our context switching. Note that
 * this function is "naked" meaning it does not save it's registers
 * on the stack. This is so we can preserve the stack of the caller.
 *
 * Interrupts will save r0-r4 in the stack and since this function
 * is short and simple, it should only use those registers. In the
 * future, this should be coded in assembly to make sure.
 */
extern volatile uint32_t systick_millis_count;
void __attribute((naked, noinline)) systick_isr(void)
{
  systick_millis_count++;
  if (currentUseSystick) {
    // we branch in order to preserve LR and the stack
    __asm volatile("b context_switch");
  }
  __asm volatile("bx lr");
}

void __attribute((naked, noinline)) svcall_isr(void)
{
  // Get the right stack so we can extract the PC (next instruction)
  // and then see the SVC calling instruction number
  __asm volatile("TST lr, #4 \n"
                 "ITE EQ \n"
                 "MRSEQ r0, msp \n"
                 "MRSNE r0, psp \n");
  register unsigned int *rsp __asm("r0");
  unsigned int svc = ((uint8_t*)rsp[6])[-2];
  if (svc == Threads::SVC_NUMBER) {
    __asm volatile("b context_switch_direct");
  }
  else if (svc == Threads::SVC_NUMBER_ACTIVE) {
    currentActive = Threads::STARTED;
    __asm volatile("b context_switch_direct_active");
  }
  __asm volatile("bx lr");
}

/*
 * del_process() - This is called when the task returns
 *
 * Turns thread off. Thread continues running until next call to
 * context_switch() at which point it all stops. The while(1) statement
 * just stalls until such time.
 */
void Threads::del_process(void)
{
  int old_state = threads.stop();
  ThreadInfo *me = &threads.thread[threads.current_thread];
  // Would love to delete stack here but the thread doesn't
  // end now. It continues until the next tick.
  // if (me->my_stack) {
  //   delete[] me->stack;
  //   me->stack = 0;
  // }
  threads.thread_count--;
  me->flags = ENDED; //clear the flags so thread can stop and be reused
  threads.start(old_state);
  while(1); // just in case, keep working until context change when execution will not return to this thread
}

/*
 * Initializes a thread's stack. Called when thread is created
 */
void *Threads::loadstack(ThreadFunction p, void * arg, void *stackaddr, int stack_size)
{
  interrupt_stack_t * process_frame = (interrupt_stack_t *)((uint8_t*)stackaddr + stack_size - sizeof(interrupt_stack_t) - 8);
  process_frame->r0 = (uint32_t)arg;
  process_frame->r1 = 0;
  process_frame->r2 = 0;
  process_frame->r3 = 0;
  process_frame->r12 = 0;
  process_frame->lr = (uint32_t)Threads::del_process;
  process_frame->pc = ((uint32_t)p);
  process_frame->xpsr = 0x1000000;
  uint8_t *ret = (uint8_t*)process_frame;
  // ret -= sizeof(software_stack_t); // uncomment this if we are saving R4-R11 to the stack
  return (void*)ret;
}

/*
 * Add a new thread to the queue.
 *    add_thread(fund, arg)
 *
 *    fund : is a function pointer. The function prototype is:
 *           void *func(void *param)
 *    arg  : is a void pointer that is passed as the first parameter
 *           of the function. In the example above, arg is passed
 *           as param.
 *    stack_size : the size of the buffer pointed to by stack. If
 *           it is 0, then "stack" must also be 0. If so, the function
 *           will allocate the default stack size of the heap using new().
 *    stack : pointer to new data stack of size stack_size. If this is 0,
 *           then it will allocate a stack on the heap using new() of size
 *           stack_size. If stack_size is 0, a default size will be used.
 *    return: an integer ID to be used for other calls
 */
int Threads::addThread(ThreadFunction p, void * arg, int stack_size, void *stack)
{
  int old_state = stop();
  if (stack_size == -1) stack_size = DEFAULT_STACK_SIZE;
  for (int i=1; i < MAX_THREADS; i++) {
    if (thread[i].flags == ENDED || thread[i].flags == EMPTY) { // free thread
      if (thread[i].stack && thread[i].my_stack) {
        delete[] thread[i].stack;
      }
      if (stack==0) {
        stack = new uint8_t[stack_size];
        thread[i].my_stack = 1;
      }
      else {
        thread[i].my_stack = 0;
      }
      thread[i].stack = (uint8_t*)stack;
      thread[i].stack_size = stack_size;
      void *psp = loadstack(p, arg, thread[i].stack, thread[i].stack_size);
      thread[i].sp = psp;
      thread[i].ticks = DEFAULT_TICKS;
      thread[i].flags = RUNNING;
      thread[i].save.lr = 0xFFFFFFF9;
      thread[i].priority = 0;
      currentActive = old_state;
      thread_count++;
      if (old_state == STARTED || old_state == FIRST_RUN) start();
      return i;
    }
  }
  if (old_state == STARTED) start();
  return -1;
}

int Threads::getState(int id)
{
  return thread[id].flags;
}

int Threads::setState(int id, int state)
{
  thread[id].flags = state;
  return state;
}

int Threads::wait(int id, unsigned int timeout_ms)
{
  unsigned int start = millis();
  // need to store state in temp volatile memory for optimizer.
  // "while (thread[id].flags != RUNNING)" will be optimized away
  volatile int state;
  while (1) {
    if (timeout_ms != 0 && millis() - start > timeout_ms) return -1;
    state = thread[id].flags;
    if (state != RUNNING) break;
    yield();
  }
  return id;
}

int Threads::kill(int id)
{
  thread[id].flags = ENDED;
  return id;
}

int Threads::suspend(int id)
{
  thread[id].flags = SUSPENDED;
  return id;
}

int Threads::restart(int id)
{
  thread[id].flags = RUNNING;
  return id;
}

void Threads::setTimeSlice(int id, unsigned int ticks)
{
  thread[id].ticks = ticks - 1;
}

void Threads::setDefaultTimeSlice(unsigned int ticks)
{
  DEFAULT_TICKS = ticks - 1;
}

void Threads::setPriority(int id, int level)
{
  if (id == -1) id = current_thread;
  thread[id].priority = level;
}

void Threads::setDefaultStackSize(unsigned int bytes_size)
{
  DEFAULT_STACK_SIZE = bytes_size;
}

void Threads::yield() {
  __asm volatile("svc %0" : : "i"(Threads::SVC_NUMBER));
}

void Threads::yield_and_start() {
  __asm volatile("svc %0" : : "i"(Threads::SVC_NUMBER_ACTIVE));
}

void Threads::delay(int millisecond) {
  int mx = millis();
  while((int)millis() - mx < millisecond) yield();
}

int Threads::id() {
  volatile int ret;
  __disable_irq();
  ret = current_thread;
  __enable_irq();
  return ret;
}

int Threads::getStackUsed(int id) {
  return thread[id].stack + thread[id].stack_size - (uint8_t*)thread[id].sp;
}
int Threads::getStackRemaining(int id) {
  return (uint8_t*)thread[id].sp - thread[id].stack;
}

/*
 * On creation, stop threading and save state
 */
Threads::Suspend::Suspend() {
  __disable_irq();
  save_state = currentActive;
  currentActive = 0;
  __enable_irq();
}

/*
 * On destruction, restore threading state
 */
Threads::Suspend::~Suspend() {
  __disable_irq();
  currentActive = save_state;
  __enable_irq();
}

int Threads::Mutex::getState() {
  int p = threads.stop();
  int ret = state;
  threads.start(p);
  return ret;
}

int __attribute__ ((noinline)) Threads::Mutex::lock(unsigned int timeout_ms) {
  if (try_lock()) return 1; // we're good, so avoid more checks

  uint32_t start = systick_millis_count;
  while (1) {
    if (try_lock()) return 1;
    if (timeout_ms && (systick_millis_count - start > timeout_ms)) return 0;
    if (waitthread==-1) { // can hold 1 thread suspend until unlock
      int p = threads.stop();
      waitthread = threads.current_thread;
      waitcount = currentCount;
      threads.suspend(waitthread);
      threads.start(p);
    }
    threads.yield();
  }
  __flush_cpu();
  return 0;
}

int Threads::Mutex::try_lock() {
  int p = threads.stop();
  if (state == 0) {
    state = 1;
    threads.start(p);
    return 1;
  }
  threads.start(p);
  return 0;
}

int __attribute__ ((noinline)) Threads::Mutex::unlock() {
  int p = threads.stop();
  if (state==1) {
    state = 0;
    if (waitthread >= 0) { // reanimate a suspended thread waiting for unlock
      threads.setPriority(waitthread, waitcount);
      threads.restart(waitthread);
      waitthread = -1;
      __flush_cpu();
      threads.yield_and_start();
      return 1;
    }
  }
  __flush_cpu();
  threads.start(p);
  return 1;
}
