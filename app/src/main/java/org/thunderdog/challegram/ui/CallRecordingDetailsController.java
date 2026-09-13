/*
 * This file is a part of Telegram X
 * Copyright © 2014 (tgx-android@pm.me)
 */
package org.thunderdog.challegram.ui;

import android.app.Activity;
import android.content.ClipData;
import android.content.ContentResolver;
import android.content.Context;
import android.content.Intent;
import android.media.AudioAttributes;
import android.media.AudioFocusRequest;
import android.media.AudioManager;
import android.media.MediaPlayer;
import android.net.Uri;
import android.os.Build;
import android.view.Gravity;
import android.view.View;
import android.view.ViewGroup;
import android.widget.FrameLayout;
import android.widget.ImageView;
import android.widget.LinearLayout;
import android.widget.ScrollView;
import android.widget.SeekBar;
import android.widget.TextView;
import android.widget.Toast;

import androidx.annotation.Nullable;

import org.drinkless.tdlib.TdApi;
import org.thunderdog.challegram.FileProvider;
import org.thunderdog.challegram.R;
import org.thunderdog.challegram.config.Config;
import org.thunderdog.challegram.core.Lang;
import org.thunderdog.challegram.data.TD;
import org.thunderdog.challegram.navigation.ActivityResultHandler;
import org.thunderdog.challegram.navigation.ViewController;
import org.thunderdog.challegram.service.TGCallService;
import org.thunderdog.challegram.support.ViewSupport;
import org.thunderdog.challegram.telegram.Tdlib;
import org.thunderdog.challegram.theme.ColorId;
import org.thunderdog.challegram.theme.Theme;
import org.thunderdog.challegram.tool.Screen;
import org.thunderdog.challegram.tool.Strings;
import org.thunderdog.challegram.tool.UI;
import org.thunderdog.challegram.voip.recording.CallRecordingItem;
import org.thunderdog.challegram.voip.recording.CallRecordingRepository;
import org.thunderdog.challegram.widget.AvatarView;

import java.io.File;
import java.util.ArrayList;
import java.util.Comparator;
import java.util.List;
import java.util.Locale;
import java.util.concurrent.TimeUnit;

public final class CallRecordingDetailsController
  extends ViewController<CallRecordingDetailsController.Args>
  implements ActivityResultHandler {

  public static final class Args {
    public final CallRecordingItem item;

    public Args (CallRecordingItem item) {
      this.item = item;
    }
  }

  private static final int REQUEST_SAVE_RECORDING = 7201;
  private final CallRecordingRepository repository;
  private CallRecordingItem item;
  private MediaPlayer player;
  private CallRecordingRepository.ExportVariant playingVariant;
  private CallRecordingRepository.ExportVariant pendingSaveVariant;
  private SeekBar seekBar;
  private TextView positionView;
  private AvatarView avatarView;
  private final AudioManager.OnAudioFocusChangeListener focusListener = change -> {
    if (change < 0 && player != null) {
      try {
        if (player.isPlaying()) player.pause();
      } catch (IllegalStateException ignored) {
      }
    }
  };
  private AudioManager audioManager;
  private AudioFocusRequest audioFocusRequest;
  private final Runnable progressUpdater = new Runnable() {
    @Override
    public void run () {
      if (player == null) return;
      if (TGCallService.currentInstance() != null) {
        releasePlayer();
        return;
      }
      try {
        int duration = Math.max(0, player.getDuration());
        int position = Math.max(0, player.getCurrentPosition());
        seekBar.setMax(duration);
        seekBar.setProgress(position);
        updatePosition(position, duration);
        if (player.isPlaying()) UI.post(this, 500L);
      } catch (IllegalStateException ignored) {
      }
    }
  };

  public CallRecordingDetailsController (Context context, Tdlib tdlib) {
    super(context, tdlib);
    repository = new CallRecordingRepository(context);
  }

  @Override
  public void setArguments (Args args) {
    super.setArguments(args);
    item = args.item;
  }

  @Override
  public int getId () {
    return R.id.controller_callRecordingDetails;
  }

  @Override
  public CharSequence getName () {
    return Lang.getString(R.string.CallRecordingDetails);
  }

  @Override
  protected View onCreateView (Context context) {
    ScrollView scroll = new ScrollView(context);
    ViewSupport.setThemedBackground(scroll, ColorId.background, this);
    LinearLayout content = new LinearLayout(context);
    content.setOrientation(LinearLayout.VERTICAL);
    content.setPadding(Screen.dp(20f), Screen.dp(20f), Screen.dp(20f), Screen.dp(32f));
    scroll.addView(content, new ScrollView.LayoutParams(
      ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.WRAP_CONTENT));

    FrameLayout avatarWrap = new FrameLayout(context);
    LinearLayout.LayoutParams avatarParams = new LinearLayout.LayoutParams(Screen.dp(80f), Screen.dp(80f));
    avatarParams.gravity = Gravity.CENTER_HORIZONTAL;
    content.addView(avatarWrap, avatarParams);
    TdApi.User currentUser = item.userId != 0 ? tdlib.cache().user(item.userId) : null;
    if (currentUser != null) {
      avatarView = new AvatarView(context);
      avatarView.setUser(tdlib, currentUser, false);
      avatarWrap.addView(avatarView, new FrameLayout.LayoutParams(
        ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.MATCH_PARENT));
    } else {
      ImageView placeholder = new ImageView(context);
      placeholder.setImageResource(R.drawable.baseline_person_24);
      placeholder.setColorFilter(Theme.getColor(ColorId.icon));
      placeholder.setPadding(Screen.dp(18f), Screen.dp(18f), Screen.dp(18f), Screen.dp(18f));
      avatarWrap.addView(placeholder, new FrameLayout.LayoutParams(
        ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.MATCH_PARENT));
    }

    TextView name = text(context, 20f, Theme.textAccentColor());
    name.setGravity(Gravity.CENTER);
    name.setText(currentUser != null ? TD.getUserName(currentUser) :
      !item.displayName.isEmpty() ? item.displayName :
        Lang.getString(R.string.CallRecordingUnknownContact));
    content.addView(name, matchWrap(10));

    String direction = item.isOutgoing == null
      ? Lang.getString(R.string.CallRecordingDirectionUnknown)
      : Lang.getString(item.isOutgoing
        ? R.string.CallRecordingOutgoing : R.string.CallRecordingIncoming);
    addInfo(content, direction);
    if (item.recordingStartTime > 0) {
      addInfo(content, Lang.getTimestamp(item.recordingStartTime, TimeUnit.MILLISECONDS));
    }
    addInfo(content, Lang.getString(R.string.CallRecordingDurationValue,
      Strings.buildDuration(item.durationMs / 1000L)));
    addInfo(content, Lang.getString(R.string.CallRecordingSizeValue,
      Strings.buildSize(item.totalSize)));
    addInfo(content, Lang.getString(R.string.CallRecordingStatusValue,
      statusText(item.status)));

    addSectionTitle(content, R.string.CallRecordingPlayback);
    if (item.hasMixed) addTrackButton(content, R.string.CallRecordingPlayMixed,
      CallRecordingRepository.ExportVariant.MIXED);
    if (item.hasLocal) addTrackButton(content, R.string.CallRecordingPlayLocal,
      CallRecordingRepository.ExportVariant.LOCAL);
    if (item.hasRemote) addTrackButton(content, R.string.CallRecordingPlayRemote,
      CallRecordingRepository.ExportVariant.REMOTE);
    if (!item.hasAnyPlayableOutput()) addInfo(content,
      Lang.getString(R.string.CallRecordingNoPlayableFiles));

    seekBar = new SeekBar(context);
    seekBar.setEnabled(false);
    seekBar.setOnSeekBarChangeListener(new SeekBar.OnSeekBarChangeListener() {
      @Override public void onProgressChanged (SeekBar seekBar, int progress, boolean fromUser) { }
      @Override public void onStartTrackingTouch (SeekBar seekBar) { }
      @Override public void onStopTrackingTouch (SeekBar seekBar) {
        if (player != null) {
          try { player.seekTo(seekBar.getProgress()); } catch (IllegalStateException ignored) { }
        }
      }
    });
    content.addView(seekBar, matchWrap(8));
    positionView = text(context, 14f, Theme.textDecentColor());
    positionView.setGravity(Gravity.CENTER);
    updatePosition(0, (int) Math.min(Integer.MAX_VALUE, item.durationMs));
    content.addView(positionView, matchWrap(0));

    addControlIntervals(content);

    addSectionTitle(content, R.string.CallRecordingActions);
    TextView share = actionButton(context, R.string.Share);
    share.setOnClickListener(v -> chooseExport(false));
    content.addView(share, matchWrap(6));
    TextView save = actionButton(context, R.string.CallRecordingSaveTo);
    save.setOnClickListener(v -> chooseExport(true));
    content.addView(save, matchWrap(6));
    TextView delete = actionButton(context, R.string.CallRecordingDelete);
    delete.setTextColor(Theme.textRedColor());
    delete.setOnClickListener(v -> confirmDelete());
    content.addView(delete, matchWrap(6));
    boolean actionsEnabled = !item.isActive();
    share.setEnabled(actionsEnabled && item.hasAnyPlayableOutput());
    save.setEnabled(actionsEnabled && item.hasAnyPlayableOutput());
    delete.setEnabled(actionsEnabled);
    share.setAlpha(share.isEnabled() ? 1f : .45f);
    save.setAlpha(save.isEnabled() ? 1f : .45f);
    delete.setAlpha(delete.isEnabled() ? 1f : .45f);
    return scroll;
  }

  private void addControlIntervals (LinearLayout content) {
    class Entry {
      final CallRecordingItem.ControlInterval interval;
      final int label;
      Entry (CallRecordingItem.ControlInterval interval, int label) {
        this.interval = interval;
        this.label = label;
      }
    }
    List<Entry> entries = new ArrayList<>();
    for (CallRecordingItem.ControlInterval value : item.pauseIntervals)
      entries.add(new Entry(value, R.string.CallRecordingPausedInterval));
    for (CallRecordingItem.ControlInterval value : item.stoppedIntervals)
      entries.add(new Entry(value, R.string.CallRecordingStoppedInterval));
    if (entries.isEmpty()) return;
    entries.sort(Comparator.comparingLong(value -> value.interval.startSample));
    addSectionTitle(content, R.string.CallRecordingIntervals);
    for (Entry entry : entries) {
      String start = formatSamplePosition(entry.interval.startSample, item.timelineSampleRate);
      String end = formatSamplePosition(entry.interval.endSample, item.timelineSampleRate);
      addInfo(content, start + " – " + end + "   " + Lang.getString(entry.label));
    }
  }

  private void addTrackButton (
    LinearLayout content,
    int label,
    CallRecordingRepository.ExportVariant variant
  ) {
    TextView button = actionButton(context, label);
    button.setOnClickListener(v -> playOrPause(variant));
    button.setEnabled(!item.isActive());
    button.setAlpha(button.isEnabled() ? 1f : .45f);
    content.addView(button, matchWrap(6));
  }

  private void playOrPause (CallRecordingRepository.ExportVariant variant) {
    if (item.isActive()) return;
    if (TGCallService.currentInstance() != null) {
      Toast.makeText(context, R.string.CallRecordingPlaybackDisabledDuringCall, Toast.LENGTH_SHORT).show();
      return;
    }
    if (player != null && playingVariant == variant) {
      try {
        if (player.isPlaying()) {
          player.pause();
        } else if (requestAudioFocus()) {
          player.start();
          UI.post(progressUpdater);
        }
      } catch (IllegalStateException ignored) {
        releasePlayer();
      }
      return;
    }
    releasePlayer();
    File source = trackFile(variant);
    if (source == null || !source.isFile()) return;
    if (!requestAudioFocus()) {
      Toast.makeText(context, R.string.CallRecordingAudioFocusFailed, Toast.LENGTH_SHORT).show();
      return;
    }
    try {
      MediaPlayer newPlayer = new MediaPlayer();
      newPlayer.setAudioAttributes(new AudioAttributes.Builder()
        .setUsage(AudioAttributes.USAGE_MEDIA)
        .setContentType(AudioAttributes.CONTENT_TYPE_MUSIC)
        .build());
      newPlayer.setDataSource(source.getAbsolutePath());
      newPlayer.setOnPreparedListener(value -> {
        if (player != value || TGCallService.currentInstance() != null) {
          releasePlayer();
          return;
        }
        seekBar.setEnabled(true);
        seekBar.setMax(value.getDuration());
        value.start();
        UI.post(progressUpdater);
      });
      newPlayer.setOnCompletionListener(value -> {
        seekBar.setProgress(seekBar.getMax());
        updatePosition(seekBar.getMax(), seekBar.getMax());
        abandonAudioFocus();
      });
      newPlayer.setOnErrorListener((value, what, extra) -> {
        releasePlayer();
        Toast.makeText(context, R.string.CallRecordingPlaybackFailed, Toast.LENGTH_SHORT).show();
        return true;
      });
      player = newPlayer;
      playingVariant = variant;
      newPlayer.prepareAsync();
    } catch (Throwable t) {
      releasePlayer();
      Toast.makeText(context, R.string.CallRecordingPlaybackFailed, Toast.LENGTH_SHORT).show();
    }
  }

  private boolean requestAudioFocus () {
    audioManager = (AudioManager) context.getSystemService(Context.AUDIO_SERVICE);
    if (audioManager == null) return false;
    if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.O) {
      audioFocusRequest = new AudioFocusRequest.Builder(AudioManager.AUDIOFOCUS_GAIN_TRANSIENT)
        .setAudioAttributes(new AudioAttributes.Builder()
          .setUsage(AudioAttributes.USAGE_MEDIA)
          .setContentType(AudioAttributes.CONTENT_TYPE_MUSIC)
          .build())
        .setOnAudioFocusChangeListener(focusListener)
        .build();
      return audioManager.requestAudioFocus(audioFocusRequest) == AudioManager.AUDIOFOCUS_REQUEST_GRANTED;
    }
    return audioManager.requestAudioFocus(focusListener, AudioManager.STREAM_MUSIC,
      AudioManager.AUDIOFOCUS_GAIN_TRANSIENT) == AudioManager.AUDIOFOCUS_REQUEST_GRANTED;
  }

  private void abandonAudioFocus () {
    if (audioManager == null) return;
    if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.O && audioFocusRequest != null) {
      audioManager.abandonAudioFocusRequest(audioFocusRequest);
    } else {
      audioManager.abandonAudioFocus(focusListener);
    }
    audioFocusRequest = null;
    audioManager = null;
  }

  private void releasePlayer () {
    UI.removePendingRunnable(progressUpdater);
    if (player != null) {
      try { player.stop(); } catch (Throwable ignored) { }
      player.release();
      player = null;
    }
    playingVariant = null;
    if (seekBar != null) {
      seekBar.setEnabled(false);
      seekBar.setProgress(0);
    }
    abandonAudioFocus();
  }

  private void chooseExport (boolean save) {
    if (item.isActive()) return;
    ArrayList<Integer> ids = new ArrayList<>();
    ArrayList<String> names = new ArrayList<>();
    ArrayList<Integer> icons = new ArrayList<>();
    if (item.hasMixed) addExportOption(ids, names, icons, R.id.btn_callRecordingExportMixed,
      R.string.CallRecordingMixed, R.drawable.baseline_play_circle_filled_24_white);
    if (item.hasLocal) addExportOption(ids, names, icons, R.id.btn_callRecordingExportLocal,
      R.string.CallRecordingLocal, R.drawable.baseline_play_circle_filled_24_white);
    if (item.hasRemote) addExportOption(ids, names, icons, R.id.btn_callRecordingExportRemote,
      R.string.CallRecordingRemote, R.drawable.baseline_play_circle_filled_24_white);
    addExportOption(ids, names, icons, R.id.btn_callRecordingExportAll,
      R.string.CallRecordingAllFiles, R.drawable.baseline_folder_24);
    showOptions(null, toIntArray(ids), names.toArray(new String[0]), null,
      toIntArray(icons), (view, id) -> {
        CallRecordingRepository.ExportVariant variant = variantForId(id);
        if (variant != null) {
          if (save) launchSave(variant); else share(variant);
        }
        return true;
      });
  }

  private void share (CallRecordingRepository.ExportVariant variant) {
    Toast.makeText(context, R.string.CallRecordingPreparingExport, Toast.LENGTH_SHORT).show();
    repository.prepareExport(item, variant, (prepared, error) -> {
      if (isDestroyed()) return;
      if (prepared == null) {
        Toast.makeText(context, R.string.CallRecordingExportFailed, Toast.LENGTH_SHORT).show();
        return;
      }
      try {
        Uri uri = FileProvider.getUriForFile(context, Config.FILE_PROVIDER_AUTHORITY, prepared.file);
        if (!ContentResolver.SCHEME_CONTENT.equals(uri.getScheme())) {
          throw new SecurityException("Export URI is not content://");
        }
        Intent share = new Intent(Intent.ACTION_SEND);
        share.setType(prepared.mimeType);
        share.putExtra(Intent.EXTRA_STREAM, uri);
        share.setClipData(ClipData.newRawUri("recording", uri));
        share.addFlags(Intent.FLAG_GRANT_READ_URI_PERMISSION);
        context.startActivity(Intent.createChooser(share, Lang.getString(R.string.Share)));
      } catch (Throwable t) {
        Toast.makeText(context, R.string.CallRecordingExportFailed, Toast.LENGTH_SHORT).show();
      }
    });
  }

  private void launchSave (CallRecordingRepository.ExportVariant variant) {
    pendingSaveVariant = variant;
    Intent intent = new Intent(Intent.ACTION_CREATE_DOCUMENT);
    intent.addCategory(Intent.CATEGORY_OPENABLE);
    intent.setType(variant == CallRecordingRepository.ExportVariant.ALL
      ? "application/zip" : "audio/ogg");
    intent.putExtra(Intent.EXTRA_TITLE, repository.suggestedExportName(item, variant));
    context.startActivityForResult(intent, REQUEST_SAVE_RECORDING);
  }

  @Override
  public void onActivityResult (int requestCode, int resultCode, Intent data) {
    if (requestCode != REQUEST_SAVE_RECORDING) return;
    CallRecordingRepository.ExportVariant variant = pendingSaveVariant;
    pendingSaveVariant = null;
    if (resultCode != Activity.RESULT_OK || data == null || data.getData() == null || variant == null) return;
    Toast.makeText(context, R.string.CallRecordingSaving, Toast.LENGTH_SHORT).show();
    repository.save(item, variant, data.getData(), (saved, error) -> {
      if (!isDestroyed()) Toast.makeText(context,
        Boolean.TRUE.equals(saved) ? R.string.CallRecordingSaved : R.string.CallRecordingExportFailed,
        Toast.LENGTH_SHORT).show();
    });
  }

  private void confirmDelete () {
    if (item.isActive()) return;
    showOptions(Lang.getString(R.string.CallRecordingDeletePrompt),
      new int[] {R.id.btn_callRecordingDeleteConfirm, R.id.btn_cancel},
      new String[] {Lang.getString(R.string.CallRecordingDelete), Lang.getString(R.string.Cancel)},
      new int[] {OptionColor.RED, OptionColor.NORMAL},
      new int[] {R.drawable.baseline_delete_24, R.drawable.baseline_cancel_24},
      (view, id) -> {
        if (id == R.id.btn_callRecordingDeleteConfirm) {
          repository.delete(item, (deleted, error) -> {
            if (isDestroyed()) return;
            if (Boolean.TRUE.equals(deleted)) {
              navigateBack();
            } else {
              Toast.makeText(context, R.string.CallRecordingDeleteFailed, Toast.LENGTH_SHORT).show();
              // Returning to the list triggers its asynchronous repository
              // refresh and exposes any files that survived a partial delete.
              navigateBack();
            }
          });
        }
        return true;
      });
  }

  private @Nullable File trackFile (CallRecordingRepository.ExportVariant variant) {
    String name;
    switch (variant) {
      case MIXED: name = "mixed.opus"; break;
      case LOCAL: name = "local.opus"; break;
      case REMOTE: name = "remote.opus"; break;
      default: return null;
    }
    try {
      File value = new File(item.sessionPath, name).getCanonicalFile();
      return value.getParentFile().equals(item.sessionPath.getCanonicalFile()) ? value : null;
    } catch (Throwable ignored) {
      return null;
    }
  }

  private static void addExportOption (List<Integer> ids, List<String> names,
                                       List<Integer> icons, int id, int name, int icon) {
    ids.add(id);
    names.add(Lang.getString(name));
    icons.add(icon);
  }

  private static int[] toIntArray (List<Integer> values) {
    int[] result = new int[values.size()];
    for (int i = 0; i < values.size(); i++) result[i] = values.get(i);
    return result;
  }

  private static @Nullable CallRecordingRepository.ExportVariant variantForId (int id) {
    if (id == R.id.btn_callRecordingExportMixed) return CallRecordingRepository.ExportVariant.MIXED;
    if (id == R.id.btn_callRecordingExportLocal) return CallRecordingRepository.ExportVariant.LOCAL;
    if (id == R.id.btn_callRecordingExportRemote) return CallRecordingRepository.ExportVariant.REMOTE;
    if (id == R.id.btn_callRecordingExportAll) return CallRecordingRepository.ExportVariant.ALL;
    return null;
  }

  private static String formatSamplePosition (long samples, int sampleRate) {
    long safeSamples = Math.max(0, samples);
    long safeRate = sampleRate > 0 ? sampleRate : 48000L;
    long secondsPart = safeSamples / safeRate;
    long remainder = safeSamples % safeRate;
    long tenths = secondsPart * 10L +
      (remainder * 10L + safeRate / 2L) / safeRate;
    long hours = tenths / 36000L;
    long minutes = tenths / 600L % 60L;
    long seconds = tenths / 10L % 60L;
    long fraction = tenths % 10L;
    return hours > 0
      ? String.format(Locale.US, "%d:%02d:%02d.%d", hours, minutes, seconds, fraction)
      : String.format(Locale.US, "%02d:%02d.%d", minutes, seconds, fraction);
  }

  private String statusText (CallRecordingItem.Status status) {
    switch (status) {
      case COMPLETED: return Lang.getString(R.string.CallRecordingStatusCompleted);
      case FAILED: return Lang.getString(R.string.CallRecordingStatusFailed);
      case IN_PROGRESS: return Lang.getString(R.string.CallRecordingStatusInProgress);
      default: return Lang.getString(R.string.CallRecordingStatusIncomplete);
    }
  }

  private static TextView text (Context context, float size, int color) {
    TextView result = new TextView(context);
    result.setTextSize(size);
    result.setTextColor(color);
    return result;
  }

  private static TextView actionButton (Context context, int label) {
    TextView result = text(context, 16f, Theme.textAccentColor());
    result.setText(label);
    result.setGravity(Gravity.CENTER);
    result.setPadding(Screen.dp(12f), Screen.dp(12f), Screen.dp(12f), Screen.dp(12f));
    result.setBackgroundColor(Theme.getColor(ColorId.filling));
    return result;
  }

  private static LinearLayout.LayoutParams matchWrap (float topMarginDp) {
    LinearLayout.LayoutParams params = new LinearLayout.LayoutParams(
      ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.WRAP_CONTENT);
    params.topMargin = Screen.dp(topMarginDp);
    return params;
  }

  private static void addInfo (LinearLayout content, String value) {
    TextView view = text(content.getContext(), 15f, Theme.textDecentColor());
    view.setText(value);
    view.setGravity(Gravity.CENTER_HORIZONTAL);
    content.addView(view, matchWrap(4));
  }

  private static void addSectionTitle (LinearLayout content, int label) {
    TextView title = text(content.getContext(), 17f, Theme.textAccentColor());
    title.setText(label);
    content.addView(title, matchWrap(22));
  }

  private void updatePosition (int positionMs, int durationMs) {
    if (positionView == null) return;
    positionView.setText(Strings.buildDuration(positionMs / 1000L) + " / " +
      Strings.buildDuration(durationMs / 1000L));
  }

  @Override
  public void destroy () {
    releasePlayer();
    if (avatarView != null) {
      avatarView.performDestroy();
      avatarView = null;
    }
    super.destroy();
  }
}
