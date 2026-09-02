package org.mini.apploader;

import java.io.File;

public final class AppLoaderClassPathTest {
    private AppLoaderClassPathTest() {
    }

    public static void main(String[] args) {
        File firstDependency = new File("/dependencies/first.jar");
        File secondDependency = new File("/dependencies/second.jar");
        String[] paths = AppLoader.createAppClassPath(
                "/apps/runtime-adapter.jar", new File[]{firstDependency, secondDependency});

        if (paths.length != 3
                || !"/apps/runtime-adapter.jar".equals(paths[0])
                || !firstDependency.getAbsolutePath().equals(paths[1])
                || !secondDependency.getAbsolutePath().equals(paths[2])) {
            throw new AssertionError("application JAR must precede embedded dependencies");
        }
        System.out.println("AppLoader application-first class path verified.");
    }
}
