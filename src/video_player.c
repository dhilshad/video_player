#include <string.h>

#include <gtk/gtk.h>
#include <gst/gst.h>
#include <gst/video/videooverlay.h>
#include <dbus/dbus.h>

#include "log.h"

#include <gdk/gdk.h>
#include <gdk/gdkx.h>

#define SEEK_INTERVAL 10
#define SESSION_MANAGER_DBUS "org.gnome.SessionManager"
#define SESSION_MANAGER_DBUS_PATH "/org/gnome/SessionManager"
#define SESSION_MANAGER_DBUS_INTERFACE "org.gnome.SessionManager"
#define SESSION_MANAGER_INHIBIT "Inhibit"
#define SESSION_MANAGER_UNINHIBIT "Uninhibit"
#define APPLICATION_NAME "vdplayer"

/* Structure to contain all our information, so we can pass it around */
typedef struct _CustomData {
  GtkWidget *main_window;        /* Reference to main video wondow */
  GstElement *playbin;           /* Our one and only pipeline */

  GtkWidget *slider;              /* Slider widget to keep track of current position */
  //GtkWidget *streams_list;        /* Text widget to display info about the streams */
  gulong slider_update_signal_id; /* Signal ID for the slider update signal */

  GstState state;                 /* Current state of the pipeline */
  gint64 duration;                /* Duration of the clip, in nanoseconds */

  DBusConnection *sessionBus;     /* Dbus connection to session bus */

  Bool isSleepInhibited;         /* Represents whether session manager sleep is inhibited or not */
  unsigned int dbusInbitCookie;  /* Cookie got while calling sleep inhibit function to session manager.
                                    used for calling uninhibit sleep */
} CustomData;

/*Function to inhibit cpu sleep. Also used for setting cpu sleep back on */
void inhibit_cpu_sleep(CustomData *data, Bool enable) {

  DBusError dbusError;
  DBusMessage *request;
  DBusMessageIter reqIter;
  DBusMessageIter replyIter;
  int windowId = 0;
  char *appName = APPLICATION_NAME;
  char *reason = "my video player is running";
  int flag = 8; /* 8: to stop session being marked as idle */
  DBusPendingCall *pendingReturn;
  DBusMessage *reply;
  unsigned int inhibitCookie = 0;

  LOGD("Entered %s", __func__);

  if (enable) {
    LOGD("Going to inhibit cpu sleep");
    request = dbus_message_new_method_call(SESSION_MANAGER_DBUS, SESSION_MANAGER_DBUS_PATH,
                             SESSION_MANAGER_DBUS_INTERFACE, SESSION_MANAGER_INHIBIT);
    if (NULL == request) {
      LOGD("Error in creating dbus method");
      goto Exit;
    }
    dbus_message_iter_init_append (request, &reqIter);

    if (!(dbus_message_iter_append_basic (&reqIter, DBUS_TYPE_STRING, &appName))) {
      LOGD("Error adding appilcation name %d", __LINE__);
      goto Exit;
    }
    if (!(dbus_message_iter_append_basic (&reqIter, DBUS_TYPE_UINT32 , &windowId))) {
      LOGD("Error adding window id %d", __LINE__);
      goto Exit;
    }
    if (!(dbus_message_iter_append_basic (&reqIter, DBUS_TYPE_STRING, &reason))) {
      LOGD("Error adding reason %d", __LINE__);
      goto Exit;
    }
    if (!(dbus_message_iter_append_basic (&reqIter, DBUS_TYPE_UINT32, &flag))) {
      LOGD("Error adding flag %d", __LINE__);
      goto Exit;
    }

  } else {
    LOGD("Enabling CPU sleep");
    request = dbus_message_new_method_call(SESSION_MANAGER_DBUS, SESSION_MANAGER_DBUS_PATH,
                           SESSION_MANAGER_DBUS_INTERFACE, SESSION_MANAGER_UNINHIBIT);
    if (NULL == request) {
      LOGD("Error in creating dbus method");
      goto Exit;
    }
    dbus_message_iter_init_append (request, &reqIter);
    if (!(dbus_message_iter_append_basic (&reqIter, DBUS_TYPE_UINT32, &(data->dbusInbitCookie)))) {
      LOGD("Error adding inhibit cookie");
      goto Exit;
    }
  }

  if (!dbus_connection_send_with_reply (data->sessionBus, request, &pendingReturn, -1)) {
    LOGD ("Error in dbus_connection_send_with_reply");
    goto Exit;
  }

  if (pendingReturn == NULL) {
    LOGD ("pending return is NULL");
    goto Exit;
  }

  dbus_pending_call_block (pendingReturn);
  if ((reply = dbus_pending_call_steal_reply (pendingReturn)) == NULL) {
    LOGD ("Error in dbus_pending_call_steal_reply");
    goto Exit;
  }

  /* parse reply according to inbit/unhibit method called */
  if (enable) {
    if (dbus_message_iter_init(reply, &replyIter)) {
      if (DBUS_TYPE_UINT32 == dbus_message_iter_get_arg_type(&replyIter)) {
        dbus_message_iter_get_basic(&replyIter, &(data->dbusInbitCookie));
        LOGD("Got inhibit cookie:%d",data->dbusInbitCookie);

      } else {
        LOGD("inhibit return type is not as expected\n");
        goto Exit;
      }
    } else {
      LOGD ("Error in creating reply iterator");
      goto Exit;
    }
    data->isSleepInhibited = True;

  } else {
    /* TODO parse the unhibit command here */
    data->isSleepInhibited = False;
  }

Exit:
  if (NULL != request) {
    dbus_message_unref (request);
  }
  if (NULL != pendingReturn) {
    dbus_pending_call_unref	(pendingReturn);
  }
  LOGD("Returning %s", __func__);
  return;
}

/* This function is called when the GUI toolkit creates the physical window that will hold the video.
 * At this point we can retrieve its handler and pass it to GStreamer through the VideoOverlay interface. */
static void realize_cb (GtkWidget *widget, CustomData *data) {
  GdkWindow *window = gtk_widget_get_window (widget);
  guintptr window_handle;

  if (!gdk_window_ensure_native (window))
    g_error ("Couldn't create native window needed for GstVideoOverlay!");

  /* Retrieve window handler from GDK.Pass it to playbin, which implements VideoOverlay
     and will forward it to the video sink */
  window_handle = GDK_WINDOW_XID (window);
  gst_video_overlay_set_window_handle (GST_VIDEO_OVERLAY (data->playbin), window_handle);
}

/* This function is called when the PLAY button is clicked */
static void play_cb (GtkButton *button, CustomData *data) {
  gst_element_set_state (data->playbin, GST_STATE_PLAYING);
}

/* This function is called when the PAUSE button is clicked */
static void pause_cb (GtkButton *button, CustomData *data) {
  gst_element_set_state (data->playbin, GST_STATE_PAUSED);
}

/* This function is called when the STOP button is clicked */
static void stop_cb (GtkButton *button, CustomData *data) {
  gst_element_set_state (data->playbin, GST_STATE_READY);
}

/* This function is called when the main window is closed */
static void delete_event_cb (GtkWidget *widget, GdkEvent *event, CustomData *data) {
  stop_cb (NULL, data);
  gtk_main_quit ();
}

/* This function is called everytime the video window needs to be redrawn (due to damage/exposure,
 * rescaling, etc). GStreamer takes care of this in the PAUSED and PLAYING states, otherwise,
 * we simply draw a black rectangle to avoid garbage showing up. */
static gboolean draw_cb (GtkWidget *widget, cairo_t *cr, CustomData *data) {
  if (data->state < GST_STATE_PAUSED) {
    GtkAllocation allocation;

    /* Cairo is a 2D graphics library which we use here to clean the video window.
     * It is used by GStreamer for other reasons, so it will always be available to us. */
    gtk_widget_get_allocation (widget, &allocation);
    cairo_set_source_rgb (cr, 0, 0, 0);
    cairo_rectangle (cr, 0, 0, allocation.width, allocation.height);
    cairo_fill (cr);
  }

  return FALSE;
}

/* This function is called when the slider changes its position. We perform a seek to the
 * new position here. */
static void slider_cb (GtkRange *range, CustomData *data) {
  gdouble value = gtk_range_get_value (GTK_RANGE (data->slider));
  gst_element_seek_simple (data->playbin, GST_FORMAT_TIME, GST_SEEK_FLAG_FLUSH | GST_SEEK_FLAG_KEY_UNIT,
      (gint64)(value * GST_SECOND));
}

/* Function to recieve keypress events */
static void keypress_cb (GtkWidget *widget, GdkEventKey *event, CustomData *data) {
  LOGD("Got keypress event");
  gint64 current = -1;
  switch (event->keyval)
  {
    case GDK_KEY_space:
      LOGD ("Space key");
      if (GST_STATE_PLAYING == data->state) {
        gst_element_set_state (data->playbin, GST_STATE_PAUSED);
        LOGD("Pause called; Current sate: %d", GST_STATE(data->playbin));
      } else if (GST_STATE_PAUSED == data->state) {
        gst_element_set_state (data->playbin, GST_STATE_PLAYING);
        LOGD("Play called; Current sate: %d", GST_STATE(data->playbin));
      }
      break;
    case GDK_KEY_Left:
      /* TODO add  proper error handlin and update the seek bar properly, if it is not correct */
      if (gst_element_query_position (data->playbin, GST_FORMAT_TIME, &current)) {
        gst_element_seek_simple (data->playbin, GST_FORMAT_TIME, GST_SEEK_FLAG_FLUSH | GST_SEEK_FLAG_KEY_UNIT,
        (gint64)(current - ((SEEK_INTERVAL) * GST_SECOND)));

        /* Block the "value-changed" signal, so the slider_cb function is not called
         * (which would trigger a seek the user has not requested) */
        g_signal_handler_block (data->slider, data->slider_update_signal_id);
        /* Set the position of the slider to the current pipeline positoin, in SECONDS */
        gtk_range_set_value (GTK_RANGE (data->slider), (gdouble)((current / GST_SECOND) - SEEK_INTERVAL));
        /* Re-enable the signal */
        g_signal_handler_unblock (data->slider, data->slider_update_signal_id);
      } else {
        LOGD("Failed to get duration");
      }
      break;
    case GDK_KEY_Right:
      /* TODO add  proper error handlin and update the seek bar properly, if it is not correct */
      if (gst_element_query_position (data->playbin, GST_FORMAT_TIME, &current)) {
        gst_element_seek_simple (data->playbin, GST_FORMAT_TIME, GST_SEEK_FLAG_FLUSH | GST_SEEK_FLAG_KEY_UNIT,
        (gint64)(current + ((SEEK_INTERVAL) * GST_SECOND)));

        /* Block the "value-changed" signal, so the slider_cb function is not called
         * (which would trigger a seek the user has not requested) */
        g_signal_handler_block (data->slider, data->slider_update_signal_id);
        /* Set the position of the slider to the current pipeline positoin, in SECONDS */
        gtk_range_set_value (GTK_RANGE (data->slider), (gdouble)((current / GST_SECOND) + SEEK_INTERVAL));
        /* Re-enable the signal */
        g_signal_handler_unblock (data->slider, data->slider_update_signal_id);
      } else {
        LOGD("Failed to get duration");
      }
      break;
    case GDK_KEY_Escape:
      gtk_window_unfullscreen(GTK_WINDOW(data->main_window));
      break;
    default:
      break;
  }
}

/*Function recieves click event on video window */
gboolean video_screen_mouse_click_cb (GtkWidget *widget, GdkEventButton *event, CustomData *data) {
  LOGD("Got mouse keyevent %d", event->type);

  switch (event->type)
  {
    case GDK_2BUTTON_PRESS:
      /*TODO: state machine for window size */
      gtk_window_unfullscreen(GTK_WINDOW(data->main_window));
      break;
    default:
      LOGD ("mouse click unhandled");
      break;
  }

  return TRUE;
}

/* This creates all the GTK+ widgets that compose our application, and registers the callbacks */
static void create_ui (CustomData *data) {
  GtkWidget *main_window;  /* The uppermost window, containing all other windows */
  GtkWidget *video_window; /* The drawing area where the video will be shown */
  GtkWidget *main_box;     /* VBox to hold main_hbox and the controls */
  GtkWidget *main_hbox;    /* HBox to hold the video_window and the stream info text widget */
  GtkWidget *controls;     /* HBox to hold the buttons and the slider */
  GtkWidget *play_button, *pause_button, *stop_button; /* Buttons */

  main_window = gtk_window_new (GTK_WINDOW_TOPLEVEL);
  g_signal_connect (G_OBJECT (main_window), "delete-event", G_CALLBACK (delete_event_cb), data);

  video_window = gtk_drawing_area_new ();
  gtk_widget_set_double_buffered (video_window, FALSE);
  g_signal_connect (video_window, "realize", G_CALLBACK (realize_cb), data);
  g_signal_connect (video_window, "draw", G_CALLBACK (draw_cb), data);
  gtk_widget_add_events(video_window, GDK_BUTTON_PRESS_MASK);
  g_signal_connect (video_window, "button-press-event", G_CALLBACK (video_screen_mouse_click_cb), data);

  play_button = gtk_button_new_from_icon_name ("media-playback-start", GTK_ICON_SIZE_SMALL_TOOLBAR);
  g_signal_connect (G_OBJECT (play_button), "clicked", G_CALLBACK (play_cb), data);

  pause_button = gtk_button_new_from_icon_name ("media-playback-pause", GTK_ICON_SIZE_SMALL_TOOLBAR);
  g_signal_connect (G_OBJECT (pause_button), "clicked", G_CALLBACK (pause_cb), data);

  stop_button = gtk_button_new_from_icon_name ("media-playback-stop", GTK_ICON_SIZE_SMALL_TOOLBAR);
  g_signal_connect (G_OBJECT (stop_button), "clicked", G_CALLBACK (stop_cb), data);

  data->slider = gtk_scale_new_with_range (GTK_ORIENTATION_HORIZONTAL, 0, 100, 1);
  gtk_scale_set_draw_value (GTK_SCALE (data->slider), 0);
  data->slider_update_signal_id = g_signal_connect (G_OBJECT (data->slider), "value-changed", G_CALLBACK (slider_cb), data);

  /* Listen for keypress events */
  gtk_widget_add_events(main_window, GDK_KEY_PRESS_MASK);
  g_signal_connect (G_OBJECT (main_window), "key_press_event", G_CALLBACK (keypress_cb), data);
#if 0
  data->streams_list = gtk_text_view_new ();
  gtk_text_view_set_editable (GTK_TEXT_VIEW (data->streams_list), FALSE);
#endif

  controls = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 0);
  gtk_box_pack_start (GTK_BOX (controls), play_button, FALSE, FALSE, 2);
  gtk_box_pack_start (GTK_BOX (controls), pause_button, FALSE, FALSE, 2);
  gtk_box_pack_start (GTK_BOX (controls), stop_button, FALSE, FALSE, 2);
  gtk_box_pack_start (GTK_BOX (controls), data->slider, TRUE, TRUE, 2);

  main_hbox = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 0);
  gtk_box_pack_start (GTK_BOX (main_hbox), video_window, TRUE, TRUE, 0);

  main_box = gtk_box_new (GTK_ORIENTATION_VERTICAL, 0);
  gtk_box_pack_start (GTK_BOX (main_box), main_hbox, TRUE, TRUE, 0);
  gtk_box_pack_start (GTK_BOX (main_box), controls, FALSE, FALSE, 0);
  gtk_container_add (GTK_CONTAINER (main_window), main_box);
  gtk_window_set_default_size (GTK_WINDOW (main_window), 640, 480);

  gtk_window_fullscreen(GTK_WINDOW (main_window));

  gtk_widget_show_all (main_window);
  data->main_window = main_window;
}

/* This function is called periodically to refresh the GUI */
static gboolean refresh_ui (CustomData *data) {
  gint64 current = -1;

  /* We do not want to update anything unless we are in the PAUSED or PLAYING states */
  if (data->state < GST_STATE_PAUSED)
    return TRUE;

  /* If we didn't know it yet, query the stream duration */
  if (!GST_CLOCK_TIME_IS_VALID (data->duration)) {
    if (!gst_element_query_duration (data->playbin, GST_FORMAT_TIME, &data->duration)) {
      LOGD ("Could not query current duration.");
    } else {
      /* Set the range of the slider to the clip duration, in SECONDS */
      gtk_range_set_range (GTK_RANGE (data->slider), 0, (gdouble)data->duration / GST_SECOND);
    }
  }

  if (gst_element_query_position (data->playbin, GST_FORMAT_TIME, &current)) {
    /* Block the "value-changed" signal, so the slider_cb function is not called
     * (which would trigger a seek the user has not requested) */
    g_signal_handler_block (data->slider, data->slider_update_signal_id);
    /* Set the position of the slider to the current pipeline positoin, in SECONDS */
    gtk_range_set_value (GTK_RANGE (data->slider), (gdouble)current / GST_SECOND);
    /* Re-enable the signal */
    g_signal_handler_unblock (data->slider, data->slider_update_signal_id);
  }
  return TRUE;
}

/* This function is called when new metadata is discovered in the stream */
static void tags_cb (GstElement *playbin, gint stream, CustomData *data) {
  /* We are possibly in a GStreamer working thread, so we notify the main
   * thread of this event through a message in the bus */
  gst_element_post_message (playbin,
    gst_message_new_application (GST_OBJECT (playbin),
      gst_structure_new_empty ("tags-changed")));
}

/* This function is called when an error message is posted on the bus */
static void error_cb (GstBus *bus, GstMessage *msg, CustomData *data) {
  GError *err;
  gchar *debug_info;

  /* Print error details on the screen */
  gst_message_parse_error (msg, &err, &debug_info);
  LOGD ("Error received from element %s: %s", GST_OBJECT_NAME (msg->src), err->message);
  LOGD ("Debugging information: %s", debug_info ? debug_info : "none");
  g_clear_error (&err);
  g_free (debug_info);

  /* Set the pipeline to READY (which stops playback) */
  gst_element_set_state (data->playbin, GST_STATE_READY);
}

/* This function is called when an End-Of-Stream message is posted on the bus.
 * We just set the pipeline to READY (which stops playback) */
static void eos_cb (GstBus *bus, GstMessage *msg, CustomData *data) {
  LOGD ("End-Of-Stream reached.");
  gst_element_set_state (data->playbin, GST_STATE_READY);
}

/* This function is called when the pipeline changes states. We use it to
 * keep track of the current state. */
static void state_changed_cb (GstBus *bus, GstMessage *msg, CustomData *data) {
  GstState old_state, new_state, pending_state;
  gst_message_parse_state_changed (msg, &old_state, &new_state, &pending_state);
  if (GST_MESSAGE_SRC (msg) == GST_OBJECT (data->playbin)) {
    data->state = new_state;
    LOGD ("State set to %s", gst_element_state_get_name (new_state));
    if (old_state == GST_STATE_READY && new_state == GST_STATE_PAUSED) {
      /* For extra responsiveness, we refresh the GUI as soon as we reach the PAUSED state */
      refresh_ui (data);
    }

    if (data->isSleepInhibited && GST_STATE_PAUSED == new_state) {
      /* Let him sleep */
      inhibit_cpu_sleep (data, False);

    } else if (!(data->isSleepInhibited) && GST_STATE_PLAYING == new_state) {
      /* Inhibit the sleep here */
      inhibit_cpu_sleep (data, True);

    }
  }
}

/* Extract metadata from all the streams and write it to the text widget in the GUI */
static void analyze_streams (CustomData *data) {
#if 0
  gint i;
  GstTagList *tags;
  gchar *str, *total_str;
  guint rate;
  gint n_video, n_audio, n_text;
  GtkTextBuffer *text;

  /* Clean current contents of the widget */
  text = gtk_text_view_get_buffer (GTK_TEXT_VIEW (data->streams_list));
  gtk_text_buffer_set_text (text, "", -1);

  /* Read some properties */
  g_object_get (data->playbin, "n-video", &n_video, NULL);
  g_object_get (data->playbin, "n-audio", &n_audio, NULL);
  g_object_get (data->playbin, "n-text", &n_text, NULL);

  for (i = 0; i < n_video; i++) {
    tags = NULL;
    /* Retrieve the stream's video tags */
    g_signal_emit_by_name (data->playbin, "get-video-tags", i, &tags);
    if (tags) {
      total_str = g_strdup_printf ("video stream %d:\n", i);
      gtk_text_buffer_insert_at_cursor (text, total_str, -1);
      g_free (total_str);
      gst_tag_list_get_string (tags, GST_TAG_VIDEO_CODEC, &str);
      total_str = g_strdup_printf ("  codec: %s\n", str ? str : "unknown");
      gtk_text_buffer_insert_at_cursor (text, total_str, -1);
      g_free (total_str);
      g_free (str);
      gst_tag_list_free (tags);
    }
  }

  for (i = 0; i < n_audio; i++) {
    tags = NULL;
    /* Retrieve the stream's audio tags */
    g_signal_emit_by_name (data->playbin, "get-audio-tags", i, &tags);
    if (tags) {
      total_str = g_strdup_printf ("\naudio stream %d:\n", i);
      gtk_text_buffer_insert_at_cursor (text, total_str, -1);
      g_free (total_str);
      if (gst_tag_list_get_string (tags, GST_TAG_AUDIO_CODEC, &str)) {
        total_str = g_strdup_printf ("  codec: %s\n", str);
        gtk_text_buffer_insert_at_cursor (text, total_str, -1);
        g_free (total_str);
        g_free (str);
      }
      if (gst_tag_list_get_string (tags, GST_TAG_LANGUAGE_CODE, &str)) {
        total_str = g_strdup_printf ("  language: %s\n", str);
        gtk_text_buffer_insert_at_cursor (text, total_str, -1);
        g_free (total_str);
        g_free (str);
      }
      if (gst_tag_list_get_uint (tags, GST_TAG_BITRATE, &rate)) {
        total_str = g_strdup_printf ("  bitrate: %d\n", rate);
        gtk_text_buffer_insert_at_cursor (text, total_str, -1);
        g_free (total_str);
      }
      gst_tag_list_free (tags);
    }
  }

  for (i = 0; i < n_text; i++) {
    tags = NULL;
    /* Retrieve the stream's subtitle tags */
    g_signal_emit_by_name (data->playbin, "get-text-tags", i, &tags);
    if (tags) {
      total_str = g_strdup_printf ("\nsubtitle stream %d:\n", i);
      gtk_text_buffer_insert_at_cursor (text, total_str, -1);
      g_free (total_str);
      if (gst_tag_list_get_string (tags, GST_TAG_LANGUAGE_CODE, &str)) {
        total_str = g_strdup_printf ("  language: %s\n", str);
        gtk_text_buffer_insert_at_cursor (text, total_str, -1);
        g_free (total_str);
        g_free (str);
      }
      gst_tag_list_free (tags);
    }
  }
#endif
}

/* This function is called when an "application" message is posted on the bus.
 * Here we retrieve the message posted by the tags_cb callback */
static void application_cb (GstBus *bus, GstMessage *msg, CustomData *data) {
  if (g_strcmp0 (gst_structure_get_name (gst_message_get_structure (msg)), "tags-changed") == 0) {
    /* If the message is the "tags-changed" (only one we are currently issuing), update
     * the stream info GUI */
    analyze_streams (data);
  }
}

/* Callback function which will be invoked when a new element is added
   to playbin */
static void element_setup_cb (GstElement *playbin, GstElement *element,  CustomData *data) {

  char *elemName = gst_element_get_name(element);
  char *renderer = "renderer";

  /* renderer is used for overlaying subtitle over text. Add property to add backround to subtitle */
  if (!strncmp(elemName, renderer, 8)) {
    LOGD("Setting property of text overlay renderer");
    g_object_set(G_OBJECT(element), "shaded-background", True, NULL);
  }

}

/* function to print dbus error */
void print_dbus_error (char *str, DBusError error)
{
  LOGD ("%s: %s", str, error.message);
  dbus_error_free (&error);
}

/* This function will get the video uri from the uri passed by the user
   For instance, if the uri is youtube link, this will get the vide uri
   from youtube link, so that gstreamer can play the file */
void get_video_uri(char *inputUri, char **opVideoUri) {
  LOGD("Entered with i/p:%p o/p:%p", inputUri, opVideoUri);
  char *videoUri = (char *)malloc (4096 * sizeof(char));
  int ipUriSize = strlen(inputUri);

  /* check whether the uri is a youtube link */
  if (NULL != strstr(inputUri, "https")) {
    if (NULL != strstr(inputUri, "youtube") || NULL != strstr(inputUri, "youtu.be")) {
      LOGD("That's possibly a youtube link");
      char command[4500] = "youtube-dl --format best[ext=mp4] --get-url ";
      char tmpCmdTail[100] = " > /tmp/youtube_video_uri.txt";
      int cmdSize = strlen (command);
      FILE *tmpVideoUrifile;
      memcpy(&command[cmdSize], inputUri, strlen(inputUri));
      cmdSize = cmdSize + ipUriSize;
      memcpy(&command[cmdSize], tmpCmdTail, 100);
      cmdSize += 100;
      system(command);

      /* the video url will be written to /tmp/youtube_video_uri.txt. Read from there */
      tmpVideoUrifile = fopen("/tmp/youtube_video_uri.txt", "r");
      if (NULL == tmpVideoUrifile) {
        LOGD(" Couldn't open file");
      }
      int size = fread(videoUri, 1024, 4, tmpVideoUrifile);
      *opVideoUri = videoUri;
    } else {

    }
  } else {
    /* Assume that the passed uri is playable by playbin */
    *opVideoUri = inputUri;
  }
  LOGD("Exiting");
}

int main(int argc, char *argv[]) {
  CustomData data;
  GstStateChangeReturn ret;
  GstBus *bus;
  DBusError dbusError;
  char *fileUri = NULL;

  /* Initialize GTK */
  gtk_init (&argc, &argv);

  /* Initialize GStreamer */
  gst_init (&argc, &argv);
  LOGD("Going to play:%s", argv[1]);

  /* Initialize our data structure */
  memset (&data, 0, sizeof (data));
  data.duration = GST_CLOCK_TIME_NONE;

  /* Create the elements */
  data.playbin = gst_element_factory_make ("playbin", "playbin");

  if (!data.playbin) {
    LOGD ("Not all elements could be created.");
    return -1;
  }

  get_video_uri(argv[1], &fileUri);
  LOGD("Got Video URI: %s", fileUri);
  /* Set the URI to play */
  g_object_set (data.playbin, "uri", fileUri, NULL);

  /* Connect to interesting signals in playbin */
  g_signal_connect (G_OBJECT (data.playbin), "video-tags-changed", (GCallback) tags_cb, &data);
  g_signal_connect (G_OBJECT (data.playbin), "audio-tags-changed", (GCallback) tags_cb, &data);
  g_signal_connect (G_OBJECT (data.playbin), "text-tags-changed", (GCallback) tags_cb, &data);
  g_signal_connect (G_OBJECT (data.playbin), "element-setup", (GCallback) element_setup_cb, &data);

  /* Create the GUI */
  create_ui (&data);

  /* initiate a dbus connection to session bus */
  data.sessionBus = dbus_bus_get (DBUS_BUS_SESSION, &dbusError);
  if (dbus_error_is_set (&dbusError) || !(data.sessionBus)) {
    print_dbus_error("error in Dbus connection", dbusError);
  } else {
    inhibit_cpu_sleep(&data, True);
  }


  /* Instruct the bus to emit signals for each received message, and connect to the interesting signals */
  bus = gst_element_get_bus (data.playbin);
  gst_bus_add_signal_watch (bus);
  g_signal_connect (G_OBJECT (bus), "message::error", (GCallback)error_cb, &data);
  g_signal_connect (G_OBJECT (bus), "message::eos", (GCallback)eos_cb, &data);
  g_signal_connect (G_OBJECT (bus), "message::state-changed", (GCallback)state_changed_cb, &data);
  g_signal_connect (G_OBJECT (bus), "message::application", (GCallback)application_cb, &data);
  gst_object_unref (bus);

  /* Start playing */
  ret = gst_element_set_state (data.playbin, GST_STATE_PLAYING);
  if (ret == GST_STATE_CHANGE_FAILURE) {
    LOGD ("Unable to set the pipeline to the playing state.");
    gst_object_unref (data.playbin);
    return -1;
  }

  /* Register a function that GLib will call every second */
  g_timeout_add_seconds (1, (GSourceFunc)refresh_ui, &data);

  /* Start the GTK main loop. We will not regain control until gtk_main_quit is called. */
  gtk_main ();

  /* remove dbus coneciton to session bus */
  dbus_connection_flush (data.sessionBus);

  /* Free resources */
  gst_element_set_state (data.playbin, GST_STATE_NULL);
  gst_object_unref (data.playbin);
  return 0;
}
