// RUN: rm -f file-io-test.txt file-io-test-mcd.txt
// RUN: arcilator %s --run --jit-entry=Top_main 2> %t.stderr | FileCheck %s --check-prefix=STDOUT
// RUN: FileCheck %s --check-prefix=STDERR --input-file=%t.stderr
// RUN: FileCheck %s --check-prefix=FILE --input-file=file-io-test.txt
// RUN: FileCheck %s --check-prefix=MCD --input-file=file-io-test-mcd.txt
// REQUIRES: arcilator-jit

// Output written through `$fwrite` reaches stdout, stderr, or files opened
// with `$fopen`, depending on the descriptor.

// STDOUT: to stdout
// STDERR: to stderr
// FILE: to file
// FILE-NEXT: appended to file
// MCD: to file through mcd
// STDOUT-NOT: to file

hw.module @Top() {
  llhd.process {
    %stdout = hw.constant -2147483647 : i32
    %stderr = hw.constant -2147483646 : i32
    %l0 = sim.fmt.literal "to stdout\0A"
    %l1 = sim.fmt.literal "to stderr\0A"
    %l2 = sim.fmt.literal "to file\0A"
    %l3 = sim.fmt.literal "appended to file\0A"
    %l4 = sim.fmt.literal "to file through mcd\0A"
    %s0 = sim.sv.channel_to_output_stream %stdout
    sim.proc.print %l0 to %s0
    %s1 = sim.sv.channel_to_output_stream %stderr
    sim.proc.print %l1 to %s1
    %name = sim.string.literal "file-io-test.txt"
    %fd = sim.sv.fopen %name mode = w
    %s2 = sim.sv.channel_to_output_stream %fd
    sim.proc.print %l2 to %s2
    sim.sv.fclose %fd
    %fd2 = sim.sv.fopen %name mode = a
    %s3 = sim.sv.channel_to_output_stream %fd2
    sim.proc.print %l3 to %s3
    sim.sv.fclose %fd2
    %mcdName = sim.string.literal "file-io-test-mcd.txt"
    %mcd = sim.sv.fopen %mcdName
    %s4 = sim.sv.channel_to_output_stream %mcd
    sim.proc.print %l4 to %s4
    sim.sv.fclose %mcd
    llhd.halt
  }
  hw.output
}
