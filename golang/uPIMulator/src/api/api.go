// Package api drives the simulator call by call, the way a real host program
// drives real DPUs: allocate a set, load a program, write memory, launch, wait,
// read results back.
//
// The command line path in main.go is the other user of the simulator. It knows
// the whole run in advance and needs none of this.
//
// Two rules the caller must respect, both enforced here:
//
//   - Memory may only be read or written while the set is idle. The MRAM path
//     flushes the memory controller, which fails if requests are in flight.
//   - Statistics accumulate for the life of the set. Use LaunchStats for the
//     cost of the most recent run.
//
// Every exported function converts the core's panics into errors. The simulator
// panics on essentially any unexpected condition, and a host process embedding
// it must not die from that.
package api

import (
	"errors"
	"fmt"
	"strconv"
	"uPIMulator/src/abi/encoding"
	"uPIMulator/src/misc"
	"uPIMulator/src/simulator"
	"uPIMulator/src/simulator/dpu"
	"uPIMulator/src/simulator/host"
)

// Config describes the machine to simulate. The zero value is not usable; call
// DefaultConfig and adjust.
type Config struct {
	NumChannels        int
	NumRanksPerChannel int
	NumDpusPerRank     int
	NumTasklets        int

	NumSimulationThreads int
	Verbose              int

	LogicFrequency  int // MHz
	MemoryFrequency int // MHz

	NumPipelineStages           int
	NumRevolverSchedulingCycles int

	WordlineSize         int
	MinAccessGranularity int

	TRcd int
	TRas int
	TRp  int
	TCl  int
	TBl  int

	// Logic cycles a DMA waits before its first byte reaches the memory
	// controller: the DMA engine's own setup, which the DRAM timings above
	// do not describe. Charged once for every DmaBurstSize row a transfer
	// touches, whatever it holds.
	DmaStartupCycles int
	DmaBurstSize     int

	// Logic cycles the DMA engine stays busy after it wakes the thread, before
	// it can start the next transfer. Hidden when the engine is idle, paid on
	// every transfer when it is saturated.
	DmaTailCycles      int
	DmaTailCyclesPerKb int

	// A write's setup, and the delay between the engine finishing a write and
	// the thread waking. The engine is free during the delay.
	DmaWriteStartupCycles int
	DmaWriteAckCycles     int

	// What the engine pays when a transfer goes the other way from the one
	// before it. Reads alone or writes alone never pay it.
	DmaTurnaroundCycles int

	// MRAM refresh: every MramRefreshInterval logic cycles the memory is
	// unavailable for MramRefreshCycles. Zero models none.
	MramRefreshInterval int
	MramRefreshCycles   int
	MramRefreshPhase    int
	MramRefreshMode     int

	// Which waiting transfer the DMA engine starts next: 0 the oldest, 1 the
	// lowest-numbered thread's.
	DmaPolicy int

	ReadBandwidth  int
	WriteBandwidth int

	// Host-to-DPU transfer rates, in MB/s. A transfer is limited both by the
	// link to one DPU and by the rank interface every DPU on it shares, so the
	// cost is whichever of the two takes longer. Measured on this machine's
	// v1A hardware by runner/tests/perf/xfer_host.c; see that directory's
	// README for what they are and how far the model is from the machine.
	DpuWriteMbps  int
	DpuReadMbps   int
	RankWriteMbps int
	RankReadMbps  int

	// What a transfer costs before a single byte moves, in nanoseconds. This
	// is the whole cost of anything small: on this machine an 8-byte transfer
	// and a 512-byte one both take about 30 us. A model without it reports a
	// kernel that makes many small transfers -- BIMSA makes eleven -- as an
	// order of magnitude faster than it is.
	TransferSetupNs int

	// Extra nanoseconds for the first transfer a set makes, which pays to
	// bring the rank up. It is 250 us here, eight times an ordinary transfer,
	// and a short run is dominated by it: it is 40% of everything BIMSA spends
	// moving data.
	TransferFirstNs int

	// BinDirpath is only used by Dump. Leave it empty if you do not call Dump.
	BinDirpath string
}

// DefaultConfig returns one DPU with the same timing parameters as the command
// line defaults, so a library run and a CLI run model the same hardware.
func DefaultConfig() Config {
	return Config{
		NumChannels:        1,
		NumRanksPerChannel: 1,
		NumDpusPerRank:     1,
		NumTasklets:        1,

		NumSimulationThreads: 16,
		Verbose:              0,

		LogicFrequency:  350,
		MemoryFrequency: 2400,

		NumPipelineStages:           14,
		NumRevolverSchedulingCycles: 11,

		WordlineSize:         1024,
		MinAccessGranularity: 8,

		// Zero, not upstream's 32/78/32. The machine shows no cost for a read
		// that opens a different row from the one before it; the engine's
		// setup, paid for every row a transfer touches, covers the activation.
		// Measured by runner/tests/perf/scatter_kernel.c.
		TRcd: 0,
		TRas: 0,
		TRp:  0,
		// 23, not the 32 upstream assumed. MRAM has no datasheet to read this
		// off. With an idle engine the machine moves 0.4985 cycles a byte (a
		// read's latency is 242.5 + 0.4985 x size cycles in
		// runner/tests/perf/refresh_kernel.c); the busy engine's 0.545 is
		// this plus DmaTailCyclesPerKb plus the time refresh takes.
		TCl: 23,
		TBl: 8,

		// Fitted together against scatter_kernel.c (8 B to 2 KB, 1 to 16
		// tasklets), mix_kernel.c, the read-tail staircase of
		// runner/tests/perf/stair_kernel.c and six GIMSA DPUs replayed alone.
		// See "Small reads, and the DMA engine idle and busy" and "A read's
		// latency, and refresh lock-in" in runner/tests/perf/README.md.
		DmaStartupCycles:      32,
		DmaBurstSize:          1024,
		DmaTailCycles:         2,
		DmaTailCyclesPerKb:    25,
		DmaWriteStartupCycles: 24,
		DmaWriteAckCycles:     9,
		DmaTurnaroundCycles:   2,
		// Every 7.8 us at 350 MHz, for about 120 cycles: measured one transfer
		// at a time by runner/tests/perf/trace_kernel.c.
		MramRefreshInterval: 2736,
		MramRefreshCycles:   120,
		DmaPolicy:           0,

		ReadBandwidth:  1,
		WriteBandwidth: 3,

		// Measured on this machine's v1A hardware, fitted to the median of many
		// repetitions rather than the best: the best is what the link can do on
		// a warm path, the median is what a program actually waits for. The
		// per-DPU rates are what a single DPU sustains. The rank rates are a
		// compromise, because the real machine's aggregate keeps creeping up
		// between 16 and 64 DPUs instead of flattening and no two-limit model
		// reproduces that. runner/tests/perf/README.md has the sweep and the
		// residual at every point.
		DpuWriteMbps:  966,
		DpuReadMbps:   250,
		RankWriteMbps: 9700,
		RankReadMbps:  3400,

		TransferSetupNs: 30000,
		TransferFirstNs: 250000,
	}
}

// Set is an allocated group of simulated DPUs.
type Set struct {
	sim  *simulator.Simulator
	dpus []*dpu.Dpu

	addresses map[string]int64
	values    map[string]int64

	launched bool
	baseline []map[string]int64

	// What the host spent moving data, which no DPU counter sees. Transfers
	// and kernels never overlap here, so these simply add to the kernel time.
	config              Config
	transfer_cycles     int64
	transfer_to_bytes   int64
	transfer_from_bytes int64
	transfer_count      int64
}

// New builds a set and installs program on every DPU. The DPUs are idle
// afterwards; call Launch to start them.
func New(config Config, program *host.Program) (set *Set, err error) {
	defer func() {
		if r := recover(); r != nil {
			set = nil
			err = fmt.Errorf("uPIMulator: allocating the set: %v", r)
		}
	}()

	if program == nil {
		return nil, errors.New("uPIMulator: program is nil")
	}

	sim := new(simulator.Simulator)
	sim.Build(config.toCommandLineParser(), program)

	// Copy the program image into every DPU's memories.
	sim.Host().Load()

	set = &Set{
		sim:       sim,
		dpus:      sim.Host().Dpus(),
		addresses: sim.Host().Addresses(),
		values:    sim.Host().Values(),
		config:    config,
	}
	set.baseline = make([]map[string]int64, len(set.dpus))

	if err := set.writeClocksPerSec(config.LogicFrequency); err != nil {
		return nil, err
	}

	return set, nil
}

// writeClocksPerSec fills in the CLOCKS_PER_SEC the runtime declares in crt0.c.
// On hardware libdpu writes the rank's real frequency there at load time, and a
// kernel that turns a cycle count into a duration reads it. Nothing wrote it
// here, so every such kernel divided by zero: BIMSA's own reconciliation of its
// perfcounter against the wall clock silently reported nothing.
//
// The value is the frequency this set is configured to model, so it agrees with
// the cycle counts the same run reports.
func (this *Set) writeClocksPerSec(logic_frequency_mhz int) error {
	address, found := this.SymbolAddress("CLOCKS_PER_SEC")

	if !found {
		// Older images, or a kernel linked without crt0. Nothing to fill in.
		return nil
	}

	hz := uint32(logic_frequency_mhz) * 1000000
	data := []byte{byte(hz), byte(hz >> 8), byte(hz >> 16), byte(hz >> 24)}

	for i := range this.dpus {
		if err := this.CopyTo(i, address, data); err != nil {
			return fmt.Errorf("uPIMulator: writing CLOCKS_PER_SEC: %v", err)
		}
	}

	return nil
}

// TransferCycles returns what a host transfer of bytes_per_dpu to or from each
// of num_dpus DPUs costs, in DPU clock cycles.
//
// A transfer costs a fixed setup plus the time to move the bytes. Two limits
// apply to the moving and the slower one wins: one DPU's link carries
// DpuWriteMbps or DpuReadMbps whatever else is going on, and every DPU on a
// rank shares an interface carrying RankWriteMbps or RankReadMbps. Writing to
// 8 DPUs at once therefore costs the same as writing to one, because the rank
// interface is nowhere near saturated; writing to a full rank is limited by
// the interface instead.
//
// The setup is not a rounding error. Below about 32 KB it is the whole cost,
// and a kernel that makes many small transfers -- BIMSA makes eleven -- is
// dominated by it.
//
// The shape is deliberately simple, and it is wrong in two known places: the
// real machine's cost per DPU grows with the chunk size somewhere between 512
// bytes and 4 KB rather than at a fixed rate, and a scatter of multi-megabyte
// buffers to a whole rank falls below the link's capability because the host
// has to gather them. runner/tests/perf/README.md measures both.
//
// Both numbers are measured, not assumed: see runner/tests/perf/README.md for
// the sweep they come from and for how far this two-limit shape is from the
// machine in between the two regimes.
func (this *Set) TransferCycles(num_dpus int, bytes_per_dpu int64, to_dpu bool) int64 {
	if num_dpus <= 0 || bytes_per_dpu <= 0 {
		return 0
	}

	dpu_mbps := int64(this.config.DpuReadMbps)
	rank_mbps := int64(this.config.RankReadMbps)

	if to_dpu {
		dpu_mbps = int64(this.config.DpuWriteMbps)
		rank_mbps = int64(this.config.RankWriteMbps)
	}

	if dpu_mbps <= 0 || rank_mbps <= 0 {
		return 0
	}

	// cycles = bytes / (MB/s) * (cycles/s), and the megahertz cancels the
	// megabytes, so this stays in integers without overflowing for any
	// transfer a DPU could hold.
	mhz := int64(this.config.LogicFrequency)

	per_dpu := bytes_per_dpu * mhz / dpu_mbps
	whole_rank := bytes_per_dpu * int64(num_dpus) * mhz / rank_mbps

	moving := per_dpu
	if whole_rank > moving {
		moving = whole_rank
	}

	setup := int64(this.config.TransferSetupNs)

	if this.transfer_count == 0 {
		setup += int64(this.config.TransferFirstNs)
	}

	return setup*mhz/1000 + moving
}

// ChargeTransfer records a host transfer's cost. The data itself has already
// moved; this is the time it would have taken.
func (this *Set) ChargeTransfer(num_dpus int, bytes_per_dpu int64, to_dpu bool) {
	this.transfer_cycles += this.TransferCycles(num_dpus, bytes_per_dpu, to_dpu)
	this.transfer_count++

	if to_dpu {
		this.transfer_to_bytes += bytes_per_dpu * int64(num_dpus)
	} else {
		this.transfer_from_bytes += bytes_per_dpu * int64(num_dpus)
	}
}

// TransferStats reports what the host transfers cost, for a caller that wants
// the whole picture rather than only what happened inside a DPU.
func (this *Set) TransferStats() map[string]int64 {
	return map[string]int64{
		"transfer_cycle":      this.transfer_cycles,
		"transfer_to_bytes":   this.transfer_to_bytes,
		"transfer_from_bytes": this.transfer_from_bytes,
		"transfer_count":      this.transfer_count,
	}
}

// NumDpus returns how many DPUs the set holds.
func (this *Set) NumDpus() int {
	return len(this.dpus)
}

// SymbolAddress resolves a label, which is how a host program addresses a
// transfer by name. Local symbols carry a "<relocatable>." prefix.
func (this *Set) SymbolAddress(name string) (int64, bool) {
	address, found := this.addresses[name]
	return address, found
}

// HeapAddress converts an offset into the MRAM heap to an absolute address.
// This is the DPU_MRAM_HEAP_POINTER_NAME base that bulk transfers use.
func (this *Set) HeapAddress(offset int64) (int64, error) {
	base, found := this.values["__sys_used_mram_end"]

	if !found {
		return 0, errors.New("uPIMulator: __sys_used_mram_end is not in the program")
	}

	return base + offset, nil
}

// CopyTo writes data into one DPU. The set must be idle.
func (this *Set) CopyTo(dpu_index int, address int64, data []byte) (err error) {
	defer func() {
		if r := recover(); r != nil {
			err = fmt.Errorf("uPIMulator: writing DPU %d at %d: %v", dpu_index, address, r)
		}
	}()

	if err := this.checkIdle(dpu_index); err != nil {
		return err
	}

	byte_stream := new(encoding.ByteStream)
	byte_stream.Init()
	for _, b := range data {
		byte_stream.Append(b)
	}

	this.dpus[dpu_index].Write(address, byte_stream)
	return nil
}

// CopyFrom reads size bytes out of one DPU. The set must be idle.
func (this *Set) CopyFrom(dpu_index int, address int64, size int64) (data []byte, err error) {
	defer func() {
		if r := recover(); r != nil {
			data = nil
			err = fmt.Errorf("uPIMulator: reading DPU %d at %d: %v", dpu_index, address, r)
		}
	}()

	if err := this.checkIdle(dpu_index); err != nil {
		return nil, err
	}

	byte_stream := this.dpus[dpu_index].Read(address, size)

	data = make([]byte, 0, byte_stream.Size())
	for i := int64(0); i < byte_stream.Size(); i++ {
		data = append(data, byte_stream.Get(int(i)))
	}

	return data, nil
}

// Launch starts the kernel on every DPU and records a statistics baseline, so
// LaunchStats can report the cost of this run alone.
func (this *Set) Launch() (err error) {
	defer func() {
		if r := recover(); r != nil {
			err = fmt.Errorf("uPIMulator: launching: %v", r)
		}
	}()

	if this.Running() {
		return errors.New("uPIMulator: the set is still running")
	}

	for i := range this.dpus {
		this.baseline[i] = this.statsOf(i)
	}

	this.sim.Host().Launch()
	this.launched = true

	return nil
}

// Running reports whether any DPU still has work in flight.
func (this *Set) Running() bool {
	if !this.launched {
		return false
	}

	return !this.sim.Host().IsZombie()
}

// Step advances the machine by one DPU logic cycle.
func (this *Set) Step() (err error) {
	defer func() {
		if r := recover(); r != nil {
			err = fmt.Errorf("uPIMulator: cycling: %v", r)
		}
	}()

	this.sim.CycleOnce()
	return nil
}

// Run cycles until every DPU is idle and returns the number of cycles taken.
//
// max_cycles guards against a kernel that never finishes, which would otherwise
// hang the host process; pass 0 for no limit.
func (this *Set) Run(max_cycles int64) (cycles int64, err error) {
	if !this.launched {
		return 0, errors.New("uPIMulator: nothing has been launched")
	}

	for this.Running() {
		if err := this.Step(); err != nil {
			return cycles, err
		}

		cycles++

		if max_cycles > 0 && cycles >= max_cycles {
			return cycles, fmt.Errorf("uPIMulator: still running after %d cycles", max_cycles)
		}
	}

	return cycles, nil
}

// Stats returns every counter for one DPU, accumulated over the life of the set.
func (this *Set) Stats(dpu_index int) map[string]int64 {
	return this.statsOf(dpu_index)
}

// LaunchStats returns the counters for one DPU attributed to the most recent
// launch, by subtracting the baseline taken when it started. This is the number
// to compare against a hardware perfcounter reading, which is also per run.
func (this *Set) LaunchStats(dpu_index int) map[string]int64 {
	current := this.statsOf(dpu_index)

	base := this.baseline[dpu_index]
	if base == nil {
		return current
	}

	delta := make(map[string]int64, len(current))
	for stat, value := range current {
		delta[stat] = value - base[stat]
	}

	return delta
}

// Free releases the set. Cycling or copying afterwards is not allowed.
func (this *Set) Free() (err error) {
	defer func() {
		if r := recover(); r != nil {
			err = fmt.Errorf("uPIMulator: freeing the set: %v", r)
		}
	}()

	// Fini asserts a clean shutdown: it panics if a thread is not a zombie or a
	// lock is still held. That is a reasonable check at the end of a batch run,
	// but a host process may free a set it never launched, or one that failed
	// part way through, and neither should be an error. Nothing is leaked by
	// skipping it, because the whole object graph becomes garbage here.
	func() {
		defer func() { _ = recover() }()
		this.sim.Fini()
	}()

	this.dpus = nil

	return nil
}

// Simulator exposes the underlying model, for callers that need more than this
// API offers.
func (this *Set) Simulator() *simulator.Simulator {
	return this.sim
}

func (this *Set) checkIdle(dpu_index int) error {
	if dpu_index < 0 || dpu_index >= len(this.dpus) {
		return fmt.Errorf("uPIMulator: DPU index %d is out of range", dpu_index)
	}

	// Reading or writing MRAM flushes the memory controller, which fails while
	// requests are outstanding. Report that as an error rather than let the
	// core panic deep inside a transfer.
	if this.Running() {
		return errors.New("uPIMulator: the set is running; wait for it before touching memory")
	}

	return nil
}

// statsOf merges the seven per-DPU counter groups, each key prefixed with the
// group name so that same-named counters stay distinct.
func (this *Set) statsOf(dpu_index int) map[string]int64 {
	dpu_ := this.dpus[dpu_index]

	factories := []*misc.StatFactory{
		dpu_.StatFactory(),
		dpu_.ThreadScheduler().StatFactory(),
		dpu_.Logic().StatFactory(),
		dpu_.Logic().CycleRule().StatFactory(),
		dpu_.MemoryController().StatFactory(),
		dpu_.MemoryController().MemoryScheduler().StatFactory(),
		dpu_.MemoryController().RowBuffer().StatFactory(),
	}

	stats := make(map[string]int64, 0)
	for _, factory := range factories {
		for _, stat := range factory.Stats() {
			stats[factory.Name()+"_"+stat] = factory.Value(stat)
		}
	}

	return stats
}

// toCommandLineParser expresses the config the way the core expects it. The
// parser is just a map of named options, so registering each value as an
// option default configures the machine without touching os.Args.
func (this Config) toCommandLineParser() *misc.CommandLineParser {
	parser := new(misc.CommandLineParser)
	parser.Init()

	add_int := func(option string, value int) {
		parser.AddOption(misc.INT, option, strconv.Itoa(value), "")
	}

	add_int("verbose", this.Verbose)
	add_int("num_simulation_threads", this.NumSimulationThreads)
	add_int("num_channels", this.NumChannels)
	add_int("num_ranks_per_channel", this.NumRanksPerChannel)
	add_int("num_dpus_per_rank", this.NumDpusPerRank)
	add_int("num_tasklets", this.NumTasklets)
	add_int("logic_frequency", this.LogicFrequency)
	add_int("memory_frequency", this.MemoryFrequency)
	add_int("num_pipeline_stages", this.NumPipelineStages)
	add_int("num_revolver_scheduling_cycles", this.NumRevolverSchedulingCycles)
	add_int("wordline_size", this.WordlineSize)
	add_int("min_access_granularity", this.MinAccessGranularity)
	add_int("t_rcd", this.TRcd)
	add_int("t_ras", this.TRas)
	add_int("t_rp", this.TRp)
	add_int("t_cl", this.TCl)
	add_int("t_bl", this.TBl)
	add_int("dma_startup_cycles", this.DmaStartupCycles)
	add_int("dma_burst_size", this.DmaBurstSize)
	add_int("dma_tail_cycles", this.DmaTailCycles)
	add_int("dma_tail_cycles_per_kb", this.DmaTailCyclesPerKb)
	add_int("dma_write_startup_cycles", this.DmaWriteStartupCycles)
	add_int("dma_write_ack_cycles", this.DmaWriteAckCycles)
	add_int("dma_turnaround_cycles", this.DmaTurnaroundCycles)
	add_int("mram_refresh_interval", this.MramRefreshInterval)
	add_int("mram_refresh_cycles", this.MramRefreshCycles)
	add_int("mram_refresh_phase", this.MramRefreshPhase)
	add_int("mram_refresh_mode", this.MramRefreshMode)
	add_int("dma_policy", this.DmaPolicy)
	add_int("read_bandwidth", this.ReadBandwidth)
	add_int("write_bandwidth", this.WriteBandwidth)

	parser.AddOption(misc.STRING, "bin_dirpath", this.BinDirpath, "")

	// Only the fixed-benchmark path reads these. They are registered because
	// the core looks them up unconditionally.
	parser.AddOption(misc.STRING, "benchmark", "", "")
	parser.AddOption(misc.STRING, "data_prep_params", "0", "")

	return parser
}
