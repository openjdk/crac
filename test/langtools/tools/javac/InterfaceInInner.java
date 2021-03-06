/*
 * @test  /nodynamiccopyright/
 * @bug 4063740 6969184
 * @summary Interfaces can be declared in inner classes only for release >= 16
 * @author turnidge
 *
 * @compile/fail/ref=InterfaceInInner.out -XDrawDiagnostics --release 15 InterfaceInInner.java
 * @compile InterfaceInInner.java
 */
class InterfaceInInner {
    InterfaceInInner() {
        class foo {
            interface A {
            }
        }
    }
}
