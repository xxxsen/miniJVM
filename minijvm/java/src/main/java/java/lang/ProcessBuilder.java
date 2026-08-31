package java.lang;

import java.io.IOException;

public final class ProcessBuilder {
    private String[] command;

    public ProcessBuilder(String... command) {
        setCommand(command);
    }

    public ProcessBuilder command(String... command) {
        setCommand(command);
        return this;
    }

    public Process start() throws IOException {
        throw new IOException("Starting native processes is not supported by miniJVM");
    }

    private void setCommand(String[] command) {
        if (command == null) throw new NullPointerException();
        this.command = new String[command.length];
        System.arraycopy(command, 0, this.command, 0, command.length);
    }
}
