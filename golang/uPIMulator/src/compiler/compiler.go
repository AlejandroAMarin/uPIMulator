package compiler

import (
	"fmt"
	"os/exec"
	"path/filepath"
	"strconv"
	"uPIMulator/src/misc"
)

type Compiler struct {
	command_line_parser *misc.CommandLineParser

	root_dirpath string
	benchmark    string

	dpu_cc string

	num_dpus     int
	num_tasklets int
}

func (this *Compiler) Init(command_line_parser *misc.CommandLineParser) {
	this.command_line_parser = command_line_parser

	this.root_dirpath = command_line_parser.StringParameter("root_dirpath")
	this.benchmark = command_line_parser.StringParameter("benchmark")

	this.dpu_cc = command_line_parser.StringParameter("dpu_cc")

	num_channels := int(command_line_parser.IntParameter("num_channels"))
	num_ranks_per_channel := int(command_line_parser.IntParameter("num_ranks_per_channel"))
	num_dpus_per_rank := int(command_line_parser.IntParameter("num_dpus_per_rank"))
	this.num_dpus = num_channels * num_ranks_per_channel * num_dpus_per_rank

	this.num_tasklets = int(command_line_parser.IntParameter("num_tasklets"))
}

func (this *Compiler) Compile() {
	// An external kernel is compiled by whoever supplied it, so only the DPU
	// runtime has to be built here.
	if this.command_line_parser.StringParameter("benchmark_asm") == "" {
		this.CompileBenchmark()
	}

	this.CompileSdk()
}

func (this *Compiler) CompileBenchmark() {
	this.Run(
		filepath.Join(this.root_dirpath, "benchmark", "build.py"),
		"--num_dpus",
		strconv.Itoa(this.num_dpus),
		"--num_tasklets",
		strconv.Itoa(this.num_tasklets),
		"--dpu_cc",
		this.dpu_cc,
	)
}

func (this *Compiler) CompileSdk() {
	this.Run(
		filepath.Join(this.root_dirpath, "sdk", "build.py"),
		"--num_tasklets",
		strconv.Itoa(this.num_tasklets),
		"--dpu_cc",
		this.dpu_cc,
	)
}

// Run builds one part of the DPU code with the UPMEM SDK compiler on this host.
// The build scripts ran in a Docker image before. The image pinned SDK 2023.2.0
// and is no longer available, so the scripts now run directly.
func (this *Compiler) Run(script_path string, args ...string) {
	fmt.Printf("Compiling with %s...\n", script_path)

	command := exec.Command("python3", append([]string{script_path}, args...)...)

	// The build writes the assembly that the linker reads next. A silent failure
	// here shows up much later as an unrelated panic in the linker, so report the
	// compiler output instead of only the exit code.
	output, err := command.CombinedOutput()

	if err != nil {
		panic(fmt.Errorf("%s failed: %w\n%s", script_path, err, string(output)))
	}
}
