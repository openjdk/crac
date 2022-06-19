import java.awt.*;
import java.awt.event.*;

public class AWTDemoApp {
    private static final Object sync = new Object();
    private static int frameCounter = 0;

    private static void createFrame() {
        frameCounter++;

        Frame frame = new Frame("AWTAppDemo");
        frame.addWindowListener(new WindowAdapter() {
            @Override
            public void windowClosing(WindowEvent e) {
                System.exit(0);
            }
        });
        
        Button button = new Button();
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