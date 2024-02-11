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
Examples of how various safety guarantees can be broken
due to a combination of data races and various aggressive compiler optimizations
can be found in the papers [[4]][4], [[5]][5], and [[6]][6].

__Safe__ languages (for example, Java) cannot fall back to fully undefined semantics
in the case of data races, because such a decision would ultimately break all 
the safety guarantees of the language.
What these languages typically guarantee instead is the so-called "no-thin-air values" property.
Intuitively, it means that each read (even racy one) must observe 
a value written by some preceding or concurrent write in the same program execution.

#### Out-of-thin-Air Values Problem (OOTA)

Although the informal definition of the "no-thin-air" guarantee given above
seems intuitive, the problem is that the rigorous formal definition
of it is an [open research question][6].
Because of this, all existing specifications of other languages
(e.g. C++, Rust, Go, etc.) claim "no-thin-air" guarantee 
(at a certain level of access atomicity),
without actually defining what constitutes "thin-air" values.

The Java memory model attempted to resolve this issue
(via the [commit mechanism](https://docs.oracle.com/javase/specs/jls/se8/html/jls-17.html#jls-17.4.8)),
but it was later shown to fail in many other aspects
(see papers [[8]][8], [[9]][9], and [[10]][10] for the details).
Thus, the JMM solution also cannot be considered satisfactory. 

This means that even though safe languages like Java may claim "no-thin-air" guarantee, 
they do not really give any definitive semantics for racy plain memory accesses. 

#### Controversy around Benign Data Races

Another controversial topic is the notion of so-called _benign data races_,
which is commonly widespread among Java developers.
Informally, a benign data race is considered to be a race that does not
break the correctness of the program.

The most famous example of the benign data race is a race in 
the `hashCode()` implementation for the `String` class.
In essence, it boils down to the following code:

```java
class String {
    /* ... */
    private int hash;
    private boolean hashIsZero;

    public int hashCode() {
        int h = hash;
        if (h == 0 && !hashIsZero) {
            h = computeHashCode();
            if (h == 0) {
                hashIsZero = true;
            } else {
                hash = h;
            }
        }
        return h;
    }
    /* ... */
}
```

In this code, there is a potential read-write race on `hash` field.
Yet it is considered benign, because hash code computation is pure and idempotent,
and thus any read from `hash` field can only either read `0` or the computed value.

Some experts argue that the notion of "benign data races" is misleading,
and that any race on non-atomic variables should be considered an error.
The motivation for this reasoning is that in the presence of
some seemingly "benign" races, certain compiler optimizations can produce invalid results
(see paper [[5]][5] for the details).

This is why in C/C++, where all races lead to undefined behavior,
all benign data races should be explicitly marked as atomic accesses 
with `memory_order_relaxed` access mode (corresponds to `Monotonic` access mode in LLVM).
This is the weakest atomic access mode in the C/C++ (comes right after `Unordered` in LLVM).
Usage of this access mode generally has no significant performance impact.
This is because relaxed accesses do not emit any memory fences when compiled
(although certain compiler optimizations are forbidden for relaxed accesses,
 and the compilers often tend to treat them more conservatively than necessary).
More use-cases of `memory_order_relaxed` access mode can be found in 
the [corresponding guide](https://www.open-std.org/jtc1/sc22/wg21/docs/papers/2020/p2055r0.pdf).

Another obstacle with the notion of benign data races is that 
in the absence of the formal semantics for racy programs 
(see subsection on the OOTA problem above),
there is no way to give a formal definition of benign data race.
One possible definition could be stated as follows:
a program is _effectively race free_ if all possible executions of 
this program have the same observable behavior,
that is, the program is _deterministic_.
However, with no formal semantics at hand,
the phrase _all possible executions of a program_ is essentially meaningless.

### Safe Publication

Yet another related problem of plain accesses semantics 
is the so-called "(un)safe publication" problem.

In safe languages, like Java, publication of a reference 
to some object, even through a data race,
should guarantee that another thread, reading this reference,
should observe the object to be in some well-defined state.
Without additional efforts from the compiler and language runtime,
the reading thread can observe the object in uninitialized state,
and even read some "garbage" value in place of one of the 
object's reference-typed fields, thus breaking memory safety.

Consider an example below:

```kotlin
class Inner(val x: Int)
class Holder(val ref: Inner)
/* ... */
var holder: Holder? = null
/* ... */
thread {
    holder = Holder(Inner(42))
}
thread {
    // inner could read "garbage" value instead of
    // a valid pointer into Kotlin heap
    val inner = holder?.inner ?: null
    if (inner != null) {
        // dereferencing the invalid pointer can result in SEGFAULT
        println(inner.x)
    }
}
```

Here, there is a read-write race on the `holder` variable.
Despite this race, the Kotlin language, being the safe language, 
should guarantee that the reader thread, 
if observes non-null value in `holder` variable,
should also observe some valid reference to an `Inner` object in the `ref` field.
Yet an earlier version of the Kotlin/Native allowed observing an invalid reference 
(see the corresponding [bug report](https://youtrack.jetbrains.com/issue/KT-58995)). 

The problem arises due to the fact, that the compiler (or processor too)
is allowed to reorder two plain stores in the writer thread:
the store initializing `ref` field in the constructor of the `Holder` class,
and the store publishing the reference to `Holder` object into `holder` variable.
To prevent such reordering, the compiler (or runtime) should
emit a memory fence between the two stores.

With respect to the (un)safe publication guarantees,
there are in fact two possible semantics choices.

1.  **Default construction guarantee:** 
    when reading object reference through a race, the language
    only guarantees that thread would not observe any "garbage" values,
    all fields will be initialized to some valid value, 
    possibly the default value (`0` for primitive types, `null` for references).
 
2.  **Full construction guarantee:** 
    when reading object reference through a race, the language
    guarantees that the thread would observe an object in a 
    "fully-constructed" state, with all fields initialized
    to the values that the constructor sets*.  

(*) With respect to the option (2) there is an important constraint. 
In order for this guarantee to hold, the developer should ensure
that the `this` reference is not prematurely published by the constructor itself.
Otherwise, there is no way the compiler/runtime can guarantee the aforementioned property,
because there is no statically known place to insert the memory fence into.

To see the difference between two approaches, consider the following example:

```kotlin
class Holder(val x: Int)
/* ... */
var holder: Holder? = null
/* ... */
thread {
    holder = Holder(1)
}
thread {
    // inner could read "garbage" value instead of
    // a valid pointer into Kotlin heap
    val inner = holder?.inner ?: -1
    println(inner)
}
```

* in case of option (1), the program can print `-1`, `0` or `1`;
* in case of option (2), the program can print only `-1` or `1`.

Importantly, if the user breaks the "no-leaking-this" contract, 
as shown below:

```kotlin
class Holder(val x: Int) {
    init { holder = this }
}
/* ... */
var holder: Holder? = null
/* ... */
thread {
    Holder(1)
}
thread {
    // inner could read "garbage" value instead of
    // a valid pointer into Kotlin heap
    val inner = holder?.inner ?: -1
    println(inner)
}
```

then even in case of option (2), the program still can print `-1`, `0` or `1`.

It means that the safe language should always provide 
at least the **default construction guarantee** (option 1),
and it might in addition provide 
the stronger **full construction guarantee** (option 2).

## Design Choices for Kotlin

## Compilation Strategies

### JVM

### LLVM

## References

1. Java Threads, Locks, and Memory Model Specification \
   [[Link]][1]

[1]: https://docs.oracle.com/javase/specs/jls/se21/html/jls-17.html 

2. JEP draft: Null-Restricted Value Class Types (Preview) \
   [[Link]][2] 

[2]: https://openjdk.org/jeps/8316779

3. LLVM Atomic Instructions and Concurrency Guide \
   [[Link]][3] 

[3]: https://llvm.org/docs/Atomics.html

4. Studying compilation schemes for shared memory accesses in Kotlin/Native \
   _Gleb Soloviev_ (BSc thesis) \
   [[Link]][4]

[4]: https://weakmemory.github.io/project-theses/studying-compilation-schemes-KN.pdf

5. How to miscompile programs with “benign” data races \
   _Hans-J. Boehm_ \
   [[Link]][5]

[5]: https://www.usenix.org/legacy/events/hotpar11/tech/final_files/Boehm.pdf

6. Bounding Data Races in Space and Time (section 2) \
   _Stephen Dolan, KC Sivaramakrishnan, and Anil Madhavapeddy_ \
   [[Link]][6]

[6]: https://core.ac.uk/download/pdf/222831817.pdf

7. Outlawing ghosts: Avoiding out-of-thin-air results \
   _Hans-J. Boehm, and Brian Demsky_ \
   [[Link]][7]

[7]: https://dl.acm.org/doi/pdf/10.1145/2618128.2618134

8. Java Memory Model Examples: Good, Bad and Ugly \
   _David Aspinall and Jaroslav Ševčik_ \
   [[Link]][8]

[8]: https://www.pure.ed.ac.uk/ws/files/25105628/jmmexamples.pdf

9. On Validity of Program Transformations in the Java Memory Model \
   _Jaroslav Ševčik and David Aspinall_ \
   [[Link]][9]

[9]: https://citeseerx.ist.psu.edu/document?repid=rep1&type=pdf&doi=3d802a3254a1a532f080bc8e713d970ea8796db5

10. Verification of Causality Requirements in Java Memory Model is Undecidable \
    _Matko Botinčan, Paola Glavan, and Davor Runje_ \
    [[Link]][10]

[10]: https://citeseerx.ist.psu.edu/document?repid=rep1&type=pdf&doi=7af2c3dd80647696ee02b56fa046f3d31da067ac