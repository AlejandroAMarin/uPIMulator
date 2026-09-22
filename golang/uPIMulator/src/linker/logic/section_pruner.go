package logic

import (
	"errors"
	"fmt"
	"strings"
	"uPIMulator/src/linker/kernel"
	"uPIMulator/src/linker/lexer"
	"uPIMulator/src/linker/parser"
	"uPIMulator/src/linker/parser/expr"
	"uPIMulator/src/linker/parser/stmt"
)

// SectionPruner finds the code nothing can reach.
//
// uPIMulator resolves symbols one object file at a time: if a program needs
// any symbol an object defines, every function in that object is linked. The
// SDK compiles each function into its own section and the real linker drops
// the ones nobody calls, so a program that fits the machine's 4096-instruction
// IRAM can still overflow here. BIMSA's gap-affine kernel is 3795 instructions
// on this machine and was 6522 without this: the whole edit-distance path,
// printf and the stdio chain, and a 630-instruction breakpoint search that the
// build does not call.
//
// The rule is the one --gc-sections uses. A section is live if it is not code,
// or if it is the entry, or if a live section names a symbol it defines.
// Everything else goes.
//
// Three things have to be got right, and each of them, got wrong, either keeps
// everything or throws away something needed:
//
//   - A section referring to its own symbols says nothing about whether
//     anything needs it: `.size f, .Lend-f` sits inside f's own section. Worse,
//     every function has a `.Lfunc_end0`, so without resolving a reference
//     within its own section first the first function to define one is kept
//     alive by every other function in the program. See the own map.
//   - `.addrsig_sym` must not count. That list names every symbol whose address
//     is taken anywhere, in one block at the end of a file, and it tells a
//     linker which functions are unsafe to fold together. Counting it keeps all
//     of them. The real linker ignores it here too.
//   - A symbol that is not global belongs to the file it is written in. The
//     linker renames those to "<file>.<name>" for that reason and so does
//     Symbol, or a jump table would fail to keep alive the function in its own
//     file that it points into.
type SectionPruner struct {
	walker *parser.Walker

	cur        sectionKey
	in_addrsig bool
	scope      string
	globals    map[string]bool

	definitions map[string]sectionKey
	own         map[sectionKey]map[string]bool
	references  map[sectionKey]map[string]bool
	sections    map[sectionKey]bool
}

type sectionKey struct {
	is_text bool
	name    string
}

func (this *SectionPruner) Init() {
	this.cur = sectionKey{is_text: true, name: ""}
	this.in_addrsig = false
	this.globals = make(map[string]bool)
	this.definitions = make(map[string]sectionKey)
	this.own = make(map[sectionKey]map[string]bool)
	this.references = make(map[sectionKey]map[string]bool)
	this.sections = make(map[sectionKey]bool)
	this.sections[this.cur] = true

	this.walker = new(parser.Walker)
	this.walker.Init()

	this.walker.RegisterStmtCallback(stmt.SECTION_IDENTIFIER, this.WalkSectionIdentifierStmt)
	this.walker.RegisterStmtCallback(
		stmt.SECTION_IDENTIFIER_NUMBER,
		this.WalkSectionIdentifierNumberStmt,
	)
	this.walker.RegisterStmtCallback(stmt.SECTION_STRING, this.WalkSectionStringStmt)
	this.walker.RegisterStmtCallback(stmt.SECTION_STRING_NUMBER, this.WalkSectionStringNumberStmt)
	this.walker.RegisterStmtCallback(stmt.SECTION_STACK_SIZES, this.WalkSectionStackSizesStmt)
	this.walker.RegisterStmtCallback(stmt.TEXT, this.WalkTextStmt)
	this.walker.RegisterStmtCallback(stmt.ADDRSIG, this.WalkAddrsigStmt)
	this.walker.RegisterStmtCallback(stmt.LABEL, this.WalkLabelStmt)
	this.walker.RegisterStmtCallback(stmt.SET, this.WalkSetStmt)
	this.walker.RegisterExprCallback(expr.PRIMARY, this.WalkPrimaryExpr)
}

// AddGlobals records the symbols one relocatable offers to the others. Call it
// for every relocatable before any call to Analyze: a file referring to a
// global does not itself declare it, so whether a name is global can only be
// known once they have all been read.
func (this *SectionPruner) AddGlobals(relocatable *kernel.Relocatable) {
	for symbol := range relocatable.Liveness().GlobalSymbols() {
		this.globals[symbol] = true
	}
}

// Analyze reads one relocatable. Call it for the benchmark and for every SDK
// object the executable takes.
func (this *SectionPruner) Analyze(relocatable *kernel.Relocatable) {
	this.scope = relocatable.Name()

	this.Enter(sectionKey{is_text: true, name: ""})
	this.walker.Walk(relocatable.Ast())
}

// DeadTextSections names the code sections nothing reaches. The names are the
// ones a `.section .text.<name>` directive carries.
func (this *SectionPruner) DeadTextSections() map[string]bool {
	live := make(map[sectionKey]bool)
	queue := make([]sectionKey, 0)

	// Roots: everything that is not code, because data is not what overflows
	// IRAM and following it is what keeps a function whose address sits in a
	// table; the default text section; and the entry point.
	for section := range this.sections {
		if !section.is_text || section.name == "" || section.name == "__bootstrap" {
			live[section] = true
			queue = append(queue, section)
		}
	}

	for len(queue) > 0 {
		section := queue[0]
		queue = queue[1:]

		for symbol := range this.references[section] {
			// A section that defines the symbol itself is talking about its
			// own code, which says nothing about whether anything needs it.
			// Every function has a ".Lfunc_end0" and names it in its own
			// ".size" directive, so without this the first function to define
			// one is kept alive by every other function in the program.
			if this.own[section][symbol] {
				continue
			}

			definition, found := this.definitions[symbol]
			if !found || live[definition] {
				continue
			}

			live[definition] = true
			queue = append(queue, definition)
		}
	}

	dead := make(map[string]bool)
	for section := range this.sections {
		if section.is_text && !live[section] {
			dead[section.name] = true
		}
	}

	return dead
}

// TextSections names every code section the pruner saw, so the caller can
// check its own reading of the assembly against the parser's.
func (this *SectionPruner) TextSections() map[string]bool {
	names := make(map[string]bool)
	for section := range this.sections {
		if section.is_text {
			names[section.name] = true
		}
	}
	return names
}

func (this *SectionPruner) Enter(section sectionKey) {
	this.cur = section
	this.in_addrsig = false
	this.sections[section] = true
}

func (this *SectionPruner) WalkTextStmt(stmt_ *stmt.Stmt) {
	this.Enter(sectionKey{is_text: true, name: ""})
}

func (this *SectionPruner) WalkAddrsigStmt(stmt_ *stmt.Stmt) {
	this.in_addrsig = true
}

func (this *SectionPruner) WalkSectionIdentifierStmt(stmt_ *stmt.Stmt) {
	section_stmt := stmt_.SectionIdentifierStmt()
	this.Enter(this.SectionOf(section_stmt.Expr1(), section_stmt.Expr2()))
}

func (this *SectionPruner) WalkSectionIdentifierNumberStmt(stmt_ *stmt.Stmt) {
	section_stmt := stmt_.SectionIdentifierNumberStmt()
	this.Enter(this.SectionOf(section_stmt.Expr1(), section_stmt.Expr2()))
}

func (this *SectionPruner) WalkSectionStringStmt(stmt_ *stmt.Stmt) {
	section_stmt := stmt_.SectionStringStmt()
	this.Enter(this.SectionOf(section_stmt.Expr1(), nil))
}

func (this *SectionPruner) WalkSectionStringNumberStmt(stmt_ *stmt.Stmt) {
	section_stmt := stmt_.SectionStringNumberStmt()
	this.Enter(this.SectionOf(section_stmt.Expr1(), nil))
}

func (this *SectionPruner) WalkSectionStackSizesStmt(stmt_ *stmt.Stmt) {
	// A stack-sizes block names the section it describes, and it refers to
	// that function's own labels. Attribute it to that section, or a block
	// describing dropped code would act as a root and keep the code alive --
	// or, worse, survive the drop and name a label that is no longer there.
	section_stmt := stmt_.SectionStackSizesStmt()

	kind := section_stmt.Expr2().SectionNameExpr().Token().TokenType()
	name := strings.TrimPrefix(this.IdentifierOf(section_stmt.Expr3()), ".")

	this.Enter(sectionKey{is_text: kind == lexer.TEXT, name: name})
}

func (this *SectionPruner) WalkLabelStmt(stmt_ *stmt.Stmt) {
	this.Define(this.IdentifierOf(stmt_.LabelStmt().Expr()))
}

func (this *SectionPruner) WalkSetStmt(stmt_ *stmt.Stmt) {
	this.Define(this.IdentifierOf(stmt_.SetStmt().Expr1()))
}

func (this *SectionPruner) Define(name string) {
	key := this.Symbol(name)

	if this.own[this.cur] == nil {
		this.own[this.cur] = make(map[string]bool)
	}
	this.own[this.cur][key] = true

	if _, found := this.definitions[key]; !found {
		this.definitions[key] = this.cur
	}
}

// Symbol names one symbol the way the linker does. A symbol that is not
// global belongs to the file it is written in -- every file has a ".LBB0_1" --
// and the linker renames those to "<file>.<name>" for exactly that reason. A
// pruner that did not would both keep code alive from an unrelated file's
// label and, worse, fail to keep alive the function a jump table in its own
// file points into.
func (this *SectionPruner) Symbol(name string) string {
	if this.globals[name] {
		return name
	}
	return this.scope + "|" + name
}

func (this *SectionPruner) WalkPrimaryExpr(expr_ *expr.Expr) {
	if this.in_addrsig {
		return
	}

	token := expr_.PrimaryExpr().Token()
	if token.TokenType() != lexer.IDENTIFIER {
		return
	}

	if this.references[this.cur] == nil {
		this.references[this.cur] = make(map[string]bool)
	}
	this.references[this.cur][this.Symbol(token.Attribute())] = true
}

func (this *SectionPruner) SectionOf(kind_expr *expr.Expr, name_expr *expr.Expr) sectionKey {
	token := kind_expr.SectionNameExpr().Token()

	name := ""
	if name_expr != nil {
		name = strings.TrimPrefix(this.IdentifierOf(name_expr), ".")
	}

	if token.TokenType() == lexer.TEXT {
		return sectionKey{is_text: true, name: name}
	}

	// Any other kind is data of some sort. Its own name is enough to keep the
	// keys apart, and the kind token's text keeps two different kinds with the
	// same name apart.
	return sectionKey{is_text: false, name: fmt.Sprintf("%d.%s", token.TokenType(), name)}
}

func (this *SectionPruner) IdentifierOf(expr_ *expr.Expr) string {
	primary_expr := expr_.ProgramCounterExpr().Expr().PrimaryExpr()

	token := primary_expr.Token()
	if token.TokenType() != lexer.IDENTIFIER {
		err := errors.New("token type is not identifier")
		panic(err)
	}

	return token.Attribute()
}
