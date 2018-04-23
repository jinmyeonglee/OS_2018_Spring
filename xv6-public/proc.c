#include "types.h"
#include "defs.h"
#include "param.h"
#include "memlayout.h"
#include "mmu.h"
#include "x86.h"
#include "proc.h"
#include "spinlock.h"

struct {
  struct spinlock lock;
  struct proc proc[NPROC];
} ptable;

static struct proc *initproc;

int nextpid = 1;

int l_ticks[3] = {1, 2, 4};
int allot[3] = {5, 10, 0};
int mlfq_ticks = 0;
int total_portion = 0;
struct proc * picked = 0;
int mlfq_pass = 0;
int mlfq_stride = 100;

extern void forkret(void);
extern void trapret(void);

static void wakeup1(void *chan);

void
pinit(void)
{
  initlock(&ptable.lock, "ptable");
}

// Must be called with interrupts disabled
int
cpuid() {
  return mycpu()-cpus;
}

// Must be called with interrupts disabled to avoid the caller being
// rescheduled between reading lapicid and running through the loop.
struct cpu*
mycpu(void)
{
  int apicid, i;

  if(readeflags()&FL_IF)
    panic("mycpu called with interrupts enabled\n");

  apicid = lapicid();
  // APIC IDs are not guaranteed to be contiguous. Maybe we should have
  // a reverse map, or reserve a register to store &cpus[i].
  for (i = 0; i < ncpu; ++i) {
    if (cpus[i].apicid == apicid)
      return &cpus[i];
  }
  panic("unknown apicid\n");
}

// Disable interrupts so that we are not rescheduled
// while reading proc from the cpu structure
struct proc*
myproc(void) {
  struct cpu *c;
  struct proc *p;
  pushcli();
  c = mycpu();
  p = c->proc;
  popcli();
  return p;
}

//PAGEBREAK: 32
// Look in the process table for an UNUSED proc.
// If found, change state to EMBRYO and initialize
// state required to run in the kernel.
// Otherwise return 0.
static struct proc*
allocproc(void)
{
  struct proc *p;
  char *sp;

  acquire(&ptable.lock);

  for(p = ptable.proc; p < &ptable.proc[NPROC]; p++)
    if(p->state == UNUSED)
      goto found;

  release(&ptable.lock);
  return 0;

found:
  p->state = EMBRYO;
  p->pid = nextpid++;

	p->q_level = 0; // init the q_level to the first level
	p->tick = 0;    // init the tick to be zero
  p->allot = 0;   // init the allot to be zero
	p->stride = 0;  // init the stride to be zero
	p->pass = 0;    // init the pass to be zero
  p->portion = 0;

  release(&ptable.lock);

  // Allocate kernel stack.
  if((p->kstack = kalloc()) == 0){
    p->state = UNUSED;
    return 0;
  }
  sp = p->kstack + KSTACKSIZE;

  // Leave room for trap frame.
  sp -= sizeof *p->tf;
  p->tf = (struct trapframe*)sp;

  // Set up new context to start executing at forkret,
  // which returns to trapret.
  sp -= 4;
  *(uint*)sp = (uint)trapret;

  sp -= sizeof *p->context;
  p->context = (struct context*)sp;
  memset(p->context, 0, sizeof *p->context);
  p->context->eip = (uint)forkret;

  return p;
}

//PAGEBREAK: 32
// Set up first user process.
void
userinit(void)
{
  struct proc *p;
  extern char _binary_initcode_start[], _binary_initcode_size[];

  p = allocproc();

  initproc = p;
  if((p->pgdir = setupkvm()) == 0)
    panic("userinit: out of memory?");
  inituvm(p->pgdir, _binary_initcode_start, (int)_binary_initcode_size);
  p->sz = PGSIZE;
  memset(p->tf, 0, sizeof(*p->tf));
  p->tf->cs = (SEG_UCODE << 3) | DPL_USER;
  p->tf->ds = (SEG_UDATA << 3) | DPL_USER;
  p->tf->es = p->tf->ds;
  p->tf->ss = p->tf->ds;
  p->tf->eflags = FL_IF;
  p->tf->esp = PGSIZE;
  p->tf->eip = 0;  // beginning of initcode.S

  safestrcpy(p->name, "initcode", sizeof(p->name));
  p->cwd = namei("/");

  // this assignment to p->state lets other cores
  // run this process. the acquire forces the above
  // writes to be visible, and the lock is also needed
  // because the assignment might not be atomic.
  acquire(&ptable.lock);

  p->state = RUNNABLE;

  release(&ptable.lock);
}

// Grow current process's memory by n bytes.
// Return 0 on success, -1 on failure.
int
growproc(int n)
{
  uint sz;
  struct proc *curproc = myproc();

  sz = curproc->sz;
  if(n > 0){
    if((sz = allocuvm(curproc->pgdir, sz, sz + n)) == 0)
      return -1;
  } else if(n < 0){
    if((sz = deallocuvm(curproc->pgdir, sz, sz + n)) == 0)
      return -1;
  }
  curproc->sz = sz;
  switchuvm(curproc);
  return 0;
}

// Create a new process copying p as the parent.
// Sets up stack to return as if from system call.
// Caller must set state of returned proc to RUNNABLE.
int
fork(void)
{
  int i, pid;
  struct proc *np;
  struct proc *curproc = myproc();

  // Allocate process.
  if((np = allocproc()) == 0){
    return -1;
  }

  // Copy process state from proc.
  if((np->pgdir = copyuvm(curproc->pgdir, curproc->sz)) == 0){
    kfree(np->kstack);
    np->kstack = 0;
    np->state = UNUSED;
    return -1;
  }
  np->sz = curproc->sz;
  np->parent = curproc;
  *np->tf = *curproc->tf;

  // Clear %eax so that fork returns 0 in the child.
  np->tf->eax = 0;

  for(i = 0; i < NOFILE; i++)
    if(curproc->ofile[i])
      np->ofile[i] = filedup(curproc->ofile[i]);
  np->cwd = idup(curproc->cwd);

  safestrcpy(np->name, curproc->name, sizeof(curproc->name));

  pid = np->pid;

  acquire(&ptable.lock);

  np->state = RUNNABLE;

  release(&ptable.lock);

  return pid;
}

// Exit the current process.  Does not return.
// An exited process remains in the zombie state
// until its parent calls wait() to find out it exited.
void
exit(void)
{
  struct proc *curproc = myproc();
  struct proc *p;
  int fd;

  if(curproc == initproc)
    panic("init exiting");

  // Close all open files.
  for(fd = 0; fd < NOFILE; fd++){
    if(curproc->ofile[fd]){
      fileclose(curproc->ofile[fd]);
      curproc->ofile[fd] = 0;
    }
  }

  begin_op();
  iput(curproc->cwd);
  end_op();
  curproc->cwd = 0;

  acquire(&ptable.lock);

  // Parent might be sleeping in wait().
  wakeup1(curproc->parent);

  // Pass abandoned children to init.
  for(p = ptable.proc; p < &ptable.proc[NPROC]; p++){
    if(p->parent == curproc){
      p->parent = initproc;
      if(p->state == ZOMBIE)
        wakeup1(initproc);
    }
  }

  // Jump into the scheduler, never to return.
  curproc->state = ZOMBIE;
  sched();
  //yield();
  panic("zombie exit");
}

// Wait for a child process to exit and return its pid.
// Return -1 if this process has no children.
int
wait(void)
{
  struct proc *p;
  int havekids, pid;
  struct proc *curproc = myproc();

  acquire(&ptable.lock);
  for(;;){
    // Scan through table looking for exited children.
    havekids = 0;
    for(p = ptable.proc; p < &ptable.proc[NPROC]; p++){
      if(p->parent != curproc)
        continue;
      havekids = 1;
      if(p->state == ZOMBIE){
        // Found one.
        pid = p->pid;
        kfree(p->kstack);
        p->kstack = 0;
        freevm(p->pgdir);
        p->pid = 0;
        p->parent = 0;
        p->name[0] = 0;
        p->killed = 0;
        p->state = UNUSED;
        release(&ptable.lock);
        return pid;
      }
    }

    // No point waiting if we don't have any children.
    if(!havekids || curproc->killed){
      release(&ptable.lock);
      return -1;
    }

    // Wait for children to exit.  (See wakeup1 call in proc_exit.)
    sleep(curproc, &ptable.lock);
    //DOC: wait-sleep
  }
}

void
boost()
{
  struct proc *p;
  //cprintf("[do boosting!]\n");
  for(p = ptable.proc; p < &ptable.proc[NPROC]; p++) {
    p->tick = 0;
    p->q_level = 0;
    p->allot = 0;
  }
  mlfq_ticks = 0;
}

//PAGEBREAK: 42
// Per-CPU process scheduler.
// Each CPU calls scheduler() after setting itself up.
// Scheduler never returns.  It loops, doing:
//  - choose a process to run
//  - swtch to start running that process
//  - eventually that process transfers control
//      via swtch back to the scheduler.

// In Project 2, the scheduler is edited.
// The new scheduler is combinded one with MLFQ and stride algorithm.
void
scheduler(void)
{
  struct proc *p, *q;
  struct cpu *c = mycpu();
	int check_proc = 3; // check the min level of the mlfq proc
  int mlfq_runnable = 0; // check the existency of mlfq proc
  int min_pass = 1000000000; // pick min_pass among stride procs and mlfq

  c->proc = 0;

  for(;;){
    // Enable interrupts on this processor.
    sti();

    // Loop over process table looking for process to run.
    acquire(&ptable.lock);

    //initialize some needed variables
    mlfq_runnable = 0;
    check_proc = 3;
    picked = 0;
    min_pass = 1000000000;


    for(p = ptable.proc; p < &ptable.proc[NPROC]; p++){

      total_portion = 0; // initilize to check current total_portion

      for(q = ptable.proc; q < &ptable.proc[NPROC]; q++) {
        if(q->q_level != 2 && allot[q->q_level] <= q->allot) {
          // increase the queue level to 1 or 2 level
          q->q_level++;
          q->allot = 0; //q->tick = 0
        }

        if(q->portion == 0 && q->state == RUNNABLE) { // if proc is mlfq
          mlfq_runnable++; // count mlfq proc
          if(q->q_level < check_proc) { // pick min_level in mlfq procs
            check_proc = q->q_level;
          }
        }

        if(q->portion != 0 && q->state == RUNNABLE) { // calculate total_portion
          total_portion += q->portion;
        }
      }

      // setting mlfq stride
      mlfq_stride = STRIDE_LARGE_NUM /(100 - total_portion);

      for(q = ptable.proc; q < &ptable.proc[NPROC]; q++) {
        if(q->state == RUNNABLE && q->portion != 0 && min_pass > q->pass) {
          // pick min_pass, and proc which has min_pass
          min_pass = q->pass;
          picked = q;
        }
      }

      if(mlfq_runnable != 0 && total_portion != 0) {
        // there are mlfq proc & stride proc
        if(min_pass > mlfq_pass) { // mlfq_pass is small, so mlfq should run
          picked = 0;
          mlfq_pass += mlfq_stride;
          min_pass = mlfq_pass;
        }
        else if(total_portion == 0) { // there is only mlfq
          picked = 0;
          mlfq_pass = 0;
        }
        // else if (mlfq_runnable == 0 && total_portion != 0) { // only stride
        // }
      }

      if(mlfq_ticks >= 100) { // should boost up!
        boost();
      }

      if(p->state != RUNNABLE) { // non-runnable proc should be passed
        continue;
      }
      else if(p->q_level > check_proc) {
        // the lower level than min_level should be passed
        continue;
      }
      if(picked != 0 && picked->state == RUNNABLE){ // should run the picked one
        p = picked;
        picked->pass += picked->stride;
        picked = 0;
      }
      else if (p->portion != 0) { // when mlfq should run, pass stride one
        continue;
      }
      if(p->portion == 0) { // if the mlfq proc, incresse proc tick
        p->tick++;
        mlfq_ticks++;
        if(l_ticks[p->q_level] <= p->tick) {
          p->allot += p->tick;
          p->tick = 0;
        }
        if(p->q_level != 2 && allot[p->q_level] <= p->allot) {
          // increase the proc level
          p->q_level++;
          p->allot = 0;
        }
      }

      // Switch to chosen process.  It is the process's job
      // to release ptable.lock and then reacquire it
      // before jumping back to us.
      //cprintf("    /exe pid: %d, state: %d, level: %d,tik: %d, allot %d, portion: %d\n", p->pid, p->state, p->q_level, p->tick, p->allot, p->portion);
      //cprintf("min: %d\n", min_pass);
      c->proc = p;
      switchuvm(p);
      p->state = RUNNING;
      swtch(&(c->scheduler), p->context);
      switchkvm();

      check_proc = 3; // init the check_proc

      // Process is done running for now.
      // It should have changed its p->state before coming back.
      c->proc = 0;
    } // ptable
    release(&ptable.lock);

  }
}
/**
  * this function is used to allocate the requied portion, fills another field.
  * @param[in]  portion  requied portion. this value should be between 0 and 80.
  * @return     If the portion is alllocated well, return the pid.
                If not return -1.
*/
int set_cpu_share(int portion) { //
  struct proc *p;
  int min_pass;

  if(portion == 0) {
    return myproc()->pid;
  }

  acquire(&ptable.lock);

  if(total_portion == 0) {
    min_pass = mlfq_pass;
  }
  else {
    total_portion = 0;
    min_pass = 1000000000;
  }

  for(p = ptable.proc; p < &ptable.proc[NPROC]; p++) {
    if((p->state == RUNNABLE || p->state == RUNNING) && p->portion != 0) {
      total_portion += p->portion;
      if(min_pass > p->pass) { // pick the min_pass among stride processes.
        min_pass = p->pass;
      }
    }
  }
  if(min_pass > mlfq_pass) { // if mlfq_pass is the min.
    min_pass = mlfq_pass;
  }

  total_portion += portion;

  release(&ptable.lock);

  if(total_portion > 80) { // the total_portion should not be over 80%.
    cprintf("ERROR: This process cannot share CPU: over 80 percent.\n");
    return -1;
  }

  myproc()->stride = STRIDE_LARGE_NUM / portion;
  myproc()->pass = min_pass;
  myproc()->portion = portion;

  return myproc()->pid;
}

// Enter scheduler.  Must hold only ptable.lock
// and have changed proc->state. Saves and restores
// intena because intena is a property of this
// kernel thread, not this CPU. It should
// be proc->intena and proc->ncli, but that would
// break in the few places where a lock is held but
// there's no process.
void
sched(void)
{
  int intena;
  struct proc *p = myproc();

  if(!holding(&ptable.lock))
    panic("sched ptable.lock");
  if(mycpu()->ncli != 1)
    panic("sched locks");
  if(p->state == RUNNING)
    panic("sched running");
  if(readeflags()&FL_IF)
    panic("sched interruptible");
  intena = mycpu()->intena;
  swtch(&p->context, mycpu()->scheduler);
  mycpu()->intena = intena;
}

// Give up the CPU for one scheduling round.
void
yield(void)
{
  acquire(&ptable.lock);  //DOC: yieldlock
  //cprintf("lock: %d\n", ptable.lock.locked);
  myproc()->state = RUNNABLE;
  //show_procs();
  sched();
  release(&ptable.lock);
}

// wrapper funcion
int
sys_yield(void)
{
	yield();
	return 0;
}

// A fork child's very first scheduling by scheduler()
// will swtch here.  "Return" to user space.
void
forkret(void)
{
  static int first = 1;
  // Still holding ptable.lock from scheduler.
  release(&ptable.lock);

  if (first) {
    // Some initialization functions must be run in the context
    // of a regular process (e.g., they call sleep), and thus cannot
    // be run from main().
    first = 0;
    iinit(ROOTDEV);
    initlog(ROOTDEV);
  }

  // Return to "caller", actually trapret (see allocproc).
}

// Atomically release lock and sleep on chan.
// Reacquires lock when awakened.
void
sleep(void *chan, struct spinlock *lk)
{
  struct proc *p = myproc();

  if(p == 0)
    panic("sleep");

  if(lk == 0)
    panic("sleep without lk");

  // Must acquire ptable.lock in order to
  // change p->state and then call sched.
  // Once we hold ptable.lock, we can be
  // guaranteed that we won't miss any wakeup
  // (wakeup runs with ptable.lock locked),
  // so it's okay to release lk.
  if(lk != &ptable.lock){  //DOC: sleeplock0
    acquire(&ptable.lock);  //DOC: sleeplock1
    release(lk);
  }
  // Go to sleep.
  p->chan = chan;
  p->state = SLEEPING;

  //yield();
  sched();

  // Tidy up.
  p->chan = 0;

  // Reacquire original lock.
  if(lk != &ptable.lock){  //DOC: sleeplock2
    release(&ptable.lock);
    acquire(lk);
  }
}

//PAGEBREAK!
// Wake up all processes sleeping on chan.
// The ptable lock must be held.
static void
wakeup1(void *chan)
{
  struct proc *p;

  for(p = ptable.proc; p < &ptable.proc[NPROC]; p++)
    if(p->state == SLEEPING && p->chan == chan)
      p->state = RUNNABLE;
}

// Wake up all processes sleeping on chan.
void
wakeup(void *chan)
{
  acquire(&ptable.lock);
  wakeup1(chan);
  release(&ptable.lock);
}

// Kill the process with the given pid.
// Process won't exit until it returns
// to user space (see trap in trap.c).
int
kill(int pid)
{
  struct proc *p;

  acquire(&ptable.lock);
  for(p = ptable.proc; p < &ptable.proc[NPROC]; p++){
    if(p->pid == pid){
      p->killed = 1;
      // Wake process from sleep if necessary.
      if(p->state == SLEEPING)
        p->state = RUNNABLE;
      release(&ptable.lock);
      return 0;
    }
  }
  release(&ptable.lock);
  return -1;
}

//PAGEBREAK: 36
// Print a process listing to console.  For debugging.
// Runs when user types ^P on console.
// No lock to avoid wedging a stuck machine further.
void
procdump(void)
{
  static char *states[] = {
  [UNUSED]    "unused",
  [EMBRYO]    "embryo",
  [SLEEPING]  "sleep ",
  [RUNNABLE]  "runble",
  [RUNNING]   "run   ",
  [ZOMBIE]    "zombie"
  };
  int i;
  struct proc *p;
  char *state;
  uint pc[10];

  for(p = ptable.proc; p < &ptable.proc[NPROC]; p++){
    if(p->state == UNUSED)
      continue;
    if(p->state >= 0 && p->state < NELEM(states) && states[p->state])
      state = states[p->state];
    else
      state = "???";
    cprintf("    %d %s %s", p->pid, state, p->name);
    if(p->state == SLEEPING){
      getcallerpcs((uint*)p->context->ebp+2, pc);
      for(i=0; i<10 && pc[i] != 0; i++)
        cprintf(" %p", pc[i]);
    }
    cprintf("\n");
  }
}
