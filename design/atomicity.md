# Design notes on atomicity guarantees and semantics of plain memory accesses in Kotlin

Atomicity is a property of an operation guaranteeing that 
the effect of the operation is observable as single indivisible unit.
In the context of concurrent programs, this means that
atomic operations are executed without an interference from other threads.
In the context of this document, we will be speaking about the 
atomicity of the memory access operations, 
that is individual memory reads and writes.

Modern hardware typically guarantees atomicity for 
properly aligned memory accesses to machine-word-sized memory cells.
On the other side, modern programming languages provide a way for developers 
to declare their custom compound data types, generally spanning multiple machine words.
Without additional measures, it is impossible to guarantee atomicity
for accesses to the instances of these custom data types.
And even for single-world data types, certain compiler optimizations 
may break the atomicity guarantee. 

This is why most modern programming languages (including Java, C++, Rust, etc.) 
distinguish between the _plain_ and _atomic_ memory accesses.
Kotlin also provides several ways to mark variables (i.e., either fields or array elements) as atomic: 
- via the [`@Volatile` annotation](https://kotlinlang.org/api/latest/jvm/stdlib/kotlin.concurrent/-volatile/);
- via the [`Atomic*` classes](https://kotlinlang.org/api/latest/jvm/stdlib/kotlin.concurrent/) from the standard library;  
- via the [`atomicfu` library](https://github.com/Kotlin/kotlinx-atomicfu).

For atomic variables, the atomicity of read and writes is guaranteed as expected.
However, for plain variables, the situation is less obvious,
and depends on several factors, including the design principles 
of the particular programming language.

This document discusses the design choices of the Kotlin language
with respect to atomicity guarantees and semantics of plain shared memory accesses.  
This design should take into account two main concerns:
providing reasonable and predictable semantics for the Kotlin developers,
while taking into account the constraints imposed by semantics of 
plain accesses in Kotlin backends (JVM, LLVM, and JS/WASM). 

## Overview

### Atomicity of Plain Accesses

In this section, we overview the atomicity guarantees 
provided by the main backends of the Kotlin language.

#### Atomicity in JVM

JVM [guarantees](https://docs.oracle.com/javase/specs/jls/se21/html/jls-17.html#jls-17.7) atomicity 
of plain memory accesses for:
* variables of primitive types, except `long` and `double`, that is:
  * `int`
  * `short`
  * `byte`
  * `char`
  * `float`
  * `boolean`
* variable of reference types (for references themselves, not the objects they point to).

In addition, with the upcoming [Valhalla project](https://openjdk.org/projects/valhalla/), 
which promises to bring user-defined value classes to the Java language,
the atomicity is by default guaranteed for the instance creation of value classes.
The developer can explicitly [give up](https://openjdk.org/jeps/8316779#Non-atomic-updates)
on this guarantee (and thus potentially enable additional optimizations by the JVM) 
by declaring that the value class implements the marking interface `LooselyConsistentValue`.

Consider as an example the `Point` from one of Valhalla's JEP drafts:

```java
value class Point implements LooselyConsistentValue {
    // note that all fields of value class are implicitly final
    double x;
    double y;
    
    // implicit constructor initializes all fields 
    // with the default values (0 for the double type) 
    public implicit Point();
    
    // custom constructor initializes point using provided values 
    public Point(double x, double y) {
        this.x = x;
        this.y = y;
    }
}
```

Now, because the `Point` class is declared as `LooselyConsistentValue`,
in the code fragment below, the last read `Point p = ps[0]` may observe
"inconsistent" value:

```java
Point![] ps = { new Point(0.0, 1.0) }; 
new Thread(() -> ps[0] = new Point(2.0, 3.0)).start(); 
Point p = ps[0]; // may be (2.0, 1.0), among other possibilities
```

Have the `Point` class not implement the `LooselyConsistentValue` marker interface,
the only possible values that the read `Point p = ps[0]` 
can observe would be either `(0.0, 1.0)` or `(2.0, 3.0)`.

#### Atomicity in LLVM

In LLVM, each memory access in the IR can be annotated 
with the [_atomic ordering mode_](https://llvm.org/docs/Atomics.html).
There are 7 ordering modes in total, 
with two of them directly related to plain memory accesses.

- `NotAtomic` access mode is intended for compiling non-atomic memory accesses
   from "unsafe" languages like C++ or (unsafe subset of) Rust, 
   where racy accesses to the same memory location result in undefined behavior.
   For this type of accesses there are no atomicity guarantees.
 
- `Unordered` access mode is intended for compiling plain memory accesses 
  from "safe" languages like Java, where it is expected that even 
  racy programs have somewhat defined semantics.
  For this type of accesses the LLVM guarantees atomicity.  

#### Atomicity in JS & WebAssembly

TODO

### Data Races

Another important aspect of the plain memory accesses semantics
is how the data races on plain variables should be treated.
In essence, there are just two options.  

__Unsafe__ languages (C++, unsafe Rust) treat data races on non-atomic variables as _undefined behavior_.
In the LLVM, for the `NotAtomic` access mode, the guarantee is a little bit stronger:
a racy load instruction reads special [`undef` value](https://llvm.org/docs/LangRef.html#undefined-values).

It is worth mentioning though that the semantics of `undef` value is a bit trickier than one may expect.
In particular, the read of `undef` value is not equivalent to a read of some arbitrary value.
The compiler is allowed to materialize each usage of the same `undef` value to different values.  
For example, for the program below (given in pseudocode), 
LLVM semantics allow to print "Error":

```
%r1 = undef;
if (%r1 <= 0 && %r1 > 0) {
  println("Error");
}   
```

The design choice of "unsafe" languages gives the compiler maximal flexibility
when it comes to optimizing the non-atomic memory accesses,
but it results in undefined semantics,
essentially breaking any possible safety guarantees.
We refer the curious reader to the papers [], [], and [],
for the examples of how various safety guarantees can be broken,
and what particular compiler optimizations may lead to these violations.

__Safe__ languages (for example, Java) cannot fall back to fully undefined semantics
in the case of data races, because such a decision would ultimately break all 
the safety guarantees of the language.
What these languages typically guarantee instead is the so-called "no-thin-air values" property.
Intuitively, it means that each read (even racy one) must observe 
a value written by some preceding or concurrent write in the same program execution.

#### Out-of-thin-Air Values Problem

Although the informal definition of the "no-thin-air" guarantee given above
seems intuitive, the problem is that the rigorous formal definition
of it is an [open research question](https://dl.acm.org/doi/pdf/10.1145/2618128.2618134).
Because of this, all existing specifications of other languages
(e.g. C++, Rust, Go, etc.) claim "no-thin-air" guarantee 
(at a certain level of access atomicity),
without actually defining what constitutes "thin-air" values.

The Java memory model attempted to resolve this issue
(via the [commit mechanism](https://docs.oracle.com/javase/specs/jls/se8/html/jls-17.html#jls-17.4.8)),
but it was later shown to fail in many other aspects
(see papers [], [], and [] for the details).
Thus, the JMM solution also cannot be considered satisfactory. 

This means that even though safe languages like Java may claim "no-thin-air" guarantee, 
they do not really give any definitive semantics for racy plain memory accesses. 

#### Controversy around Benign Data Races

### Safe Publication

## Design Choices for Kotlin

## Compilation Strategies

### JVM

### LLVM

## References

1. Java Threads, Locks, and Memory Model Specification \
   https://docs.oracle.com/javase/specs/jls/se21/html/jls-17.html

2. JEP draft: Null-Restricted Value Class Types (Preview) \
   https://openjdk.org/jeps/8316779 

3. LLVM Atomic Instructions and Concurrency Guide \
   https://llvm.org/docs/Atomics.html 

4. Studying compilation schemes for shared memory accesses in Kotlin/Native \
   _Gleb Soloviev_ (BSc thesis) \
   TODO 

5. How to miscompile programs with “benign” data races \
   _Hans-J. Boehm_ \
   https://www.usenix.org/legacy/events/hotpar11/tech/final_files/Boehm.pdf

6. Bounding Data Races in Space and Time (section 2) \
   _Stephen Dolan, KC Sivaramakrishnan, and Anil Madhavapeddy_ 
   https://core.ac.uk/download/pdf/222831817.pdf

7. Outlawing ghosts: Avoiding out-of-thin-air results \
   _Hans-J. Boehm, and Brian Demsky_ \
   https://dl.acm.org/doi/pdf/10.1145/2618128.2618134  