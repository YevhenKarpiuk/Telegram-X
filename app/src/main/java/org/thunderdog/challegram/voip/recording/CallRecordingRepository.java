/*
 * This file is a part of Telegram X
 * Copyright © 2014 (tgx-android@pm.me)
 */
package org.thunderdog.challegram.voip.recording;

import android.content.ContentResolver;
import android.content.Context;
import android.net.Uri;
import android.os.Process;

import androidx.annotation.NonNull;
import androidx.annotation.Nullable;

import org.json.JSONArray;
import org.json.JSONException;
import org.json.JSONObject;
import org.thunderdog.challegram.core.Background;
import org.thunderdog.challegram.tool.UI;

import java.io.BufferedInputStream;
import java.io.BufferedOutputStream;
import java.io.ByteArrayOutputStream;
import java.io.File;
import java.io.FileInputStream;
import java.io.FileOutputStream;
import java.io.IOException;
import java.io.InputStream;
import java.io.OutputStream;
import java.nio.charset.StandardCharsets;
import java.text.ParseException;
import java.text.SimpleDateFormat;
import java.util.ArrayList;
import java.util.Arrays;
import java.util.Collections;
import java.util.Comparator;
import java.util.Date;
import java.util.List;
import java.util.Locale;
import java.util.TimeZone;
import java.util.concurrent.TimeUnit;
import java.util.concurrent.atomic.AtomicLong;
import java.util.zip.ZipEntry;
import java.util.zip.ZipOutputStream;

public final class CallRecordingRepository {
  public enum ExportVariant { MIXED, LOCAL, REMOTE, ALL }

  public interface Callback<T> {
    void onResult (@Nullable T result, @Nullable String error);
  }

  public static final class PreparedExport {
    public final File file;
    public final String mimeType;

    PreparedExport (File file, String mimeType) {
      this.file = file;
      this.mimeType = mimeType;
    }
  }

  private static final String INFO_FILE = "info.json";
  private static final String MIXED_FILE = "mixed.opus";
  private static final String LOCAL_FILE = "local.opus";
  private static final String REMOTE_FILE = "remote.opus";
  private static final String ACTIVE_MARKER = ".in_progress";
  private static final int COPY_BUFFER_SIZE = 32 * 1024;
  private static final int MAX_METADATA_BYTES = 2 * 1024 * 1024;
  private static final int MAX_EXPORT_NAME_CODE_POINTS = 64;
  private static final int DEFAULT_TIMELINE_SAMPLE_RATE = 48000;
  static final long EXPORT_MAX_AGE_MS = TimeUnit.HOURS.toMillis(24);
  private static final String EXPORT_DIRECTORY_PREFIX = "export_";
  private static final AtomicLong NEXT_EXPORT_ID = new AtomicLong();

  private final @Nullable Context context;
  private final File callsRoot;
  private final File exportRoot;
  private final int currentPid;

  public CallRecordingRepository (@NonNull Context context) {
    this(context, new File(context.getFilesDir(), "calls"),
      new File(context.getCacheDir(), "call_recording_exports"), Process.myPid());
  }

  private CallRecordingRepository (Context context, File callsRoot, File exportRoot,
                                   int currentPid) {
    this.context = context.getApplicationContext();
    this.callsRoot = callsRoot;
    this.exportRoot = exportRoot;
    this.currentPid = currentPid;
  }

  CallRecordingRepository (File callsRoot, File exportRoot, int currentPid) {
    this.context = null;
    this.callsRoot = callsRoot;
    this.exportRoot = exportRoot;
    this.currentPid = currentPid;
  }

  public void scan (Callback<List<CallRecordingItem>> callback) {
    Background.instance().post(() -> {
      List<CallRecordingItem> result;
      String error = null;
      try {
        result = scanNow();
      } catch (Throwable t) {
        result = Collections.emptyList();
        error = t.getMessage();
      }
      final List<CallRecordingItem> immutable = Collections.unmodifiableList(result);
      final String finalError = error;
      UI.post(() -> callback.onResult(immutable, finalError));
    });
  }

  List<CallRecordingItem> scanNow () throws IOException {
    ArrayList<CallRecordingItem> result = new ArrayList<>();
    File[] directories = callsRoot.listFiles(File::isDirectory);
    if (directories == null) {
      return result;
    }
    for (File directory : directories) {
      try {
        if (isDirectChild(callsRoot, directory)) {
          result.add(parseDirectory(directory));
        }
      } catch (Throwable ignored) {
        // A broken recording never hides healthy siblings.
        try {
          result.add(fallbackItem(directory));
        } catch (Throwable ignoredAgain) {
        }
      }
    }
    result.sort(Comparator
      .comparingLong((CallRecordingItem item) -> item.recordingStartTime)
      .reversed()
      .thenComparing(item -> item.sessionId, Comparator.reverseOrder()));
    return result;
  }

  public void delete (CallRecordingItem item, Callback<Boolean> callback) {
    Background.instance().post(() -> {
      boolean deleted = false;
      String error = null;
      try {
        deleted = deleteNow(item);
      } catch (Throwable t) {
        error = t.getMessage();
      }
      final boolean finalDeleted = deleted;
      final String finalError = error;
      UI.post(() -> callback.onResult(finalDeleted, finalError));
    });
  }

  public void prepareExport (
    CallRecordingItem item,
    ExportVariant variant,
    Callback<PreparedExport> callback
  ) {
    Background.instance().post(() -> {
      PreparedExport result = null;
      String error = null;
      try {
        result = prepareExportNow(item, variant);
      } catch (Throwable t) {
        error = t.getMessage();
      }
      final PreparedExport finalResult = result;
      final String finalError = error;
      UI.post(() -> callback.onResult(finalResult, finalError));
    });
  }

  public void save (
    CallRecordingItem item,
    ExportVariant variant,
    Uri destination,
    Callback<Boolean> callback
  ) {
    Background.instance().post(() -> {
      boolean saved = false;
      String error = null;
      try {
        if (item.isActive()) {
          throw new IOException("Active recording cannot be exported");
        }
        requireDirectSession(item);
        if (context == null) {
          throw new IOException("Android context is unavailable");
        }
        ContentResolver resolver = context.getContentResolver();
        try (OutputStream rawOutput = resolver.openOutputStream(destination, "w")) {
          if (rawOutput == null) {
            throw new IOException("Document provider did not open the destination");
          }
          try (OutputStream output = new BufferedOutputStream(rawOutput)) {
            writeExport(item, variant, output);
          }
        }
        saved = true;
      } catch (Throwable t) {
        error = t.getMessage();
      }
      final boolean finalSaved = saved;
      final String finalError = error;
      UI.post(() -> callback.onResult(finalSaved, finalError));
    });
  }

  public String suggestedExportName (CallRecordingItem item, ExportVariant variant) {
    String base = friendlyBaseName(item);
    return variant == ExportVariant.ALL
      ? base + ".zip"
      : base + "_" + variantLabel(variant) + ".opus";
  }

  boolean deleteNow (CallRecordingItem item) throws IOException {
    if (item.isActive()) {
      throw new IOException("Active recording cannot be deleted");
    }
    requireDirectSession(item);
    if (!item.sessionPath.isDirectory()) {
      throw new IOException("Recording directory is missing");
    }
    boolean deleted = deleteTreeInside(callsRoot, item.sessionPath);
    if (!deleted || item.sessionPath.exists()) {
      throw new IOException("Recording was only partially deleted");
    }
    return true;
  }

  PreparedExport prepareExportNow (CallRecordingItem item, ExportVariant variant)
    throws IOException {
    if (item.isActive()) {
      throw new IOException("Active recording cannot be exported");
    }
    requireDirectSession(item);
    if (!item.sessionPath.isDirectory()) {
      throw new IOException("Recording directory is missing");
    }
    if (!exportRoot.mkdirs() && !exportRoot.isDirectory()) {
      throw new IOException("Cannot create export cache");
    }
    cleanupStaleExports(System.currentTimeMillis());
    File exportDirectory = createUniqueExportDirectory();
    String baseName = friendlyBaseName(item);
    File destination = variant == ExportVariant.ALL
      ? new File(exportDirectory, baseName + ".zip")
      : new File(exportDirectory, baseName + "_" + variantLabel(variant) + ".opus");
    try {
      try (OutputStream output = new BufferedOutputStream(new FileOutputStream(destination))) {
        writeExport(item, variant, output);
      }
      return new PreparedExport(destination,
        variant == ExportVariant.ALL ? "application/zip" : "audio/ogg");
    } catch (Throwable e) {
      // Never leave a truncated artifact that could be shared as a valid export.
      //noinspection ResultOfMethodCallIgnored
      destination.delete();
      try {
        deleteTreeInside(exportRoot, exportDirectory);
      } catch (IOException ignored) {
      }
      if (e instanceof IOException) throw (IOException) e;
      throw new IOException("Cannot prepare recording export", e);
    }
  }

  void writeExport (CallRecordingItem item, ExportVariant variant,
                    OutputStream output) throws IOException {
    if (item.isActive()) {
      throw new IOException("Active recording cannot be exported");
    }
    requireDirectSession(item);
    if (variant == ExportVariant.ALL) {
      writeZip(item, output);
    } else {
      try (InputStream input = new BufferedInputStream(
        new FileInputStream(requireTrack(item, variant)))) {
        copy(input, output);
      }
    }
  }

  public static String sanitizeFilename (@Nullable String value) {
    if (value == null) {
      return "Unknown";
    }
    StringBuilder result = new StringBuilder();
    boolean previousUnderscore = false;
    int acceptedCodePoints = 0;
    for (int offset = 0; offset < value.length() && acceptedCodePoints < MAX_EXPORT_NAME_CODE_POINTS;) {
      int codePoint = value.codePointAt(offset);
      offset += Character.charCount(codePoint);
      boolean invalid = codePoint < 0x20 || codePoint == 0x7f ||
        codePoint == '/' || codePoint == '\\' || codePoint == ':' ||
        codePoint == '*' || codePoint == '?' || codePoint == '"' ||
        codePoint == '<' || codePoint == '>' || codePoint == '|';
      if (invalid || Character.isWhitespace(codePoint)) {
        if (!previousUnderscore && result.length() > 0) {
          result.append('_');
          previousUnderscore = true;
        }
      } else {
        result.appendCodePoint(codePoint);
        previousUnderscore = false;
      }
      acceptedCodePoints++;
    }
    while (result.length() > 0 && result.charAt(result.length() - 1) == '_') {
      result.setLength(result.length() - 1);
    }
    return result.length() == 0 ? "Unknown" : result.toString();
  }

  private CallRecordingItem parseDirectory (File directory) throws IOException, JSONException {
    File info = safeChild(directory, INFO_FILE);
    JSONObject json = null;
    boolean metadataValid = false;
    if (info.isFile()) {
      try {
        json = new JSONObject(readMetadata(info));
        metadataValid = true;
      } catch (Throwable ignored) {
        json = null;
      }
    }

    File mixed = safeChild(directory, MIXED_FILE);
    File local = safeChild(directory, LOCAL_FILE);
    File remote = safeChild(directory, REMOTE_FILE);
    boolean hasMixed = isPlayableFile(mixed);
    boolean hasLocal = isPlayableFile(local);
    boolean hasRemote = isPlayableFile(remote);
    long mixedSize = hasMixed ? mixed.length() : 0;
    long localSize = hasLocal ? local.length() : 0;
    long remoteSize = hasRemote ? remote.length() : 0;

    String directoryId = directory.getName();
    int schemaVersion = json != null ? json.optInt("schemaVersion", 0) : 0;
    String sessionId = json != null ? json.optString("sessionId", directoryId) : directoryId;
    // A metadata sessionId is descriptive only and never used as a path.
    if (sessionId.isEmpty()) {
      sessionId = directoryId;
    }
    JSONObject call = json != null ? json.optJSONObject("call") : null;
    long callId = call != null ? call.optLong("callId", json.optLong("callId", 0))
      : json != null ? json.optLong("callId", 0) : 0;
    long userId = call != null ? call.optLong("userId", 0) : 0;
    String displayName = call != null ? call.optString("displayName", "") : "";
    Boolean isOutgoing = call != null && call.has("isOutgoing")
      ? call.optBoolean("isOutgoing") : null;
    long startTime = parseTime(firstNonEmpty(json,
      "recordingStartTime", "startTime"));
    if (startTime == 0) {
      startTime = parseDirectoryTime(directoryId);
    }
    if (startTime == 0) {
      startTime = directory.lastModified();
    }
    long endTime = parseTime(firstNonEmpty(json,
      "recordingEndTime", "stopTime"));
    long callEndTime = parseTime(firstNonEmpty(json, "callEndTime"));
    if (callEndTime == 0) {
      callEndTime = endTime;
    }
    long durationMs = json != null ? json.optLong("durationMs", 0) : 0;
    if (durationMs <= 0 && endTime >= startTime) {
      durationMs = endTime - startTime;
    }
    String outputMode = json != null
      ? json.optString("outputMode", inferOutputMode(hasMixed, hasLocal, hasRemote))
      : inferOutputMode(hasMixed, hasLocal, hasRemote);
    int timelineSampleRate = resolveTimelineSampleRate(json);

    ArrayList<CallRecordingItem.ControlInterval> pauses = new ArrayList<>();
    ArrayList<CallRecordingItem.ControlInterval> stopped = new ArrayList<>();
    if (json != null) {
      JSONArray intervals = json.optJSONArray("controlIntervals");
      if (intervals != null) {
        for (int i = 0; i < intervals.length(); i++) {
          JSONObject interval = intervals.optJSONObject(i);
          if (interval == null) continue;
          long start = interval.optLong("startSample", 0);
          long end = interval.optLong("endSample", 0);
          if (end <= start) continue;
          CallRecordingItem.ControlInterval value =
            new CallRecordingItem.ControlInterval(start, end);
          if ("pause".equals(interval.optString("type"))) {
            pauses.add(value);
          } else if ("stopped".equals(interval.optString("type"))) {
            stopped.add(value);
          }
        }
      }
    }

    CallRecordingItem.Status status = statusFor(directory, json, metadataValid,
      outputMode, hasMixed, hasLocal, hasRemote);
    return new CallRecordingItem(sessionId, directory, schemaVersion, callId,
      userId, displayName, isOutgoing, startTime, endTime, callEndTime,
      Math.max(0, durationMs), timelineSampleRate, outputMode,
      hasMixed, hasLocal, hasRemote,
      mixedSize, localSize, remoteSize, mixedSize + localSize + remoteSize +
      (info.isFile() ? info.length() : 0), status, pauses, stopped);
  }

  private CallRecordingItem fallbackItem (File directory) throws IOException {
    File mixed = safeChild(directory, MIXED_FILE);
    File local = safeChild(directory, LOCAL_FILE);
    File remote = safeChild(directory, REMOTE_FILE);
    boolean hasMixed = isPlayableFile(mixed);
    boolean hasLocal = isPlayableFile(local);
    boolean hasRemote = isPlayableFile(remote);
    long start = parseDirectoryTime(directory.getName());
    if (start == 0) start = directory.lastModified();
    return new CallRecordingItem(directory.getName(), directory, 0, 0, 0, "",
      null, start, 0, 0, 0, DEFAULT_TIMELINE_SAMPLE_RATE,
      inferOutputMode(hasMixed, hasLocal, hasRemote),
      hasMixed, hasLocal, hasRemote, hasMixed ? mixed.length() : 0,
      hasLocal ? local.length() : 0, hasRemote ? remote.length() : 0,
      (hasMixed ? mixed.length() : 0) + (hasLocal ? local.length() : 0) +
        (hasRemote ? remote.length() : 0), CallRecordingItem.Status.INCOMPLETE,
      new ArrayList<>(), new ArrayList<>());
  }

  private CallRecordingItem.Status statusFor (
    File directory,
    @Nullable JSONObject json,
    boolean metadataValid,
    String outputMode,
    boolean hasMixed,
    boolean hasLocal,
    boolean hasRemote
  ) throws IOException {
    if (hasLiveMarker(directory)) {
      return CallRecordingItem.Status.IN_PROGRESS;
    }
    if (!metadataValid || json == null) {
      return CallRecordingItem.Status.INCOMPLETE;
    }
    JSONObject control = json.optJSONObject("control");
    JSONObject invariant = json.optJSONObject("invariant");
    String finalState = control != null ? control.optString("finalState", "") : "";
    boolean timelineFailed = invariant != null &&
      invariant.optBoolean("timelineInternalFailure", false);
    if ("failed".equals(finalState) || timelineFailed) {
      return CallRecordingItem.Status.FAILED;
    }
    boolean expectedPresent;
    switch (outputMode) {
      case "mixed_only": expectedPresent = hasMixed; break;
      case "separate_only": expectedPresent = hasLocal && hasRemote; break;
      default: expectedPresent = hasMixed && hasLocal && hasRemote; break;
    }
    boolean writerFailed = enabledWriterFailed(json, "mixed") ||
      enabledWriterFailed(json, "local") || enabledWriterFailed(json, "remote");
    boolean finalized = "finalized".equals(finalState) ||
      (json.has("stopTime") && !json.optString("stopTime").isEmpty());
    return finalized && expectedPresent && !writerFailed
      ? CallRecordingItem.Status.COMPLETED
      : CallRecordingItem.Status.INCOMPLETE;
  }

  private static boolean enabledWriterFailed (JSONObject json, String key) {
    JSONObject writer = json.optJSONObject(key);
    return writer != null && writer.optBoolean("enabled", false) &&
      (writer.optBoolean("writerFailed", false) ||
       writer.optBoolean("failed", false));
  }

  private static int resolveTimelineSampleRate (@Nullable JSONObject json) {
    if (json == null) return DEFAULT_TIMELINE_SAMPLE_RATE;
    int explicit = json.optInt("timelineSampleRate", 0);
    if (explicit > 0) return explicit;
    int inferred = 0;
    for (String key : Arrays.asList("local", "remote", "mixed")) {
      JSONObject stream = json.optJSONObject(key);
      int sampleRate = stream != null ? stream.optInt("outputSampleRate", 0) : 0;
      if (sampleRate <= 0) continue;
      if (inferred == 0) {
        inferred = sampleRate;
      } else if (inferred != sampleRate) {
        return DEFAULT_TIMELINE_SAMPLE_RATE;
      }
    }
    return inferred > 0 ? inferred : DEFAULT_TIMELINE_SAMPLE_RATE;
  }

  private boolean hasLiveMarker (File directory) throws IOException {
    File marker = safeChild(directory, ACTIVE_MARKER);
    if (!marker.isFile() || marker.length() > 32) return false;
    try {
      String value = readSmallFile(marker, 32).trim();
      return Integer.parseInt(value) == currentPid;
    } catch (Throwable ignored) {
      return false;
    }
  }

  private void requireDirectSession (CallRecordingItem item) throws IOException {
    if (!isDirectChild(callsRoot, item.sessionPath)) {
      throw new IOException("Recording path escapes calls root");
    }
  }

  private static boolean isDirectChild (File root, File child) throws IOException {
    File canonicalRoot = root.getCanonicalFile();
    File canonicalChild = child.getCanonicalFile();
    return canonicalChild.getParentFile() != null &&
      canonicalChild.getParentFile().equals(canonicalRoot) &&
      canonicalChild.getName().equals(child.getName());
  }

  private static File safeChild (File directory, String fixedName) throws IOException {
    File child = new File(directory, fixedName).getCanonicalFile();
    if (!child.getParentFile().equals(directory.getCanonicalFile())) {
      throw new IOException("Unsafe recording child path");
    }
    return child;
  }

  private static boolean isPlayableFile (File file) {
    return file.isFile() && file.length() > 0;
  }

  private File requireTrack (CallRecordingItem item, ExportVariant variant) throws IOException {
    String name;
    boolean exists;
    switch (variant) {
      case MIXED: name = MIXED_FILE; exists = item.hasMixed; break;
      case LOCAL: name = LOCAL_FILE; exists = item.hasLocal; break;
      case REMOTE: name = REMOTE_FILE; exists = item.hasRemote; break;
      default: throw new IOException("All files must be exported as ZIP");
    }
    File result = safeChild(item.sessionPath, name);
    if (!exists || !isPlayableFile(result)) {
      throw new IOException("Recording track is missing");
    }
    return result;
  }

  private void writeZip (CallRecordingItem item, OutputStream destination) throws IOException {
    try (ZipOutputStream zip = new ZipOutputStream(destination)) {
      for (String name : Arrays.asList(MIXED_FILE, LOCAL_FILE, REMOTE_FILE, INFO_FILE)) {
        File source = safeChild(item.sessionPath, name);
        if (!source.isFile() || (name.endsWith(".opus") && source.length() == 0)) {
          continue;
        }
        zip.putNextEntry(new ZipEntry(name));
        try (InputStream input = new BufferedInputStream(new FileInputStream(source))) {
          copy(input, zip);
        }
        zip.closeEntry();
      }
    }
  }

  private static void copy (InputStream input, OutputStream output) throws IOException {
    byte[] buffer = new byte[COPY_BUFFER_SIZE];
    int read;
    while ((read = input.read(buffer)) != -1) {
      output.write(buffer, 0, read);
    }
  }

  void cleanupStaleExports (long nowMs) throws IOException {
    if (!exportRoot.exists()) return;
    File[] entries = exportRoot.listFiles();
    if (entries == null) {
      throw new IOException("Cannot inspect export cache");
    }
    for (File entry : entries) {
      try {
        if (!isRecorderOwnedExportDirectory(entry) ||
            nowMs - entry.lastModified() <= EXPORT_MAX_AGE_MS) {
          continue;
        }
        if (!deleteTreeInside(exportRoot, entry) || entry.exists()) {
          // A stale or malicious entry must not prevent a new independent
          // export. Leave it untouched if it cannot be deleted safely.
          continue;
        }
      } catch (IOException ignored) {
        // Isolate an unreadable or unsafe cache entry from a new export.
      }
    }
  }

  private File createUniqueExportDirectory () throws IOException {
    for (int attempt = 0; attempt < 100; attempt++) {
      long counter = NEXT_EXPORT_ID.incrementAndGet();
      String exportId = EXPORT_DIRECTORY_PREFIX + System.currentTimeMillis() +
        "_" + currentPid + "_" + counter;
      File directory = new File(exportRoot, exportId);
      if (!isDirectChild(exportRoot, directory)) {
        throw new IOException("Unsafe export directory");
      }
      if (directory.mkdir()) return directory;
    }
    throw new IOException("Cannot create unique export directory");
  }

  private boolean isRecorderOwnedExportDirectory (File entry) throws IOException {
    if (!entry.isDirectory() || !isDirectChild(exportRoot, entry)) return false;
    String name = entry.getName();
    if (!name.startsWith(EXPORT_DIRECTORY_PREFIX)) return false;
    int separators = 0;
    boolean hasDigit = false;
    for (int i = EXPORT_DIRECTORY_PREFIX.length(); i < name.length(); i++) {
      char character = name.charAt(i);
      if (character >= '0' && character <= '9') {
        hasDigit = true;
      } else if (character == '_' && hasDigit && separators < 2) {
        separators++;
        hasDigit = false;
      } else {
        return false;
      }
    }
    return separators == 2 && hasDigit;
  }

  private static boolean deleteTreeInside (File root, File target) throws IOException {
    File canonicalRoot = root.getCanonicalFile();
    File canonicalTarget = target.getCanonicalFile();
    String prefix = canonicalRoot.getPath() + File.separator;
    if (!canonicalTarget.equals(canonicalRoot) &&
        !canonicalTarget.getPath().startsWith(prefix)) {
      return false;
    }
    if (!target.exists()) return true;
    if (!canonicalTarget.equals(canonicalRoot)) {
      File canonicalParent = target.getParentFile().getCanonicalFile();
      File expectedTarget = new File(canonicalParent, target.getName());
      if (!canonicalTarget.equals(expectedTarget)) {
        // Do not follow symlinks, even when their destination happens to be
        // another session inside the otherwise permitted root.
        return false;
      }
    }
    if (target.isDirectory()) {
      File[] children = target.listFiles();
      if (children == null) return false;
      for (File child : children) {
        if (!deleteTreeInside(root, child)) return false;
      }
    }
    return target.delete();
  }

  private String friendlyBaseName (CallRecordingItem item) {
    SimpleDateFormat date = new SimpleDateFormat("yyyy-MM-dd_HH-mm", Locale.US);
    return "Telegram_Call_" + sanitizeFilename(item.displayName) + "_" +
      date.format(new Date(item.recordingStartTime > 0
        ? item.recordingStartTime : System.currentTimeMillis()));
  }

  private static String variantLabel (ExportVariant variant) {
    switch (variant) {
      case MIXED: return "Mixed";
      case LOCAL: return "Local";
      case REMOTE: return "Remote";
      default: return "All";
    }
  }

  private static String inferOutputMode (boolean mixed, boolean local, boolean remote) {
    if (mixed && !local && !remote) return "mixed_only";
    if (!mixed && (local || remote)) return "separate_only";
    return "mixed_and_separate";
  }

  private static @Nullable String firstNonEmpty (@Nullable JSONObject json, String... keys) {
    if (json == null) return null;
    for (String key : keys) {
      String value = json.optString(key, "");
      if (!value.isEmpty()) return value;
    }
    return null;
  }

  private static long parseTime (@Nullable String value) {
    if (value == null || value.isEmpty()) return 0;
    for (String pattern : Arrays.asList(
      "yyyy-MM-dd'T'HH:mm:ss.SSS'Z'", "yyyy-MM-dd'T'HH:mm:ss'Z'")) {
      SimpleDateFormat format = new SimpleDateFormat(pattern, Locale.US);
      format.setLenient(false);
      format.setTimeZone(TimeZone.getTimeZone("UTC"));
      try {
        return format.parse(value).getTime();
      } catch (ParseException ignored) {
      }
    }
    return 0;
  }

  private static long parseDirectoryTime (String name) {
    int suffix = name.lastIndexOf('_');
    String value = suffix > 0 ? name.substring(0, suffix) : name;
    SimpleDateFormat format = new SimpleDateFormat("yyyy-MM-dd_HH-mm-ss", Locale.US);
    format.setLenient(false);
    try {
      return format.parse(value).getTime();
    } catch (ParseException ignored) {
      return 0;
    }
  }

  private static String readMetadata (File file) throws IOException {
    if (file.length() > MAX_METADATA_BYTES) {
      throw new IOException("Recording metadata is too large");
    }
    return readSmallFile(file, MAX_METADATA_BYTES);
  }

  private static String readSmallFile (File file, int limit) throws IOException {
    try (InputStream input = new BufferedInputStream(new FileInputStream(file));
         ByteArrayOutputStream output = new ByteArrayOutputStream(
           (int) Math.min(file.length(), 8192))) {
      byte[] buffer = new byte[4096];
      int total = 0;
      int read;
      while ((read = input.read(buffer)) != -1) {
        total += read;
        if (total > limit) throw new IOException("File exceeds size limit");
        output.write(buffer, 0, read);
      }
      return new String(output.toByteArray(), StandardCharsets.UTF_8);
    }
  }
}
