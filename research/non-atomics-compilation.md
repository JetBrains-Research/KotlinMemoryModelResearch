## Introduction

[LLVM](https://llvm.org/docs/Atomics.html#atomic-orderings) guide 
recommends to use `Unordered` memory access mode to compile 
plain memory accesses in safe languages (e.g. Kotlin), 
instead of `NonAtomic` access mode. 
The difference between these two modes is that races on `NonAtomic` accesses result 
in undefined behavior according to LLVM spec, 
while racy `Unordered` accesses should have defined semantics.

However, it looks like none of the existing compilers 
actually use `Unordered` mode (see links to source code in the 
[related question](https://discourse.llvm.org/t/atomic-ordering-non-atomic-vs-unordered/66259)).

We should answer two questions.
1. Whether usage of `NonAtomic` access mode instead of `Unordered` by the compilers 
   threaten safety guarantees of the source languages (e.g. Kotlin) in practice?
2. What is a performance penalty for using `NonAtomic` access mode instead of `Unordered`. 
   Is it significant or not?


## Safety Issues

According to the LLVM spec 
"if there is a race on a given memory location, loads from that location return `undef`".
Above the term "race" refers to a data race between two memory accesses, such that 
at least one of the accesses has `NonAtomic` memory ordering.  
[`undef`](https://llvm.org/docs/LangRef.html#undefined-values) is a special 
constant defined by LLVM spec so that "the user of its value may receive an unspecified bit-pattern".
What is more problematic is that `undef` can change its "value" over time.
Quoting the spec.

> This can be surprising to people (and also matches C semantics) where they assume that `X^X` is always zero, even if X is undefined. 
> This isn’t true for a number of reasons, but the short answer is that an `undef` variable can arbitrarily change its value over its live range. 
> This is true because the variable doesn’t actually have a live range. 
> Instead, the value is logically read from arbitrary registers that happen to be around when needed, 
> so the value is not necessarily consistent over time.

This can lead to surprising behaviors.
Consider the following example 
(note that all the examples are given in pseudo-code, not an actual LLVM IR syntax).

```LLVM
;; Initialization
store x, 0

;; Thread 1 
%r1 = load x;
if (%r1 <= 0 && %r1 > 0) {
  println("Error");
} 

...

;; Thread 2
store x, 1;
```

The LLVM spec allows an outcome when "Error" is printed.

From the theoretical standpoint, it is allowed 
because there is a race on `x` location, thus load from `x`
in the first thread reads `undef` value. 
Each "use" of `undef` values is allowed to observe different value, 
thus both comparisons can succeed.

From the practical standpoint, this behavior can occur 
because of register [spilling and rematerialization](https://en.wikipedia.org/wiki/Rematerialization).
In other words, the compiler can split single load `%r1 = load x;` 
into two loads `%r1 = load x; %r2 = load x`. Then the two loads can observe different values:
`%r1` loads `0`, while `%r2` loads `1`.

Situation like this one can happen, for example, if there is a high pressure on the register allocator
at some location of the source code. The register allocator can first load from memory into register `%r`,
perform some computations with this register, then decide that it needs this register for other purposes,
and later re-load value back to the register.
If there are no racy writes, then two loads are guaranteed to load the same value, 
however, this assumption is invalid in the presences of data races.

Such behavior is not only contradictory by itself, but it can also
threaten "safety" guarantees of the source languages compiled to LLVM IR,
that allow racy accesses to non-atomic variables and 
compile them with `NonAtomic` memory ordering.

For example, languages like `Java` and `Kotlin` guarantee memory safety.
In particular, the program should never access an array by index
greater than array's size --- in this case the exception `ArrayIndexOutOfBoundsException` should be raised.

Consider the following example (in pseudo Java/Kotlin syntax):

```Java
// Initialization
int[] a = new int[10];
int i = 0;

// Thread 1
i = 1      
a[i] = 42  

// Thread 2
i = 100;
```

When compiled to LLVM IR, because of array bounds checks, the code should look somewhat like this:

```Java
;; Initialization
%r1 = new int[10];
store a, %r1;
memset(a, 0, 10);

;; Thread 1
%r2 = load i;
if (0 <= %r2 < 10) { 
    a[%r2] = 42;     
} else {             
    throw();         
}

;; Thread 2
store i, 100;
```
 
Similarly to the previous example, the compiler 
can split single load `%r2 = load i` into two loads, 
place one of them inside the "then" branch, and thus 
bypass the array bounds check.

## Questions

* Can we reproduce behavior described above in practice?
* Can we break safety guarantees of the Kotlin language using racy accesses on non-atomic variables?

### Steps to Answer the Questions

1. Try to reproduce the behavior with different uses of `undef` producing different results in LLVM due to races. 
   Try to write a test program reproducing this behavior. 
   Likely, the program should perform a lot of non-atomic loads from different memory locations 
   in the first thread, and a lot of non-atomic racy writes to these memory locations in the second thread. 
   Each memory location should be read only once, the results of the load should be saved in a LLVM virtual register. 
   Periodically, the content of these registers should be dumped to another memory. 
   When the threads finish execution, the dumps are inspected to check if the same registers varies its value between dumps.

2. If we will be able to reproduce racy `undef` behavior in practice, 
   we can also try to use it to break the source language (e.g. Kotlin) safety guarantees.
   We can try to construct an example similar to "array bounds check" example above or some another example.

3. If we will be able to break safety guarantees because of racy accesses on non-atomics, 
   we should report bugs back to compilers authors and discuss usage of `Unordered` 
   instead of `NonAtomic` memory order accesses. 
   Example of compilers not using `Unordered`: Kotlin/Native itself, GraalVM, Swift compiler, GHC.
