/*
 * This file is a part of Telegram X
 * Copyright © 2014 (tgx-android@pm.me)
 */
package org.thunderdog.challegram.voip.recording;

import org.junit.Before;
import org.junit.After;
import org.junit.Rule;
import org.junit.Test;
import org.junit.rules.TemporaryFolder;

import java.io.File;
import java.io.FileOutputStream;
import java.io.IOException;
import java.io.InputStream;
import java.nio.charset.StandardCharsets;
import java.nio.file.Files;
import java.security.MessageDigest;
import java.util.ArrayList;
import java.util.Arrays;
import java.util.HashSet;
import java.util.List;
import java.util.Set;
import java.util.concurrent.ExecutorService;
import java.util.concurrent.Executors;
import java.util.concurrent.Future;
import java.util.concurrent.TimeUnit;
import java.util.zip.ZipEntry;
import java.util.zip.ZipInputStream;

import static org.junit.Assert.assertArrayEquals;
import static org.junit.Assert.assertEquals;
import static org.junit.Assert.assertFalse;
import static org.junit.Assert.assertNotNull;
import static org.junit.Assert.assertNotEquals;
import static org.junit.Assert.assertThrows;
import static org.junit.Assert.assertTrue;

public class CallRecordingRepositoryTest {
  private static final int TEST_PID = 4242;

  @Rule public final TemporaryFolder temporary = new TemporaryFolder();

  private File callsRoot;
  private File exportRoot;
  private CallRecordingRepository repository;

  @Before
  public void setUp () throws IOException {
    CallRecordingActiveSessions.clearForTests();
    callsRoot = temporary.newFolder("calls");
    exportRoot = new File(temporary.getRoot(), "exports");
    repository = new CallRecordingRepository(callsRoot, exportRoot, TEST_PID);
  }

  @After
  public void tearDown () {
    CallRecordingActiveSessions.clearForTests();
  }

  @Test
  public void parsesSchemaLegacyBrokenAndActualFilesNewestFirst () throws Exception {
    File legacy = session("2026-09-11_10-00-00_1");
    tracks(legacy, "mixed.opus", "local.opus", "remote.opus");
    write(new File(legacy, "info.json"), legacyMetadata(
      "2026-09-11T10:00:00.000Z", "mixed_and_separate",
      "[{\"type\":\"pause\",\"startSample\":100,\"endSample\":100}]"));

    File valid = session("2026-09-12_10-00-00_2");
    tracks(valid, "mixed.opus", "local.opus", "remote.opus");
    write(new File(valid, "info.json"), metadata(true,
      "schema-id", "2026-09-12T10:00:00.000Z", "mixed_and_separate",
      "finalized", false, false,
      "[{\"type\":\"pause\",\"startSample\":480,\"endSample\":960}," +
        "{\"type\":\"stopped\",\"startSample\":1200,\"endSample\":1200}]")
      .replace("Test User", "Иван \\\"Test\\\""));

    File missingMetadata = session("2026-09-10_10-00-00_3");
    tracks(missingMetadata, "mixed.opus");

    File malformed = session("2026-09-09_10-00-00_4");
    tracks(malformed, "remote.opus");
    write(new File(malformed, "info.json"), "{not json");

    File missingOutput = session("2026-09-08_10-00-00_5");
    write(new File(missingOutput, "info.json"), metadata(true,
      "missing-output", "2026-09-08T10:00:00.000Z", "mixed_only",
      "finalized", false, false, "[]"));

    File writerFailed = session("2026-09-07_10-00-00_6");
    tracks(writerFailed, "mixed.opus");
    write(new File(writerFailed, "info.json"), metadata(true,
      "writer-failed", "2026-09-07T10:00:00.000Z", "mixed_only",
      "finalized", true, false, "[]"));

    File fatal = session("2026-09-06_10-00-00_7");
    tracks(fatal, "mixed.opus");
    write(new File(fatal, "info.json"), metadata(true,
      "fatal", "2026-09-06T10:00:00.000Z", "mixed_only",
      "failed", false, false, "[]"));

    File actualWins = session("2026-09-05_10-00-00_8");
    tracks(actualWins, "remote.opus");
    write(new File(actualWins, "info.json"), metadata(true,
      "actual-wins", "2026-09-05T10:00:00.000Z", "mixed_only",
      "finalized", false, false, "[]"));

    List<CallRecordingItem> items = repository.scanNow();
    assertEquals(8, items.size());
    assertEquals(valid.getName(), items.get(0).sessionId);

    CallRecordingItem schema = findByDirectory(items, valid.getName());
    assertEquals(1, schema.schemaVersion);
    assertEquals(48000, schema.timelineSampleRate);
    assertEquals(123456789L, schema.userId);
    assertEquals("Иван \"Test\"", schema.displayName);
    assertEquals(Boolean.TRUE, schema.isOutgoing);
    assertEquals(CallRecordingItem.Status.COMPLETED, schema.status);
    assertEquals(1, schema.pauseIntervals.size());
    assertEquals(0, schema.stoppedIntervals.size());
    assertThrows(UnsupportedOperationException.class,
      () -> schema.pauseIntervals.clear());
    assertTrue(schema.recordingEndTime > schema.recordingStartTime);
    assertTrue(schema.callEndTime > schema.recordingEndTime);

    CallRecordingItem old = findByDirectory(items, legacy.getName());
    assertEquals(legacy.getName(), old.sessionId);
    assertEquals(0, old.schemaVersion);
    assertEquals(48000, old.timelineSampleRate);
    assertEquals(0, old.userId);
    assertEquals("", old.displayName);
    assertEquals(null, old.isOutgoing);
    assertEquals(CallRecordingItem.Status.COMPLETED, old.status);

    assertEquals(CallRecordingItem.Status.INCOMPLETE,
      findByDirectory(items, missingMetadata.getName()).status);
    assertEquals(CallRecordingItem.Status.INCOMPLETE,
      findByDirectory(items, malformed.getName()).status);
    assertEquals(CallRecordingItem.Status.INCOMPLETE,
      findByDirectory(items, missingOutput.getName()).status);
    assertEquals(CallRecordingItem.Status.INCOMPLETE,
      findByDirectory(items, writerFailed.getName()).status);
    assertEquals(CallRecordingItem.Status.FAILED, findByDirectory(items, fatal.getName()).status);
    CallRecordingItem actual = findByDirectory(items, actualWins.getName());
    assertFalse(actual.hasMixed);
    assertTrue(actual.hasRemote);

    File inferredDirectory = session("inferred-rate");
    tracks(inferredDirectory, "mixed.opus");
    write(new File(inferredDirectory, "info.json"), metadata(true,
      "inferred-rate", "2026-09-04T10:00:00.000Z", "mixed_only",
      "finalized", false, false, "[]")
      .replace("\"timelineSampleRate\":48000,", "")
      .replace("\"mixed\":{", "\"mixed\":{\"outputSampleRate\":24000,"));
    assertEquals(24000, find(repository.scanNow(), "inferred-rate").timelineSampleRate);
  }

  @Test
  public void productionSessionRemainsDiscoverableAcrossMetadataRecoveryStates () throws Exception {
    File directory = session("2026-09-14_11-11-08_5");
    tracks(directory, "local.opus", "remote.opus", "mixed.opus");

    List<CallRecordingItem> items = repository.scanNow();
    assertEquals(1, items.size());
    assertEquals(CallRecordingItem.Status.INCOMPLETE,
      find(items, directory.getName()).status);

    File info = new File(directory, "info.json");
    write(info, metadata(true, directory.getName(),
      "2026-09-14T11:11:08.000Z", "mixed_and_separate", "finalized",
      false, false, "[]"));
    items = repository.scanNow();
    assertEquals(1, items.size());
    assertEquals(CallRecordingItem.Status.COMPLETED,
      find(items, directory.getName()).status);

    assertTrue(info.delete());
    File temporaryInfo = new File(directory, "info.json.tmp");
    write(temporaryInfo, checkpointMetadata(directory.getName(), 96000));
    items = repository.scanNow();
    assertEquals(1, items.size());
    assertEquals(CallRecordingItem.Status.INTERRUPTED,
      find(items, directory.getName()).status);

    assertTrue(temporaryInfo.delete());
    File marker = new File(directory, ".in_progress");
    write(marker, Integer.toString(TEST_PID));
    items = repository.scanNow();
    assertEquals(1, items.size());
    assertEquals(CallRecordingItem.Status.INTERRUPTED,
      find(items, directory.getName()).status);

    assertTrue(marker.delete());
    write(info, "{malformed");
    items = repository.scanNow();
    assertEquals(1, items.size());
    CallRecordingItem malformed = find(items, directory.getName());
    assertEquals(CallRecordingItem.Status.INCOMPLETE, malformed.status);
    assertTrue(malformed.hasLocal);
    assertTrue(malformed.hasRemote);
    assertTrue(malformed.hasMixed);
  }

  @Test
  public void canonicalAncestorAliasDoesNotHideLegitimateSession () throws Exception {
    File physicalFiles = temporary.newFolder("physical-files");
    File physicalCalls = new File(physicalFiles, "calls");
    assertTrue(physicalCalls.mkdir());
    File aliasedFiles = new File(temporary.getRoot(), "files-alias");
    Files.createSymbolicLink(aliasedFiles.toPath(), physicalFiles.toPath());
    File aliasedCalls = new File(aliasedFiles, "calls");
    assertNotEquals(aliasedCalls.getAbsolutePath(), aliasedCalls.getCanonicalPath());

    File directory = new File(physicalCalls, "2026-09-14_11-11-08_5");
    assertTrue(directory.mkdir());
    tracks(directory, "local.opus", "remote.opus", "mixed.opus");

    CallRecordingRepository aliasedRepository = new CallRecordingRepository(
      aliasedCalls, new File(temporary.getRoot(), "aliased-exports"), TEST_PID);
    List<CallRecordingItem> items = aliasedRepository.scanNow();
    assertEquals(1, items.size());
    CallRecordingItem item = find(items, directory.getName());
    assertEquals(CallRecordingItem.Status.INCOMPLETE, item.status);
    assertTrue(item.hasLocal);
    assertTrue(item.hasRemote);
    assertTrue(item.hasMixed);
  }

  @Test
  public void scansOneThousandRecordingsWithoutReadingAudio () throws Exception {
    for (int i = 0; i < 1000; i++) {
      File directory = session(String.format("2026-01-01_00-00-%02d_%04d", i % 60, i));
      tracks(directory, "mixed.opus");
    }
    List<CallRecordingItem> items = repository.scanNow();
    assertEquals(1000, items.size());
    assertTrue(items.get(0).recordingStartTime >=
      items.get(items.size() - 1).recordingStartTime);
  }

  @Test
  public void exportsTracksAndExpectedZipContents () throws Exception {
    CallRecordingItem full = makeItem("full", "mixed_and_separate",
      "finalized", false, "mixed.opus", "local.opus", "remote.opus");
    assertTrackExport(full, CallRecordingRepository.ExportVariant.MIXED, "mixed.opus");
    assertTrackExport(full, CallRecordingRepository.ExportVariant.LOCAL, "local.opus");
    assertTrackExport(full, CallRecordingRepository.ExportVariant.REMOTE, "remote.opus");
    assertZip(full, set("mixed.opus", "local.opus", "remote.opus", "info.json"));

    CallRecordingItem mixed = makeItem("mixed", "mixed_only",
      "finalized", false, "mixed.opus");
    assertZip(mixed, set("mixed.opus", "info.json"));
    assertThrows(IOException.class, () -> repository.prepareExportNow(
      mixed, CallRecordingRepository.ExportVariant.LOCAL));

    CallRecordingItem separate = makeItem("separate", "separate_only",
      "finalized", false, "local.opus", "remote.opus");
    assertZip(separate, set("local.opus", "remote.opus", "info.json"));

    CallRecordingItem incomplete = makeItemWithoutMetadata("incomplete", "mixed.opus");
    assertTrackExport(incomplete, CallRecordingRepository.ExportVariant.MIXED, "mixed.opus");
    CallRecordingItem failed = makeItem("failed", "mixed_only",
      "failed", false, "mixed.opus");
    assertTrackExport(failed, CallRecordingRepository.ExportVariant.MIXED, "mixed.opus");

    CallRecordingItem legacy = makeLegacyItem("legacy-export", "mixed.opus");
    assertTrackExport(legacy, CallRecordingRepository.ExportVariant.MIXED, "mixed.opus");
  }

  @Test
  public void exportUsesStreamingPathForLargeInput () throws Exception {
    File directory = session("large");
    File mixed = new File(directory, "mixed.opus");
    byte[] chunk = new byte[8192];
    Arrays.fill(chunk, (byte) 0x5a);
    try (FileOutputStream output = new FileOutputStream(mixed)) {
      for (int i = 0; i < 640; i++) output.write(chunk);
    }
    write(new File(directory, "info.json"), metadata(true, "large",
      "2026-09-12T10:00:00.000Z", "mixed_only", "finalized",
      false, false, "[]"));
    CallRecordingItem item = find(repository.scanNow(), "large");
    CallRecordingRepository.PreparedExport prepared = repository.prepareExportNow(
      item, CallRecordingRepository.ExportVariant.MIXED);
    assertEquals(mixed.length(), prepared.file.length());
    assertArrayEquals(digest(mixed), digest(prepared.file));
  }

  @Test
  public void previousShareSurvivesSubsequentExportUnchanged () throws Exception {
    CallRecordingItem item = makeItem("share-lifetime", "mixed_and_separate",
      "finalized", false, "mixed.opus", "local.opus", "remote.opus");
    CallRecordingRepository.PreparedExport first = repository.prepareExportNow(
      item, CallRecordingRepository.ExportVariant.MIXED);
    byte[] firstDigest = digest(first.file);
    CallRecordingRepository.PreparedExport second = repository.prepareExportNow(
      item, CallRecordingRepository.ExportVariant.LOCAL);

    assertTrue(first.file.isFile());
    assertTrue(second.file.isFile());
    assertNotEquals(first.file.getParentFile().getCanonicalPath(),
      second.file.getParentFile().getCanonicalPath());
    assertArrayEquals(firstDigest, digest(first.file));
  }

  @Test
  public void threeExportsRemainIndependent () throws Exception {
    CallRecordingItem item = makeItem("three-exports", "mixed_and_separate",
      "finalized", false, "mixed.opus", "local.opus", "remote.opus");
    CallRecordingRepository.PreparedExport mixed = repository.prepareExportNow(
      item, CallRecordingRepository.ExportVariant.MIXED);
    CallRecordingRepository.PreparedExport local = repository.prepareExportNow(
      item, CallRecordingRepository.ExportVariant.LOCAL);
    CallRecordingRepository.PreparedExport zip = repository.prepareExportNow(
      item, CallRecordingRepository.ExportVariant.ALL);

    assertTrue(mixed.file.isFile());
    assertTrue(local.file.isFile());
    assertTrue(zip.file.isFile());
    assertEquals(3, set(mixed.file.getParent(), local.file.getParent(),
      zip.file.getParent()).size());
  }

  @Test
  public void staleCleanupKeepsFreshCurrentAndUnownedEntries () throws Exception {
    assertTrue(exportRoot.mkdir());
    long now = System.currentTimeMillis();
    File old = exportDirectory("export_1_1_1", "old");
    File fresh = exportDirectory("export_2_1_2", "fresh");
    File unowned = exportDirectory("not_recorder_owned", "unowned");
    File outside = temporary.newFolder("export-cleanup-outside");
    write(new File(outside, "secret"), "keep");
    File unsafeLink = new File(exportRoot, "export_3_1_3");
    Files.createSymbolicLink(unsafeLink.toPath(), outside.toPath());
    assertTrue(old.setLastModified(now - CallRecordingRepository.EXPORT_MAX_AGE_MS - 1000L));
    assertTrue(fresh.setLastModified(now));
    assertTrue(unowned.setLastModified(now - CallRecordingRepository.EXPORT_MAX_AGE_MS - 1000L));

    repository.cleanupStaleExports(now);

    assertFalse(old.exists());
    assertTrue(fresh.isDirectory());
    assertTrue(unowned.isDirectory());
    assertTrue(new File(outside, "secret").isFile());
    assertTrue(unsafeLink.exists());
    Files.deleteIfExists(unsafeLink.toPath());

    CallRecordingItem currentItem = makeItem("current-export", "mixed_only",
      "finalized", false, "mixed.opus");
    CallRecordingRepository.PreparedExport current = repository.prepareExportNow(
      currentItem, CallRecordingRepository.ExportVariant.MIXED);
    repository.cleanupStaleExports(System.currentTimeMillis());
    assertTrue(current.file.isFile());
  }

  @Test
  public void concurrentExportsUseUniqueDirectoriesAndKeepBothFiles () throws Exception {
    CallRecordingItem item = makeItem("concurrent", "mixed_and_separate",
      "finalized", false, "mixed.opus", "local.opus", "remote.opus");
    ExecutorService executor = Executors.newFixedThreadPool(2);
    try {
      Future<CallRecordingRepository.PreparedExport> mixedFuture = executor.submit(
        () -> repository.prepareExportNow(item, CallRecordingRepository.ExportVariant.MIXED));
      Future<CallRecordingRepository.PreparedExport> remoteFuture = executor.submit(
        () -> repository.prepareExportNow(item, CallRecordingRepository.ExportVariant.REMOTE));
      CallRecordingRepository.PreparedExport mixed = mixedFuture.get(10, TimeUnit.SECONDS);
      CallRecordingRepository.PreparedExport remote = remoteFuture.get(10, TimeUnit.SECONDS);
      assertTrue(mixed.file.isFile());
      assertTrue(remote.file.isFile());
      assertNotEquals(mixed.file.getParentFile().getCanonicalPath(),
        remote.file.getParentFile().getCanonicalPath());
      assertArrayEquals(digest(new File(item.sessionPath, "mixed.opus")), digest(mixed.file));
      assertArrayEquals(digest(new File(item.sessionPath, "remote.opus")), digest(remote.file));
    } finally {
      executor.shutdownNow();
    }
  }

  @Test
  public void sanitizesUnicodeInvalidEmptyAndLongNames () {
    assertEquals("John_Doe", CallRecordingRepository.sanitizeFilename("John Doe"));
    assertEquals("Євген_Карпюк", CallRecordingRepository.sanitizeFilename("Євген Карпюк"));
    assertEquals("O_Connor", CallRecordingRepository.sanitizeFilename("O\"Connor"));
    assertEquals("Test_Test", CallRecordingRepository.sanitizeFilename("Test/Test"));
    assertEquals("A_B_C_D", CallRecordingRepository.sanitizeFilename("A:B*C?D"));
    assertEquals("emoji_😀", CallRecordingRepository.sanitizeFilename("emoji 😀"));
    assertEquals("Unknown", CallRecordingRepository.sanitizeFilename(" \n\t/\\:*?\"<>|"));
    String longName = CallRecordingRepository.sanitizeFilename(repeat("😀", 100));
    assertTrue(longName.codePointCount(0, longName.length()) <= 64);
  }

  @Test
  public void deleteIsConfinedAndBlocksMissingPartialTraversalAndActive () throws Exception {
    CallRecordingItem normal = makeItemWithoutMetadata("normal-delete", "mixed.opus");
    assertTrue(repository.deleteNow(normal));
    assertFalse(normal.sessionPath.exists());

    CallRecordingItem missing = itemFor(new File(callsRoot, "missing"),
      CallRecordingItem.Status.INCOMPLETE);
    assertThrows(IOException.class, () -> repository.deleteNow(missing));

    File outside = temporary.newFolder("outside");
    write(new File(outside, "secret"), "keep");
    CallRecordingItem traversal = itemFor(new File(callsRoot, "../outside"),
      CallRecordingItem.Status.INCOMPLETE);
    assertThrows(IOException.class, () -> repository.deleteNow(traversal));
    assertTrue(new File(outside, "secret").isFile());

    File sibling = session("sibling-target");
    write(new File(sibling, "keep"), "keep");
    File siblingAlias = new File(callsRoot, "sibling-alias");
    Files.createSymbolicLink(siblingAlias.toPath(), sibling.toPath());
    CallRecordingItem alias = itemFor(siblingAlias, CallRecordingItem.Status.INCOMPLETE);
    assertThrows(IOException.class, () -> repository.deleteNow(alias));
    assertTrue(new File(sibling, "keep").isFile());
    Files.deleteIfExists(siblingAlias.toPath());

    File activeDirectory = session("active");
    tracks(activeDirectory, "mixed.opus");
    write(new File(activeDirectory, ".in_progress"), Integer.toString(TEST_PID));
    CallRecordingActiveSessions.update("active", org.thunderdog.challegram.voip.annotation.CallRecordingState.RECORDING);
    CallRecordingItem active = findByDirectory(repository.scanNow(), "active");
    assertEquals(CallRecordingItem.Status.IN_PROGRESS, active.status);
    assertThrows(IOException.class, () -> repository.deleteNow(active));
    assertThrows(IOException.class, () -> repository.prepareExportNow(
      active, CallRecordingRepository.ExportVariant.MIXED));

    File partialDirectory = session("partial");
    write(new File(partialDirectory, "mixed.opus"), "audio");
    File link = new File(partialDirectory, "external-link");
    Files.createSymbolicLink(link.toPath(), outside.toPath());
    CallRecordingItem partial = findByDirectory(repository.scanNow(), "partial");
    assertThrows(IOException.class, () -> repository.deleteNow(partial));
    assertTrue(partialDirectory.exists());
    assertTrue(new File(outside, "secret").isFile());
    Files.deleteIfExists(link.toPath());
  }

  @Test
  public void staleMarkerIsInterruptedNotActive () throws Exception {
    File directory = session("stale-marker");
    tracks(directory, "mixed.opus");
    write(new File(directory, ".in_progress"), Integer.toString(TEST_PID));

    CallRecordingItem item = find(repository.scanNow(), "stale-marker");
    assertEquals(CallRecordingItem.Status.INTERRUPTED, item.status);
    assertFalse(item.isActive());
  }

  @Test
  public void exactProcessLocalSessionIsActiveWithoutTrustingPid () throws Exception {
    File active = session("registered-active");
    tracks(active, "mixed.opus");
    File stale = session("same-pid-stale");
    tracks(stale, "mixed.opus");
    write(new File(stale, ".in_progress"), Integer.toString(TEST_PID));
    CallRecordingActiveSessions.update("registered-active",
      org.thunderdog.challegram.voip.annotation.CallRecordingState.PAUSED);

    List<CallRecordingItem> items = repository.scanNow();
    assertEquals(CallRecordingItem.Status.IN_PROGRESS,
      find(items, "registered-active").status);
    assertEquals(CallRecordingItem.Status.INTERRUPTED,
      find(items, "same-pid-stale").status);
  }

  @Test
  public void validCanonicalInfoWinsOverStaleMarkerAndTemp () throws Exception {
    File directory = session("canonical-wins");
    tracks(directory, "mixed.opus");
    write(new File(directory, "info.json"), metadata(true, "untrusted-id",
      "2026-09-12T10:00:00.000Z", "mixed_only", "finalized",
      false, false, "[]"));
    String temporaryMetadata = checkpointMetadata("canonical-wins", 96000);
    write(new File(directory, "info.json.tmp"), temporaryMetadata);
    write(new File(directory, ".in_progress"), "old");

    CallRecordingItem item = find(repository.scanNow(), "canonical-wins");
    assertEquals("canonical-wins", item.sessionId);
    assertEquals(1, item.schemaVersion);
    assertEquals(CallRecordingItem.Status.COMPLETED, item.status);
    assertTrue(new File(directory, "info.json.tmp").isFile());
    assertEquals(temporaryMetadata,
      new String(Files.readAllBytes(new File(directory, "info.json.tmp").toPath()),
        StandardCharsets.UTF_8));
  }

  @Test
  public void validTempIsReadWithoutFilesystemMutation () throws Exception {
    File directory = session("promote-temp");
    tracks(directory, "mixed.opus");
    write(new File(directory, "info.json.tmp"), checkpointMetadata("promote-temp", 96000));

    CallRecordingItem item = find(repository.scanNow(), "promote-temp");
    assertEquals(2, item.schemaVersion);
    assertEquals(2000, item.durationMs);
    assertEquals(CallRecordingItem.Status.INTERRUPTED, item.status);
    assertTrue(new File(directory, "info.json.tmp").isFile());
    assertFalse(new File(directory, "info.json").exists());
  }

  @Test
  public void activeTmpBecomesInterruptedAfterExactSessionUnregister () throws Exception {
    File directory = session("active-tmp");
    tracks(directory, "mixed.opus");
    String checkpoint = checkpointMetadata("active-tmp", 96000);
    write(new File(directory, "info.json.tmp"), checkpoint);
    write(new File(directory, ".in_progress"), Integer.toString(TEST_PID));
    CallRecordingActiveSessions.update("active-tmp",
      org.thunderdog.challegram.voip.annotation.CallRecordingState.RECORDING);

    CallRecordingItem active = find(repository.scanNow(), "active-tmp");
    assertEquals(CallRecordingItem.Status.IN_PROGRESS, active.status);
    assertEquals(2000, active.durationMs);
    assertTrue(new File(directory, "info.json.tmp").isFile());
    assertFalse(new File(directory, "info.json").exists());

    CallRecordingActiveSessions.update("active-tmp",
      org.thunderdog.challegram.voip.annotation.CallRecordingState.FINALIZED);
    CallRecordingItem interrupted = find(repository.scanNow(), "active-tmp");
    assertEquals(CallRecordingItem.Status.INTERRUPTED, interrupted.status);
    assertTrue(new File(directory, "info.json.tmp").isFile());
    assertFalse(new File(directory, "info.json").exists());
  }

  @Test
  public void malformedCanonicalFallsBackToValidTempWithoutMutation () throws Exception {
    File directory = session("malformed-canonical-valid-temp");
    tracks(directory, "mixed.opus");
    String malformed = "{broken-canonical";
    String checkpoint = checkpointMetadata(directory.getName(), 144000);
    write(new File(directory, "info.json"), malformed);
    write(new File(directory, "info.json.tmp"), checkpoint);
    write(new File(directory, ".in_progress"), "stale");

    CallRecordingItem item = find(repository.scanNow(), directory.getName());
    assertEquals(2, item.schemaVersion);
    assertEquals(3000, item.durationMs);
    assertEquals(CallRecordingItem.Status.INTERRUPTED, item.status);
    assertEquals(malformed,
      new String(Files.readAllBytes(new File(directory, "info.json").toPath()),
        StandardCharsets.UTF_8));
    assertEquals(checkpoint,
      new String(Files.readAllBytes(new File(directory, "info.json.tmp").toPath()),
        StandardCharsets.UTF_8));
  }

  @Test
  public void activeRegistryRemovesExactSessionForTerminalStates () {
    CallRecordingActiveSessions.update("terminal-session",
      org.thunderdog.challegram.voip.annotation.CallRecordingState.RECORDING);
    assertTrue(CallRecordingActiveSessions.isActive("terminal-session"));
    CallRecordingActiveSessions.update("terminal-session",
      org.thunderdog.challegram.voip.annotation.CallRecordingState.FAILED);
    assertFalse(CallRecordingActiveSessions.isActive("terminal-session"));

    CallRecordingActiveSessions.update("terminal-session",
      org.thunderdog.challegram.voip.annotation.CallRecordingState.INACTIVE);
    assertTrue(CallRecordingActiveSessions.isActive("terminal-session"));
    CallRecordingActiveSessions.update("terminal-session",
      org.thunderdog.challegram.voip.annotation.CallRecordingState.FINALIZED);
    assertFalse(CallRecordingActiveSessions.isActive("terminal-session"));
    CallRecordingActiveSessions.update("",
      org.thunderdog.challegram.voip.annotation.CallRecordingState.RECORDING);
    assertFalse(CallRecordingActiveSessions.isActive(""));
  }

  @Test
  public void corruptTempIsIsolatedAndDoesNotHideSibling () throws Exception {
    File broken = session("corrupt-temp");
    tracks(broken, "mixed.opus");
    write(new File(broken, "info.json.tmp"), "{not-json");
    File healthy = session("healthy-sibling");
    tracks(healthy, "mixed.opus");
    write(new File(healthy, "info.json"), metadata(true, "ignored",
      "2026-09-12T10:00:00.000Z", "mixed_only", "finalized",
      false, false, "[]"));

    List<CallRecordingItem> items = repository.scanNow();
    assertEquals(CallRecordingItem.Status.INCOMPLETE, find(items, "corrupt-temp").status);
    assertEquals(CallRecordingItem.Status.COMPLETED, find(items, "healthy-sibling").status);
    assertFalse(new File(broken, "info.json").exists());
  }

  @Test
  public void schemaTwoCheckpointUsesCommittedDuration () throws Exception {
    File directory = session("checkpoint-v2");
    tracks(directory, "mixed.opus");
    write(new File(directory, "info.json"), checkpointMetadata("checkpoint-v2", 144000)
      .replace("\"durationMs\":3000,", ""));

    CallRecordingItem item = find(repository.scanNow(), "checkpoint-v2");
    assertEquals(2, item.schemaVersion);
    assertEquals(3000, item.durationMs);
    assertEquals(48000, item.timelineSampleRate);
    assertEquals(CallRecordingItem.Status.INTERRUPTED, item.status);
  }

  @Test
  public void interruptedMixedTrackCanBeExportedAndDeleted () throws Exception {
    File directory = session("interrupted-actions");
    tracks(directory, "mixed.opus");
    write(new File(directory, "info.json"), checkpointMetadata("interrupted-actions", 48000));
    write(new File(directory, ".in_progress"), "stale");
    CallRecordingItem item = find(repository.scanNow(), "interrupted-actions");

    assertTrackExport(item, CallRecordingRepository.ExportVariant.MIXED, "mixed.opus");
    assertTrue(repository.deleteNow(item));
    assertFalse(directory.exists());
  }

  @Test
  public void interruptedSeparateTracksSurviveIndependently () throws Exception {
    File directory = session("interrupted-separate");
    tracks(directory, "local.opus", "remote.opus");
    write(new File(directory, "info.json"), checkpointMetadata("interrupted-separate", 48000)
      .replace("mixed_and_separate", "separate_only"));
    CallRecordingItem item = find(repository.scanNow(), "interrupted-separate");

    assertFalse(item.hasMixed);
    assertTrue(item.hasLocal);
    assertTrue(item.hasRemote);
    assertTrackExport(item, CallRecordingRepository.ExportVariant.LOCAL, "local.opus");
    assertTrackExport(item, CallRecordingRepository.ExportVariant.REMOTE, "remote.opus");
  }

  @Test
  public void symlinkAndTinyTracksAreNotPlayable () throws Exception {
    File outside = temporary.newFile("outside.opus");
    tracks(outside.getParentFile(), outside.getName());
    File symlinkSession = session("symlink-track");
    Files.createSymbolicLink(new File(symlinkSession, "mixed.opus").toPath(), outside.toPath());
    File tinySession = session("tiny-track");
    write(new File(tinySession, "mixed.opus"), "tiny");

    List<CallRecordingItem> items = repository.scanNow();
    assertEquals(1, items.size());
    assertFalse(find(items, "tiny-track").hasMixed);
  }

  private CallRecordingItem makeItem (String id, String mode, String finalState,
                                      boolean writerFailed, String... names) throws Exception {
    File directory = session(id);
    tracks(directory, names);
    write(new File(directory, "info.json"), metadata(true, id,
      "2026-09-12T10:00:00.000Z", mode, finalState, writerFailed,
      false, "[]"));
    return find(repository.scanNow(), id);
  }

  private CallRecordingItem makeLegacyItem (String id, String... names) throws Exception {
    File directory = session(id);
    tracks(directory, names);
    write(new File(directory, "info.json"), legacyMetadata(
      "2026-09-12T10:00:00.000Z", "mixed_only", "[]"));
    return find(repository.scanNow(), id);
  }

  private CallRecordingItem makeItemWithoutMetadata (String id, String... names) throws Exception {
    File directory = session(id);
    tracks(directory, names);
    return findByDirectory(repository.scanNow(), id);
  }

  private void assertTrackExport (CallRecordingItem item,
                                  CallRecordingRepository.ExportVariant variant,
                                  String sourceName) throws Exception {
    File source = new File(item.sessionPath, sourceName);
    CallRecordingRepository.PreparedExport prepared = repository.prepareExportNow(item, variant);
    assertNotNull(prepared);
    assertEquals("audio/ogg", prepared.mimeType);
    assertEquals(source.length(), prepared.file.length());
    assertArrayEquals(digest(source), digest(prepared.file));
  }

  private void assertZip (CallRecordingItem item, Set<String> expected) throws Exception {
    CallRecordingRepository.PreparedExport prepared = repository.prepareExportNow(
      item, CallRecordingRepository.ExportVariant.ALL);
    assertEquals("application/zip", prepared.mimeType);
    Set<String> entries = new HashSet<>();
    try (ZipInputStream input = new ZipInputStream(Files.newInputStream(prepared.file.toPath()))) {
      ZipEntry entry;
      byte[] buffer = new byte[1024];
      while ((entry = input.getNextEntry()) != null) {
        entries.add(entry.getName());
        while (input.read(buffer) != -1) { }
      }
    }
    assertEquals(expected, entries);
  }

  private File session (String name) throws IOException {
    File directory = new File(callsRoot, name);
    assertTrue(directory.mkdir());
    return directory;
  }

  private File exportDirectory (String name, String content) throws IOException {
    File directory = new File(exportRoot, name);
    assertTrue(directory.mkdir());
    write(new File(directory, "artifact"), content);
    return directory;
  }

  private static void tracks (File directory, String... names) throws IOException {
    for (String name : names) write(new File(directory, name), repeat("audio-" + name, 8));
  }

  private static void write (File file, String value) throws IOException {
    try (FileOutputStream output = new FileOutputStream(file)) {
      output.write(value.getBytes(StandardCharsets.UTF_8));
    }
  }

  private static String metadata (boolean schema, String sessionId, String start,
                                  String mode, String finalState,
                                  boolean mixedWriterFailed,
                                  boolean timelineFailed, String intervals) {
    return "{" + (schema ? "\"schemaVersion\":1," : "") +
      "\"sessionId\":\"" + sessionId + "\"," +
      "\"call\":{\"callId\":7,\"userId\":123456789," +
        "\"displayName\":\"Test User\",\"isOutgoing\":true}," +
      "\"recordingStartTime\":\"" + start + "\"," +
      "\"recordingEndTime\":\"2026-09-12T10:00:31.000Z\"," +
      "\"callEndTime\":\"2026-09-12T10:00:40.000Z\"," +
      "\"timelineSampleRate\":48000," +
      "\"durationMs\":31000," +
      "\"outputMode\":\"" + mode + "\"," +
      "\"control\":{\"finalState\":\"" + finalState + "\"}," +
      "\"controlIntervals\":" + intervals + "," +
      "\"mixed\":{\"enabled\":true,\"failed\":" + mixedWriterFailed + "}," +
      "\"local\":{\"enabled\":true,\"writerFailed\":false}," +
      "\"remote\":{\"enabled\":true,\"writerFailed\":false}," +
      "\"invariant\":{\"timelineInternalFailure\":" + timelineFailed + "}" +
      "}";
  }

  private static String legacyMetadata (String start, String mode, String intervals) {
    return "{" +
      "\"callId\":7," +
      "\"startTime\":\"" + start + "\"," +
      "\"stopTime\":\"2026-09-12T10:00:31.000Z\"," +
      "\"durationMs\":31000," +
      "\"outputMode\":\"" + mode + "\"," +
      "\"control\":{\"finalState\":\"finalized\"}," +
      "\"controlIntervals\":" + intervals + "," +
      "\"mixed\":{\"enabled\":true,\"failed\":false}," +
      "\"local\":{\"enabled\":true,\"writerFailed\":false}," +
      "\"remote\":{\"enabled\":true,\"writerFailed\":false}," +
      "\"invariant\":{\"timelineInternalFailure\":false}" +
      "}";
  }

  private static String checkpointMetadata (String sessionId, long committedSample) {
    return "{" +
      "\"schemaVersion\":2," +
      "\"sessionId\":\"" + sessionId + "\"," +
      "\"callId\":7," +
      "\"call\":{\"callId\":7,\"userId\":123456789," +
        "\"displayName\":\"Test User\",\"isOutgoing\":true}," +
      "\"recordingStartTime\":\"2026-09-12T10:00:00.000Z\"," +
      "\"timelineSampleRate\":48000," +
      "\"durationMs\":" + (committedSample / 48) + "," +
      "\"lastCommittedTimelineSample\":" + committedSample + "," +
      "\"outputMode\":\"mixed_and_separate\"," +
      "\"state\":\"in_progress\"," +
      "\"control\":{\"runtimeState\":\"recording\",\"finalState\":\"in_progress\"}" +
      "}";
  }

  private static byte[] digest (File file) throws Exception {
    MessageDigest digest = MessageDigest.getInstance("SHA-256");
    byte[] buffer = new byte[8192];
    try (InputStream input = Files.newInputStream(file.toPath())) {
      int read;
      while ((read = input.read(buffer)) != -1) digest.update(buffer, 0, read);
    }
    return digest.digest();
  }

  private static CallRecordingItem find (List<CallRecordingItem> items, String sessionId) {
    for (CallRecordingItem item : items) {
      if (sessionId.equals(item.sessionId)) return item;
    }
    throw new AssertionError("Missing sessionId " + sessionId);
  }

  private static CallRecordingItem findByDirectory (List<CallRecordingItem> items, String name) {
    for (CallRecordingItem item : items) {
      if (name.equals(item.sessionPath.getName())) return item;
    }
    throw new AssertionError("Missing directory " + name);
  }

  private static CallRecordingItem itemFor (File path, CallRecordingItem.Status status) {
    return new CallRecordingItem(path.getName(), path, 0, 0, 0, "", null,
      0, 0, 0, 0, 48000, "mixed_only", true, false, false,
      1, 0, 0, 1, status, new ArrayList<>(), new ArrayList<>());
  }

  private static Set<String> set (String... values) {
    return new HashSet<>(Arrays.asList(values));
  }

  private static String repeat (String value, int count) {
    StringBuilder result = new StringBuilder(value.length() * count);
    for (int i = 0; i < count; i++) result.append(value);
    return result.toString();
  }
}
