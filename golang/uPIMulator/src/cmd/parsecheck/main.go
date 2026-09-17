// Command parsecheck reports the first source line of an assembly file that the
// linker's grammar cannot accept. The linker itself only says "stack are not
// stmts" after building the whole SDK, which is far too slow to bisect against.
//
//	go run ./cmd/parsecheck <file.S>
package main

import (
	"fmt"
	"os"
	"strings"
	"uPIMulator/src/linker/lexer"
	"uPIMulator/src/linker/parser"
)

// parses reports whether the first n lines of the file parse as statements.
func parses(lines []string, n int, scratch string) (ok bool, reason string) {
	if err := os.WriteFile(scratch, []byte(strings.Join(lines[:n], "\n")+"\n"), 0o644); err != nil {
		panic(err)
	}

	defer func() {
		if r := recover(); r != nil {
			ok = false
			reason = fmt.Sprint(r)
		}
	}()

	lexer_ := new(lexer.Lexer)
	lexer_.Init()

	parser_ := new(parser.Parser)
	parser_.Init()
	parser_.Parse(lexer_.Lex(scratch))

	return true, ""
}

func main() {
	if len(os.Args) != 2 {
		fmt.Fprintln(os.Stderr, "usage: parsecheck <file.S>")
		os.Exit(2)
	}

	raw, err := os.ReadFile(os.Args[1])
	if err != nil {
		fmt.Fprintln(os.Stderr, err)
		os.Exit(1)
	}

	lines := strings.Split(strings.TrimRight(string(raw), "\n"), "\n")
	scratch := os.Args[1] + ".parsecheck"
	defer os.Remove(scratch)

	if ok, _ := parses(lines, len(lines), scratch); ok {
		fmt.Println("parses cleanly")
		return
	}

	// The failure is somewhere; find the shortest prefix that shows it.
	bad := -1
	for n := 1; n <= len(lines); n++ {
		if ok, reason := parses(lines, n, scratch); !ok {
			bad = n
			fmt.Printf("line %d: %s\n  %s\n", n, reason, lines[n-1])
			break
		}
	}

	if bad == -1 {
		fmt.Println("no single prefix reproduces it")
	}
	os.Exit(1)
}
