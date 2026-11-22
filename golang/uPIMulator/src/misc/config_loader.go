package misc

import "sync/atomic"

type ConfigLoader struct {
}

// hardware_map selects UPMEM's own address map, the one a DPU ELF is linked
// against, in place of the map uPIMulator's linker lays programs out in. It is
// set once, before a simulator is built, by whoever loads an ELF; every DPU in
// the process then uses it.
//
//	                   uPIMulator's linker        a DPU ELF
//	IRAM               384 KB, 12 bytes a step    0, 1 a step (an instruction index)
//	WRAM               512, 128 KB                0, 64 KB
//	MRAM               512 KB, 64 MB              0x08000000, 64 MB
//
// A code address in a register or in data is an instruction index on the
// machine: a jump table holds indices, and call writes the next index. The
// encoding in IRAM is uPIMulator's either way, IramDataWidth bits long.
var hardware_map atomic.Bool

// UseHardwareMemoryMap switches every ConfigLoader to UPMEM's address map.
func UseHardwareMemoryMap(on bool) {
	hardware_map.Store(on)
}

// HardwareMemoryMap reports whether UPMEM's address map is in use.
func HardwareMemoryMap() bool {
	return hardware_map.Load()
}

func (this *ConfigLoader) Init() {
}

func (this *ConfigLoader) AddressWidth() int {
	return 32
}

func (this *ConfigLoader) AtomicDataWidth() int {
	return 32
}

func (this *ConfigLoader) AtomicOffset() int64 {
	return 0
}

func (this *ConfigLoader) AtomicSize() int64 {
	return 256
}

func (this *ConfigLoader) IramDataWidth() int {
	return 96
}

func (this *ConfigLoader) IramOffset() int64 {
	if HardwareMemoryMap() {
		return 0
	}
	return 384 * 1024
}

// IramSize is the span of IRAM addresses: IramInstructions steps of
// IramStride.
func (this *ConfigLoader) IramSize() int64 {
	return this.IramInstructions() * this.IramStride()
}

// IramStride is how far the program counter moves for one instruction.
func (this *ConfigLoader) IramStride() int64 {
	if HardwareMemoryMap() {
		return 1
	}
	return int64(this.IramDataWidth() / 8)
}

// IramInstructions is how many instructions IRAM holds.
func (this *ConfigLoader) IramInstructions() int64 {
	return 4096
}

func (this *ConfigLoader) WramDataWidth() int {
	return 32
}

func (this *ConfigLoader) WramOffset() int64 {
	if HardwareMemoryMap() {
		return 0
	}
	return 512
}

func (this *ConfigLoader) WramSize() int64 {
	if HardwareMemoryMap() {
		return 64 * 1024
	}
	return 128 * 1024
}

func (this *ConfigLoader) MramDataWidth() int {
	return 32
}

func (this *ConfigLoader) MramOffset() int64 {
	if HardwareMemoryMap() {
		return 0x08000000
	}
	return 512 * 1024
}

func (this *ConfigLoader) MramSize() int64 {
	return 64 * 1024 * 1024
}

func (this *ConfigLoader) StackSize() int64 {
	return 2 * 1024
}

func (this *ConfigLoader) HeapSize() int64 {
	return 4 * 1024
}

func (this *ConfigLoader) NumGpRegisters() int {
	return 24
}

func (this *ConfigLoader) MaxNumTasklets() int {
	return 24
}
