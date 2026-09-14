/*
 * This file is a part of Telegram X Recorder.
 */
package org.thunderdog.challegram;

import org.junit.Test;
import org.thunderdog.challegram.config.Config;

import static org.junit.Assert.assertEquals;

public class ApplicationIdentityTest {
  @Test
  public void fileProviderAuthorityTracksApplicationId () {
    assertEquals("ka.soft.tgxr", BuildConfig.APPLICATION_ID);
    assertEquals(BuildConfig.APPLICATION_ID + ".provider", Config.FILE_PROVIDER_AUTHORITY);
  }

  @Test
  public void recorderReleaseIdentityIsIndependentFromUpstreamVersioning () {
    assertEquals(1, BuildConfig.TGXR_RELEASE_VERSION);
    assertEquals("1.0", BuildConfig.TGXR_RELEASE_NAME);
    assertEquals(1808, BuildConfig.ORIGINAL_VERSION_CODE);
  }
}
