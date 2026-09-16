package api

import (
	"os"
	"path/filepath"
	"strconv"
	"strings"
	"testing"
	"uPIMulator/src/simulator/host"
)

// TestReplayVA drives a linked benchmark through the API the way a host program
// would: load the image, write the inputs, launch, wait, read the results back.
//
// It then checks the cycle count against the command line path. The two must
// agree exactly; if they do not, the API is not reproducing what the simulator
// does on its own, and every comparison against hardware built on top of it
// would be measuring the wrong thing.
//
// Point UPIM_TEST_BINDIR at the bin directory of a completed run, for example:
//
//	./build/uPIMulator --root_dirpath $PWD --bin_dirpath /tmp/binM --benchmark VA \
//	  --num_channels 1 --num_ranks_per_channel 1 --num_dpus_per_rank 1 \
//	  --num_tasklets 16 --data_prep_params 1024
//	UPIM_TEST_BINDIR=/tmp/binM go test ./api/ -run TestReplayVA -v
func TestReplayVA(t *testing.T) {
	bin_dirpath := os.Getenv("UPIM_TEST_BINDIR")

	if bin_dirpath == "" {
		t.Skip("set UPIM_TEST_BINDIR to the bin directory of a completed run")
	}

	num_tasklets := 16
	if value := os.Getenv("UPIM_TEST_TASKLETS"); value != "" {
		parsed, err := strconv.Atoi(value)

		if err != nil {
			t.Fatalf("UPIM_TEST_TASKLETS: %v", err)
		}

		num_tasklets = parsed
	}

	program := host.LoadProgramFromDir(bin_dirpath)

	config := DefaultConfig()
	config.NumTasklets = num_tasklets

	set, err := New(config, program)

	if err != nil {
		t.Fatalf("New: %v", err)
	}
	defer set.Free()

	if set.NumDpus() != 1 {
		t.Fatalf("expected 1 DPU, got %d", set.NumDpus())
	}

	// Write the inputs the assembler prepared, exactly where the host model
	// would have put them.
	for _, chunk := range inputChunks(t, bin_dirpath) {
		var address int64

		if chunk.heap {
			address, err = set.HeapAddress(chunk.offset)

			if err != nil {
				t.Fatalf("HeapAddress: %v", err)
			}
		} else {
			resolved, found := set.SymbolAddress(chunk.symbol)

			if !found {
				t.Fatalf("symbol %q is not in the program", chunk.symbol)
			}

			address = resolved
		}

		if err := set.CopyTo(0, address, chunk.data); err != nil {
			t.Fatalf("CopyTo %s: %v", chunk.name, err)
		}
	}

	// Writing while idle must be allowed, and reading back must return what we
	// wrote. This is the property the shim's dpu_copy_to/dpu_copy_from rely on.
	if err := set.Launch(); err != nil {
		t.Fatalf("Launch: %v", err)
	}

	if !set.Running() {
		t.Fatal("the set reports idle immediately after a launch")
	}

	// A VA run of this size takes tens of thousands of cycles. The cap is a
	// guard against a kernel that never retires, not a tight bound.
	cycles, err := set.Run(50 * 1000 * 1000)

	if err != nil {
		t.Fatalf("Run: %v", err)
	}

	stats := set.LaunchStats(0)

	logic_cycle := findStat(t, stats, "logic_cycle")
	num_instructions := findStat(t, stats, "num_instructions")

	t.Logf("cycles stepped %d, logic_cycle %d, num_instructions %d",
		cycles, logic_cycle, num_instructions)

	if logic_cycle == 0 || num_instructions == 0 {
		t.Fatal("the kernel did no work; the inputs were probably not delivered")
	}

	// Compare against the command line result for the same run, which the
	// simulator wrote next to the inputs.
	expected := expectedStats(t, bin_dirpath)

	for stat, want := range expected {
		got, found := stats[stat]

		if !found {
			t.Errorf("%s: missing from the API statistics", stat)
			continue
		}

		if got != want {
			t.Errorf("%s: API reports %d, the command line path reported %d", stat, got, want)
		}
	}
}

// TestCopyRejectedWhileRunning checks the quiescence rule is enforced, because
// breaking it panics deep inside the memory controller rather than failing
// cleanly.
func TestCopyRejectedWhileRunning(t *testing.T) {
	bin_dirpath := os.Getenv("UPIM_TEST_BINDIR")

	if bin_dirpath == "" {
		t.Skip("set UPIM_TEST_BINDIR to the bin directory of a completed run")
	}

	config := DefaultConfig()
	config.NumTasklets = 16

	set, err := New(config, host.LoadProgramFromDir(bin_dirpath))

	if err != nil {
		t.Fatalf("New: %v", err)
	}
	defer set.Free()

	if err := set.Launch(); err != nil {
		t.Fatalf("Launch: %v", err)
	}

	if err := set.Step(); err != nil {
		t.Fatalf("Step: %v", err)
	}

	if !set.Running() {
		t.Skip("the kernel retired immediately, so there is no running state to test")
	}

	if _, err := set.CopyFrom(0, 0, 8); err == nil {
		t.Fatal("CopyFrom succeeded while the set was running; it must be refused")
	}
}

// TestRelaunch checks a set can run a kernel more than once. Real host programs
// launch repeatedly against one loaded binary: GIMSA does three launches, and
// BIMSA does one per batch. It also checks that LaunchStats reports the cost of
// the latest run rather than the running total, which is what makes a
// per-launch comparison against a hardware perfcounter meaningful.
func TestRelaunch(t *testing.T) {
	bin_dirpath := os.Getenv("UPIM_TEST_BINDIR")

	if bin_dirpath == "" {
		t.Skip("set UPIM_TEST_BINDIR to the bin directory of a completed run")
	}

	config := DefaultConfig()
	config.NumTasklets = 16

	set, err := New(config, host.LoadProgramFromDir(bin_dirpath))

	if err != nil {
		t.Fatalf("New: %v", err)
	}
	defer set.Free()

	for _, chunk := range inputChunks(t, bin_dirpath) {
		address := resolveChunk(t, set, chunk)

		if err := set.CopyTo(0, address, chunk.data); err != nil {
			t.Fatalf("CopyTo %s: %v", chunk.name, err)
		}
	}

	first := runOnce(t, set)
	second := runOnce(t, set)

	if first == 0 || second == 0 {
		t.Fatalf("a launch did no work: first %d, second %d", first, second)
	}

	// The second run repeats the same kernel over the same memory, so its cost
	// should be close to the first. An order-of-magnitude difference means the
	// DPU did not restart cleanly.
	ratio := float64(second) / float64(first)

	if ratio < 0.5 || ratio > 2.0 {
		t.Errorf("second launch cost %d cycles against %d for the first; "+
			"the DPU may not be restarting cleanly", second, first)
	}

	// Cumulative statistics must exceed the per-launch delta, or the baseline
	// is not being applied.
	total := findStat(t, set.Stats(0), "logic_cycle")

	if total <= second {
		t.Errorf("cumulative logic_cycle %d is not greater than the last launch %d",
			total, second)
	}

	t.Logf("first launch %d cycles, second %d, cumulative %d", first, second, total)
}

func runOnce(t *testing.T, set *Set) int64 {
	t.Helper()

	if err := set.Launch(); err != nil {
		t.Fatalf("Launch: %v", err)
	}

	if _, err := set.Run(50 * 1000 * 1000); err != nil {
		t.Fatalf("Run: %v", err)
	}

	return findStat(t, set.LaunchStats(0), "logic_cycle")
}

func resolveChunk(t *testing.T, set *Set, c chunk) int64 {
	t.Helper()

	if c.heap {
		address, err := set.HeapAddress(c.offset)

		if err != nil {
			t.Fatalf("HeapAddress: %v", err)
		}

		return address
	}

	address, found := set.SymbolAddress(c.symbol)

	if !found {
		t.Fatalf("symbol %q is not in the program", c.symbol)
	}

	return address
}

type chunk struct {
	name   string
	symbol string
	offset int64
	heap   bool
	data   []byte
}

// inputChunks reads the host-to-DPU data the assembler wrote. The file name
// carries the destination: either a symbol name, or an offset into the MRAM
// heap.
func inputChunks(t *testing.T, bin_dirpath string) []chunk {
	t.Helper()

	entries, err := os.ReadDir(bin_dirpath)

	if err != nil {
		t.Fatalf("reading %s: %v", bin_dirpath, err)
	}

	chunks := make([]chunk, 0)

	for _, entry := range entries {
		name := entry.Name()

		if !strings.HasPrefix(name, "input_") || !strings.HasSuffix(name, ".bin") {
			continue
		}

		stem := strings.TrimSuffix(strings.TrimPrefix(name, "input_"), ".bin")
		words := strings.Split(stem, "_")

		// Trailing fields are always <execution>_<dpu_id>.
		if len(words) < 3 {
			t.Fatalf("cannot parse chunk name %q", name)
		}
		words = words[:len(words)-2]

		current := chunk{name: name, data: readByteFile(t, filepath.Join(bin_dirpath, name))}

		if strings.Join(words[:len(words)-1], "_") == "dpu_mram_heap_pointer_name" {
			offset, err := strconv.ParseInt(words[len(words)-1], 10, 64)

			if err != nil {
				t.Fatalf("chunk %q: parsing the heap offset: %v", name, err)
			}

			current.heap = true
			current.offset = offset
		} else {
			current.symbol = strings.Join(words, "_")
		}

		chunks = append(chunks, current)
	}

	return chunks
}

// readByteFile reads one of the simulator's memory images, which hold one
// decimal byte per line rather than raw bytes.
func readByteFile(t *testing.T, path string) []byte {
	t.Helper()

	content, err := os.ReadFile(path)

	if err != nil {
		t.Fatalf("reading %s: %v", path, err)
	}

	data := make([]byte, 0)

	for _, line := range strings.Split(strings.TrimSpace(string(content)), "\n") {
		if line == "" {
			continue
		}

		value, err := strconv.Atoi(strings.TrimSpace(line))

		if err != nil {
			t.Fatalf("%s: %v", path, err)
		}

		data = append(data, uint8(value))
	}

	return data
}

// expectedStats reads log.txt, the statistics the command line path dumped for
// the same run.
func expectedStats(t *testing.T, bin_dirpath string) map[string]int64 {
	t.Helper()

	content, err := os.ReadFile(filepath.Join(bin_dirpath, "log.txt"))

	if err != nil {
		t.Fatalf("reading log.txt: %v", err)
	}

	stats := make(map[string]int64)

	for _, line := range strings.Split(string(content), "\n") {
		name, value, found := strings.Cut(line, ": ")

		if !found {
			continue
		}

		parsed, err := strconv.ParseInt(strings.TrimSpace(value), 10, 64)

		if err != nil {
			continue
		}

		stats[name] = parsed
	}

	if len(stats) == 0 {
		t.Fatal("log.txt held no statistics")
	}

	return stats
}

func findStat(t *testing.T, stats map[string]int64, suffix string) int64 {
	t.Helper()

	for stat, value := range stats {
		if strings.HasSuffix(stat, "_"+suffix) {
			return value
		}
	}

	t.Fatalf("no statistic ending in %q", suffix)
	return 0
}
