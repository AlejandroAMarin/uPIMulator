package core

import (
	"errors"
	"sync"
	"sync/atomic"
)

// ThreadPool runs a batch of independent jobs and waits for all of them.
//
// It is created fresh for each batch, and the simulator creates two per
// simulated cycle, so the cost of starting the batch matters as much as the
// work inside it.
type ThreadPool struct {
	num_threads int

	jobs []Job
}

func (this *ThreadPool) Init(num_threads int) {
	if num_threads <= 0 {
		err := errors.New("num threads <= 0")
		panic(err)
	}

	this.num_threads = num_threads

	this.jobs = make([]Job, 0)
}

func (this *ThreadPool) Enque(job Job) {
	this.jobs = append(this.jobs, job)
}

// Start runs every queued job and returns once they have all finished.
//
// Jobs are shared out among at most num_threads workers. The previous version
// spawned one goroutine per job and so ignored num_threads entirely, which at
// full machine size meant thousands of goroutines twice per simulated cycle.
func (this *ThreadPool) Start() {
	jobs := this.jobs
	this.jobs = make([]Job, 0)

	if len(jobs) == 0 {
		return
	}

	// A single job is the common case with one DPU. Running it on the calling
	// goroutine avoids a handoff and two futex wakeups per cycle, which
	// dominated the profile of a one-DPU run.
	if len(jobs) == 1 {
		jobs[0].Execute()
		return
	}

	num_workers := this.num_threads
	if num_workers > len(jobs) {
		num_workers = len(jobs)
	}

	var next atomic.Int64
	var wait_group sync.WaitGroup

	wait_group.Add(num_workers)

	for i := 0; i < num_workers; i++ {
		go func() {
			defer wait_group.Done()

			for {
				index := int(next.Add(1)) - 1

				if index >= len(jobs) {
					return
				}

				jobs[index].Execute()
			}
		}()
	}

	wait_group.Wait()
}
