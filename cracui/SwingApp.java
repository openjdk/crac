import javax.swing.*;

public class SwingApp {
	public static void main(String[] args) throws Exception {
		int sleep1 = Integer.parseInt(args[0]);
		int sleep2 = Integer.parseInt(args[1]);
		boolean fst = 0 != Integer.parseInt(args[2]);
		boolean snd = 0 != Integer.parseInt(args[3]);

		JFrame jframe = new JFrame("SwingApp");
		jframe.setDefaultCloseOperation(WindowConstants.EXIT_ON_CLOSE);
		jframe.setSize(800, 600);
		jframe.setVisible(true);
		System.out.println("Window 1 created");
		Thread.sleep(sleep1);

		System.out.println("Try to make a checkpoint and then restore...");
		try {
			jdk.crac.Core.checkpointRestore();
		} catch (jdk.crac.CheckpointException | jdk.crac.RestoreException e) {
			e.printStackTrace();
		}
		System.out.println("Checkpoint restored");

		jframe = new JFrame("SwingApp");
		jframe.setDefaultCloseOperation(WindowConstants.EXIT_ON_CLOSE);
		jframe.setSize(800, 600);
		jframe.setVisible(true);
		System.out.println("Window 2 created");
		Thread.sleep(sleep2);

		System.out.println("Exit");
		System.exit(0);
	}
}
