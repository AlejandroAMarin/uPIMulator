package kernel

import (
	"errors"
	"fmt"
	"sort"
	"strings"
	"uPIMulator/src/abi/encoding"
	"uPIMulator/src/linker/lexer"
	"uPIMulator/src/linker/parser"
	"uPIMulator/src/misc"
)

type Executable struct {
	name string
	path string

	benchmark_relocatable *Relocatable
	sdk_relocatables      map[*Relocatable]bool

	token_stream *lexer.TokenStream
	ast          *parser.Ast
	liveness     *Liveness

	dead_sections map[string]bool
	all_sections  map[string]bool

	sections    map[*Section]bool
	cur_section *Section
}

func (this *Executable) Init(name string) {
	this.name = name

	this.sdk_relocatables = make(map[*Relocatable]bool, 0)

	this.liveness = new(Liveness)
	this.liveness.Init()

	this.sections = make(map[*Section]bool, 0)
}

func (this *Executable) Name() string {
	return this.name
}

func (this *Executable) Path() string {
	return this.path
}

func (this *Executable) SetPath(path string) {
	this.path = path
}

func (this *Executable) SetBenchmarkRelocatable(relocatable *Relocatable) {
	this.benchmark_relocatable = relocatable

	this.UpdateUnresolvedSymbols(relocatable)
}

// SdkRelocatables are the SDK objects the executable takes, which is what the
// section pruner has to read besides the benchmark's own.
func (this *Executable) SdkRelocatables() map[*Relocatable]bool {
	return this.sdk_relocatables
}

func (this *Executable) AddSdkRelocatable(relocatable *Relocatable) {
	this.sdk_relocatables[relocatable] = true

	this.UpdateLocalSymbols(relocatable)
	this.UpdateUnresolvedSymbols(relocatable)
}

func (this *Executable) TokenStream() *lexer.TokenStream {
	return this.token_stream
}

func (this *Executable) SetTokenStream(token_stream *lexer.TokenStream) {
	this.token_stream = token_stream
}

func (this *Executable) Ast() *parser.Ast {
	return this.ast
}

func (this *Executable) SetAst(ast *parser.Ast) {
	this.ast = ast
}

func (this *Executable) Liveness() *Liveness {
	return this.liveness
}

func (this *Executable) DumpAssembly() {
	lines := this.DropDeadSections(
		this.benchmark_relocatable.Name(),
		this.benchmark_relocatable.Lines(),
	)

	for sdk_relocatable, _ := range this.sdk_relocatables {
		lines = append(
			lines,
			this.DropDeadSections(sdk_relocatable.Name(), sdk_relocatable.Lines())...,
		)
	}

	file_dumper := new(misc.FileDumper)
	file_dumper.Init(this.path)
	file_dumper.WriteLines(lines)
}

// SetDeadSections names the sections to leave out, and the sections there are,
// so that this file's reading of a section directive can be checked against
// the parser's. See logic.SectionPruner.
func (this *Executable) SetDeadSections(dead map[string]bool, all map[string]bool) {
	this.dead_sections = dead
	this.all_sections = all
}

// DropDeadSections removes the lines of the sections nothing reaches. It takes
// one object file at a time, because a data section such as ".mram.noinit" is
// one output section built from a piece of every object that puts something
// there, and each piece goes or stays on its own.
//
// The executable is dumped as text and read back, so this is the one place a
// section can be left out without every later stage having to know. Finding
// where a section begins needs a second reading of the assembly, which is a
// thing to be careful with: the check below fails the link rather than let a
// misreading quietly drop something that was needed.
func (this *Executable) DropDeadSections(scope string, lines []string) []string {
	if this.dead_sections == nil || len(this.dead_sections) == 0 {
		return lines
	}

	kept := make([]string, 0, len(lines))
	seen := make(map[string]bool)
	drop := false

	for _, line := range lines {
		if key, tracked, is_section := sectionOfLine(scope, line); is_section {
			if tracked {
				seen[key] = true
				drop = this.dead_sections[key]
			} else {
				drop = false
			}
		}

		if !drop {
			kept = append(kept, line)
		}
	}

	for key := range seen {
		if !this.all_sections[key] {
			err := fmt.Errorf(
				"the assembly has a section %q that the parser did not report; "+
					"reading section directives and parsing them disagree, so nothing was dropped",
				key,
			)
			panic(err)
		}
	}

	return kept
}

// PrunableSectionKinds are the data section kinds DropDeadSections is able to
// leave out. logic.SectionPruner decides which sections those are, and it
// checks this list against its own: the two read the same assembly by
// different routes, and a kind in one list and not in the other makes them
// disagree about what a section is called.
var PrunableSectionKinds = []string{"rodata", "mram"}

// sectionOfLine reports the section a directive opens: the name both this file
// and the pruner give it, whether it is a kind the pruner tracks, and whether
// the line was a section directive at all. The forms are "\t.text" and
// "\t.section\t.text.<name>,...", as clang emits them.
//
// Keep this in step with logic.sectionKey.Key and logic.prunable_kinds.
func sectionOfLine(scope string, line string) (string, bool, bool) {
	trimmed := strings.TrimSpace(line)

	if trimmed == ".text" {
		return "text.", true, true
	}

	if !strings.HasPrefix(trimmed, ".section") {
		return "", false, false
	}

	fields := strings.Fields(trimmed)
	if len(fields) < 2 {
		return "", false, true
	}

	operands := strings.Split(fields[1], ",")
	name := operands[0]

	// A stack-sizes block belongs to the section named in its fourth operand.
	// It refers to that function's own labels, so it has to go when the
	// function goes.
	if name == ".stack_sizes" && len(operands) >= 4 {
		name = operands[3]
	}

	if name == ".text" {
		return "text.", true, true
	}

	if strings.HasPrefix(name, ".text.") {
		return "text." + sectionName(scope, strings.TrimPrefix(name, ".text.")), true, true
	}

	for _, kind := range PrunableSectionKinds {
		if strings.HasPrefix(name, "."+kind+".") {
			return scope + "|" + kind + "." +
				sectionName(scope, strings.TrimPrefix(name, "."+kind+".")), true, true
		}
	}

	return name, false, true
}

// sectionName takes the scope back off a section's name. Relocatable.Lines
// puts the object file's name in front of every local symbol, and a section
// directive that names one -- ".section .text.f" for a static f -- is rewritten
// with the rest: the parser reads "f" from the object and this reads
// "<file>.f". A name that is not a symbol, such as ".rodata.str1.1", is left
// alone, so the scope only comes off when it is there.
func sectionName(scope string, name string) string {
	return strings.TrimPrefix(name, scope+".")
}

func (this *Executable) DumpAddresses(path string) {
	lines := make([]string, 0)

	for label_name, label_address := range this.Addresses() {
		line := fmt.Sprintf("%s: %d", label_name, label_address)
		lines = append(lines, line)
	}

	file_dumper := new(misc.FileDumper)
	file_dumper.Init(path)
	file_dumper.WriteLines(lines)
}

func (this *Executable) DumpAtomic(path string) {
	atomic_byte_stream := this.AtomicByteStream()

	lines := make([]string, 0)
	for i := int64(0); i < atomic_byte_stream.Size(); i++ {
		line := fmt.Sprintf("%d", atomic_byte_stream.Get(int(i)))

		lines = append(lines, line)
	}

	file_dumper := new(misc.FileDumper)
	file_dumper.Init(path)
	file_dumper.WriteLines(lines)
}

func (this *Executable) DumpIram(path string) {
	iram_byte_stream := this.IramByteStream()

	lines := make([]string, 0)
	for i := int64(0); i < iram_byte_stream.Size(); i++ {
		line := fmt.Sprintf("%d", iram_byte_stream.Get(int(i)))

		lines = append(lines, line)
	}

	file_dumper := new(misc.FileDumper)
	file_dumper.Init(path)
	file_dumper.WriteLines(lines)
}

func (this *Executable) DumpWram(path string) {
	wram_byte_stream := this.WramByteStream()

	lines := make([]string, 0)
	for i := int64(0); i < wram_byte_stream.Size(); i++ {
		line := fmt.Sprintf("%d", wram_byte_stream.Get(int(i)))

		lines = append(lines, line)
	}

	file_dumper := new(misc.FileDumper)
	file_dumper.Init(path)
	file_dumper.WriteLines(lines)
}

func (this *Executable) DumpMram(path string) {
	mram_byte_stream := this.MramByteStream()

	lines := make([]string, 0)
	for i := int64(0); i < mram_byte_stream.Size(); i++ {
		line := fmt.Sprintf("%d", mram_byte_stream.Get(int(i)))

		lines = append(lines, line)
	}

	file_dumper := new(misc.FileDumper)
	file_dumper.Init(path)
	file_dumper.WriteLines(lines)
}

func (this *Executable) Section(section_name SectionName, name string) *Section {
	for section, _ := range this.sections {
		if section.SectionName() == section_name && section.Name() == name {
			return section
		}
	}
	return nil
}

func (this *Executable) Sections(section_name SectionName) map[*Section]bool {
	sections := make(map[*Section]bool, 0)
	for section, _ := range this.sections {
		if section.SectionName() == section_name {
			sections[section] = true
		}
	}
	return sections
}

func (this *Executable) AddSection(
	section_name SectionName,
	name string,
	section_flags map[SectionFlag]bool,
	section_type SectionType,
) {
	if this.Section(section_name, name) == nil {
		section := new(Section)
		section.Init(section_name, name, section_flags, section_type)
		this.sections[section] = true
	}
}

func (this *Executable) CurSection() *Section {
	return this.cur_section
}

func (this *Executable) CheckoutSection(section_name SectionName, name string) {
	if section := this.Section(section_name, name); section != nil {
		this.cur_section = section
	} else {
		err := errors.New("section is not found")
		panic(err)
	}
}

func (this *Executable) Label(label_name string) *Label {
	var label *Label = nil
	for section, _ := range this.sections {
		section_label := section.Label(label_name)

		if section_label != nil {
			if label != nil {
				err := errors.New("labels are duplicated")
				panic(err)
			}

			label = section_label
		}
	}
	return label
}

func (this *Executable) Addresses() map[string]int64 {
	addresses := make(map[string]int64, 0)
	for section, _ := range this.sections {
		for _, label := range section.Labels() {
			addresses[label.Name()] = label.Address()
		}
	}
	return addresses
}

func (this *Executable) AtomicByteStream() *encoding.ByteStream {
	config_loader := new(misc.ConfigLoader)
	config_loader.Init()

	atomic_sections := this.Sort(
		config_loader.AtomicOffset(),
		config_loader.AtomicOffset()+config_loader.AtomicSize(),
	)

	byte_stream := new(encoding.ByteStream)
	byte_stream.Init()

	for _, atomic_section := range atomic_sections {
		byte_stream.Merge(atomic_section.ToByteStream())
	}

	return byte_stream
}

func (this *Executable) IramByteStream() *encoding.ByteStream {
	config_loader := new(misc.ConfigLoader)
	config_loader.Init()

	iram_sections := this.Sort(
		config_loader.IramOffset(),
		config_loader.IramOffset()+config_loader.IramSize(),
	)

	byte_stream := new(encoding.ByteStream)
	byte_stream.Init()

	for _, iram_section := range iram_sections {
		byte_stream.Merge(iram_section.ToByteStream())
	}

	return byte_stream
}

func (this *Executable) WramByteStream() *encoding.ByteStream {
	config_loader := new(misc.ConfigLoader)
	config_loader.Init()

	wram_sections := this.Sort(
		config_loader.WramOffset(),
		config_loader.WramOffset()+config_loader.WramSize(),
	)

	byte_stream := new(encoding.ByteStream)
	byte_stream.Init()

	for _, wram_section := range wram_sections {
		byte_stream.Merge(wram_section.ToByteStream())
	}

	return byte_stream
}

func (this *Executable) MramByteStream() *encoding.ByteStream {
	config_loader := new(misc.ConfigLoader)
	config_loader.Init()

	mram_sections := this.Sort(
		config_loader.MramOffset(),
		config_loader.MramOffset()+config_loader.MramSize(),
	)

	byte_stream := new(encoding.ByteStream)
	byte_stream.Init()

	for _, mram_section := range mram_sections {
		byte_stream.Merge(mram_section.ToByteStream())
	}

	return byte_stream
}

func (this *Executable) UpdateLocalSymbols(relocatable *Relocatable) {
	for old_name, _ := range relocatable.Liveness().LocalSymbols() {
		new_name := relocatable.Name() + "." + old_name

		relocatable.RenameLocalSymbol(old_name, new_name)
	}
}

func (this *Executable) UpdateUnresolvedSymbols(relocatable *Relocatable) {
	for def, _ := range relocatable.Liveness().Defs() {
		this.liveness.AddDef(def)
	}

	for use, _ := range relocatable.Liveness().Uses() {
		this.liveness.AddUse(use)
	}

	for global_symbol, _ := range relocatable.Liveness().GlobalSymbols() {
		this.liveness.AddGlobalSymbol(global_symbol)
	}
}

func (this *Executable) Sort(begin_address int64, end_address int64) []*Section {
	sections := make([]*Section, 0)

	for section, _ := range this.sections {
		address := section.Address()

		if begin_address <= address && address < end_address {
			sections = append(sections, section)
		}
	}

	sort_fn := func(i int, j int) bool {
		return sections[i].Address() < sections[j].Address()
	}

	sort.Slice(sections, sort_fn)

	return sections
}
