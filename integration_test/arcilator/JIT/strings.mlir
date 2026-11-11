// RUN: arcilator %s --run --jit-entry=Top_main +name=World | FileCheck %s
// RUN: arcilator %s --run --jit-entry=Top_main | FileCheck %s --check-prefix=NOARGS
// REQUIRES: arcilator-jit

// End-to-end test for dynamic strings, corresponding to SystemVerilog's
// `string` type.

// CHECK:      [Hello, World!] len=13
// NOARGS:     [Hello, nobody!] len=14
// CHECK-NEXT: [HELLO] [hello] [ell] [llo] [] get=101,0
// CHECK-NEXT: [   ab] [ab   ] [xxxab]
// CHECK-NEXT: eq=1 ne=0 lt=1 le=1 gt=0 ge=0 compare=-1
// CHECK-NEXT: int_to_string=[AB] string_to_int=4142 truncated=42
// CHECK-NEXT: [n=42 h=2a]
// CHECK-NEXT: s=[a]
// CHECK-NEXT: s=[ab]
// CHECK-NEXT: s=[abc]
// CHECK-NOT:  s=

hw.module @Top() {
  %step = llhd.constant_time <1ns, 0d>
  %empty = sim.string.literal ""
  %s = llhd.sig %empty : <!sim.dstring>
  %nl = sim.fmt.literal "\0A"
  %lb = sim.fmt.literal "["
  %rb = sim.fmt.literal "]"
  %sp = sim.fmt.literal "] ["

  llhd.process {
    // Plusargs, concatenation, length, and formatting.
    %hello = sim.string.literal "Hello, "
    %nobody = sim.string.literal "nobody"
    %bang = sim.string.literal "!"
    %found, %name0 = sim.plusargs.value "name=%s" : !sim.dstring
    %name = comb.mux %found, %name0, %nobody : !sim.dstring
    %greeting = sim.string.concat (%hello, %name, %bang)
    %len = sim.string.length %greeting
    %f0 = sim.fmt.string %greeting : !sim.dstring
    %l0 = sim.fmt.literal "] len="
    %f1 = sim.fmt.dec %len specifierWidth 0 : i32
    %m0 = sim.fmt.concat (%lb, %f0, %l0, %f1, %nl)
    sim.proc.print %m0

    // Case conversion, substrings, and characters.
    %c0 = hw.constant 0 : i32
    %c1 = hw.constant 1 : i32
    %c2 = hw.constant 2 : i32
    %c4 = hw.constant 4 : i32
    %c99 = hw.constant 99 : i32
    %word = sim.string.literal "HeLLo"
    %upper = sim.string.toupper %word
    %lower = sim.string.tolower %word
    %sub0 = sim.string.substr %lower[%c1 : %c4]
    %sub1 = sim.string.substr %lower[%c2]
    %sub2 = sim.string.substr %lower[%c4 : %c1]
    %get0 = sim.string.get %lower[%c1]
    %get1 = sim.string.get %lower[%c99]
    %f2 = sim.fmt.string %upper : !sim.dstring
    %f3 = sim.fmt.string %lower : !sim.dstring
    %f4 = sim.fmt.string %sub0 : !sim.dstring
    %f5 = sim.fmt.string %sub1 : !sim.dstring
    %f6 = sim.fmt.string %sub2 : !sim.dstring
    %l1 = sim.fmt.literal "] get="
    %comma = sim.fmt.literal ","
    %f7 = sim.fmt.dec %get0 specifierWidth 0 : i8
    %f8 = sim.fmt.dec %get1 specifierWidth 0 : i8
    %m1 = sim.fmt.concat (%lb, %f2, %sp, %f3, %sp, %f4, %sp, %f5, %sp, %f6, %l1, %f7, %comma, %f8, %nl)
    sim.proc.print %m1

    // Padding.
    %ab = sim.string.literal "ab"
    %f9 = sim.fmt.string %ab specifierWidth 5 : !sim.dstring
    %f10 = sim.fmt.string %ab isLeftAligned true specifierWidth 5 : !sim.dstring
    %f11 = sim.fmt.string %ab paddingChar 120 specifierWidth 5 : !sim.dstring
    %m2 = sim.fmt.concat (%lb, %f9, %sp, %f10, %sp, %f11, %rb, %nl)
    sim.proc.print %m2

    // Comparisons. The strings are created at runtime, such that they are not
    // identical pointers.
    %abc = sim.string.literal "abc"
    %abd = sim.string.literal "abd"
    %cstr = sim.string.literal "c"
    %abc2 = sim.string.concat (%ab, %empty, %cstr)
    %eq = sim.string.cmp eq %abc, %abc2 : !sim.dstring
    %ne = sim.string.cmp ne %abc, %abc2 : !sim.dstring
    %lt = sim.string.cmp lt %abc, %abd : !sim.dstring
    %le = sim.string.cmp le %abc, %abd : !sim.dstring
    %gt = sim.string.cmp gt %abc, %abd : !sim.dstring
    %ge = sim.string.cmp ge %abc, %abd : !sim.dstring
    %compare = sim.string.compare %abc, %abd
    %f13 = sim.fmt.dec %eq specifierWidth 0 : i1
    %f14 = sim.fmt.dec %ne specifierWidth 0 : i1
    %f15 = sim.fmt.dec %lt specifierWidth 0 : i1
    %f16 = sim.fmt.dec %le specifierWidth 0 : i1
    %f17 = sim.fmt.dec %gt specifierWidth 0 : i1
    %f18 = sim.fmt.dec %ge specifierWidth 0 : i1
    %f19 = sim.fmt.dec %compare specifierWidth 0 signed : i32
    %l2 = sim.fmt.literal "eq="
    %l3 = sim.fmt.literal " ne="
    %l4 = sim.fmt.literal " lt="
    %l5 = sim.fmt.literal " le="
    %l6 = sim.fmt.literal " gt="
    %l7 = sim.fmt.literal " ge="
    %l8 = sim.fmt.literal " compare="
    %m3 = sim.fmt.concat (%l2, %f13, %l3, %f14, %l4, %f15, %l5, %f16, %l6, %f17, %l7, %f18, %l8, %f19, %nl)
    sim.proc.print %m3

    // Conversions between integers and strings. NUL bytes are skipped.
    %int = hw.constant 0x00410042 : i32
    %fromInt = sim.string.int_to_string %int : i32
    %ab2 = sim.string.literal "AB"
    %toInt = sim.string.string_to_int %ab2 : i16
    %toInt8 = sim.string.string_to_int %ab2 : i8
    %f20 = sim.fmt.string %fromInt : !sim.dstring
    %f21 = sim.fmt.hex %toInt, isUpper false specifierWidth 0 : i16
    %f22 = sim.fmt.hex %toInt8, isUpper false specifierWidth 0 : i8
    %l9 = sim.fmt.literal "int_to_string=["
    %l10 = sim.fmt.literal "] string_to_int="
    %l11 = sim.fmt.literal " truncated="
    %m4 = sim.fmt.concat (%l9, %f20, %l10, %f21, %l11, %f22, %nl)
    sim.proc.print %m4

    // Formatting into a string.
    %c42 = hw.constant 42 : i8
    %l12 = sim.fmt.literal "n="
    %l13 = sim.fmt.literal " h="
    %f23 = sim.fmt.dec %c42 specifierWidth 0 : i8
    %f24 = sim.fmt.hex %c42, isUpper false specifierWidth 0 : i8
    %fmt = sim.fmt.concat (%l12, %f23, %l13, %f24)
    %formatted = sim.string.format_to_string %fmt
    %f25 = sim.fmt.string %formatted : !sim.dstring
    %m5 = sim.fmt.concat (%lb, %f25, %rb, %nl)
    sim.proc.print %m5
    llhd.halt
  }

  // A writer process appends a character to a string signal every 1ns, and a
  // reader process prints the signal whenever its length changes.
  llhd.process {
    %a = sim.string.literal "a"
    %b = sim.string.literal "b"
    %c = sim.string.literal "c"
    llhd.wait delay %step, ^bb1
  ^bb1:
    %v0 = llhd.prb %s : !sim.dstring
    %n0 = sim.string.concat (%v0, %a)
    llhd.assign %s, %n0 : !sim.dstring
    llhd.wait delay %step, ^bb2
  ^bb2:
    %v1 = llhd.prb %s : !sim.dstring
    %n1 = sim.string.concat (%v1, %empty)
    llhd.assign %s, %n1 : !sim.dstring
    llhd.wait delay %step, ^bb3
  ^bb3:
    %v2 = llhd.prb %s : !sim.dstring
    %n2 = sim.string.concat (%v2, %b)
    llhd.assign %s, %n2 : !sim.dstring
    llhd.wait delay %step, ^bb4
  ^bb4:
    %v3 = llhd.prb %s : !sim.dstring
    %n3 = sim.string.concat (%v3, %c)
    %delta = llhd.constant_time <0ns, 1d>
    llhd.drv %s, %n3 after %delta : !sim.dstring
    llhd.wait delay %step, ^bb5
  ^bb5:
    sim.terminate success, quiet
    llhd.halt
  }

  %p = llhd.prb %s : !sim.dstring
  %len = sim.string.length %p
  llhd.process {
    cf.br ^bb1
  ^bb1:
    llhd.wait (%len : i32), ^bb2
  ^bb2:
    %v2 = llhd.prb %s : !sim.dstring
    %l = sim.fmt.literal "s=["
    %f = sim.fmt.string %v2 : !sim.dstring
    %m = sim.fmt.concat (%l, %f, %rb, %nl)
    sim.proc.print %m
    cf.br ^bb1
  }
  hw.output
}
