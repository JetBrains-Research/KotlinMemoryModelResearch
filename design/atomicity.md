# Design notes on atomicity guarantees for plain memory accesses in Kotlin

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
Kotlin also provides several ways to mark variables (i.e., either fields of array elements) as atomic: 
- via the [`@Volatile` annotation](https://kotlinlang.org/api/latest/jvm/stdlib/kotlin.concurrent/-volatile/);
- via the [`Atomic*` classes](https://kotlinlang.org/api/latest/jvm/stdlib/kotlin.concurrent/) from a standard library;  
- via the [`atomicfu` library](https://github.com/Kotlin/kotlinx-atomicfu).

For atomic variables, the atomicity of read and writes is guaranteed as expected.
However, for plain variables, the situation is less obvious,
and depends on several factors, including the design principles 
of the particular programming language.

This document discusses the design choices of the Kotlin language
with respect to atomicity guarantees for plain memory accesses.  
This design should take into account two main concerns:
providing reasonable and predictable semantics for the Kotlin developers,
while taking into account the constraints imposed by semantics of 
plain accesses in Kotlin backends (JVM, LLVM, and JS/WASM). 

## Atomicity of Plain Accesses

In this section, we overview the atomicity guarantees 
provided by the main backends of the Kotlin language.

### Atomicity in JVM

JVM [guarantees](https://docs.oracle.com/javase/specs/jls/se8/html/jls-17.html#jls-17.7) atomicity 
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

### Atomicity in LLVM

### Atomicity in JS & WebAssembly

## Data Races

## Compilation Strategies

### JVM

### LLVM