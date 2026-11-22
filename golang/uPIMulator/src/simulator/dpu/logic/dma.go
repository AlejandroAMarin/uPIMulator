package logic

import (
	"errors"
	"uPIMulator/src/abi/encoding"
	"uPIMulator/src/linker/kernel/instruction"
	"uPIMulator/src/misc"
	"uPIMulator/src/simulator/dpu/dram"
	"uPIMulator/src/simulator/dpu/sram"
)

type Dma struct {
	atomic            *sram.Atomic
	iram              *sram.Iram
	operand_collector *OperandCollector
	memory_controller *dram.MemoryController

	input_q *dram.DmaCommandQ
	ready_q *dram.DmaCommandQ

	startup    int64
	burst_size int64

	// tail is how long the engine stays busy after it hands a finished
	// transfer back, plus tail_per_kb for each KB the transfer moved; busy
	// counts it down. The machine wakes the thread before the engine is done
	// with a transfer: with the engine idle a thread sees about 0.50 cycles a
	// byte, and with it saturated each transfer costs about 0.545. The tail
	// is the difference, hidden when the engine is idle and paid in full when
	// it is not.
	tail        int64
	tail_per_kb int64
	busy        int64

	// A write sets up faster than a read, and its completion reaches the
	// thread write_ack cycles after the engine is done with it. The engine is
	// free during that time. On the machine a saturated 8-byte write costs
	// about 33 cycles against a read's 43, while a thread waits for one about
	// as long as for a read, less the slot a read spends when it completes.
	write_startup int64
	write_ack     int64
	// read_ack is the same for a read: its completion reaches the thread
	// read_ack cycles after the engine is done with it.
	read_ack int64

	// turnaround is what the engine pays when a transfer goes the other way
	// from the one before it, a read after a write or a write after a read.
	// A stream of reads alone or of writes alone never pays it. last_op is
	// the direction of the transfer handed to the controller last.
	turnaround  int64
	last_op     dram.MemoryOperation
	has_last_op bool
	switch_paid bool

	// policy picks the next transfer when the engine is free to start one:
	// 0 serves them in the order they came, 1 serves the lowest-numbered
	// thread first. front_started is set once the chosen transfer's setup
	// has begun; a started setup is never preempted.
	policy        int64
	front_started bool
}

func (this *Dma) Init(command_line_parser *misc.CommandLineParser) {
	this.atomic = nil
	this.iram = nil
	this.operand_collector = nil
	this.memory_controller = nil

	config_loader := new(misc.ConfigLoader)
	config_loader.Init()

	max_num_tasklets := config_loader.MaxNumTasklets()

	// The DMA engine's own startup, in logic cycles, before a transfer reaches
	// the memory controller. The machine charges it once for every row the
	// transfer touches, whatever the row holds, and it does not overlap the
	// transfer before it. runner/tests/perf/scatter_kernel.c measures it.
	this.startup = command_line_parser.IntParameter("dma_startup_cycles")
	this.burst_size = command_line_parser.IntParameter("dma_burst_size")
	this.tail = command_line_parser.IntParameter("dma_tail_cycles")
	this.tail_per_kb = command_line_parser.IntParameter("dma_tail_cycles_per_kb")
	this.write_startup = command_line_parser.IntParameter("dma_write_startup_cycles")
	this.write_ack = command_line_parser.IntParameter("dma_write_ack_cycles")
	this.read_ack = command_line_parser.IntParameter("dma_read_ack_cycles")
	this.turnaround = command_line_parser.IntParameter("dma_turnaround_cycles")
	this.has_last_op = false
	this.switch_paid = false
	this.policy = command_line_parser.IntParameter("dma_policy")
	this.front_started = false
	this.busy = 0

	this.input_q = new(dram.DmaCommandQ)
	this.input_q.Init(max_num_tasklets, this.startup)

	this.ready_q = new(dram.DmaCommandQ)
	this.ready_q.Init(max_num_tasklets, 0)
}

func (this *Dma) Fini() {
	this.input_q.Fini()
	this.ready_q.Fini()
}

func (this *Dma) ConnectAtomic(atomic *sram.Atomic) {
	if this.atomic != nil {
		err := errors.New("atomic is already set")
		panic(err)
	}

	this.atomic = atomic
}

func (this *Dma) ConnectIram(iram *sram.Iram) {
	if this.iram != nil {
		err := errors.New("IRAM is already set")
		panic(err)
	}

	this.iram = iram
}

func (this *Dma) ConnectOperandCollector(operand_collector *OperandCollector) {
	if this.operand_collector != nil {
		err := errors.New("operand collector is already set")
		panic(err)
	}

	this.operand_collector = operand_collector
}

func (this *Dma) ConnectMemoryController(memory_controller *dram.MemoryController) {
	if this.memory_controller != nil {
		err := errors.New("memory controller is already set")
		panic(err)
	}

	this.memory_controller = memory_controller
}

func (this *Dma) IsEmpty() bool {
	return this.input_q.IsEmpty() && this.ready_q.IsEmpty()
}

func (this *Dma) TransferToAtomic(address int64, byte_stream *encoding.ByteStream) {
	for i := int64(0); i < byte_stream.Size(); i++ {
		if byte_stream.Get(int(i)) != 0 {
			err := errors.New("atomic byte is not set to 0")
			panic(err)
		}
	}
}

func (this *Dma) TransferToIram(address int64, byte_stream *encoding.ByteStream) {
	config_loader := new(misc.ConfigLoader)
	config_loader.Init()

	iram_offset := config_loader.IramOffset()
	iram_data_size := int64(config_loader.IramDataWidth() / 8)

	if address != this.iram.Address() {
		err := errors.New("address != IRAM's address")
		panic(err)
	} else if byte_stream.Size()%iram_data_size != 0 {
		err := errors.New("byte stream's size is not aligned with IRAM data size")
		panic(err)
	}

	this.iram.Write(iram_offset, byte_stream)
}

func (this *Dma) TransferFromWram(address int64, size int64) *encoding.ByteStream {
	byte_stream := new(encoding.ByteStream)
	byte_stream.Init()

	for i := int64(0); i < size; i++ {
		value := this.operand_collector.Lbu(address + i)
		byte_stream.Append(uint8(value))
	}

	return byte_stream
}

func (this *Dma) TransferToWram(address int64, byte_stream *encoding.ByteStream) {
	for i := int64(0); i < byte_stream.Size(); i++ {
		value := byte_stream.Get(int(i))
		this.operand_collector.Sb(address+i, int64(value))
	}
}

func (this *Dma) TransferFromMram(address int64, size int64) *encoding.ByteStream {
	this.memory_controller.Flush()
	return this.memory_controller.Read(address, size)
}

func (this *Dma) TransferToMram(address int64, byte_stream *encoding.ByteStream) {
	this.memory_controller.Write(address, byte_stream.Size(), byte_stream)
}

func (this *Dma) TransferFromWramToMram(
	wram_address int64,
	mram_address int64,
	size int64,
	instruction_ *instruction.Instruction,
	thread_id int,
) {
	if !this.CanPush() {
		err := errors.New("DMA cannot be pushed")
		panic(err)
	}

	byte_stream := this.TransferFromWram(wram_address, size)

	dma_command := new(dram.DmaCommand)
	dma_command.InitWriteToMramFromWram(wram_address, mram_address, size, byte_stream, instruction_)
	dma_command.SetThreadId(thread_id)

	this.Push(dma_command)
}

func (this *Dma) TransferFromMramToWram(
	wram_address int64,
	mram_address int64,
	size int64,
	instruction_ *instruction.Instruction,
	thread_id int,
) {
	if !this.CanPush() {
		err := errors.New("DMA cannot be pushed")
		panic(err)
	}

	dma_command := new(dram.DmaCommand)
	dma_command.InitReadFromMramToWram(wram_address, mram_address, size, instruction_)
	dma_command.SetThreadId(thread_id)

	this.Push(dma_command)
}

func (this *Dma) CanPush() bool {
	return this.input_q.CanPush(1)
}

func (this *Dma) Push(dma_command *dram.DmaCommand) {
	if !this.CanPush() {
		err := errors.New("DMA cannot be pushed")
		panic(err)
	}

	// The engine sets up once for each burst_size-aligned row the transfer
	// touches, not once per burst_size bytes: a 1 KB read at a random address
	// usually spans two rows and costs the setup twice, and a 2 KB one spans
	// three. Measured by runner/tests/perf/scatter_kernel.c.
	first := dma_command.MramAddress() / this.burst_size
	last := (dma_command.MramAddress() + dma_command.Size() - 1) / this.burst_size
	bursts := last - first + 1
	if bursts < 1 {
		bursts = 1
	}

	startup := this.startup
	if dma_command.MemoryOperation() == dram.WRITE {
		startup = this.write_startup
	}

	this.input_q.PushWithTimer(dma_command, startup*bursts)
}

func (this *Dma) CanPop() bool {
	return this.ready_q.CanPop(1)
}

func (this *Dma) Pop() *dram.DmaCommand {
	if !this.CanPop() {
		err := errors.New("DMA cannot be popped")
		panic(err)
	}

	return this.ready_q.Pop()
}

func (this *Dma) Cycle() {
	this.ServiceInputQ()
	this.ServiceReadyQ()

	// One engine serves all 24 threads and does one thing at a time: either it
	// sets a transfer up, or it moves that transfer's bytes. The two do not
	// overlap, which is why the machine's cost per transfer is the sum of the
	// two terms and not the larger of them. So the startup only counts down
	// while the memory controller is idle, and only the transfer at the front
	// counts at all. After a transfer the engine stays busy for its tail
	// before the next setup can start.
	if this.busy > 0 {
		this.busy--
	} else if this.memory_controller.IsEmpty() {
		if !this.front_started && this.policy == 1 && !this.input_q.IsEmpty() {
			this.input_q.PromoteLowestThread()
		}
		if this.NeedsTurnaround() {
			this.busy = this.turnaround
			this.switch_paid = true
			this.front_started = true
		} else {
			if !this.input_q.IsEmpty() {
				this.front_started = true
			}
			this.input_q.Cycle()
		}
	}

	this.ready_q.CycleAll()
}

func (this *Dma) ServiceInputQ() {
	if this.input_q.CanPop(1) && this.busy == 0 && !this.NeedsTurnaround() &&
		this.memory_controller.IsEmpty() && this.memory_controller.CanPush() {
		dma_command := this.input_q.Pop()
		this.memory_controller.Push(dma_command)

		this.last_op = dma_command.MemoryOperation()
		this.has_last_op = true
		this.switch_paid = false
		this.front_started = false
	}
}

// NeedsTurnaround says whether the transfer at the front goes the other way
// from the last one and has not paid for that yet.
func (this *Dma) NeedsTurnaround() bool {
	if this.turnaround <= 0 || !this.has_last_op || this.switch_paid || this.input_q.IsEmpty() {
		return false
	}

	dma_command, _ := this.input_q.Front(0)
	return dma_command.MemoryOperation() != this.last_op
}

func (this *Dma) ServiceReadyQ() {
	if this.memory_controller.CanPop() && this.ready_q.CanPush(1) {
		dma_command := this.memory_controller.Pop()
		if dma_command.MemoryOperation() == dram.WRITE {
			this.ready_q.PushWithTimer(dma_command, this.write_ack)
		} else {
			this.ready_q.PushWithTimer(dma_command, this.read_ack)
		}
		this.busy = this.tail + (dma_command.Size()*this.tail_per_kb+512)/1024

		if dma_command.MemoryOperation() == dram.READ {
			wram_address := dma_command.WramAddress()
			mram_address := dma_command.MramAddress()
			size := dma_command.Size()
			byte_stream := dma_command.ByteStream(mram_address, size)

			this.TransferToWram(wram_address, byte_stream)
		}
	}
}
