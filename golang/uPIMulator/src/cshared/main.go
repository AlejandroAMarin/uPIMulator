// Command cshared exposes the simulator to C as libupimulator.so, so the libdpu
// shim can drive it in place of real hardware.
//
// Build:
//
//	go build -buildmode=c-shared -o libupimulator.so ./cshared
//
// Sets are addressed by integer handle rather than by pointer, because cgo does
// not allow a Go pointer to be stored on the C side. Every function returns 0 on
// success and -1 on failure; call upim_last_error for the reason.
package main

/*
#include <stdlib.h>
#include <string.h>
*/
import "C"

import (
	"fmt"
	"sync"
	"uPIMulator/src/api"
	"uPIMulator/src/simulator/host"
	"unsafe"
)

// DPU_MRAM_HEAP_POINTER_NAME expands to this symbol, and the simulator's linker
// happens to publish it under the same name, so a bulk transfer resolves
// through the ordinary constant table.
const mram_heap_symbol = "__sys_used_mram_end"

var (
	registry_mutex sync.Mutex
	registry       = make(map[int]*api.Set)
	next_handle    = 1

	error_mutex sync.Mutex
	last_error  string
)

func setError(format string, args ...interface{}) C.int {
	error_mutex.Lock()
	last_error = fmt.Sprintf(format, args...)
	error_mutex.Unlock()
	return -1
}

func lookup(handle C.int) (*api.Set, bool) {
	registry_mutex.Lock()
	defer registry_mutex.Unlock()

	set, found := registry[int(handle)]
	return set, found
}

//export upim_last_error
func upim_last_error() *C.char {
	error_mutex.Lock()
	defer error_mutex.Unlock()

	// The caller must free this.
	return C.CString(last_error)
}

// upim_set_create builds a set of num_dpus DPUs running the program in
// program_dir, which is the bin directory produced by a linker run.
//
//export upim_set_create
func upim_set_create(program_dir *C.char, num_dpus C.int, num_tasklets C.int) C.int {
	if num_dpus <= 0 {
		return setError("number of DPUs must be positive, got %d", int(num_dpus))
	}

	if num_tasklets <= 0 {
		return setError("number of tasklets must be positive, got %d", int(num_tasklets))
	}

	program, err := loadProgram(C.GoString(program_dir))

	if err != nil {
		return setError("%v", err)
	}

	config := api.DefaultConfig()
	config.NumTasklets = int(num_tasklets)

	// One rank of the requested width. The simulator places no limit on how
	// many DPUs a rank may hold.
	config.NumChannels = 1
	config.NumRanksPerChannel = 1
	config.NumDpusPerRank = int(num_dpus)

	set, err := api.New(config, program)

	if err != nil {
		return setError("%v", err)
	}

	registry_mutex.Lock()
	defer registry_mutex.Unlock()

	handle := next_handle
	next_handle++
	registry[handle] = set

	return C.int(handle)
}

func loadProgram(program_dir string) (program *host.Program, err error) {
	defer func() {
		if r := recover(); r != nil {
			program = nil
			err = fmt.Errorf("reading the program from %s: %v", program_dir, r)
		}
	}()

	if program_dir == "" {
		return nil, fmt.Errorf("no program directory was given")
	}

	return host.LoadProgramFromDir(program_dir), nil
}

//export upim_set_free
func upim_set_free(handle C.int) C.int {
	set, found := lookup(handle)

	if !found {
		return setError("no such set: %d", int(handle))
	}

	registry_mutex.Lock()
	delete(registry, int(handle))
	registry_mutex.Unlock()

	if err := set.Free(); err != nil {
		return setError("%v", err)
	}

	return 0
}

//export upim_num_dpus
func upim_num_dpus(handle C.int) C.int {
	set, found := lookup(handle)

	if !found {
		return setError("no such set: %d", int(handle))
	}

	return C.int(set.NumDpus())
}

// upim_resolve turns a symbol name and offset into an absolute DPU address.
// Returns -1 if the symbol is unknown.
//
//export upim_resolve
func upim_resolve(handle C.int, symbol *C.char, offset C.longlong) C.longlong {
	set, found := lookup(handle)

	if !found {
		setError("no such set: %d", int(handle))
		return -1
	}

	name := C.GoString(symbol)

	if name == mram_heap_symbol {
		address, err := set.HeapAddress(int64(offset))

		if err != nil {
			setError("%v", err)
			return -1
		}

		return C.longlong(address)
	}

	address, ok := set.SymbolAddress(name)

	if !ok {
		setError("symbol %q is not in the program", name)
		return -1
	}

	return C.longlong(address + int64(offset))
}

//export upim_copy_to
func upim_copy_to(handle C.int, dpu_index C.int, address C.longlong, buffer unsafe.Pointer, length C.int) C.int {
	set, found := lookup(handle)

	if !found {
		return setError("no such set: %d", int(handle))
	}

	data := C.GoBytes(buffer, length)

	if err := set.CopyTo(int(dpu_index), int64(address), data); err != nil {
		return setError("%v", err)
	}

	return 0
}

//export upim_copy_from
func upim_copy_from(handle C.int, dpu_index C.int, address C.longlong, buffer unsafe.Pointer, length C.int) C.int {
	set, found := lookup(handle)

	if !found {
		return setError("no such set: %d", int(handle))
	}

	data, err := set.CopyFrom(int(dpu_index), int64(address), int64(length))

	if err != nil {
		return setError("%v", err)
	}

	if len(data) != int(length) {
		return setError("read %d bytes, expected %d", len(data), int(length))
	}

	C.memcpy(buffer, unsafe.Pointer(&data[0]), C.size_t(length))
	return 0
}

//export upim_launch
func upim_launch(handle C.int) C.int {
	set, found := lookup(handle)

	if !found {
		return setError("no such set: %d", int(handle))
	}

	if err := set.Launch(); err != nil {
		return setError("%v", err)
	}

	return 0
}

//export upim_running
func upim_running(handle C.int) C.int {
	set, found := lookup(handle)

	if !found {
		return setError("no such set: %d", int(handle))
	}

	if set.Running() {
		return 1
	}

	return 0
}

// upim_run cycles until every DPU is idle. max_cycles guards against a kernel
// that never retires; pass 0 for no limit.
//
//export upim_run
func upim_run(handle C.int, max_cycles C.longlong) C.int {
	set, found := lookup(handle)

	if !found {
		return setError("no such set: %d", int(handle))
	}

	if _, err := set.Run(int64(max_cycles)); err != nil {
		return setError("%v", err)
	}

	return 0
}

// upim_stat reads one counter for one DPU. When per_launch is non-zero the
// value covers the most recent launch only, which is what matches a hardware
// perfcounter reading.
//
//export upim_stat
func upim_stat(handle C.int, dpu_index C.int, name *C.char, per_launch C.int) C.longlong {
	set, found := lookup(handle)

	if !found {
		setError("no such set: %d", int(handle))
		return -1
	}

	if int(dpu_index) < 0 || int(dpu_index) >= set.NumDpus() {
		setError("DPU index %d is out of range", int(dpu_index))
		return -1
	}

	var stats map[string]int64

	if per_launch != 0 {
		stats = set.LaunchStats(int(dpu_index))
	} else {
		stats = set.Stats(int(dpu_index))
	}

	wanted := C.GoString(name)

	// Counter keys are prefixed with the component that owns them, for example
	// "Logic[0_0_0]_logic_cycle", so match on the suffix.
	for stat, value := range stats {
		if stat == wanted || endsWith(stat, "_"+wanted) {
			return C.longlong(value)
		}
	}

	setError("no counter named %q", wanted)
	return -1
}

func endsWith(s string, suffix string) bool {
	return len(s) >= len(suffix) && s[len(s)-len(suffix):] == suffix
}

func main() {}
