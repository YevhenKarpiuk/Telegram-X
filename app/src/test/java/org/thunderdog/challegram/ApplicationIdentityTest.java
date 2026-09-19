/*
 * This file is a part of Telegram X Recorder.
 */
package org.thunderdog.challegram;

import org.junit.Test;
import org.thunderdog.challegram.config.Config;

import static org.junit.Assert.assertEquals;
import static org.junit.Assert.assertTrue;

public class ApplicationIdentityTest {
  @Test
  public void fileProviderAuthorityTracksApplicationId () {
    assertEquals("ka.soft.tgxr", BuildConfig.APPLICATION_ID);
    assertEquals(BuildConfig.APPLICATION_ID + ".provider", Config.FILE_PROVIDER_AUTHORITY);
  }

  @Test
  public void productIdentityMatchesRecorder () {
    assertEquals("Telegram X Recorder", BuildConfig.PROJECT_NAME);
  }

  @Test
  public void recorderReleaseIdentityIsIndependentFromUpstreamVersioning () {
    assertEquals(3, BuildConfig.TGXR_RELEASE_VERSION);
    assertEquals("1.1.0", BuildConfig.TGXR_RELEASE_NAME);
    assertTrue(BuildConfig.VERSION_CODE > 1808303);
    assertTrue(BuildConfig.VERSION_NAME.startsWith(BuildConfig.TGXR_RELEASE_NAME));
  }
}
