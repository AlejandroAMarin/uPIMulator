package logic

import (
	"errors"
	"fmt"
	"uPIMulator/src/misc"
)

type ThreadScheduler struct {
	channel_id int
	rank_id    int
	dpu_id     int

	num_revolver_scheduling_cycles int64

	// policy picks the thread that issues when more than one may: 0 goes
	// round the threads by number, as upstream uPIMulator does; 1 is the
	// machine's order, see ScheduleOldest. A thread is ready once the
	// revolver lets it issue again, or from the cycle something made it
	// runnable, whichever is later. runnable_since holds the second, cycle
	// counts the scheduler's cycles, and woken marks a thread that a boot or
	// a resume made runnable and that has not issued since.
	//
	// With policy 1 a booted thread issues boot_delay cycles after the boot,
	// and a resumed one resume_delay cycles after the resume. On the machine
	// both are 13: tasklet k boots k+1 with its third instruction, and k+1
	// starts 35 cycles after k did.
	policy         int64
	boot_delay     int64
	resume_delay   int64
	cycle          int64
	runnable_since []int64
	woken          []bool

	threads  []*Thread
	thread_q *ThreadQ

	stat_factory *misc.StatFactory
}

func (this *ThreadScheduler) Init(
	channel_id int,
	rank_id int,
	dpu_id int,
	threads []*Thread,
	command_line_parser *misc.CommandLineParser,
) {
	if channel_id < 0 {
		err := errors.New("channel ID < 0")
		panic(err)
	} else if rank_id < 0 {
		err := errors.New("rank ID < 0")
		panic(err)
	} else if dpu_id < 0 {
		err := errors.New("DPU ID < 0")
		panic(err)
	}

	this.channel_id = channel_id
	this.rank_id = rank_id
	this.dpu_id = dpu_id

	this.num_revolver_scheduling_cycles = command_line_parser.IntParameter(
		"num_revolver_scheduling_cycles",
	)

	this.policy = command_line_parser.IntParameter("thread_policy")
	this.boot_delay = command_line_parser.IntParameter("boot_delay_cycles")
	this.resume_delay = command_line_parser.IntParameter("resume_delay_cycles")
	this.cycle = 0
	this.runnable_since = make([]int64, len(threads))
	this.woken = make([]bool, len(threads))

	this.threads = threads

	this.thread_q = new(ThreadQ)
	this.thread_q.Init(len(this.threads), 0)
	for _, thread := range threads {
		this.thread_q.Push(thread)
	}

	name := fmt.Sprintf("ThreadScheduler[%d_%d_%d]", channel_id, rank_id, dpu_id)
	this.stat_factory = new(misc.StatFactory)
	this.stat_factory.Init(name)
}

func (this *ThreadScheduler) Fini() {
	for this.thread_q.CanPop(1) {
		this.thread_q.Pop()
	}

	this.thread_q.Fini()
}

func (this *ThreadScheduler) StatFactory() *misc.StatFactory {
	return this.stat_factory
}

func (this *ThreadScheduler) NumIssuableThreads() int {
	num_issuable_threads := 0

	for _, thread := range this.threads {
		if thread.ThreadState() == RUNNABLE {
			num_issuable_threads++
		}
	}

	return num_issuable_threads
}

func (this *ThreadScheduler) Schedule() *Thread {
	if this.policy == 1 {
		return this.ScheduleOldest()
	}

	var is_blocked bool
	is_blocked = false
	for i := 0; i < this.thread_q.Size(); i++ {
		thread := this.thread_q.Pop()
		this.thread_q.Push(thread)

		if thread.IssueCycle() >= this.num_revolver_scheduling_cycles {
			if thread.ThreadState() == RUNNABLE {
				thread.ResetIssueCycle()

				this.stat_factory.Increment("breakdown_run", 1)

				return thread
			} else if thread.ThreadState() == BLOCK {
				is_blocked = true
			}
		}
	}

	if is_blocked {
		this.stat_factory.Increment("breakdown_dma", 1)
	} else {
		this.stat_factory.Increment("breakdown_etc", 1)
	}

	return nil
}

// ScheduleOldest issues the thread that has been ready the longest, with one
// exception: a woken thread goes ahead of every thread waiting for its
// revolver turn. So tasklets that a barrier resumes one after another pack
// into the rotation in reverse order, and a tasklet booted into a full
// rotation takes the next slot. Ties go to the lowest number.
//
// The order decides who gets a contended lock: the bit goes to the first
// thread that issues its acquire after the release. With it the simulator
// grants every lock in runner/tests/perf/grant_kernel.c and ring_kernel.c in
// the machine's order, from 3 to 16 tasklets, and PrIM va and hst execute
// the machine's instruction counts exactly. sched_sweep.sh checks the order.
func (this *ThreadScheduler) ScheduleOldest() *Thread {
	var chosen *Thread
	var chosen_woken bool
	var chosen_since int64
	is_blocked := false

	for _, thread := range this.threads {
		if thread.IssueCycle() < this.num_revolver_scheduling_cycles {
			continue
		}
		if thread.ThreadState() == BLOCK {
			is_blocked = true
		}
		if thread.ThreadState() != RUNNABLE ||
			this.runnable_since[thread.ThreadId()] > this.cycle {
			continue
		}

		// A thread is woken until it issues for the first time after it was
		// made runnable.
		since := this.cycle - thread.IssueCycle() + this.num_revolver_scheduling_cycles
		if this.runnable_since[thread.ThreadId()] > since {
			since = this.runnable_since[thread.ThreadId()]
		}
		woken := this.woken[thread.ThreadId()]

		better := chosen == nil ||
			(woken && !chosen_woken) ||
			(woken == chosen_woken && since < chosen_since)
		if better {
			chosen = thread
			chosen_woken = woken
			chosen_since = since
		}
	}

	if chosen != nil {
		chosen.ResetIssueCycle()
		this.woken[chosen.ThreadId()] = false
		this.stat_factory.Increment("breakdown_run", 1)
		return chosen
	}

	if is_blocked {
		this.stat_factory.Increment("breakdown_dma", 1)
	} else {
		this.stat_factory.Increment("breakdown_etc", 1)
	}
	return nil
}

func (this *ThreadScheduler) Boot(thread_id int) bool {
	thread := this.threads[thread_id]

	if thread.ThreadId() != thread_id {
		err := errors.New("thread's thread ID != thread ID")
		panic(err)
	}

	thread_state := thread.ThreadState()
	if thread_state == EMBRYO {
		thread.SetThreadState(RUNNABLE)
		this.runnable_since[thread_id] = this.cycle + this.boot_delay
		this.woken[thread_id] = true
		return true
	} else if thread_state == ZOMBIE {
		thread.SetThreadState(RUNNABLE)
		this.runnable_since[thread_id] = this.cycle + this.boot_delay
		this.woken[thread_id] = true
		return true
	} else {
		err := errors.New("thread is not bootable")
		panic(err)
	}
}

func (this *ThreadScheduler) Sleep(thread_id int) bool {
	thread := this.threads[thread_id]

	if thread.ThreadId() != thread_id {
		err := errors.New("thread's thread ID != thread ID")
		panic(err)
	}

	thread_state := thread.ThreadState()
	if thread_state == RUNNABLE {
		thread.SetThreadState(SLEEP)
		return true
	} else {
		err := errors.New("thread is not sleepable")
		panic(err)
	}
}

func (this *ThreadScheduler) Block(thread_id int) bool {
	thread := this.threads[thread_id]

	if thread.ThreadId() != thread_id {
		err := errors.New("thread's thread ID != thread ID")
		panic(err)
	}

	thread_state := thread.ThreadState()
	if thread_state == RUNNABLE {
		thread.SetThreadState(BLOCK)
		return true
	} else {
		err := errors.New("thread is not blockable")
		panic(err)
	}
}

func (this *ThreadScheduler) Awake(thread_id int) bool {
	thread := this.threads[thread_id]

	if thread.ThreadId() != thread_id {
		err := errors.New("thread's thread ID != thread ID")
		panic(err)
	}

	thread_state := thread.ThreadState()
	if thread_state == EMBRYO {
		thread.SetThreadState(RUNNABLE)
		this.runnable_since[thread_id] = this.cycle + this.boot_delay
		this.woken[thread_id] = true
		return true
	} else if thread_state == SLEEP {
		thread.SetThreadState(RUNNABLE)
		this.runnable_since[thread_id] = this.cycle + this.resume_delay
		this.woken[thread_id] = true
		return true
	} else if thread_state == BLOCK {
		thread.SetThreadState(RUNNABLE)
		// A transfer that completes does not make its thread woken: in
		// PrIM va and hst the two choices give the same cycle counts.
		this.runnable_since[thread_id] = this.cycle
		return true
	} else {
		err := errors.New("thread is not awakable")
		panic(err)
	}
}

func (this *ThreadScheduler) Shutdown(thread_id int) bool {
	thread := this.threads[thread_id]

	if thread.ThreadId() != thread_id {
		err := errors.New("thread's thread ID != thread ID")
		panic(err)
	}

	thread_state := thread.ThreadState()
	if thread_state == SLEEP {
		thread.SetThreadState(ZOMBIE)
		return true
	} else {
		err := errors.New("thread is not shotdownable")
		panic(err)
	}
}

func (this *ThreadScheduler) Cycle() {
	this.cycle++
}
