/*
 * @test /nodynamiccopyright/
 * @bug 8206986
 * @summary Verify "case null" is not allowed for --release 16
 * @compile/fail/ref=SwitchNullDisabled.out -XDrawDiagnostics --release 16 SwitchNullDisabled.java
 * @compile/fail/ref=SwitchNullDisabled-preview.out -XDrawDiagnostics SwitchNullDisabled.java
 * @compile --enable-preview -source ${jdk.version} SwitchNullDisabled.java
 */

public class SwitchNullDisabled {
    private int switchNull(String str) {
        switch (str) {
            case null: return 0;
            case "": return 1;
            default: return 2;
        }
    }
}
