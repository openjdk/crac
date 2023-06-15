package jdk.crac.impl;

import sun.security.action.GetPropertyAction;

import java.io.FileInputStream;
import java.io.FileReader;
import java.io.IOException;
import java.nio.charset.StandardCharsets;
import java.util.Properties;
import java.util.regex.Pattern;

final class PolicyUtils {
    static Pattern NUMERIC = Pattern.compile("[0-9]+");

    private PolicyUtils() {}

    static Properties loadProperties(String type, String systemProperty) {
        Properties properties = new Properties();
        String file = GetPropertyAction.privilegedGetProperty(systemProperty + ".file");
        if (file != null) {
            try {
                if (file.length() >= 4 && file.substring(file.length() - 4).equalsIgnoreCase(".xml")) {
                    try (var fis = new FileInputStream(file)) {
                        properties.loadFromXML(fis);
                    }
                } else {
                    try (var fr = new FileReader(file, StandardCharsets.UTF_8)) {
                        properties.load(fr);
                    }
                }
            } catch (IOException e) {
                throw new RuntimeException(String.format(
                        "Failed to read %s policies from %s: %s", type, file, e.getMessage()));
            }
        }
        String property = GetPropertyAction.privilegedGetProperty(systemProperty);
        if (property != null) {
            for (var item : property.split(";")) {
                int eqIndex = findNonEscaped(item, 0, '=');
                if (eqIndex < 0) {
                    throw new IllegalArgumentException(String.format(
                            "Invalid specification for %s policy: %s", type, item));
                } else {
                    properties.put(unescape(item, 0, eqIndex), item.substring(eqIndex + 1));
                }
            }
        }
        return properties;
    }

    static String unescape(String str, int fromIndex, int toIndex) {
        boolean escaped = false;
        StringBuilder sb = new StringBuilder(str.length() - fromIndex);
        for (int i = fromIndex; i < toIndex; ++i) {
            char c = str.charAt(i);
            if (!escaped && c == '\\') {
                escaped = true;
            } else {
                sb.append(c);
                escaped = false;
            }
        }
        return sb.toString();
    }

    static int findNonEscaped(String str, int fromIndex, char character) {
        boolean escaped = false;
        for (int i = fromIndex; i < str.length(); ++i) {
            char c = str.charAt(i);
            if (c == '\\') {
                escaped = !escaped;
            } else if (c == character && !escaped) {
                return i;
            }
        }
        return -1;
    }
}
