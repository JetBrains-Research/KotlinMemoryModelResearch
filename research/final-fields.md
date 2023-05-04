# Semantics of Final Fields 

This doc discuss research questions related to `final` fields semantics 
in the JMM and whether we need/should provide the same semantics in Kotlin memory model.

## Introduction

In Java, final fields have special treatment in the memory model. 
The classic example demonstrating this looks as follows:

```java
class A {
    final int x;
    A() { x = 42; }

    static A a;
}

void writer() {
    a = new A();
}

void reader() {
    if (a != null) {
        int y = a.x; // guarantee to read 42
    }     
}

```

Note that despite the program above has a race on variable `a`
the JMM guarantees that the read of the final field will observe 
the initialization write of this field in the constructor.
Moreover, the read will synchronize with this write.
There are special JMM rule around final fields that induces happens-before
between write to `x` and read from `x` in this scenario.

The motivation for this special treatment of final fields was (apparently) to allow 
safe publication of immutable objects even in the presence of data races.

Additional reading:

* [1] [Final fields](https://shipilev.net/blog/2014/jmm-pragmatics/#_part_v_finals) in the Aleksey Shipilev's JMM blogpost.
* [2] [Specification](https://docs.oracle.com/javase/specs/jls/se8/html/jls-17.html#jls-17.5) of final fields semantics.
* [3] Another [blogpost](https://shipilev.net/blog/2014/all-fields-are-final/) by Aleksey Shipilev about 
  enforcing same semantics for all fields (not just final).
 
## Problem

In Kotlin, `val` fields are likely to be compiled as `final` fields for JVM target.
Thus, the equivalent of the Java program given above in Kotlin should look like.

```kotlin
class A {
   val x: Int

   init {
      x = 42
   }
}

var a: A? = null

fun writer() {
   a = A()
}

fun reader() {
   if (a != null) {
      val y = a.x // what can be read here?
   }
}
```

Thus, on JVM this Kotlin program is likely to produce the same outcome as in Java.
The question is --- does the same behavior is guaranteed in Kotlin/Native?
And should we provide the Java's `final` fields semantics in Kotlin at all?
There are several obstacles with it. 


### Current Kotlin/Native compiler bug

In Java, in order to guarantee the `final` fields semantics described above, 
the compiler **emit `release` fence** at the end of every constructor
of an object containing `final` fields (see [1,3]).
This fence is required to prevent the reordering 
of publication write `a = A()` with field initialization write `x = 42`
(and, possibly, with reads on which initialization writes depend).
Currently, K/N compiler omits this `release` fence.

Moreover, even with the `release` fence in place, 
the LLVM specification formally allows to reorder non-atomic write before fence,
although the current LLVM compiler implementation likely never does this.
This means that if the plain non-atomic Kotlin accesses are compiled as `NotAtomic` accesses in LLVM,
`release` fence still cannot prevent reordering of publication and initialization writes.
It is unclear from the LLVM spec, whether using `Unordered` memory order solves this issue. 
However, we can speculate that it does, because the 
[relevant fragment of the C++ spec](https://en.cppreference.com/w/cpp/atomic/atomic_thread_fence)
(from which LLVM inherits), says that atomic store with any memory order cannot be reordered 
with `release` fence.

### Unsoundness w.r.t. LLVM

Even with release fence inserted at the end of constructor,
and with publication write marked as `Unordered`,
the synchronization pattern described above is **formally unsound** according to the LLVM memory model. 
In LLVM the only way to obtain happens-before relation
between events is by using acquire/release (or stronger) atomics or fences. 
Thus, the LLVM memory model prescribes to mark load from variable `a` in reader thread as acquire load.

Without the acquire fence, the code above has the data race. 
If non-atomic accesses in Kotlin are compiled with `NotAtomic` access mode in LLVM, 
then the racy code above has undefined behavior (the load returns special `undef` value)
and thus we do not have any guarantees on the behavior of the program.
If non-atomic accesses in Kotlin are compiled with `Unordered` access mode in LLVM,
then, technically, there is no undefined behavior, but in reality the behavior 
of `Unordered` accesses is vaguely defined, and we still cannot formally reason about it
(see this [doc](https://github.com/JetBrains-Research/KotlinMemoryModelResearch/blob/master/research/non-atomics-compilation.md)). 

In practice, the above code example (and similar simple examples) are expected to behave correctly. 
But what can happen in larger non-trivial examples is unclear, because various optimizations 
performed by LLVM can interfere in unexpected ways. 
For example, LLVM can perform [speculative load introduction](https://discourse.llvm.org/t/speculative-load-optimization/46527) 
optimization, which can move a load instruction out of the branches of conditional.

To sum up, the final field synchronization patter is unsound according to the LLVM memory model.
Relying on this unsound pattern would put Kotlin at risk:
we cannot expect anything from LLVM, we cannot report back to LLVM any unexpected behavior as a bug, etc.   

### Semantics Discrepancy w.r.t. LLVM

In JMM spec the definition of happens-before rules w.r.t. final fields is quite complicated (see [2]). 
What is worse, it defines "dereference chain" relation 
(roughly, this is similar to address dependency relation in some hardware models)
which has no direct counterpart in C/C++ or LLVM memory models.
The problem is similar to the abandoned `memory_order_consume` access mode in C/C++ and LLVM models. 
Programming language memory models deliberately avoid defining "dependency" relations 
because in the presence of compiler optimizations, that can remove syntactic dependencies, 
precise definition of "dependency" relation is an open research problem.
All of this means that at least from the specification point of view it will be impossible
to provide same guarantees for `final` fields in Kotlin/Native as in Kotlin/JVM.   

### `this` reference leakage problem

The `final` fields semantics in JMM itself is often criticised for being tricky (see [1,3]).
In particular, one of the problems is that `final` fields guarantees are not provided by the JMM
if the field can leak from the constructor into another thread (see [1] for an example why this case is problematic).

Here is an example:

```kotlin
class A {
   val x: Int

   init {
      x = 42
      a = this
   }
}

var a: A? = null

fun writer() {
   a = A()
}

fun reader() {
   if (a != null) {
      val y = a.x // can read 0
   }
}
```

## Safe Initialization Problem

Note that the problem similar to `final` fields semantics 
arises with respect to default initialization of fields.

```kotlin
class A {
   // default initialization; 
   // note that the actual store is elided when compiling to Java bytecode,
   // because Java guarantees all fields are initialized with default value of corresponding type
   val x: Int = 0
}

var a: A? = null

fun writer() {
   a = A()
}

fun reader() {
   if (a != null) {
      val y = a.x // guarantee to read 0
   }
}
```

In Java, this behavior is achieved using the same technique --- by putting 
a release fence after object initialization.
Thus, the code above is subject to the same unsoundness issue according to LLVM memory model. 

However, unlike with the case of `final` fields semantics, 
Kotlin, as any other "safe" language, cannot abandon safe initialization guarantee ---
otherwise it would be possible for loads to read "garbage" values from uninitialized fields. 

Thus, Kotlin has to guarantee safe initialization, even though the current 
implementation in Kotlin/Native is formally unsound. 

However, unlike the case of controversial `final` fields semantics,
the failure to deliver the very fundamental safe initialization guarantee in practice 
could be classified as serious problem in LLVM, that would 
require the LLVM developers to take some action in case we ever found any bugs,
because it would affect all "safe" languages compiled through LLVM.

Another difference of safe initialization, is that unlike the case of `final` fields,
it is not a subject to the `this` reference leakage, 
because there is no user code run before the default initialization.  

## Questions

* What should be the semantics of `val` and `var` fields initialization in Kotlin?
* Do we want to provide same semantics for `val` Kotlin fields as for Java's `final` fields?
* Do we want to provide same semantics for all Kotlin fields (`val` and `var`) as for Java's `final` fields?
* If we do not guarantee `final` fields semantics for Kotlin `val`, can it break safe publication patterns 
  in existing Kotlin code when it is compiled to Native target?
* What are performance impact for Kotlin/Native if we enforce `final` field semantics for `val` (and `var`)?

### Steps to Answer the Questions

1. Write a litmus test to check if `final` fields guarantee can be broken on Kotlin/Native for `val` fields
   (it is likely to be so, because my guess that K/N compiler currently do not put release fence at the end of the constructor). 

2. Evaluate performance impact of supporting `final` fields semantics in Kotlin/Native.

3. Review how safe publication idioms are currently implemented in typical Kotlin code 
   (e.g. singleton pattern --- `object` [keyword](https://kotlinlang.org/docs/object-declarations.html#object-declarations-overview) in Kotlin). 
   If they rely on `val` field with the intended `final` semantics, try to reproduce a bug with Kotlin/Native.

## Possible Solutions

1. Do not support `final` fields semantics in Kotlin in any form.

   **Pros**: simplicity, requires no modification in K/N compiler.

   **Cons**: might deem some safe publication idioms already prevailed in Kotlin codebase incorrect.

2. Support `final` fields semantics in Kotlin for `val` fields only.

   **Pros**: close to current JMM formulation.

   **Cons**: requires changes to K/N compiler, formally invalid according to the current LLVM spec, inherits problems of JMM `final` fields semantics.

3. Support `final` fields semantics in Kotlin for all fields.

   **Pros**: more consistent and simpler model than current JMM formulation.

   **Cons**: requires changes both to K/JVM and K/N compiler, still invalid according to the current LLVM spec, 
             inherits problems of JMM `final` fields semantics, we still cannot enforce this semantics for Java classes.


## Alternative Solution to Safe Publication Problem

If it will be decided to drop `final` fields semantics in Kotlin, 
there should be some alternative provided that will cover at least some use-cases of `final` fields.
One of the most important use-cases is sharing of immutable data structures
with lightweight synchronization. 

We argue that this case can be covered with the help of `release` and `acquire` access modes.
The idea is to use `release` write as a publication write, and `acquire` read in the reader thread.
Note that there is no need to mark all accesses to immutable data structure as release/acquire, 
the user only need one write/read pair to achieve synchronization, all subsequent reads can be non-atomic.
So when the user wants to share a reference to immutable data structure, 
he/she should explicitly use atomic variable as a "communication channel".

Advantages of this approach are listed below.

* Unlike the case of `final` fields semantics, the synchronization idiom 
  with a pair of `release` store and `acquire` load is formally sound with respect to LLVM memory model.

* Synchronization through atomic variable with `release` and `acquire` accesses is *explicit*, 
  it directly expresses the intention of the code author, it can be easily searched in the codebase, etc.  

* Many other languages also already adopted `acquire` and `release` access modes,
  while `final` fields semantics remains Java-specific feature.

