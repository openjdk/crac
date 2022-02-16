import javax.swing.*;

public class UIApp {
	public static void main(String[] args) throws Exception {
		int sleep1 = Integer.parseInt(args[0]);
		int sleep2 = Integer.parseInt(args[1]);
		boolean fst = 0 != Integer.parseInt(args[2]);
		boolean snd = 0 != Integer.parseInt(args[3]);

		SwingUtilities.invokeAndWait(() -> {
			if (fst) {
				(new JFrame("test")).setVisible(true);
			}
		});

		System.out.println("Window created");
		Thread.sleep(sleep1);

		System.out.println("Try to make a checkpoint and then restore...");
		SwingUtilities.invokeAndWait(() -> {
			try {
				jdk.crac.Core.checkpointRestore();
			} catch (jdk.crac.CheckpointException | jdk.crac.RestoreException e) {
				e.printStackTrace();
			}
		});

		System.out.println("Checkpoint restored");
		Thread.sleep(sleep2);

		SwingUtilities.invokeAndWait(() -> {
			if (snd) {
				(new JFrame("test")).setVisible(true);
			}
		});
	}
}
