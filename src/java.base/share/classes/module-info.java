/*
 * Copyright (c) 2014, 2025, Oracle and/or its affiliates. All rights reserved.
 * DO NOT ALTER OR REMOVE COPYRIGHT NOTICES OR THIS FILE HEADER.
 *
 * This code is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License version 2 only, as
 * published by the Free Software Foundation.  Oracle designates this
 * particular file as subject to the "Classpath" exception as provided
 * by Oracle in the LICENSE file that accompanied this code.
 *
 * This code is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License
 * version 2 for more details (a copy is included in the LICENSE file that
 * accompanied this code).
 *
 * You should have received a copy of the GNU General Public License version
 * 2 along with this work; if not, write to the Free Software Foundation,
 * Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301 USA.
 *
 * Please contact Oracle, 500 Oracle Parkway, Redwood Shores, CA 94065 USA
 * or visit www.oracle.com if you need additional information or have any
 * questions.
 */

/**
 * Defines the foundational APIs of the Java SE Platform.
 *
 * <dl class="notes">
 * <dt>Providers:</dt>
 * <dd> The JDK implementation of this module provides an implementation of
 *      the {@index jrt jrt} {@linkplain java.nio.file.spi.FileSystemProvider
 *      file system provider} to enumerate and read the class and resource
 *      files in a run-time image.
 *      The jrt file system can be created by calling
 *      {@link java.nio.file.FileSystems#getFileSystem
 *      FileSystems.getFileSystem(URI.create("jrt:/"))}.
 *      </dd>
 * </dl>
 *
 * @toolGuide java java launcher
 * @toolGuide keytool
 *
 * @provides java.nio.file.spi.FileSystemProvider
 *
 * @uses java.lang.System.LoggerFinder
 * @uses java.net.ContentHandlerFactory
 * @uses java.net.spi.URLStreamHandlerProvider
 * @uses java.nio.channels.spi.AsynchronousChannelProvider
 * @uses java.nio.channels.spi.SelectorProvider
 * @uses java.nio.charset.spi.CharsetProvider
 * @uses java.nio.file.spi.FileSystemProvider
 * @uses java.nio.file.spi.FileTypeDetector
 * @uses java.security.Provider
 * @uses java.text.spi.BreakIteratorProvider
 * @uses java.text.spi.CollatorProvider
 * @uses java.text.spi.DateFormatProvider
 * @uses java.text.spi.DateFormatSymbolsProvider
 * @uses java.text.spi.DecimalFormatSymbolsProvider
 * @uses java.text.spi.NumberFormatProvider
 * @uses java.time.chrono.AbstractChronology
 * @uses java.time.chrono.Chronology
 * @uses java.time.zone.ZoneRulesProvider
 * @uses java.util.spi.CalendarDataProvider
 * @uses java.util.spi.CalendarNameProvider
 * @uses java.util.spi.CurrencyNameProvider
 * @uses java.util.spi.LocaleNameProvider
 * @uses java.util.spi.ResourceBundleControlProvider
 * @uses java.util.spi.ResourceBundleProvider
 * @uses java.util.spi.TimeZoneNameProvider
 * @uses java.util.spi.ToolProvider
 * @uses javax.security.auth.spi.LoginModule
 *
 * @moduleGraph
 * @since 9
 */
module java.base {

    exports java.io;
    exports java.lang;
    exports java.lang.annotation;
    exports java.lang.classfile;
    exports java.lang.classfile.attribute;
    exports java.lang.classfile.constantpool;
    exports java.lang.classfile.instruction;
    exports java.lang.constant;
    exports java.lang.foreign;
    exports java.lang.invoke;
    exports java.lang.module;
    exports java.lang.ref;
    exports java.lang.reflect;
    exports java.lang.runtime;
    exports java.math;
    exports java.net;
    exports java.net.spi;
    exports java.nio;
    exports java.nio.channels;
    exports java.nio.channels.spi;
    exports java.nio.charset;
    exports java.nio.charset.spi;
    exports java.nio.file;
    exports java.nio.file.attribute;
    exports java.nio.file.spi;
    exports java.security;
    exports java.security.cert;
    exports java.security.interfaces;
    exports java.security.spec;
    exports java.text;
    exports java.text.spi;
    exports java.time;
    exports java.time.chrono;
    exports java.time.format;
    exports java.time.temporal;
    exports java.time.zone;
    exports java.util;
    exports java.util.concurrent;
    exports java.util.concurrent.atomic;
    exports java.util.concurrent.locks;
    exports java.util.function;
    exports java.util.jar;
    exports java.util.random;
    exports java.util.regex;
    exports java.util.spi;
    exports java.util.stream;
    exports java.util.zip;
    exports javax.crypto;
    exports javax.crypto.interfaces;
    exports javax.crypto.spec;
    exports javax.net;
    exports javax.net.ssl;
    exports javax.security.auth;
    exports javax.security.auth.callback;
    exports javax.security.auth.login;
    exports javax.security.auth.spi;
    exports javax.security.auth.x500;
    exports javax.security.cert;

    // additional qualified exports may be inserted at build time
    // see make/gensrc/GenModuleInfo.gmk

    exports com.sun.crypto.provider to
        jdk.crypto.cryptoki;
    exports sun.invoke.util to
        jdk.compiler;
    exports com.sun.security.ntlm to
        java.security.sasl;
    exports jdk.internal to
        jdk.incubator.vector;
    // Note: all modules in the exported list participate in preview  features
    // and therefore if they use preview features they do not need to be
    // compiled with "--enable-preview".
    // It is recommended for any modules that do participate that their
    // module declaration be annotated with jdk.internal.javac.ParticipatesInPreview
    exports jdk.internal.javac to
        java.compiler,
        java.desktop, // for ScopedValue
        jdk.compiler,
        jdk.incubator.vector, // participates in preview features
        jdk.jartool, // participates in preview features
        jdk.jdeps, // participates in preview features
        jdk.jfr, // participates in preview features
        jdk.jlink,   // participates in preview features
        jdk.jshell; // participates in preview features
    exports jdk.internal.access to
        java.desktop,
        java.logging,
        java.management,
        java.rmi,
        jdk.charsets,
        jdk.jartool,
        jdk.jlink,
        jdk.jfr,
        jdk.management,
        jdk.net,
        jdk.sctp,
        jdk.crypto.cryptoki;
    exports jdk.internal.crac to
        java.rmi,
        jdk.jfr,
        jdk.management.agent,
        jdk.sctp;
    exports jdk.internal.classfile.components to
        jdk.jfr;
    exports jdk.internal.foreign to
        jdk.incubator.vector;
    exports jdk.internal.event to
        jdk.jfr;
    exports jdk.internal.io to
        jdk.internal.le,
        jdk.jshell;
    exports jdk.internal.jimage to
        jdk.jlink;
    exports jdk.internal.jimage.decompressor to
        jdk.jlink;
    exports jdk.internal.loader to
        java.instrument,
        java.logging,
        java.naming;
    exports jdk.internal.jmod to
        jdk.compiler,
        jdk.jlink;
    exports jdk.internal.logger to
        java.logging;
    exports jdk.internal.org.xml.sax to
        jdk.jfr;
    exports jdk.internal.org.xml.sax.helpers to
        jdk.jfr;
    exports jdk.internal.misc to
        java.desktop,
        java.logging,
        java.management,
        java.naming,
        java.net.http,
        java.rmi,
        java.security.jgss,
        jdk.attach,
        jdk.charsets,
        jdk.compiler,
        jdk.crypto.cryptoki,
        jdk.incubator.vector,
        jdk.jfr,
        jdk.jshell,
        jdk.nio.mapmode,
        jdk.unsupported,
        jdk.internal.vm.ci,
        jdk.graal.compiler;
    exports jdk.internal.module to
        java.instrument,
        java.management.rmi,
        jdk.jartool,
        jdk.compiler,
        jdk.jfr,
        jdk.jlink,
        jdk.jpackage;
    exports jdk.internal.perf to
        java.management,
        jdk.management.agent,
        jdk.internal.jvmstat;
    exports jdk.internal.platform to
        jdk.management,
        jdk.jfr;
    exports jdk.internal.ref to
        java.desktop,
        java.net.http,
        jdk.naming.dns;
    exports jdk.internal.reflect to
        java.logging,
        java.sql,
        java.sql.rowset,
        jdk.dynalink,
        jdk.internal.vm.ci,
        jdk.unsupported;
    exports jdk.internal.vm to
        java.management,
        jdk.internal.jvmstat,
        jdk.management,
        jdk.management.agent,
        jdk.internal.vm.ci,
        jdk.jfr;
    exports jdk.internal.vm.annotation to
        java.instrument,
        jdk.internal.vm.ci,
        jdk.incubator.vector,
        jdk.jfr,
        jdk.unsupported;
    exports jdk.internal.vm.vector to
        jdk.incubator.vector;
    exports jdk.internal.util.xml to
        jdk.jfr;
    exports jdk.internal.util.xml.impl to
        jdk.jfr;
    exports jdk.internal.util to
        java.desktop,
        java.prefs,
        java.security.jgss,
        java.smartcardio,
        java.naming,
        java.rmi,
        java.net.http,
        jdk.charsets,
        jdk.incubator.vector,
        jdk.internal.vm.ci,
        jdk.httpserver,
        jdk.jlink,
        jdk.jpackage,
        jdk.net;
    exports sun.net to
        java.net.http,
        jdk.naming.dns;
    exports sun.net.ext to
        jdk.net;
    exports sun.net.dns to
        java.security.jgss,
        jdk.naming.dns;
    exports sun.net.util to
        java.net.http,
        jdk.jconsole,
        jdk.sctp;
    exports sun.net.www to
        java.net.http,
        jdk.jartool;
    exports sun.net.www.protocol.http to
        java.security.jgss;
    exports sun.nio.ch to
        java.management,
        jdk.crypto.cryptoki,
        jdk.net,
        jdk.sctp;
    exports sun.nio.cs to
        jdk.charsets;
    exports sun.nio.fs to
        jdk.net;
    exports sun.reflect.annotation to
        jdk.compiler;
    exports sun.reflect.generics.reflectiveObjects to
        java.desktop;
    exports sun.reflect.misc to
        java.desktop,
        java.management;
    exports sun.security.internal.interfaces to
        jdk.crypto.cryptoki;
    exports sun.security.internal.spec to
        jdk.crypto.cryptoki;
    exports sun.security.jca to
        java.smartcardio,
        jdk.crypto.cryptoki,
        jdk.naming.dns;
    exports sun.security.pkcs to
        jdk.jartool;
    exports sun.security.provider to
        java.security.jgss,
        jdk.crypto.cryptoki,
        jdk.security.auth;
    exports sun.security.provider.certpath to
        java.naming,
        jdk.jartool;
    exports sun.security.rsa to
        jdk.crypto.cryptoki;
    exports sun.security.timestamp to
        jdk.jartool;
    exports sun.security.tools to
        jdk.jartool;
    exports sun.security.util to
        java.naming,
        java.security.jgss,
        java.security.sasl,
        java.smartcardio,
        java.xml.crypto,
        jdk.crypto.cryptoki,
        jdk.jartool,
        jdk.security.auth,
        jdk.security.jgss;
    exports sun.security.x509 to
        jdk.crypto.cryptoki,
        jdk.jartool;
    exports sun.security.validator to
        jdk.jartool;
    exports sun.util.cldr to
        jdk.jlink;
    exports sun.util.locale.provider to
        java.desktop,
        jdk.jlink,
        jdk.localedata;
    exports sun.util.logging to
        java.desktop,
        java.logging,
        java.prefs;
    exports sun.util.resources to
        jdk.localedata;

    // the service types defined by the APIs in this module

    uses java.lang.System.LoggerFinder;
    uses java.net.ContentHandlerFactory;
    uses java.net.spi.InetAddressResolverProvider;
    uses java.net.spi.URLStreamHandlerProvider;
    uses java.nio.channels.spi.AsynchronousChannelProvider;
    uses java.nio.channels.spi.SelectorProvider;
    uses java.nio.charset.spi.CharsetProvider;
    uses java.nio.file.spi.FileSystemProvider;
    uses java.nio.file.spi.FileTypeDetector;
    uses java.security.Provider;
    uses java.text.spi.BreakIteratorProvider;
    uses java.text.spi.CollatorProvider;
    uses java.text.spi.DateFormatProvider;
    uses java.text.spi.DateFormatSymbolsProvider;
    uses java.text.spi.DecimalFormatSymbolsProvider;
    uses java.text.spi.NumberFormatProvider;
    uses java.time.chrono.AbstractChronology;
    uses java.time.chrono.Chronology;
    uses java.time.zone.ZoneRulesProvider;
    uses java.util.spi.CalendarDataProvider;
    uses java.util.spi.CalendarNameProvider;
    uses java.util.spi.CurrencyNameProvider;
    uses java.util.spi.LocaleNameProvider;
    uses java.util.spi.ResourceBundleControlProvider;
    uses java.util.spi.ResourceBundleProvider;
    uses java.util.spi.TimeZoneNameProvider;
    uses java.util.spi.ToolProvider;
    uses javax.security.auth.spi.LoginModule;

    // JDK-internal service types

    uses jdk.internal.io.JdkConsoleProvider;
    uses jdk.internal.logger.DefaultLoggerFinder;
    uses sun.text.spi.JavaTimeDateTimePatternProvider;
    uses sun.util.spi.CalendarProvider;
    uses sun.util.locale.provider.LocaleDataMetaInfo;
    uses sun.util.resources.LocaleData.CommonResourceBundleProvider;
    uses sun.util.resources.LocaleData.SupplementaryResourceBundleProvider;

    // Built-in service providers that are located via ServiceLoader

    provides java.nio.file.spi.FileSystemProvider with
        jdk.internal.jrtfs.JrtFileSystemProvider;

    exports jdk.internal.crac.mirror to
        java.rmi,
        jdk.crac,
        jdk.jfr,
        jdk.management.agent;

    exports jdk.internal.crac.mirror.impl to
        jdk.crac;
}
