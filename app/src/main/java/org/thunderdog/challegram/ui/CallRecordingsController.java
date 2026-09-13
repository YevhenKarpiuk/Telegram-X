/*
 * This file is a part of Telegram X
 * Copyright © 2014 (tgx-android@pm.me)
 */
package org.thunderdog.challegram.ui;

import android.content.Context;
import android.graphics.Typeface;
import android.view.Gravity;
import android.view.View;
import android.view.ViewGroup;
import android.widget.FrameLayout;
import android.widget.ImageView;
import android.widget.LinearLayout;
import android.widget.TextView;

import androidx.annotation.NonNull;
import androidx.recyclerview.widget.RecyclerView;

import org.drinkless.tdlib.TdApi;
import org.thunderdog.challegram.R;
import org.thunderdog.challegram.core.Lang;
import org.thunderdog.challegram.data.TD;
import org.thunderdog.challegram.telegram.Tdlib;
import org.thunderdog.challegram.theme.ColorId;
import org.thunderdog.challegram.theme.Theme;
import org.thunderdog.challegram.tool.Screen;
import org.thunderdog.challegram.tool.Strings;
import org.thunderdog.challegram.v.CustomRecyclerView;
import org.thunderdog.challegram.voip.recording.CallRecordingItem;
import org.thunderdog.challegram.voip.recording.CallRecordingRepository;
import org.thunderdog.challegram.widget.AvatarView;

import java.util.ArrayList;
import java.util.Collections;
import java.util.List;
import java.util.concurrent.TimeUnit;

public final class CallRecordingsController extends RecyclerViewController<Void> {
  private final CallRecordingRepository repository;
  private RecordingsAdapter adapter;
  private boolean loading;

  public CallRecordingsController (Context context, Tdlib tdlib) {
    super(context, tdlib);
    repository = new CallRecordingRepository(context);
  }

  @Override
  public int getId () {
    return R.id.controller_callRecordings;
  }

  @Override
  public CharSequence getName () {
    return Lang.getString(R.string.CallRecordings);
  }

  @Override
  public boolean needAsynchronousAnimation () {
    return loading;
  }

  @Override
  protected void onCreateView (Context context, CustomRecyclerView recyclerView) {
    adapter = new RecordingsAdapter();
    recyclerView.setAdapter(adapter);
    load();
  }

  @Override
  public void onFocus () {
    super.onFocus();
    if (adapter != null) load();
  }

  private void load () {
    if (loading) return;
    loading = true;
    repository.scan((items, error) -> {
      if (isDestroyed() || adapter == null) return;
      loading = false;
      adapter.setItems(items != null ? items : Collections.emptyList());
      executeScheduledAnimation();
    });
  }

  private String displayName (CallRecordingItem item) {
    TdApi.User user = item.userId != 0 ? tdlib.cache().user(item.userId) : null;
    if (user != null) return TD.getUserName(user);
    return !item.displayName.isEmpty()
      ? item.displayName : Lang.getString(R.string.CallRecordingUnknownContact);
  }

  private String statusText (CallRecordingItem.Status status) {
    switch (status) {
      case FAILED: return Lang.getString(R.string.CallRecordingStatusFailed);
      case INCOMPLETE: return Lang.getString(R.string.CallRecordingStatusIncomplete);
      case IN_PROGRESS: return Lang.getString(R.string.CallRecordingStatusInProgress);
      default: return "";
    }
  }

  private final class RecordingsAdapter extends RecyclerView.Adapter<RecordingHolder> {
    private List<CallRecordingItem> items = Collections.emptyList();
    private final ArrayList<RecordingHolder> holders = new ArrayList<>();

    void setItems (List<CallRecordingItem> items) {
      this.items = items;
      notifyDataSetChanged();
    }

    @NonNull
    @Override
    public RecordingHolder onCreateViewHolder (@NonNull ViewGroup parent, int viewType) {
      RecordingHolder holder = new RecordingHolder(parent.getContext());
      holders.add(holder);
      return holder;
    }

    @Override
    public void onBindViewHolder (@NonNull RecordingHolder holder, int position) {
      holder.bind(items.isEmpty() ? null : items.get(position));
    }

    @Override
    public int getItemCount () {
      return Math.max(1, items.size());
    }

    @Override
    public void onViewRecycled (@NonNull RecordingHolder holder) {
      holder.clearAvatar();
      super.onViewRecycled(holder);
    }

    void destroy () {
      for (RecordingHolder holder : holders) holder.destroyAvatar();
      holders.clear();
    }
  }

  private final class RecordingHolder extends RecyclerView.ViewHolder implements View.OnClickListener {
    private final AvatarView avatar;
    private final ImageView placeholder;
    private final TextView title;
    private final TextView subtitle;
    private final TextView details;
    private CallRecordingItem item;

    RecordingHolder (Context context) {
      super(new LinearLayout(context));
      LinearLayout row = (LinearLayout) itemView;
      row.setOrientation(LinearLayout.HORIZONTAL);
      row.setGravity(Gravity.CENTER_VERTICAL);
      row.setPadding(Screen.dp(16f), Screen.dp(10f), Screen.dp(16f), Screen.dp(10f));
      row.setLayoutParams(new RecyclerView.LayoutParams(
        ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.WRAP_CONTENT));
      row.setOnClickListener(this);

      FrameLayout avatarWrap = new FrameLayout(context);
      row.addView(avatarWrap, new LinearLayout.LayoutParams(Screen.dp(52f), Screen.dp(52f)));
      avatar = new AvatarView(context);
      avatarWrap.addView(avatar, new FrameLayout.LayoutParams(
        ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.MATCH_PARENT));
      placeholder = new ImageView(context);
      placeholder.setImageResource(R.drawable.baseline_person_24);
      placeholder.setColorFilter(Theme.getColor(ColorId.icon));
      placeholder.setPadding(Screen.dp(12f), Screen.dp(12f), Screen.dp(12f), Screen.dp(12f));
      avatarWrap.addView(placeholder, new FrameLayout.LayoutParams(
        ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.MATCH_PARENT));

      LinearLayout textWrap = new LinearLayout(context);
      textWrap.setOrientation(LinearLayout.VERTICAL);
      LinearLayout.LayoutParams textParams = new LinearLayout.LayoutParams(
        0, ViewGroup.LayoutParams.WRAP_CONTENT, 1f);
      textParams.leftMargin = Screen.dp(14f);
      row.addView(textWrap, textParams);
      title = text(context, 16f, Theme.textAccentColor());
      title.setTypeface(Typeface.DEFAULT, Typeface.BOLD);
      subtitle = text(context, 14f, Theme.textDecentColor());
      details = text(context, 14f, Theme.textDecentColor());
      textWrap.addView(title);
      textWrap.addView(subtitle);
      textWrap.addView(details);
    }

    void bind (CallRecordingItem item) {
      this.item = item;
      if (item == null) {
        clearAvatar();
        avatar.setVisibility(View.GONE);
        placeholder.setVisibility(View.GONE);
        title.setText(R.string.CallRecordingEmpty);
        title.setTypeface(Typeface.DEFAULT, Typeface.NORMAL);
        subtitle.setText("");
        details.setText("");
        itemView.setOnClickListener(null);
        return;
      }
      itemView.setOnClickListener(this);
      title.setTypeface(Typeface.DEFAULT, Typeface.BOLD);
      title.setText(displayName(item));
      TdApi.User user = item.userId != 0 ? tdlib.cache().user(item.userId) : null;
      avatar.setVisibility(user != null ? View.VISIBLE : View.GONE);
      placeholder.setVisibility(user == null ? View.VISIBLE : View.GONE);
      if (user != null) avatar.setUser(tdlib, user, false);
      String direction = item.isOutgoing == null
        ? Lang.getString(R.string.CallRecordingDirectionUnknown)
        : Lang.getString(item.isOutgoing
          ? R.string.CallRecordingOutgoing : R.string.CallRecordingIncoming);
      String date = item.recordingStartTime > 0
        ? Lang.getTimestamp(item.recordingStartTime, TimeUnit.MILLISECONDS) : "";
      subtitle.setText(direction + (date.isEmpty() ? "" : " · " + date));
      String value = Strings.buildDuration(item.durationMs / 1000L) +
        " · " + Strings.buildSize(item.totalSize);
      String status = statusText(item.status);
      details.setText(status.isEmpty() ? value : value + " · " + status);
    }

    void clearAvatar () {
      avatar.setUser(tdlib, (TdApi.User) null, false);
    }

    void destroyAvatar () {
      avatar.performDestroy();
    }

    @Override
    public void onClick (View view) {
      if (item == null || adapter.items.isEmpty()) return;
      CallRecordingDetailsController controller =
        new CallRecordingDetailsController(context, tdlib);
      controller.setArguments(new CallRecordingDetailsController.Args(item));
      navigateTo(controller);
    }
  }

  private static TextView text (Context context, float size, int color) {
    TextView view = new TextView(context);
    view.setTextSize(size);
    view.setTextColor(color);
    return view;
  }

  @Override
  public void destroy () {
    if (adapter != null) {
      adapter.destroy();
      adapter = null;
    }
    super.destroy();
  }
}
