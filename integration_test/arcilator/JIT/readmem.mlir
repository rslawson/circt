// RUN: printf '// comment\n11 22\n@3 44 /* block\n comment */ 5_5\n' > readmem-test.hex
// RUN: arcilator %s --run --jit-entry=Top_main | FileCheck %s
// REQUIRES: arcilator-jit

// `$readmemh` loads words into consecutive addresses of a memory, starting at
// its lowest address or at the address given by an `@` directive. Addresses
// without data in the file keep their values. The first memory corresponds to
// `logic [7:0] mem [1:5]`, whose address 1 is the last array element. The
// second one corresponds to `logic [7:0] mem [5:1]`, whose address 1 is the
// first array element.

// CHECK: mem[1]=11 mem[2]=22 mem[3]=44 mem[4]=55 mem[5]=ff
// CHECK: mem[1]=11 mem[2]=22 mem[3]=44 mem[4]=55 mem[5]=ff

hw.module @Top() {
  llhd.process {
    %name = sim.string.literal "readmem-test.hex"
    %c-1_i40 = hw.constant -1 : i40
    %init = hw.bitcast %c-1_i40 : (i40) -> !hw.array<5xi8>
    %mem = sim.sv.readmem %name, %init {dimDescending = array<i1: false>, dimLows = array<i64: 1>} : !hw.array<5xi8>
    %c4 = hw.constant 4 : i3
    %c3 = hw.constant 3 : i3
    %c2 = hw.constant 2 : i3
    %c1 = hw.constant 1 : i3
    %c0 = hw.constant 0 : i3
    %m1 = hw.array_get %mem[%c4] : !hw.array<5xi8>, i3
    %m2 = hw.array_get %mem[%c3] : !hw.array<5xi8>, i3
    %m3 = hw.array_get %mem[%c2] : !hw.array<5xi8>, i3
    %m4 = hw.array_get %mem[%c1] : !hw.array<5xi8>, i3
    %m5 = hw.array_get %mem[%c0] : !hw.array<5xi8>, i3
    %l1 = sim.fmt.literal "mem[1]="
    %l2 = sim.fmt.literal " mem[2]="
    %l3 = sim.fmt.literal " mem[3]="
    %l4 = sim.fmt.literal " mem[4]="
    %l5 = sim.fmt.literal " mem[5]="
    %nl = sim.fmt.literal "\0A"
    %f1 = sim.fmt.hex %m1, isUpper false : i8
    %f2 = sim.fmt.hex %m2, isUpper false : i8
    %f3 = sim.fmt.hex %m3, isUpper false : i8
    %f4 = sim.fmt.hex %m4, isUpper false : i8
    %f5 = sim.fmt.hex %m5, isUpper false : i8
    %msg = sim.fmt.concat (%l1, %f1, %l2, %f2, %l3, %f3, %l4, %f4, %l5, %f5, %nl)
    sim.proc.print %msg

    %dmem = sim.sv.readmem %name, %init {dimDescending = array<i1: true>, dimLows = array<i64: 1>} : !hw.array<5xi8>
    %d1 = hw.array_get %dmem[%c0] : !hw.array<5xi8>, i3
    %d2 = hw.array_get %dmem[%c1] : !hw.array<5xi8>, i3
    %d3 = hw.array_get %dmem[%c2] : !hw.array<5xi8>, i3
    %d4 = hw.array_get %dmem[%c3] : !hw.array<5xi8>, i3
    %d5 = hw.array_get %dmem[%c4] : !hw.array<5xi8>, i3
    %g1 = sim.fmt.hex %d1, isUpper false : i8
    %g2 = sim.fmt.hex %d2, isUpper false : i8
    %g3 = sim.fmt.hex %d3, isUpper false : i8
    %g4 = sim.fmt.hex %d4, isUpper false : i8
    %g5 = sim.fmt.hex %d5, isUpper false : i8
    %dmsg = sim.fmt.concat (%l1, %g1, %l2, %g2, %l3, %g3, %l4, %g4, %l5, %g5, %nl)
    sim.proc.print %dmsg
    llhd.halt
  }
  hw.output
}
