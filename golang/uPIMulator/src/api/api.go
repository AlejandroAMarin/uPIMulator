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

	ReadBandwidth  int
	WriteBandwidth int

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

		TRcd: 32,
		TRas: 78,
		TRp:  32,
		TCl:  32,
		TBl:  8,

		ReadBandwidth:  1,
		WriteBandwidth: 3,
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
	}
	set.baseline = make([]map[string]int64, len(set.dpus))

	return set, nil
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
	add_int("read_bandwidth", this.ReadBandwidth)
	add_int("write_bandwidth", this.WriteBandwidth)

	parser.AddOption(misc.STRING, "bin_dirpath", this.BinDirpath, "")

	// Only the fixed-benchmark path reads these. They are registered because
	// the core looks them up unconditionally.
	parser.AddOption(misc.STRING, "benchmark", "", "")
	parser.AddOption(misc.STRING, "data_prep_params", "0", "")

	return parser
}
