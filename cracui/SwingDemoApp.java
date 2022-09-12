import javax.swing.*;
import java.awt.event.*;

public class SwingDemoApp {
    private static final Object sync = new Object();
    private static int frameCounter = 0;

    private static void createFrame() {
        frameCounter++;

        JFrame frame = new JFrame("SwingAppDemo");
        frame.setDefaultCloseOperation(WindowConstants.EXIT_ON_CLOSE);

        JButton button = new JButton();
        button.addActionListener(e -> {
            synchronized (sync) {
                sync.notify();
            }
        });

        frame.add(button);
        frame.setSize(800, 600);
        frame.setVisible(true);

        System.out.println(frame.getTitle() + " created " + frameCounter);
    }

    public static void main(String[] args) throws Exception {
        while (true) {
            createFrame();

            synchronized (sync) {
                sync.wait();
            }

            System.out.println("Try to make a checkpoint and then restore...");
            try {
                jdk.crac.Core.checkpointRestore();
            } catch (jdk.crac.CheckpointException | jdk.crac.RestoreException e) {
                e.printStackTrace();
            }
            System.out.println("Checkpoint restored");
        }
    }
}