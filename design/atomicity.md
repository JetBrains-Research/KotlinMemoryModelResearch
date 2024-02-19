# Design notes on atomicity guarantees and semantics of plain memory accesses in Kotlin

<!-- TOC -->
* [Design notes on atomicity guarantees and semantics of plain memory accesses in Kotlin](#design-notes-on-atomicity-guarantees-and-semantics-of-plain-memory-accesses-in-kotlin)
  * [Overview](#overview)
    * [Atomicity of Plain Accesses](#atomicity-of-plain-accesses)
      * [Atomicity in JVM](#atomicity-in-jvm)
      * [Atomicity in LLVM](#atomicity-in-llvm)
      * [Atomicity in JavaScript & WebAssembly](#atomicity-in-javascript--webassembly)
    * [Data Races](#data-races)
      * [Out-of-thin-Air Values Problem (OOTA)](#out-of-thin-air-values-problem-oota)
      * [Controversy around Benign Data Races](#controversy-around-benign-data-races)
    * [Safe Publication](#safe-publication)
  * [Compilation Strategies](#compilation-strategies)
    * [JVM](#jvm)
    * [LLVM](#llvm)
    * [JS/WASM](#jswasm)
  * [Design Choices for Kotlin](#design-choices-for-kotlin)
    * [Minimal Required Set of Guarantees](#minimal-required-set-of-guarantees)
    * [Atomicity](#atomicity)
      * [Atomicity for some primitive types](#atomicity-for-some-primitive-types)
      * [Atomicity by-default for value classes](#atomicity-by-default-for-value-classes)
      * [No By-Default Atomicity Guarantees for Any Types](#no-by-default-atomicity-guarantees-for-any-types-)
    * [Semantics of Data Races](#semantics-of-data-races)
      * [Claim no-thin-air guarantee](#claim-no-thin-air-guarantee)
      * [Claim no guarantees for racy accesses](#claim-no-guarantees-for-racy-accesses)
    * [Safe publication](#safe-publication-1)
      * [Only the default construction guarantee](#only-the-default-construction-guarantee)
      * [Full construction guarantee for `val` fields](#full-construction-guarantee-for-val-fields)
      * [Full construction guarantee for all fields](#full-construction-guarantee-for-all-fields)
    * [Advantages of Strict Separation of Plain and Atomic Accesses](#advantages-of-strict-separation-of-plain-and-atomic-accesses)
      * [Advantages for the Developers](#advantages-for-the-developers)
      * [Advantages for the Compiler](#advantages-for-the-compiler)
      * [Advantages for the Tooling](#advantages-for-the-tooling)
  * [References](#references)
<!-- TOC -->

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
And even for single-word data types, certain compiler optimizations 
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

In this section we overview the set of problems 
associated with the atomicity and semantics of plain memory accesses in general,
as well as how these problems are resolved in the languages targeted 
by the Kotlin compiler backend.

### Atomicity of Plain Accesses

In this section, we overview the atomicity guarantees 
provided by the main backends of the Kotlin language.

#### Atomicity in JVM

JVM [guarantees](https://docs.oracle.com/javase/specs/jls/se21/html/jls-17.html#jls-17.7) atomicity 
of plain memory accesses for:
* variables of some primitive types: `int`, `short`, `byte`, `char`, `boolean`, `float`,
  * that is all primitive types in JVM except `long` and `double`; 
* variable of reference types (for references themselves, not the objects they point to).

In addition, with the upcoming [Valhalla project](https://openjdk.org/projects/valhalla/), 
which promises to bring user-defined value classes to the Java language,
the atomicity is by default guaranteed for the instance creation of value classes.
The developer can explicitly give up on this guarantee, 
and thus potentially enable additional optimizations, 
by declaring that the value class implements the marking interface `LooselyConsistentValue`.

Consider as an example the `Point` class from one of Valhalla's 
[JEP drafts](https://openjdk.org/jeps/8316779#Non-atomic-updates):

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

Because the `Point` class is declared as `LooselyConsistentValue`
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

To understand why the behavior may differ 
with or without `LooselyConsistentValue` interface,
it is important to understand how the compiler can utilize this information.
With the `LooselyConsistentValue` interface in place, 
the compiler/runtime can allocate the array of `Point` objects 
as a flat array of `double`-s. 
Write to this array then can be compiled as two stores to the
individual elements of this array.
However, if the `Point` class is atomic (does not implement `LooselyConsistentValue` interface),
the compiler has to allocate the array as an array of references to `Point` objects,
and then the write to this array becomes an atomic write of reference to a newly allocated `Point` object.

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
  For this type of accesses LLVM guarantees atomicity.  

#### Atomicity in JavaScript & WebAssembly

Contrary to common belief, the JavaScript language has concurrency capabilities, 
atomic variables, and even [memory model specification][12].
In JavaScript, concurrent access can arise from 
several [Workers](https://developer.mozilla.org/en-US/docs/Web/API/Worker) accessing the same 
[SharedArrayBuffer](https://developer.mozilla.org/en-US/docs/Web/JavaScript/Reference/Global_Objects/SharedArrayBuffer).

Similarly, there is a WebAssembly [proposal][13] specifying
shared linear memory and atomic accesses.
The WASM GC is also expected to eventually support
[threads and shared accesses](https://github.com/WebAssembly/gc/blob/main/proposals/gc/Post-MVP.md#threads-and-shared-references).

It is worth mentioning that unlike the case of JVM and LLVM memory models,
which define it in terms of abstract typed memory locations,
JS and WASM define shared memory in terms of untyped sequence of bytes.
This opens up a whole new set of challenges related to 
[mixed-sized accesses and tearing](http://cambium.inria.fr/~maranget/papers/mixed-size.pdf). 
We will ignore the mixed-sized nature of this memory model,
and assume all accesses are properly aligned and are not subject to tearing.

Both JS and WASM provide just two types of memory accesses: `Unordered` and `SeqCst`.
The `Unordered` accesses are intended for accesses to plain variables,
while `SeqCst` accesses are for atomics.

Both JS and WASM intend to provide safe semantics with no undefined behavior,
and thus `Unordered` access mode in general should have same semantics
as `Unordered` in LLVM.

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
can be found in the papers [[4]], [[5]], and [[6]].

__Safe__ languages like Java cannot fall back to fully undefined semantics
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
(at a certain level of access atomicity)
without actually defining what constitutes "thin-air" values.

The Java memory model attempted to resolve this issue
(via the [commit mechanism](https://docs.oracle.com/javase/specs/jls/se8/html/jls-17.html#jls-17.4.8)),
but it was later shown to fail in many other aspects
(see papers [[8]], [[9]], and [[10]] for the details).
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
(see paper [[5]] for the details).

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
observes the object to be in some well-defined state.
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
    // a valid pointer into heap
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

The problem arises due to the fact, that the compiler (or the hardware)
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
that `this` reference is not prematurely published by the constructor itself.
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
    val inner = holder?.inner ?: -1
    println(inner)
}
```

then even in case of option (2), the program still can print `-1`, `0` or `1`.

It means that the safe language should always provide 
at least the **default construction guarantee** (option 1),
and it might in addition provide 
the stronger **full construction guarantee** (option 2).

## Compilation Strategies

Before we discuss the design choices for the Kotlin language,
it is also important to discuss the compilation strategies 
for plain accesses when compiling them into the Kotlin backends.
This is because the design choices for Kotlin are ultimately restricted 
by what semantics can be pragmatically, efficiently, and soundly implemented
on top of existing backends.

### JVM

When compiling Kotlin code to the JVM, the only reasonable strategy
is to compile plain accesses as the JVM plain accesses.

JVM plain accesses semantics guarantees:

* "no-thin-air values" property;
* atomicity for references;
* atomicity for certain primitive types (e.g. `int`);
* atomicity by default for values classes.

With respect to the safe publication problem, 
the JVM provides a mixed set of guarantees:

* by default, only the _default construction guarantee_ is provided;
* however, for the `final` fields the _full construction_ guarantee is provided.

### LLVM

When compiling Kotlin code to the JVM, 
there is a choice whether to use accesses annotated 
with the `NotAtomic` access mode, or the `Unordered` access mode:

* the `NotAtomic` access mode guarantees nothing: 
  no atomicity and undefined semantics in case of data races;

* the `Unordered` access mode guarantees atomicity 
  and "no-thin-air values" property.

The LLVM spec itself [recommends](https://llvm.org/docs/Atomics.html#unordered) 
using the `Unordered` access mode for safe languages.
However, we failed to find any real compiler that would actually follow
this recommendation and use `Unordered` access mode: 
they typically just use `NotAtomic` for plain accesses.

Another obstacle with using `Unordered` is that it seems like 
the LLVM compiler currently does not optimize `Unordered` accesses efficiently, 
and thus the switch of access mode results in observable performance degradation.
Our preliminary [experiments][4] revealed that 
changing all `NotAtomic` accesses to `Unordered` 
in the LLVM code produced by Kotlin/Native compiler results 
in performance degradation on MacOS Arm64 
of **10% in average**, and **up to 20% maximum**.

Finally, `Unordered` does not give any safe publication guarantees.
In fact, it is impossible to implement any safe publication semantics 
on top of LLVM without using stronger atomic accesses.
This problem can be considered a serious loophole in the current LLVM spec
(this sentiment was also expressed by other compiler devs, see, for example, the 
[relevant thread](https://forums.swift.org/t/se-0282-review-2-interoperability-with-the-c-atomic-operations-library/37360/24?page=2) 
on the Swift language forum).

In more detail, the publication pattern when compiled to LLVM,
should result in the somewhat following code:

```llvm
;; writer thread 
%r1 = alloca Holder;
call void @llvm.memset.p0.i64(i8* %r1, i8 0, i32 sizeof(Holder), i1 false);
<* optional constructor code *>
fence release;
store ptr %r1, ptr %holder;

;; reader thread
%r2 = load ptr, ptr %holder
if (%r2 != nullptr) {
    %r3 = load i32, %r2::<Holder.x>
}
```

To make the code snippet above formally sound, according to the current LLVM spec,
the following changes are necessary:

* the publication store to variable `%holder` should be atomic (at least `Unordered`);
* the load from `%holder` in the reader thread should be atomic with at least `Acquire` access mode.

As was mentioned above, changing plain accesses to `Unordered` access mode 
yields observable performance penalty. 
Promoting the plain loads to `Acquire` loads would result in even 
more drastic performance degradation.
Thus, strictly following the LLVM spec to ensure the safe publication guarantee
is currently infeasible due to performance considerations.

With that being said, one can argue that at least in the case of _default construction_,
any unexpected behavior of the code above, manifesting in practice,
should be considered a bug in the LLVM compiler.
This is because _default construction_ is a prerequisite for the memory safety 
and thus a minimal guarantee for any safe language.
Without this guarantee, it would be impossible to implement any safe language on top of LLVM. 

It is worth noting that with the _full construction_ guarantee, the situation is less obvious.
This is because the _full construction_ guarantee involves running arbitrary
user code in the constructor, as opposed to just the `memset` intrinsic 
in the case of _default construction_.
Whether the LLVM has to provide any guarantees in this case is debatable.

Despite all said above, the aforementioned problems of the LLVM spec, like: 
* usage of `NotAtomic` accesses leading to undefined behavior in case of races,
* absence of any sound way to provide the safe publication guarantee,

do not lead to observable bugs in practice (at least we have not found any yet). 
This is possibly due to the fact that the actual compiler implementation 
is more conservative than the LLVM specification allows. 
Thus using `NotAtomic` access mode for plain accesses 
is a tolerable decision under the giving circumstances.

### JS/WASM

When compiling Kotlin code to the JS/WASM, the only reasonable strategy
is to compile plain accesses as the JS/WASM plain accesses.

## Design Choices for Kotlin

Here we discuss possible design choices for the 
semantics of shared plain memory accesses in the Koltin language.

### Minimal Required Set of Guarantees

As a safe language, the Kotlin has to provide at least 
the following guarantees:

* atomicity for references;
* safe publication with the _default construction guarantee_.

### Atomicity

There are options of what atomicity guarantees to provide 
for the other types of variables beyond reference-typed variables:

* atomicity for some primitive types: `Int`, `Short`, `Byte`, `Char`, `Boolean`, `Float`;
* atomicity by-default for value classes;

with an alternative radical option to 
* give no by-default atomicity guarantees for any types. 

#### Atomicity for some primitive types

Given that the aforementioned primitive types are already atomic in Java,
it might be reasonable to provide same guarantees in Kotlin.

**Benefits:**

* Being similar to Java.
* Follows the principle of [the least surprise](https://en.wikipedia.org/wiki/Principle_of_least_astonishment):
  * that is, this behavior is expected by most of the developers. 

**Risks:**

* Given that at least currently plain accesses are compiled as `NotAtomic` on LLVM,
  thus formally having no atomicity guarantees, 
  it might be risky to rely on the assumption that atomicity violations 
  do not manifest in practice.

#### Atomicity by-default for value classes

Given that at some point, Kotlin will adopt the notion of 
[value classes](https://github.com/Kotlin/KEEP/blob/master/notes/value-classes.md),
we need to consider what atomicity guarantees for these classes Kotlin is going to provide.

In Java, the design choice is to provide atomicity by-default,
with the option to give up on this guarantee by marking a class
with the `LooselyConsistentValue` marker interface.
Kotlin might want to adopt a similar design choice.

**Benefits:**

* Being similar to Java.

**Risks:**

* The users may tend to defensively choose **not to mark** value classes 
  with `LooselyConsistentValue` interface, thus precluding the compiler 
  from efficiently optimizing the code. 

#### No by-default atomicity guarantees for any types 

Alternatively, there is an option to just not give any atomicity guarantees
for any types in Kotlin, forcing the user to explicitly mark
all variables that require atomicity
(using one of the available in Kotlin mechanisms).

**Benefits:**

* Simple uniform semantics for all types.
* Plays nicely with the aforementioned limitations and problems around LLVM semantics.  

**Risks:**

* Might be surprising for users and needs to be emphasized in 
  Kotlin docs, learning materials, etc.

### Semantics of Data Races

With respect to guarantees for racy programs, there are two options:

* claim no-thin-air guarantee, without formally specifying it (for now);
* claim no guarantees for data races.

It is worth mentioning that both options would allow us
to improve the specification in the future when/if the OOTA problem will be solved.

#### Claim no-thin-air guarantee

We can claim the no-thin-air guarantee without formally specifying it.
It is, in fact, a commonly accepted approach in the specifications of other programming languages (for example,
[C++](https://en.cppreference.com/w/cpp/atomic/memory_order#Relaxed_ordering),
[Rust](https://marabos.nl/atomics/memory-ordering.html#oota),
[Go](https://go.dev/ref/mem#restrictions),
[LLVM](https://llvm.org/docs/Atomics.html#unordered)
)

**Benefits:**

* Being similar to Java.
* Follows the principle of the least surprise.

**Risks:**

* Given that at least currently plain accesses are compiled as `NotAtomic` on LLVM,
  thus do not providing no-thin-air guarantee,
  it might be risky to rely on the assumption that such thin-air-like behaviors
  do not manifest in practice.
* With no formal definition of thin-air values, this guarantee is not very useful.

#### Claim no guarantees for racy accesses

Alternatively, we can simply give no guarantees about the semantics of racy plain accesses
(beyond the minimal set of guarantees mentioned above).

**Benefits:**

* We do not guarantee what we cannot provide (or even define).
* Plays nicely with the fact that currently `NotAtomic` accesses are used in LLVM.

**Risks:**

* Might be surprising for users and needs to be emphasized in
  Kotlin docs, learning materials, etc.

### Safe publication

With respect to the safe publication, there is again a choice of what guarantees should Kotlin provide:

* only the _default construction guarantee_;

or additionally to this minimally required guarantee, also provide:

* _full construction_ guarantee for `val` fields;
* _full construction_ guarantee for all fields.

Before listing the benefits and risks of each option, 
it is worth mentioning common pitfall of _full construction_ guarantee in general.

Firstly, as was already mentioned, this guarantee breaks 
if the `this` reference is leaked in the constructor
(see an example [here](https://shipilev.net/blog/2014/jmm-pragmatics/#_premature_publication)).
The leaking `this` and associated initialization errors already pose a serious problem in Kotlin language.
So the question is, do we want to make the problem more complicated
by adding yet another aspect to it?

Secondly, as was mentioned in the previous section,
there is currently no pragmatic way to formally guarantee safe publication on LLVM.
While this contra-point is technically also applicable to _default construction_,
we have already mentioned why it is more relevant in case of _full construction_ guarantee.

On the other hand, the _default construction only_ guarantee also has drawbacks 
in the context of Kotlin.

Firstly, there is a problem that even if the constructor does not leak `this` reference,
under a racy publication another thread may observe an object in invalid partly initialized state,
that potentially breaks some of the class invariants:

```kotlin
data class Range(val l: Int, val r: Int) {
    init {
        // invariant: l <= r
        require(l <= r)
    }
    
    override fun toString(): String =
        "[$l, $r]"
    
    /* ... */
}
/* ... */
var range: Range? = null
/* ... */
thread {
    range = Range(-2, -1)
}
thread {
    val x = range ?: return
    // may print `[0, -1]`
    println(x)
}
```

Similarly, the program may raise `NullPointerException`,
even though it seemingly has no `null` at all,
resulting in what can be perceived as a violation of Kotlin's null safety guarantee:

```kotlin
class Inner(val x: Int)
class Holder(val ref: Inner)
/* ... */
var holder: Holder = Holder(Inner(23))
/* ... */
thread {
    holder = Holder(Inner(42))
}
thread {
    val h = holder
    if (h != null) {
        // may throw NullPointerException
        println(h.ref)
    }
}
```

#### Only the default construction guarantee

This is the simplest choice for Kotlin. 

**Benefits:**

* Simple and uniform semantics for all kinds of fields. 
* No special rules for leaking `this` case.
* Lesser soundness risk given the problem of safe publication on LLVM. 

**Risks:**

* Different semantics compared to Java, which may confuse developers coming from Java.
* Another thread may observe an object in an invalid partly constructed state (see above). 
* Breaks the null-safety guarantee for well-typed programs (see above).

#### Full construction guarantee for `val` fields

This is the same approach as taken by Java
(assuming we interpret Kotlin's `val` fields as Java's `final` fields).

**Benefits:**

* Being similar to Java.

**Risks:**

* This semantics breaks under "leaking `this`" problem.
* Formally unsound on LLVM (according to current LLVM spec).
* Requires changes in the Kotlin/Native compiler backend
  to ensure that memory fences are always inserted at the end of each constructor.
* Inherits the problematic and confusing behavior of Java, where
  initialization of regular and `final` fields is treated differently.

#### Full construction guarantee for all fields

Alternatively, it is possible for Kotlin to provide 
_full construction_ guarantee for both `val` and `var` fields.
A similar [experiment][11] was done for Java, 
and was shown to have neglectable performance impact.

**Benefits:**

* Simple and uniform semantics for all kinds of fields,
  and an improvement compared to Java.

**Risks:**

* This semantics breaks under "leaking `this`" problem.
* Formally unsound on LLVM (according to current LLVM spec).
* This requires changes in both in the Kotlin/Native and Kotlin/JVM compiler backends 
  to ensure that memory fences are always inserted at the end of each constructor. 
* Kotlin can only guarantee this for classes compiled by the Kotlin compiler.
  In Kotlin/JVM code, where classes from both Kotlin and Java can coexist,
  this results in a confusing situation where different classes can have
  different initialization semantics.

### Advantages of Strict Separation of Plain and Atomic Accesses

Having listed above the possible design choices with respect to 
different guarantees provided for plain accesses,
in this section we want to re-iterate on the advantages of an approach 
where the programming language provides only a minimal required set of guarantees.

With such a design choice, the language provides a clear separation between
the plain and atomic accesses.
Plain accesses are not meant to be used in a concurrent setting, 
and any data race on plain accesses should be treated as an error.
Every variable that could be accessed concurrently should be 
explicitly marked as atomic.

As of today Kotlin provides only `Volatile` atomics (`SeqCst` in terms of LLVM).
Marking all atomic variables as `Volatile`, 
including those which are subject only to "benign" data races, 
would lead to significant performance overhead.
This is the strongest access mode (both in JVM and LLVM), 
which upon compilation emits full memory fences.
However, in the future, it might be beneficial to also support in Kotlin 
other kinds of atomics with weaker access modes
(similar to the ones provided by JVM and LLVM).
Marking benign data races as relaxed atomics 
(`Opaque` [in terms of Java](https://gee.cs.oswego.edu/dl/html/j9mm.html#opaquesec), 
  and `Monotonic` [in terms of LLVM](https://llvm.org/docs/Atomics.html#monotonic))
would likely result in no observable performance penalty
compared to just using plain accesses. 

#### Advantages for the Developers

When all the racy variables are explicitly marked in the source code,
it becomes easier to understand the intents of the code author:
the atomicity marker serves as additional documentation for the semantics of a variable. 
Moreover, it becomes easier to navigate across the code base 
and search for all possible concurrent interactions.

Even in highly concurrent programs, atomic variables usually constitute only
a small fraction of all variables, with most of the variables being
scope or thread local, or protected by some synchronization primitive.
For this reason, it is not expected that the requirement to explicitly 
annotate all atomic variables would pose a serious burden to the developers.
 
#### Advantages for the Compiler

The compiler also can benefit from the explicit atomicity annotations.
With most of the variables being non-concurrently accessed,
and with a very relaxed set of guarantees for such variables,
the compiler has more freedom to optimize accesses to plain variables.

The example with the array of value classes from the Valhalla docs (see above)
is just one example of how excessive atomicity guarantees can 
preclude the compiler from various optimizations.
It is indeed possible to invent more examples of this sort.

These observations suggest that from the compiler's point of view,
the no-atomicity-guarantees (and minimal guarantees for racy accesses)
is a saner default than the atomicity-by-default.

In other words, the explicit atomicity annotations serve 
as an interface between the developer and the compiler,
where the developer clearly communicates what accesses are intended to be atomic, 
and gives the compiler a freedom to optimize the rest.

#### Advantages for the Tooling

Other language tooling, like the race detectors 
(e.g. [TSAN](https://clang.llvm.org/docs/ThreadSanitizer.html)), 
various static or runtime analyzers 
(e.g. [RacerD](https://fbinfer.com/docs/checker-racerd/)), 
and others, also benefit from the explicit atomicity annotations.

These tools often cannot compute the program semantics precisely,
they do not have any domain-specific knowledge about the user program,
and thus they cannot distinguish between "benign" and "malicious" data races.
Therefore, these tools usually just report all data races as errors.
This fact, combined with the "benign" data races perception in the Java community,
forces the developers to treat such reports as false positives, 
leading to a frustration with the tooling.
In such cases, a tool may provide some escape-hatch to allow a user 
to suppress reports on some data races.
In an afterthought, it is evident that such a solution 
is not radically different from just asking the user to 
explicitly mark the variable as atomic in the source code.

The problem of false positive data race reports already manifests 
in the Kotlin ecosystem today.
For example, there are several reported issues in the 
[kotlinx-coroutines](https://github.com/Kotlin/kotlinx.coroutines) library:
[1](https://github.com/Kotlin/kotlinx.coroutines/issues/3834), 
[2](https://github.com/Kotlin/kotlinx.coroutines/issues/3843).
The [solution](https://github.com/Kotlin/kotlinx.coroutines/pull/3873) 
currently employed by the library is to explicitly mark fields 
subject to "benign" data races with the custom `@BenignDataRace` 
[annotation](https://github.com/Kotlin/kotlinx.coroutines/blob/1a0287ca3fb5d6c59594d62131e878da4929c5f8/kotlinx-coroutines-core/common/src/internal/Concurrent.common.kt#L26),
which on the Kotlin/Native backed transform into `@Volatile`
[annotation](https://github.com/Kotlin/kotlinx.coroutines/blob/1a0287ca3fb5d6c59594d62131e878da4929c5f8/kotlinx-coroutines-core/native/src/internal/Concurrent.kt#L35).
Once Kotlin implements relaxed atomics, it would be possible to simply 
use relaxed atomic variable. 
This would also improve performance on Native by avoiding excessive memory fences.

The approach with explicit atomicity annotations has another advantage for the tooling.
Because non-concurrent variables usually constitute a dominant number 
of all variables in a program, an implementation of the analyzer tool
may benefit from optimizing the internal algorithms and data structures
for a common case of non-concurrent accesses.
Assumption that correct programs have no data races on plain variables
(and any data race should be reported as an error)
could help to additionally optimize the tool's algorithm.
See, for example, this [paper](http://www.cs.williams.edu/~freund/papers/09-pldi.pdf), 
for an example of such an optimization for a dynamic data race detector.

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

11. All Fields Are Final \
    _Aleksey Shipilëv_ (Blogpost) \
    [[Link]][11] 

[11]: https://shipilev.net/blog/2014/all-fields-are-final/

12. JavaScript Memory Model Specification (part of ECMA-262) \
    [[Link]][12]

[12]: https://262.ecma-international.org/14.0/?_gl=1*7kz3f*_ga*ODA0MDU0MDc5LjE3MDc3NjAxOTE.*_ga_TDCK4DWEPP*MTcwNzc2MDE5MC4xLjEuMTcwNzc2MDIwOC4wLjAuMA..#sec-memory-model

13. WebAssembly Threading and Atomic Memory Accesses Proposal \
    [[Link]][13]

[13]: https://github.com/WebAssembly/threads/blob/main/proposals/threads/Overview.md