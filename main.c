#include <gst/gst.h>
#include <gst/sdp/sdp.h>

#define GST_USE_UNSTABLE_API
#include <gst/webrtc/webrtc.h>

#include <libsoup/soup.h>
#include <json-glib/json-glib.h>

#include <string.h>

static GMainLoop *loop;
static GstElement *pipe1, *webrtc1;

static SoupWebsocketConnection *ws_conn = NULL;
static const gchar *peer_id = NULL;
static const gchar *server_url = "wss://webrtc.nirbheek.in:8443";

static GOptionEntry entries[] =
{
        { "peer-id", 0, 0, G_OPTION_ARG_STRING, &peer_id, "String ID of the peer to connect to", "ID" },
        { "server", 0, 0, G_OPTION_ARG_STRING, &server_url, "Signalling server to connect to", "URL" },
        { NULL },
};

static gchar* get_string_from_json_object (JsonObject* object) {
        JsonNode *root;
        JsonGenerator *generator;
        gchar *text;

        /* Make it the root node */
        root = json_node_init_object (json_node_alloc (), object);
        generator = json_generator_new ();
        json_generator_set_root (generator, root);
        text = json_generator_to_data (generator, NULL);

        g_object_unref (generator);
        json_node_free (root);
        return text;
}

static void send_ice_candidate_message (GstElement* webrtc G_GNUC_UNUSED, guint mlineindex,
                                        gchar* candidate, gpointer user_data G_GNUC_UNUSED) {
        gchar *text;
        JsonObject *ice, *msg;

        ice = json_object_new ();
        json_object_set_string_member (ice, "candidate", candidate);
        json_object_set_int_member (ice, "sdpMLineIndex", mlineindex);
        msg = json_object_new ();
        json_object_set_object_member (msg, "ice", ice);
        text = get_string_from_json_object (msg);
        json_object_unref (msg);

        soup_websocket_connection_send_text (ws_conn, text);
        g_free (text);
}

static void send_sdp_offer (GstWebRTCSessionDescription* offer) {
        gchar *text;
        JsonObject *msg, *sdp;

        text = get_sdp_message_as_text (offer->sdp);
        g_print ("Sending offer:\n%s\n", text);

        sdp = json_object_new ();
        json_object_set_string_member (sdp, "type", "offer");
        json_object_set_string_member (sdp, "sdp", text);
        g_free (text);

        msg = json_object_new ();
        json_object_set_object_member (msg, "sdp", sdp);
        text = get_string_from_json_object (msg);
        json_object_unref (msg);

        soup_websocket_connection_send_text (ws_conn, text);
        g_free (text);

}

/* Offer created by our pipeline, to be sent to the peer */
static void on_offer_created (GstPromise* promise, gpointer user_data) {
        GstWebRTCSessionDescription *offer = NULL;
        const GstStructure *reply;

        reply = gst_promise_get_reply (promise);
        gst_structure_get (reply, "offer", GST_TYPE_WEBRTC_SESSION_DESCRIPTION, &offer, NULL);
        gst_promise_unref (promise);

        promise = gst_promise_new ();
        g_signal_emit_by_name (webrtc1, "set-local-description", offer, promise);
        gst_promise_interrupt (promise);
        gst_promise_unref (promise);

        /* Send offer to peer */
        send_sdp_offer (offer);
        gst_webrtc_session_description_free (offer);
}

static void on_negotiation_needed (GstElement* element, gpointer user_data) {
        GstPromise *promise;

        promise = gst_promise_new_with_change_func (on_offer_created, user_data, NULL);
        g_signal_emit_by_name (webrtc1, "create-offer", NULL, promise);
}

#define STUN_SERVER " stun-server=stun://stun.1.google.com:19302"
#define RTP_CAPS_OPUS "application/x-rtp,media=audio,encoding-name=OPUS,payload="
#define RTP_CAPS_VP8 "application/x-rtp,media=video,encoding-name=VP8,payload="


static gboolean start_pipeline (void) {
        GstStateChangeReturn ret;
        GError *error = NULL;

        pipe1 = gst_parse_launch ("webrtcbin name=sendrecv " STUN_SERVER
                                  "videotestsrc pattern=ball ! queue ! vp8enc deadline=1 ! rtpvp8pay ! "
                                  "queue ! " RTP_CAPS_VP8 "96 ! sendrecv. "
                                  "audiotestsrc wave=red-noise ! queue ! opusenc ! rtpopuspay ! "
                                  "queue ! " RTP_CAPS_OPUS "97 ! sendrecv. ",
                                  &error);

        if error {
                        g_printerr ("Failed to parse launch: %s\n", error->message);
                        g_error_free (error);
                        goto err;
                }

        webrtc1 = gst_bin_get_by_name (GST_BIN (pipe1), "sendrecv");
        g_assert_nonnull (webrtc1);

        /* This is the gstwebrtc entry point where we create the offer and so on. It will be called when the
           pipeline goes to PLAYING. */
        g_signal_connect (webrtc1, "on-negotiation-needed",
                          G_CALLBACK (on_negotiation_needed), NULL);
        /* We need to transmit this ICE candidate to the browser via the websockets signalling server.
           Incoming ice candidates from the browser need to be added by us too, see on_server_message() */
        g_signal_connect (webrtc1, "on-ice-candidate",
                          G_CALLBACK (send_ice_candidate_message), NULL);
        /* Lifetime is the same as the pipeline itself */
        gst_object_unref (webrtc1);

        g_print ("Starting pipeline...\n");
        ret = gst_element_set_state (GST_ELEMENT (pipe1), GST_STATE_PLAYING);
        if (ret == GST_STATE_CHANGE_FAILURE)
                goto err;

        return true;

err:
        if (pipe1)
                g_clear_object (&pipe1);
        if (webrtc1)
                webrtc1 = NULL;
        return FALSE;
}

static gboolean check_plugins (void) {
        int i;
        gboolean ret;
        GstPlugin *plugin;
        GstRegistry *registry;
        const gchar *needed[] = { "opus", "vpx", "nice", "webrtc", "dtls", "srtp", "rtpmanager",
                                  "videotestsrc", "audiotestsrc", NULL };

        registry = gst_registry_get ();
        ret = TRUE;
        for (i = 0; i < g_strv_length ((gchar **) needed); i++) {
                plugin = gst_registry_find_plugin (registry, needed[i]);
                if (!plugin) {
                        g_print ("Required gstreamer plugin '%s' not found\n", needed[i]);
                        ret = FALSE;
                        continue;
                }
                gst_object_unref (plugin);
        }
        return ret;
}

int main (int argc, char *argv[]) {
        GOptionContext *context;
        GError *error = NULL;

        context = g_option_context_new ("- gstreamer webrtcbin test");
        g_option_context_add_main_entries (context, entries, NULL);
        g_option_context_add_group (context, gst_init_get_option_group ());
        if (!g_option_context_parse (context, &argc, &argv, &error)) {
                g_printerr ("Error initializing: %s\n", error->message);
                return -1;
        }

        if (!check_plugins ()) {
                return -1;
        }

        if (!peer_id) {
                g_printerr ("--peer-id is a required argument\n");
                return - 1;
        }

        g_print("Testing gst webrtcbin plugin\n");

        loop = g_main_loop_new (NULL, FALSE);

        connect_to_websocket_server_async ();

        g_main_loop_run (loop);

        gst_element_set_state (GST_ELEMENT (pipe1), GST_STATE_NULL);
        g_print ("Pipeline stopped\n");

        gst_object_unref (pipe1);

        return 0;

}
