package dpu

import (
	"errors"
	"fmt"
	"uPIMulator/src/abi/encoding"
	"uPIMulator/src/misc"
	"uPIMulator/src/simulator/dpu/dram"
	"uPIMulator/src/simulator/dpu/logic"
	"uPIMulator/src/simulator/dpu/sram"
)

type Dpu struct {
	channel_id int
	rank_id    int
	dpu_id     int

	logic_frequency  int64
	memory_frequency int64
	frequency_ratio  float64
	cycles           int64

	// MRAM refreshes every refresh_interval logic cycles and is unavailable
	// for refresh_cycles of them. The memory controller does not advance
	// during that window; the DMA engine's own setup does. On the machine a
	// loop whose period is near a simple fraction of the interval locks to
	// it, and the disturbance it gives a transfer is what keeps threads that
	// share the engine from locking into a convoy. Zero turns it off.
	refresh_interval int64
	refresh_cycles   int64
	refresh_phase    int64

	// refresh_mode 0 refreshes on the schedule whatever is running; 1 waits
	// for the first cycle the memory controller holds no transfer, as a DRAM
	// controller issues a due refresh between two accesses. refresh_due is
	// when the next one falls due and refresh_until when the one running ends.
	refresh_mode  int64
	refresh_due   int64
	refresh_until int64

	threads           []*logic.Thread
	thread_scheduler  *logic.ThreadScheduler
	atomic            *sram.Atomic
	iram              *sram.Iram
	wram              *sram.Wram
	mram              *dram.Mram
	operand_collector *logic.OperandCollector
	memory_controller *dram.MemoryController
	dma               *logic.Dma
	logic             *logic.Logic

	stat_factory *misc.StatFactory
}

func (this *Dpu) Init(
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

	this.logic_frequency = command_line_parser.IntParameter("logic_frequency")
	this.memory_frequency = command_line_parser.IntParameter("memory_frequency")
	this.frequency_ratio = float64(this.memory_frequency) / float64(this.logic_frequency)
	this.cycles = 0
	this.refresh_interval = command_line_parser.IntParameter("mram_refresh_interval")
	this.refresh_cycles = command_line_parser.IntParameter("mram_refresh_cycles")
	this.refresh_phase = command_line_parser.IntParameter("mram_refresh_phase")
	this.refresh_mode = command_line_parser.IntParameter("mram_refresh_mode")
	this.refresh_due = this.refresh_interval - this.refresh_phase
	this.refresh_until = 0

	this.threads = make([]*logic.Thread, 0)
	num_threads := int(command_line_parser.IntParameter("num_tasklets"))
	for i := 0; i < num_threads; i++ {
		thread := new(logic.Thread)
		thread.Init(i)
		this.threads = append(this.threads, thread)
	}

	this.thread_scheduler = new(logic.ThreadScheduler)
	this.thread_scheduler.Init(channel_id, rank_id, dpu_id, this.threads, command_line_parser)

	this.atomic = new(sram.Atomic)
	this.atomic.Init()

	this.iram = new(sram.Iram)
	this.iram.Init()

	this.wram = new(sram.Wram)
	this.wram.Init()

	this.mram = new(dram.Mram)
	this.mram.Init(command_line_parser)

	this.operand_collector = new(logic.OperandCollector)
	this.operand_collector.Init()
	this.operand_collector.ConnectWram(this.wram)

	this.memory_controller = new(dram.MemoryController)
	this.memory_controller.Init(channel_id, rank_id, dpu_id, command_line_parser)
	this.memory_controller.ConnectMram(this.mram)

	this.dma = new(logic.Dma)
	this.dma.Init(command_line_parser)
	this.dma.ConnectAtomic(this.atomic)
	this.dma.ConnectIram(this.iram)
	this.dma.ConnectOperandCollector(this.operand_collector)
	this.dma.ConnectMemoryController(this.memory_controller)

	this.logic = new(logic.Logic)
	this.logic.Init(channel_id, rank_id, dpu_id, command_line_parser)
	this.logic.ConnectThreadScheduler(this.thread_scheduler)
	this.logic.ConnectAtomic(this.atomic)
	this.logic.ConnectIram(this.iram)
	this.logic.ConnectOperandCollector(this.operand_collector)
	this.logic.ConnectDma(this.dma)

	name := fmt.Sprintf("DPU%d-%d-%d", channel_id, rank_id, dpu_id)
	this.stat_factory = new(misc.StatFactory)
	this.stat_factory.Init(name)
}

func (this *Dpu) Fini() {
	for _, thread := range this.threads {
		thread.Fini()
	}

	this.atomic.Fini()
	this.iram.Fini()
	this.wram.Fini()
	this.mram.Fini()

	this.operand_collector.Fini()
	this.memory_controller.Fini()

	this.logic.Fini()
	this.dma.Fini()
}

func (this *Dpu) ChannelId() int {
	return this.channel_id
}

func (this *Dpu) RankId() int {
	return this.rank_id
}

func (this *Dpu) DpuId() int {
	return this.dpu_id
}

func (this *Dpu) ThreadScheduler() *logic.ThreadScheduler {
	return this.thread_scheduler
}

func (this *Dpu) Logic() *logic.Logic {
	return this.logic
}

func (this *Dpu) MemoryController() *dram.MemoryController {
	return this.memory_controller
}

func (this *Dpu) Dma() *logic.Dma {
	return this.dma
}

func (this *Dpu) Threads() []*logic.Thread {
	return this.threads
}

func (this *Dpu) StatFactory() *misc.StatFactory {
	return this.stat_factory
}

// Read returns size bytes starting at address, choosing WRAM or MRAM by which
// region the range falls in.
//
// The DPU must be quiescent: the MRAM path flushes the memory controller, which
// panics if requests are still in flight. Call it before a launch or after the
// DPU has become a zombie, never mid-run.
func (this *Dpu) Read(address int64, size int64) *encoding.ByteStream {
	config_loader := new(misc.ConfigLoader)
	config_loader.Init()

	if config_loader.WramOffset() <= address &&
		address+size <= config_loader.WramOffset()+config_loader.WramSize() {
		return this.dma.TransferFromWram(address, size)
	} else if config_loader.MramOffset() <= address &&
		address+size <= config_loader.MramOffset()+config_loader.MramSize() {
		return this.dma.TransferFromMram(address, size)
	} else {
		err := errors.New("address does not fall under WRAM nor MRAM region")
		panic(err)
	}
}

// Write stores a byte stream at address, choosing WRAM or MRAM by region.
// The same quiescence rule as Read applies.
func (this *Dpu) Write(address int64, byte_stream *encoding.ByteStream) {
	config_loader := new(misc.ConfigLoader)
	config_loader.Init()

	if config_loader.WramOffset() <= address &&
		address+byte_stream.Size() <= config_loader.WramOffset()+config_loader.WramSize() {
		this.dma.TransferToWram(address, byte_stream)
	} else if config_loader.MramOffset() <= address &&
		address+byte_stream.Size() <= config_loader.MramOffset()+config_loader.MramSize() {
		this.dma.TransferToMram(address, byte_stream)
	} else {
		err := errors.New("address does not fall under WRAM nor MRAM region")
		panic(err)
	}
}

func (this *Dpu) Boot() {
	this.thread_scheduler.Boot(0)
}

func (this *Dpu) IsZombie() bool {
	for _, thread := range this.threads {
		if thread.ThreadState() != logic.ZOMBIE {
			return false
		}
	}
	return this.logic.IsEmpty() && this.memory_controller.IsEmpty()
}

func (this *Dpu) Cycle() {
	for _, thread := range this.threads {
		thread.IncrementIssueCycle()
	}

	this.thread_scheduler.Cycle()
	this.logic.Cycle()
	this.dma.Cycle()

	refreshing := false
	if this.refresh_interval > 0 && this.refresh_mode == 0 {
		refreshing = (this.cycles+this.refresh_phase)%this.refresh_interval < this.refresh_cycles
	} else if this.refresh_interval > 0 {
		if this.cycles >= this.refresh_due && this.cycles >= this.refresh_until &&
			this.memory_controller.IsEmpty() {
			this.refresh_until = this.cycles + this.refresh_cycles
			this.refresh_due += this.refresh_interval
		}
		refreshing = this.cycles < this.refresh_until
	}

	if !refreshing {
		num_memory_cycles := int(this.frequency_ratio*float64(this.cycles) - this.frequency_ratio*float64(this.cycles-1))
		for i := 0; i < num_memory_cycles; i++ {
			this.memory_controller.Cycle()
		}
	}

	this.cycles++
}
