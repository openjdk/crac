import java.awt.*;

public class AWTApp {
	public static void main(String[] args) throws Exception {
		int sleep1 = Integer.parseInt(args[0]);
		int sleep2 = Integer.parseInt(args[1]);
		boolean fst = 0 != Integer.parseInt(args[2]);
		boolean snd = 0 != Integer.parseInt(args[3]);

		Frame frame = new Frame("AWTApp");
		frame.setVisible(true);
		System.out.println("Window 1 created");
		Thread.sleep(sleep1);

		frame.removeNotify();
		System.out.println("Window 1 deleted");
		Thread.sleep(sleep1);

		System.out.println("Try to make a checkpoint and then restore...");
		try {
			jdk.crac.Core.checkpointRestore();
		} catch (jdk.crac.CheckpointException | jdk.crac.RestoreException e) {
			e.printStackTrace();
		}
		System.out.println("Checkpoint restored");
		Thread.sleep(sleep2);

		frame = new Frame();
		frame.setVisible(true);
		System.out.println("Window 2 created");
		Thread.sleep(sleep2);

		System.out.println("Exit");
	}
}
