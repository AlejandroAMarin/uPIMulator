package logic

import (
	"errors"
	"fmt"
	"uPIMulator/src/linker/kernel/instruction"
	"uPIMulator/src/misc"
)

// REG_BANK_PORTS is how many registers a single bank of the register file
// serves in a cycle: the ones an instruction reads, together with the one the
// thread's previous instruction writes.
const REG_BANK_PORTS = 2

type CycleRule struct {
	channel_id int
	rank_id    int
	dpu_id     int

	input_q *InstructionQ
	wait_q  *InstructionQ
	ready_q *InstructionQ

	scoreboard map[*instruction.Instruction]*Thread
	reg_sets   []*RegSet

	// The instruction each thread issued last. Its destination occupies a
	// bank port while the thread's next instruction reads its sources.
	prev_instructions []*instruction.Instruction

	stat_factory *misc.StatFactory
}

func (this *CycleRule) Init(
	channel_id int,
	rank_id int,
	dpu_id int,
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

	num_tasklets := int(command_line_parser.IntParameter("num_tasklets"))

	this.input_q = new(InstructionQ)
	this.input_q.Init(1, 0)

	this.wait_q = new(InstructionQ)
	this.wait_q.Init(1, 0)

	this.ready_q = new(InstructionQ)
	this.ready_q.Init(1, 0)

	this.scoreboard = make(map[*instruction.Instruction]*Thread, 0)

	for i := 0; i < num_tasklets; i++ {
		reg_set := new(RegSet)
		reg_set.Init(i)

		this.reg_sets = append(this.reg_sets, reg_set)
	}
	this.prev_instructions = make([]*instruction.Instruction, num_tasklets)

	name := fmt.Sprintf("CycleRule[%d_%d_%d]", channel_id, rank_id, dpu_id)
	this.stat_factory = new(misc.StatFactory)
	this.stat_factory.Init(name)
}

func (this *CycleRule) Fini() {
	this.input_q.Fini()
	this.wait_q.Fini()
	this.ready_q.Fini()
}

func (this *CycleRule) StatFactory() *misc.StatFactory {
	return this.stat_factory
}

func (this *CycleRule) IsEmpty() bool {
	return this.input_q.IsEmpty() && this.wait_q.IsEmpty() && this.ready_q.IsEmpty()
}

func (this *CycleRule) CanPush() bool {
	return this.input_q.CanPush(1)
}

func (this *CycleRule) Push(instruction_ *instruction.Instruction, thread *Thread) {
	if !this.CanPush() {
		err := errors.New("cycle rule cannot be pushed")
		panic(err)
	}

	this.input_q.Push(instruction_)
	this.scoreboard[instruction_] = thread
}

func (this *CycleRule) CanPop() bool {
	return this.ready_q.CanPop(1)
}

func (this *CycleRule) Pop() *instruction.Instruction {
	if !this.CanPop() {
		err := errors.New("cycle rule cannot be popped")
		panic(err)
	}

	instruction_ := this.ready_q.Pop()

	delete(this.scoreboard, instruction_)
	return instruction_
}

func (this *CycleRule) Cycle() {
	this.ServiceInputQ()
	this.ServiceWaitQ()

	this.input_q.Cycle()
	this.wait_q.Cycle()
	this.ready_q.Cycle()
}

func (this *CycleRule) ServiceInputQ() {
	if this.input_q.CanPop(1) && this.wait_q.CanPush(1) {
		instruction_ := this.input_q.Pop()

		// The bank conflict itself is charged where the machine charges it,
		// against the thread's next issue slot, in Logic.ServiceThreadScheduler.
		// This stage is only the register read and write-back, one per cycle.
		this.wait_q.PushWithTimer(instruction_, 0)
	}
}

func (this *CycleRule) ServiceWaitQ() {
	if this.wait_q.CanPop(1) && this.ready_q.CanPush(1) {
		instruction_ := this.wait_q.Pop()
		this.ready_q.Push(instruction_)
	}
}

// ConflictTurns is how many extra issue slots one instruction loses to the
// register file, and it must be called once for each instruction a thread
// issues, in order. The file has an even bank and an odd bank, and each serves
// at most two registers in a cycle. Those two ports are shared between the
// registers this instruction reads and the register the thread's previous
// instruction writes. This instruction's own destination does not count. So
// three distinct registers of one parity, counted that way, need a second slot.
//
// Measured on this machine, at one tasklet where a lost slot is 11 unmistakable
// cycles, with eight copies of the instruction and the loop's own
// `add r0, r0, -1, nz` in an iteration (runner/tests/perf/regbank_kernel.c):
//
//	add r4, r6, r8  reads 2 even after an even write  20.80, all 8 lose a slot
//	add r5, r7, r9  reads 2 odd after an odd write    19.58, 7 of 8: the first
//	                                                   follows r0, which is even
//	add r5, r4, r6  reads 2 even after an odd write   12.24, 1 of 8: the first
//	add r6, r7, r9  reads 2 odd after an even write   11.02, none
//	add r4, r5, r6  one of each                        11.02, none
//
// and in runner/tests/perf/pipe_kernel.c, `add r8, r8, r10` loses a slot after
// `add r4, r4, 1` and none after itself, because r8 is then one register.
//
// uPIMulator used to count this instruction's destination with its sources.
// That gets the first three right and the next two wrong, and it misses every
// case where the previous instruction's write is the third register -- the
// loop's own counter update in front of a two-source instruction is the usual
// one. Before that it charged floor(even/2) + floor(odd/2), which fired on
// pairs the machine serves for free, and held the whole DPU's pipeline rather
// than the thread.
func (this *CycleRule) ConflictTurns(
	instruction_ *instruction.Instruction,
	thread_id int,
) int64 {
	reg_set := this.reg_sets[thread_id]

	if reg_set.ThreadId() != thread_id {
		err := errors.New("reg set's thread ID != thread ID")
		panic(err)
	}

	reg_set.Clear()
	if prev := this.prev_instructions[thread_id]; prev != nil {
		reg_set.CollectWriteGpRegs(prev)
	}
	reg_set.CollectReadGpRegs(instruction_)
	this.prev_instructions[thread_id] = instruction_

	even_counter := 0
	odd_counter := 0
	for reg_index := range reg_set.RegIndices() {
		if reg_index%2 == 0 {
			even_counter++
		} else {
			odd_counter++
		}
	}

	turns := int64(0)
	if even_counter > REG_BANK_PORTS {
		turns += int64(even_counter - REG_BANK_PORTS)
	}
	if odd_counter > REG_BANK_PORTS {
		turns += int64(odd_counter - REG_BANK_PORTS)
	}

	return turns
}
