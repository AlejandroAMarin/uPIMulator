package logic

import (
	"errors"
	"fmt"
	"math"
	"strconv"
	"strings"
	"uPIMulator/src/linker/kernel"
	"uPIMulator/src/misc"
)

type LinkerScript struct {
	command_line_parser *misc.CommandLineParser

	num_tasklets           int
	min_access_granularity int64

	linker_constants map[string]*LinkerConstant
}

func (this *LinkerScript) Init(command_line_parser *misc.CommandLineParser) {
	this.command_line_parser = command_line_parser

	this.num_tasklets = int(command_line_parser.IntParameter("num_tasklets"))
	this.min_access_granularity = command_line_parser.IntParameter("min_access_granularity")

	this.linker_constants = make(map[string]*LinkerConstant, 0)

	this.InitLinkerConstants()
}

func (this *LinkerScript) Assign(executable *kernel.Executable) {
	this.AssignAtomic(executable)
	this.AssignIram(executable)
	this.AssignWram(executable)
	this.AssignMram(executable)
}

func (this *LinkerScript) HasLinkerConstant(name string) bool {
	_, found := this.linker_constants[name]
	return found
}

func (this *LinkerScript) LinkerConstant(name string) *LinkerConstant {
	return this.linker_constants[name]
}

func (this *LinkerScript) InitLinkerConstants() {
	config_loader := new(misc.ConfigLoader)
	config_loader.Init()

	max_num_tasklets := config_loader.MaxNumTasklets()
	stack_size := config_loader.StackSize()

	this.linker_constants["NR_TASKLETS"] = new(LinkerConstant)
	this.linker_constants["NR_TASKLETS"].Init("NR_TASKLETS")
	this.linker_constants["NR_TASKLETS"].SetValue(int64(this.num_tasklets))

	for i := 0; i < max_num_tasklets; i++ {
		stack_size_tasklet := "STACK_SIZE_TASKLET_" + strconv.Itoa(i)

		this.linker_constants[stack_size_tasklet] = new(LinkerConstant)
		this.linker_constants[stack_size_tasklet].Init(stack_size_tasklet)
		this.linker_constants[stack_size_tasklet].SetValue(stack_size)
	}

	this.linker_constants["__atomic_start_addr"] = new(LinkerConstant)
	this.linker_constants["__atomic_start_addr"].Init("__atomic_start_addr")

	this.linker_constants["__atomic_used_addr"] = new(LinkerConstant)
	this.linker_constants["__atomic_used_addr"].Init("__atomic_used_addr")

	this.linker_constants["__atomic_end_addr"] = new(LinkerConstant)
	this.linker_constants["__atomic_end_addr"].Init("__atomic_end_addr")

	this.linker_constants["__rodata_start_addr"] = new(LinkerConstant)
	this.linker_constants["__rodata_start_addr"].Init("__rodata_start_addr")

	this.linker_constants["__rodata_end_addr"] = new(LinkerConstant)
	this.linker_constants["__rodata_end_addr"].Init("__rodata_end_addr")

	for i := 0; i < max_num_tasklets; i++ {
		sys_stack_thread := "__sys_stack_thread_" + strconv.Itoa(i)

		this.linker_constants[sys_stack_thread] = new(LinkerConstant)
		this.linker_constants[sys_stack_thread].Init(sys_stack_thread)
	}

	this.linker_constants["__sw_cache_buffer"] = new(LinkerConstant)
	this.linker_constants["__sw_cache_buffer"].Init("__sw_cache_buffer")

	this.linker_constants["__sys_heap_pointer_reset"] = new(LinkerConstant)
	this.linker_constants["__sys_heap_pointer_reset"].Init("__sys_heap_pointer_reset")

	this.linker_constants["__sys_used_mram_end"] = new(LinkerConstant)
	this.linker_constants["__sys_used_mram_end"].Init("__sys_used_mram_end")
}

func (this *LinkerScript) AssignAtomic(executable *kernel.Executable) {
	config_loader := new(misc.ConfigLoader)
	config_loader.Init()

	cur_address := config_loader.AtomicOffset()

	this.linker_constants["__atomic_start_addr"].SetValue(cur_address)

	this.linker_constants["__atomic_used_addr"].SetValue(cur_address)

	for section, _ := range executable.Sections(kernel.ATOMIC) {
		section.SetAddress(cur_address)
		cur_address += section.Size()
	}

	this.linker_constants["__atomic_end_addr"].SetValue(cur_address)

	if cur_address >= config_loader.AtomicOffset()+config_loader.AtomicSize() {
		err := errors.New("address is larger than the atomic end address")
		panic(err)
	}
}

func (this *LinkerScript) AssignIram(executable *kernel.Executable) {
	config_loader := new(misc.ConfigLoader)
	config_loader.Init()

	cur_address := config_loader.IramOffset()

	bootstrap := executable.Section(kernel.TEXT, "__bootstrap")
	if bootstrap == nil {
		err := errors.New("bootstrap is not found")
		panic(err)
	}

	bootstrap.SetAddress(cur_address)
	cur_address += bootstrap.Size()

	text_default := executable.Section(kernel.TEXT, "")
	if text_default != nil {
		text_default.SetAddress(cur_address)
		cur_address += text_default.Size()
	}

	for section, _ := range executable.Sections(kernel.TEXT) {
		if section.Name() != "__bootstrap" && section.Name() != "" {
			section.SetAddress(cur_address)
			cur_address += section.Size()
		}
	}

	if cur_address >= config_loader.IramOffset()+config_loader.IramSize() {
		// Say by how much. uPIMulator pulls in whole SDK objects where the
		// real linker drops the functions nobody calls, so a kernel that fits
		// the machine can still overflow here, and the first thing anyone
		// needs to know is whether it is over by a little or by a lot.
		used := cur_address - config_loader.IramOffset()
		width := int64(config_loader.IramDataWidth() / 8)
		err := fmt.Errorf(
			"IRAM overflow: the program needs %d instructions and there is room for %d",
			used/width,
			config_loader.IramSize()/width,
		)
		panic(err)
	}
}

func (this *LinkerScript) AssignWram(executable *kernel.Executable) {
	config_loader := new(misc.ConfigLoader)
	config_loader.Init()

	cur_address := config_loader.WramOffset()

	sys_zero := executable.Section(kernel.DATA, "__sys_zero")
	if sys_zero != nil {
		sys_zero.SetAddress(cur_address)
		cur_address += sys_zero.Size()
	}

	immediate_memory := executable.Section(kernel.DATA, "immediate_memory")
	if immediate_memory != nil {
		immediate_memory.SetAddress(cur_address)
		cur_address += immediate_memory.Size()
	}

	for section, _ := range executable.Sections(kernel.DATA) {
		if strings.Contains(section.Name(), "immediate_memory.") {
			section.SetAddress(cur_address)
			cur_address += section.Size()
		}
	}

	this.linker_constants["__rodata_start_addr"].SetValue(cur_address)

	rodata_default := executable.Section(kernel.RODATA, "")
	if rodata_default != nil {
		rodata_default.SetAddress(cur_address)
		cur_address += rodata_default.Size()
	}

	for section, _ := range executable.Sections(kernel.RODATA) {
		if section.Name() != "" {
			section.SetAddress(cur_address)
			cur_address += section.Size()
		}
	}

	this.linker_constants["__rodata_end_addr"].SetValue(cur_address)

	bss_default := executable.Section(kernel.BSS, "")
	if bss_default != nil {
		bss_default.SetAddress(cur_address)
		cur_address += bss_default.Size()
	}

	for section, _ := range executable.Sections(kernel.BSS) {
		if section.Name() != "" {
			section.SetAddress(cur_address)
			cur_address += section.Size()
		}
	}

	sys_keep := executable.Section(kernel.DATA, "__sys_keep")
	if sys_keep != nil {
		sys_keep.SetAddress(cur_address)
		cur_address += sys_keep.Size()
	}

	data_default := executable.Section(kernel.DATA, "")
	if data_default != nil {
		data_default.SetAddress(cur_address)
		cur_address += data_default.Size()
	}

	for section, _ := range executable.Sections(kernel.DATA) {
		if section.Name() != "__sys_zero" &&
			section.Name() != "__sys_keep" &&
			!strings.Contains(section.Name(), "immediate_memory") &&
			section.Name() != "" {
			section.SetAddress(cur_address)
			cur_address += section.Size()
		}
	}

	// TODO(bongjoon.hyun@gmail.com): figure out ".data.__sys_host" section

	dpu_host := executable.Section(kernel.DPU_HOST, "")
	if dpu_host != nil {
		dpu_host.SetAddress(cur_address)
		cur_address += dpu_host.Size()
	}

	// TODO(bongjoon.hyun@gmail.com): figure out ".data.__sys_profilng" section

	// TODO(bongjoon.hyun@gmail.com): figure out ".data.stacks" section

	for i := 0; i < config_loader.MaxNumTasklets(); i++ {
		sys_stack_thread := "__sys_stack_thread_" + strconv.Itoa(i)
		this.linker_constants[sys_stack_thread].SetValue(cur_address)

		stack_size_tasklet := "STACK_SIZE_TASKLET_" + strconv.Itoa(i)
		cur_address += this.linker_constants[stack_size_tasklet].Value()
	}

	// TODO(bongjoon.hyun@gmail.com): figure out ".data.sw_cache" section

	this.linker_constants["__sw_cache_buffer"].SetValue(cur_address)
	cur_address += int64(8 * config_loader.MaxNumTasklets())

	// TODO(bongjoon.hyun@gmail.com): figure out ".data.heap_pointer_reset" section

	cur_address = int64(
		math.Ceil(float64(cur_address)/float64(this.min_access_granularity)),
	) * int64(
		this.min_access_granularity,
	)
	this.linker_constants["__sys_heap_pointer_reset"].SetValue(cur_address)

	if cur_address >= config_loader.WramOffset()+config_loader.WramSize() {
		err := errors.New("address is larger than the WRAM end address")
		panic(err)
	}
}

func (this *LinkerScript) AssignMram(executable *kernel.Executable) {
	config_loader := new(misc.ConfigLoader)
	config_loader.Init()

	cur_address := config_loader.MramOffset()

	// Debug and stack-size sections are metadata. The machine's linker marks
	// them not-allocated and they occupy no memory there at all, but this one
	// used to lay them at the bottom of MRAM, ahead of the program's own data.
	// That cost BIMSA 104 bytes and, worse, pushed the heap off the boundary
	// the machine puts it on: 1 MB + 104 instead of 1 MB. Every buffer the
	// program then allocated straddled the 1 KB wordlines differently from the
	// machine, which is worth a few tenths of a percent on a kernel that moves
	// much data. Park them one past the end of MRAM, where Executable.Sort
	// leaves them out of the image and they push nothing.
	metadata_address := config_loader.MramOffset() + config_loader.MramSize()
	metadata := []kernel.SectionName{
		kernel.DEBUG_ABBREV,
		kernel.DEBUG_FRAME,
		kernel.DEBUG_INFO,
		kernel.DEBUG_LINE,
		kernel.DEBUG_LOC,
		kernel.DEBUG_RANGES,
		kernel.DEBUG_STR,
		kernel.STACK_SIZES,
	}

	for _, section_name := range metadata {
		for section := range executable.Sections(section_name) {
			section.SetAddress(metadata_address)
		}
	}

	noinit := executable.Section(kernel.MRAM, "noinit")
	if noinit != nil {
		noinit.SetAddress(cur_address)
		cur_address += noinit.Size()
	}

	for section, _ := range executable.Sections(kernel.MRAM) {
		if strings.Contains(section.Name(), "noinit.") &&
			!strings.Contains(section.Name(), "noinit.keep") {
			section.SetAddress(cur_address)
			cur_address += section.Size()
		}
	}

	noinit_keep := executable.Section(kernel.MRAM, "noinit.keep")
	if noinit_keep != nil {
		noinit_keep.SetAddress(cur_address)
		cur_address += noinit_keep.Size()
	}

	for section, _ := range executable.Sections(kernel.MRAM) {
		if strings.Contains(section.Name(), "noinit.keep") &&
			!strings.Contains(section.Name(), "noinit.keep.") {
			section.SetAddress(cur_address)
			cur_address += section.Size()
		}
	}

	for section, _ := range executable.Sections(kernel.MRAM) {
		if strings.Contains(section.Name(), "noinit.keep.") {
			section.SetAddress(cur_address)
			cur_address += section.Size()
		}
	}

	mram_default := executable.Section(kernel.MRAM, "")
	if mram_default != nil {
		mram_default.SetAddress(cur_address)
		cur_address += mram_default.Size()
	}

	for section, _ := range executable.Sections(kernel.MRAM) {
		if section.Name() != "" &&
			!strings.Contains(section.Name(), "noinit") &&
			!strings.Contains(section.Name(), "keep") {
			section.SetAddress(cur_address)
			cur_address += section.Size()
		}
	}

	for section, _ := range executable.Sections(kernel.MRAM) {
		if strings.Contains(section.Name(), "keep") &&
			!strings.Contains(section.Name(), "keep.") {
			section.SetAddress(cur_address)
			cur_address += section.Size()
		}
	}

	for section, _ := range executable.Sections(kernel.MRAM) {
		if strings.Contains(section.Name(), "keep.") {
			section.SetAddress(cur_address)
			cur_address += section.Size()
		}
	}

	cur_address = int64(
		math.Ceil(float64(cur_address)/float64(this.min_access_granularity)),
	) * this.min_access_granularity

	this.linker_constants["__sys_used_mram_end"].SetValue(cur_address)

	if cur_address >= config_loader.MramOffset()+config_loader.MramSize() {
		err := errors.New("address is larger than the MRAM end address")
		panic(err)
	}
}

// Values returns the linker constants by name, so a caller can take them
// directly instead of reading back what DumpValues wrote.
func (this *LinkerScript) Values() map[string]int64 {
	values := make(map[string]int64, len(this.linker_constants))

	for _, linker_constant := range this.linker_constants {
		values[linker_constant.Name()] = linker_constant.Value()
	}

	return values
}

func (this *LinkerScript) DumpValues(path string) {
	lines := make([]string, 0)

	for _, linker_constant := range this.linker_constants {
		line := fmt.Sprintf("%s: %d", linker_constant.Name(), linker_constant.Value())
		lines = append(lines, line)
	}

	file_dumper := new(misc.FileDumper)
	file_dumper.Init(path)
	file_dumper.WriteLines(lines)
}
