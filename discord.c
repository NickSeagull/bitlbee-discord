/*
 * Copyright 2015 Artem Savkov <artem.savkov@gmail.com>
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */
#include <stdio.h>
#include <time.h>
#include <libwebsockets.h>
#include <bitlbee/bitlbee.h>
#include <bitlbee/http_client.h>
#include <bitlbee/json.h>
#include <bitlbee/json_util.h>

#define DISCORD_HOST "discordapp.com"
#define DEFAULT_KA_INTERVAL 30

typedef enum {
  WS_IDLE,
  WS_CONNECTING,
  WS_CONNECTED,
  WS_READY
} ws_state;

typedef struct _discord_data {
  char     *token;
  char     *id;
  char     *uname;
  char     *gateway;
  struct libwebsocket_context *lwsctx;
  struct libwebsocket *lws;
  GSList   *servers;
  GSList   *channels;
  gint     main_loop_id;
  GString  *ws_buf;
  ws_state state;
  guint32  ka_interval;
  time_t   ka_timestamp;
} discord_data;

typedef struct _server_info {
  char                 *name;
  char                 *id;
  GSList               *users;
  struct im_connection *ic;
} server_info;

typedef struct _channel_info {
  char                 *id;
  guint64              last_msg;
  union {
    struct {
      struct groupchat     *gc;
      server_info          *sinfo;
    } channel;
    struct {
      char                 *handle;
      struct im_connection *ic;
    } user;
  } to;
  gboolean             is_private;
} channel_info;

typedef struct _user_info {
  char                 *id;
  bee_user_t           *user;
} user_info;

typedef struct _cadd {
  server_info *sinfo;
  char *name;
  char *id;
  char *last_msg;
  char *topic;
} cadd;

static void discord_http_get(struct im_connection *ic, const char *api_path,
                             http_input_function cb_func, gpointer data);

static void free_user_info(user_info *uinfo) {
  g_free(uinfo->id);

  g_free(uinfo);
}

static void free_channel_info(channel_info *cinfo) {
  g_free(cinfo->id);
  cinfo->id = NULL;

  if (cinfo->is_private) {
    g_free(cinfo->to.user.handle);
  } else {
    imcb_chat_free(cinfo->to.channel.gc);
  }

  g_free(cinfo);
}

static void free_server_info(server_info *sinfo) {
  g_free(sinfo->name);
  g_free(sinfo->id);

  g_slist_free_full(sinfo->users, (GDestroyNotify)free_user_info);

  g_free(sinfo);
}

static void discord_logout(struct im_connection *ic) {
  discord_data *dd = ic->proto_data;

  b_event_remove(dd->main_loop_id);

  g_slist_free_full(dd->channels, (GDestroyNotify)free_channel_info);
  g_slist_free_full(dd->servers, (GDestroyNotify)free_server_info);

  g_free(dd->gateway);
  g_free(dd->token);
  g_free(dd->uname);
  g_free(dd->id);

  g_free(dd);
}

static void discord_dump_http_reply(struct http_request *req) {
  g_print("============================\nstatus=%d\n", req->status_code);
  g_print("\nrh=%s\nrb=%s\n", req->reply_headers, req->reply_body);
}

static void discord_send_msg_cb(struct http_request *req) {
  struct im_connection *ic = req->data;
  if (req->status_code != 200) {
    imcb_error(ic, "Failed to send message (%d).", req->status_code);
  }
}

static int lws_send_payload(struct libwebsocket *wsi, const char *pload,
			    size_t psize) {
  int ret = 0;
  unsigned char *buf = g_malloc0(LWS_SEND_BUFFER_PRE_PADDING + \
				 psize + LWS_SEND_BUFFER_POST_PADDING);
  strncpy((char*)&buf[LWS_SEND_BUFFER_PRE_PADDING], pload, psize);
  g_print(">>> %s\n", pload);
  ret = libwebsocket_write(wsi, &buf[LWS_SEND_BUFFER_PRE_PADDING], psize,
			  LWS_WRITE_TEXT);
  g_free(buf);
  return ret;
}

static gint cmp_chan_id(const channel_info *cinfo, const char *chan_id) {
  return g_strcmp0(cinfo->id, chan_id);
}

static void lws_send_keepalive(discord_data *dd) {
  time_t ctime = time(NULL);
  if (ctime - dd->ka_timestamp > dd->ka_interval) {
    GString *buf = g_string_new("");

    g_string_printf(buf, "{\"op\":1,\"d\":%tu}", ctime);
    lws_send_payload(dd->lws, buf->str, buf->len);
    g_string_free(buf, TRUE);
    dd->ka_timestamp = ctime;
  }
}

static gboolean lws_service_loop(gpointer data, gint fd,
                                 b_input_condition cond) {
  struct im_connection *ic = data;
  discord_data *dd = ic->proto_data;

  libwebsocket_service(dd->lwsctx, 0);
  if (dd->state == WS_READY) {
    lws_send_keepalive(dd);
  }
  return TRUE;
}

static void discord_add_channel(cadd *ca) {
  struct im_connection *ic = ca->sinfo->ic;
  discord_data *dd = ic->proto_data;

  char *title;
  GSList *l;

  title = g_strdup_printf("%s/%s", ca->sinfo->name,
                          ca->name);
  struct groupchat *gc = imcb_chat_new(ic, title);
  imcb_chat_name_hint(gc, ca->name);
  if (ca->topic != NULL) {
    imcb_chat_topic(gc, "root", ca->topic, 0);
  }
  g_free(title);

  for (l = ca->sinfo->users; l; l = l->next) {
    user_info *uinfo = l->data;
    if (uinfo->user->ic == ic &&
        g_strcmp0(uinfo->user->handle, dd->uname) != 0) {
      imcb_chat_add_buddy(gc, uinfo->user->handle);
    }
  }

  imcb_chat_add_buddy(gc, dd->uname);

  channel_info *ci = g_new0(channel_info, 1);
  ci->is_private = FALSE;
  ci->to.channel.gc = gc;
  ci->to.channel.sinfo = ca->sinfo;
  ci->id = g_strdup(ca->id);
  if (ca->last_msg != NULL) {
    ci->last_msg = g_ascii_strtoull(ca->last_msg, NULL, 10);
  }

  gc->data = ci;

  dd->channels = g_slist_prepend(dd->channels, ci);
}

static void parse_message(struct im_connection *ic) {
  discord_data *dd = ic->proto_data;
  json_value *js = json_parse(dd->ws_buf->str, dd->ws_buf->len);
  if (!js || js->type != json_object) {
    imcb_error(ic, "Failed to parse json reply.");
    imc_logout(ic, TRUE);
    goto exit;
  }

  const char *event = json_o_str(js, "t");
  if (g_strcmp0(event, "READY") == 0) {
    dd->state = WS_READY;
    json_value *data = json_o_get(js, "d");

    if (data == NULL || data->type != json_object) {
      goto exit;
    }

    json_value *hbeat = json_o_get(data, "heartbeat_interval");
    if (hbeat != NULL && hbeat->type == json_integer) {
      dd->ka_interval = hbeat->u.integer / 1000;
      if (dd->ka_interval == 0) {
	dd->ka_interval = DEFAULT_KA_INTERVAL;
      }
      g_print("Updated ka_interval to %u\n", dd->ka_interval);
    }

    json_value *user = json_o_get(data, "user");
    if (user != NULL && user->type == json_object) {
      g_print("uinfo: name=%s; id=%s;\n", json_o_str(user, "username"), json_o_strdup(user, "id"));
      dd->id = json_o_strdup(user, "id");
      dd->uname = json_o_strdup(user, "username");
    }

    json_value *guilds = json_o_get(data, "guilds");
    if (guilds != NULL && guilds->type == json_array) {
      for (int gidx = 0; gidx < guilds->u.array.length; gidx++) {
	if (guilds->u.array.values[gidx]->type == json_object) {
	  json_value *ginfo = guilds->u.array.values[gidx];
	  g_print("ginfo: name=%s; id=%s;\n", json_o_str(ginfo, "name"), json_o_strdup(ginfo, "id"));

	  server_info *sinfo = g_new0(server_info, 1);

	  sinfo->name = json_o_strdup(ginfo, "name");
	  sinfo->id = json_o_strdup(ginfo, "id");
	  sinfo->ic = ic;

	  json_value *members = json_o_get(ginfo, "members");
	  if (members != NULL && members->type == json_array) {
	    for (int midx = 0; midx < members->u.array.length; midx++) {
	      json_value *uinfo = json_o_get(members->u.array.values[midx], "user");

	      g_print("uinfo: name=%s; id=%s;\n", json_o_str(uinfo, "username"), json_o_strdup(uinfo, "id"));
	      const char *name = json_o_str(uinfo, "username");

	      if (name && !bee_user_by_handle(ic->bee, ic, name)) {
		user_info *ui = g_new0(user_info, 1);

		imcb_add_buddy(ic, name, NULL);

		ui->user = bee_user_by_handle(ic->bee, ic, name);
		ui->id = json_o_strdup(uinfo, "id");

		sinfo->users = g_slist_prepend(sinfo->users, ui);
	      }
	    }
	  }

	  json_value *channels = json_o_get(ginfo, "channels");
	  if (channels != NULL && channels->type == json_array) {
	    for (int cidx = 0; cidx < channels->u.array.length; cidx++) {
	      json_value *cinfo = channels->u.array.values[cidx];

	      g_print("cinfo: name=%s; topic=%s; id=%s;\n", json_o_str(cinfo, "name"), json_o_str(cinfo, "topic"), json_o_strdup(cinfo, "id"));
	      if (g_strcmp0(json_o_str(cinfo, "type"), "text") == 0) {
		cadd *ca = g_new0(cadd, 1);
		ca->sinfo = sinfo;
		ca->topic = json_o_strdup(cinfo, "topic");
		ca->id = json_o_strdup(cinfo, "id");
		ca->name = json_o_strdup(cinfo, "name");
		ca->last_msg = json_o_strdup(cinfo, "last_message_id");

		// TODO: Check access
		discord_add_channel(ca);
	      }
	    }
	  }

	  dd->servers = g_slist_prepend(dd->servers, sinfo);
	}
      }
    }

    json_value *pcs = json_o_get(data, "private_channels");
    if (pcs != NULL && pcs->type == json_array) {
      for (int pcidx = 0; pcidx < pcs->u.array.length; pcidx++) {
	if (pcs->u.array.values[pcidx]->type == json_object) {
	  json_value *pcinfo = pcs->u.array.values[pcidx];
	  g_print("pcinfo: name=%s; id=%s;\n", json_o_str(json_o_get(pcinfo, "recipient"), "username"), json_o_strdup(pcinfo, "id"));

	  char *lmsg = (char *)json_o_str(pcinfo, "last_message_id");

	  channel_info *ci = g_new0(channel_info, 1);
	  ci->is_private = TRUE;
	  if (lmsg != NULL) {
	    ci->last_msg = g_ascii_strtoull(lmsg, NULL, 10);
	  }
	  ci->to.user.handle = json_o_strdup(json_o_get(pcinfo, "recipient"),
					     "username");
	  ci->id = json_o_strdup(pcinfo, "id");
	  ci->to.user.ic = ic;

	  dd->channels = g_slist_prepend(dd->channels, ci);
	}
      }
    }

    imcb_connected(ic);
  } else if (g_strcmp0(event, "PRESENCE_UPDATE") == 0) {
    // TODO: We don't care about this right now but we should
  } else if (g_strcmp0(event, "MESSAGE_CREATE") == 0) {
    json_value *minfo = json_o_get(js, "d");

    if (minfo == NULL || minfo->type != json_object) {
      goto exit;
    }

    guint64 msgid = g_ascii_strtoull(json_o_str(minfo, "id"), NULL, 10);
    GSList *cl = g_slist_find_custom(dd->channels,
				     json_o_str(minfo, "channel_id"),
				     (GCompareFunc)cmp_chan_id);
    if (cl == NULL) {
      goto exit;
    }
    channel_info *cinfo = cl->data;

    if (msgid > cinfo->last_msg) {
      if (cinfo->is_private) {
	if (!g_strcmp0(json_o_str(json_o_get(minfo, "author"), "username"),
		       cinfo->to.user.handle)) {
	  imcb_buddy_msg(cinfo->to.user.ic,
			 cinfo->to.user.handle,
			 (char *)json_o_str(minfo, "content"), 0, 0);
	}
      } else {
	struct groupchat *gc = cinfo->to.channel.gc;
	imcb_chat_msg(gc, json_o_str(json_o_get(minfo, "author"), "username"),
		      (char *)json_o_str(minfo, "content"), 0, 0);
      }
      cinfo->last_msg = msgid;
    }
  } else {
    g_print("%s: unhandled event: %s\n", __func__, event);
    g_print("%s\n", dd->ws_buf->str);
  }

exit:
  json_value_free(js);
  return;
}

static int
discord_lws_http_only_cb(struct libwebsocket_context *this,
                         struct libwebsocket *wsi,
                         enum libwebsocket_callback_reasons reason,
                         void *user, void *in, size_t len) {
  struct im_connection *ic = libwebsocket_context_user(this);
  discord_data *dd = ic->proto_data;
  switch(reason) {
    case LWS_CALLBACK_CLIENT_ESTABLISHED:
      g_print("%s: client established:\n", __func__);
      dd->state = WS_CONNECTED;
      libwebsocket_callback_on_writable(this, wsi);
      break;
    case LWS_CALLBACK_CLIENT_CONNECTION_ERROR:
      g_print("%s: client connection error\n", __func__);
      imc_logout(ic, FALSE);
      break;
    case LWS_CALLBACK_CLIENT_WRITEABLE:
      g_print("%s: client writable\n", __func__);
      GString *buf = g_string_new("");

      g_string_printf(buf, "{\"d\":{\"v\":3,\"token\":\"%s\",\"properties\":{\"$referring_domain\":\"\",\"$browser\":\"bitlbee-discord\",\"$device\":\"bitlbee\",\"$referrer\":\"\",\"$os\":\"linux\"}},\"op\":2}", dd->token);
      lws_send_payload(wsi, buf->str, buf->len);
      g_string_free(buf, TRUE);
      break;
    case LWS_CALLBACK_CLIENT_RECEIVE:
      {
	size_t rpload = libwebsockets_remaining_packet_payload(wsi);
	g_print("%s: client receive: %p %zd [%zd]\n", __func__, dd->ws_buf, len, rpload);
	if (dd->ws_buf == NULL) {
	  dd->ws_buf = g_string_new("");
	}
	dd->ws_buf = g_string_append(dd->ws_buf, in);
	if (rpload == 0) {
	  //g_print("<<< %s\n", dd->ws_buf->str);
	  parse_message(ic);
	  g_string_free(dd->ws_buf, TRUE);
	  dd->ws_buf = NULL;
	}
	break;
      }
    case LWS_CALLBACK_CLIENT_CONFIRM_EXTENSION_SUPPORTED:
      g_print("%s: client extension:\n%s\n", __func__, (char*)in);
      break;
    case LWS_CALLBACK_CLOSED:
      g_print("%s: closed\n", __func__);
      imc_logout(ic, TRUE);
      break;
    case LWS_CALLBACK_ADD_POLL_FD:
      {
	struct libwebsocket_pollargs *pargs = in;
	g_print("%s: lws add loop: %d 0x%x\n", __func__, pargs->fd, pargs->events);
	dd->main_loop_id = b_input_add(pargs->fd, B_EV_IO_READ | B_EV_IO_WRITE,
				       lws_service_loop, ic);
	break;
      }
    case LWS_CALLBACK_DEL_POLL_FD:
      g_print("%s: lws remove loop: %d\n", __func__, dd->main_loop_id);
      b_event_remove(dd->main_loop_id);
      break;
    case LWS_CALLBACK_CHANGE_MODE_POLL_FD:
    case LWS_CALLBACK_GET_THREAD_ID:
    case LWS_CALLBACK_LOCK_POLL:
    case LWS_CALLBACK_UNLOCK_POLL:
      break;
    default:
      g_print("%s: unknown rsn=%d\n", __func__, reason);
      break;
  }
  return 0;
}

static struct libwebsocket_protocols protocols[] = {
	{ "http-only,chat", discord_lws_http_only_cb, 0, 0 },
	{ NULL, NULL, 0, 0 } /* end */
};

static void discord_gateway_cb(struct http_request *req) {
  struct im_connection *ic = req->data;

  //discord_dump_http_reply(req);

  if (req->status_code == 200) {
    json_value *js = json_parse(req->reply_body, req->body_size);
    if (!js || js->type != json_object) {
      imcb_error(ic, "Failed to parse json reply.");
      imc_logout(ic, TRUE);
      json_value_free(js);
      return;
    }
    discord_data *dd = ic->proto_data;

    const char *gw = json_o_str(js, "url");
    char *tmp;
    if ((tmp = g_strstr_len(gw, MIN(strlen(gw), 6), "://"))) {
      dd->gateway = g_strdup(tmp + 3);
    } else {
      dd->gateway = g_strdup(gw);
    }

    g_print("%s: gateway=%s\n", __func__, dd->gateway);
    struct lws_context_creation_info info;
    memset(&info, 0, sizeof(info));
    info.port = CONTEXT_PORT_NO_LISTEN;
    info.protocols = protocols;
    info.extensions = NULL;
#ifndef LWS_NO_EXTENSIONS
    info.extensions = libwebsocket_get_internal_extensions();
#endif
    info.gid = -1;
    info.uid = -1;
    info.user = ic;

    lws_set_log_level(255, NULL);

    dd->lwsctx = libwebsocket_create_context(&info);
    if (dd->lwsctx == NULL) {
      imcb_error(ic, "Failed to create websockets context.");
      imc_logout(ic, TRUE);
      json_value_free(js);
      return;
    }

    dd->lws = libwebsocket_client_connect(dd->lwsctx, dd->gateway,
                      443, 2, "/", dd->gateway,
                      "discordapp.com",
                      protocols[0].name, -1);

    dd->state = WS_CONNECTING;

    json_value_free(js);
  } else {
    imcb_error(ic, "Failed to get info about self.");
    imc_logout(ic, TRUE);
  }
}

static void discord_login_cb(struct http_request *req) {
  struct im_connection *ic = req->data;

  if (req->status_code == 200) {
    json_value *js = json_parse(req->reply_body, req->body_size);
    if (!js || js->type != json_object) {
      imcb_error(ic, "Failed to parse json reply.");
      imc_logout(ic, TRUE);
      json_value_free(js);
      return;
    }
    if (req->status_code == 200) {
      discord_data *dd = ic->proto_data;
      dd->token = json_o_strdup(js, "token");

      discord_http_get(ic, "gateway", discord_gateway_cb, ic);
    } else {
      JSON_O_FOREACH(js, k, v){
        if (v->type != json_array) {
          continue;
        }

        int i;
        GString *err = g_string_new("");
        g_string_printf(err, "%s:", k);
        for (i = 0; i < v->u.array.length; i++) {
          if (v->u.array.values[i]->type == json_string) {
            g_string_append_printf(err, " %s",
                                   v->u.array.values[i]->u.string.ptr);
          }
        }
        imcb_error(ic, err->str);
        g_string_free(err, TRUE);
      }
    }
    json_value_free(js);
  } else {
    imcb_error(ic, "Failed to login: %d.", req->status_code);
    imc_logout(ic, TRUE);
  }
}

static void discord_login(account_t *acc) {
  struct im_connection *ic = imcb_new(acc);
  GString *request = g_string_new("");
  GString *jlogin = g_string_new("");


  discord_data *dd = g_new0(discord_data, 1);
  dd->ka_interval = DEFAULT_KA_INTERVAL;
  ic->proto_data = dd;

  g_string_printf(jlogin, "{\"email\":\"%s\",\"password\":\"%s\"}",
                  acc->user,
                  acc->pass);

  g_string_printf(request, "POST /api/auth/login HTTP/1.1\r\n"
                  "Host: %s\r\n"
                  "User-Agent: Bitlbee-Discord\r\n"
                  "Content-Type: application/json\r\n"
                  "Content-Length: %zd\r\n\r\n"
                  "%s",
                  set_getstr(&ic->acc->set, "host"),
                  jlogin->len,
                  jlogin->str);

  (void) http_dorequest(set_getstr(&ic->acc->set, "host"), 80, 0,
                        request->str, discord_login_cb, acc->ic);

  g_string_free(jlogin, TRUE);
  g_string_free(request, TRUE);
}

static gboolean discord_is_self(struct im_connection *ic, const char *who) {
  discord_data *dd = ic->proto_data;
  return !g_strcmp0(dd->uname, who);
}

static void discord_send_msg(struct im_connection *ic, char *id, char *msg) {
  discord_data *dd = ic->proto_data;
  GString *request = g_string_new("");
  GString *content = g_string_new("");

  g_string_printf(content, "{\"content\":\"%s\"}", msg);
  g_string_printf(request, "POST /api/channels/%s/messages HTTP/1.1\r\n"
                  "Host: %s\r\n"
                  "User-Agent: Bitlbee-Discord\r\n"
                  "authorization: %s\r\n"
                  "Content-Type: application/json\r\n"
                  "Content-Length: %zd\r\n\r\n"
                  "%s",
                  id,
                  set_getstr(&ic->acc->set, "host"),
                  dd->token,
                  content->len,
                  content->str);

  (void) http_dorequest(set_getstr(&ic->acc->set, "host"), 80, 0,
                        request->str, discord_send_msg_cb, ic);

  g_string_free(content, TRUE);
  g_string_free(request, TRUE);
}

static void discord_chat_msg(struct groupchat *gc, char *msg, int flags) {
  channel_info *cinfo = gc->data;

  discord_send_msg(cinfo->to.channel.gc->ic, cinfo->id, msg);
}

static int discord_buddy_msg(struct im_connection *ic, char *to, char *msg,
                              int flags) {
  discord_data *dd = ic->proto_data;
  GSList *l;

  for (l = dd->channels; l; l = l->next) {
    channel_info *cinfo = l->data;
    if (cinfo->is_private && g_strcmp0(cinfo->to.user.handle, to) == 0) {
      discord_send_msg(ic, cinfo->id, msg);
    }
  }

  return 0;
}

static void discord_init(account_t *acc) {
  set_t *s;

  s = set_add(&acc->set, "host", DISCORD_HOST, NULL, acc);
  s->flags |= ACC_SET_OFFLINE_ONLY;
}

G_MODULE_EXPORT void init_plugin(void)
{
  struct prpl *dpp;

  static const struct prpl pp = {
    .name = "discord",
    .init = discord_init,
    .login = discord_login,
    .logout = discord_logout,
    .chat_msg = discord_chat_msg,
    .buddy_msg = discord_buddy_msg,
    .handle_cmp = g_strcmp0,
    .handle_is_self = discord_is_self
  };
  dpp = g_memdup(&pp, sizeof pp);
  register_protocol(dpp);
}

static void discord_http_get(struct im_connection *ic, const char *api_path,
                             http_input_function cb_func, gpointer data) {
  discord_data *dd = ic->proto_data;
  GString *request = g_string_new("");
  g_string_printf(request, "GET /api/%s HTTP/1.1\r\n"
                  "Host: %s\r\n"
                  "User-Agent: Bitlbee-Discord\r\n"
                  "Content-Type: application/json\r\n"
                  "authorization: %s\r\n\r\n",
                  api_path,
                  set_getstr(&ic->acc->set, "host"),
                  dd->token);

  (void) http_dorequest(set_getstr(&ic->acc->set, "host"), 80, 0,
                        request->str, cb_func, data);
  g_string_free(request, TRUE);
}
