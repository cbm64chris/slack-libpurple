#include <cmds.h>

#include "slack-json.h"
#include "slack-api.h"
#include "slack-message.h"
#include "slack-conversation.h"
#include "slack-cmd.h"

/* really most commands are handled server-side, but OPT_PROTO_SLACK_COMMANDS_NATIVE doesn't quite work right (when the same command is registered for other things), so we defensively register a trivial handler for at least all the builtin commands.
 * copied from https://get.slack.help/hc/en-us/articles/201259356-using-slash-commands */
static const char *slack_cmds[] = {
	"me [your text]:  Display italicized action text, e.g. \"/me does a dance\" will display as \"does a dance\"",
	"msg @someone [your message]:  Send a private direct message to another member",
	"dm @someone [your message]:  Send a private direct message to another member",
	"shrug [your message]:  Appends ¯\\_(&#x30c4;)_/¯ to the end of your message",
	"archive:  Archive the current channel",
	"collapse:  Collapse all inline images and video in the current channel (opposite of /expand)",
	"expand:  Expand all inline images and video in the current channel (opposite of /collapse)",
	"invite @someone [#channel]:  Invite a member to a channel",
	"join [#channel]:  Open a channel and become a member",
	"kick @someone:  Remove a member from the current channel. This action may be restricted to Workspace Owners or Admins",
	"remove @someone:  Remove a member from the current channel. This action may be restricted to Workspace Owners or Admins",
	"leave:  Leave a channel",
	"close:  Leave a channel",
	"part:  Leave a channel",
	"away:  Toggle your \"away\" status",
	"mute:  Mute a channel (or unmute a channel that is muted)",
	"open [#channel]:  Open a channel",
	"rename [new name]:  Rename a channel (Admin only)",
	"topic [text]:  Set the channel topic",
	"who:  List members in the current channel",
	"remind [@someone or #channel] to [What] [When]:  Set a reminder a member or a channel",
	"remind help:  Learn more about how to set reminders",
	"remind list:  Get a list of reminders you have set",
	"apps:  Search for Slack apps in the App Directory",
	"search [your text]:  Search Slack messages and files",
	"dnd [some description of time]:  Start or end a Do Not Disturb session",
	"feed help [or subscribe, list, remove]:  Manage RSS subscriptions",
	"feedback [your text]:  Send feedback to Slack",
	"prefs:  Open your preferences",
	"shortcuts:  Open the keyboard shortcuts menu",
	"star:  Star the current channel or conversation",
	NULL
};

static gboolean send_cmd_cb(SlackAccount *sa, gpointer data, json_value *json, const char *error) {
	PurpleConversation *conv = data;

	if (error) {
		purple_conversation_write(conv, NULL, error, PURPLE_MESSAGE_ERROR, time(NULL));
		return FALSE;
	}

	char *response = json_get_prop_strptr(json, "response");
	if (response) {
		GString *html = g_string_new(NULL);
		PurpleMessageFlags flags = PURPLE_MESSAGE_SYSTEM;
		slack_message_to_html(html, sa, response, &flags, NULL);
		purple_conversation_write(conv, NULL, html->str, flags, time(NULL));
		g_string_free(html, TRUE);
	}
	return FALSE;
}

static PurpleCmdRet send_cmd(PurpleConversation *conv, const gchar *cmd, gchar **args, gchar **error, void *data) {
	SlackAccount *sa = get_slack_account(conv->account);
	if (!sa)
		return PURPLE_CMD_RET_FAILED;

	SlackObject *obj = slack_conversation_get_conversation(sa, conv);
	if (!obj)
		return PURPLE_CMD_RET_FAILED;

	GString *msg = g_string_sized_new(strlen(cmd)+1);
	g_string_append_c(msg, '/');
	g_string_append(msg, cmd);

	/* https://github.com/ErikKalkoken/slackApiDoc/blob/master/chat.command.md */
	slack_api_post(sa, send_cmd_cb, conv, "chat.command", "channel", slack_conversation_id(obj), "command", msg->str, "text", args && args[0] ? args[0] : "", NULL);
	g_string_free(msg, TRUE);

	return PURPLE_CMD_RET_OK;
}

static PurpleCmdRet cmd_edit(PurpleConversation *conv, const gchar *cmd, gchar **args, gchar **error, void *data) {
	SlackAccount *sa = get_slack_account(conv->account);
	if (!sa)
		return PURPLE_CMD_RET_FAILED;

	SlackObject *obj = slack_conversation_get_conversation(sa, conv);
	if (!obj || !obj->last_sent) {
		*error = g_strdup("No last sent message");
		return PURPLE_CMD_RET_FAILED;
	}

	slack_api_post(sa, NULL, NULL, "chat.update", "channel", slack_conversation_id(obj), "ts", obj->last_sent, "as_user", "true", "text", args && args[0] ? args[0] : "", NULL);
	return PURPLE_CMD_RET_OK;
}

static PurpleCmdRet cmd_delete(PurpleConversation *conv, const gchar *cmd, gchar **args, gchar **error, void *data) {
	SlackAccount *sa = get_slack_account(conv->account);
	if (!sa)
		return PURPLE_CMD_RET_FAILED;

	SlackObject *obj = slack_conversation_get_conversation(sa, conv);
	if (!obj || !obj->last_sent) {
		*error = g_strdup("No last sent message");
		return PURPLE_CMD_RET_FAILED;
	}

	slack_api_post(sa, NULL, NULL, "chat.delete", "channel", slack_conversation_id(obj), "ts", obj->last_sent, "as_user", "true", NULL);
	return PURPLE_CMD_RET_OK;
}

static PurpleCmdRet cmd_thread(PurpleConversation *conv, const gchar *cmd, gchar **args, gchar **error, void *data) {
	SlackAccount *sa = get_slack_account(conv->account);
	if (!sa)
		return PURPLE_CMD_RET_FAILED;

	SlackObject *obj = slack_conversation_get_conversation(sa, conv);
	if (!obj) {
		return PURPLE_CMD_RET_FAILED;
	}

	if (!args || !args[0]) {
		slack_thread_switch_to_latest(sa, obj);
		return PURPLE_CMD_RET_OK;
	}

	gchar *line = g_strdup(args[0]);
	gchar **split = g_strsplit(line, " ", 2);
	if (split[0] == NULL)
		slack_thread_switch_to_latest(sa, obj);
	else if (split[0][0] == '/') {
		if (split[0][1] != '\0')
			slack_write_message(sa, obj, "thread: First argument must be '/' or a valid timestamp", PURPLE_MESSAGE_SYSTEM);
		else {
			if (split[1] == NULL) {
				slack_thread_switch_to_channel(sa, obj);
			} else {
				slack_thread_post_to_channel(sa, obj, split[1]);
			}
		}
	} else {
		if (split[1] == NULL) {
			slack_thread_switch_to_timestamp(sa, obj, split[0]);
		} else {
			slack_thread_post_to_timestamp(sa, obj, split[0], split[1]);
		}
	}

	g_strfreev(split);
	g_free(line);

	return PURPLE_CMD_RET_OK;
}

static PurpleCmdRet cmd_getthread(PurpleConversation *conv, const gchar *cmd, gchar **args, gchar **error, void *data) {
	SlackAccount *sa = get_slack_account(conv->account);
	if (!sa)
		return PURPLE_CMD_RET_FAILED;

	SlackObject *obj = slack_conversation_get_conversation(sa, conv);
	if (!obj) {
		return PURPLE_CMD_RET_FAILED;
	}

	slack_thread_get_replies(sa, obj, args ? args[0] : "");

	return PURPLE_CMD_RET_OK;
}

static GSList *commands = NULL;

void slack_cmd_register() {
	const char **cmdp = slack_cmds;
	char cmdbuf[16] = "";
	PurpleCmdId id;
	while (*cmdp) {
		const char *cmd = *cmdp;
		unsigned i = 0;
		for (i = 0; cmd[i] != ' ' && cmd[i] != ':' && cmd[i] && i < sizeof(cmdbuf)-1; i++)
			cmdbuf[i] = cmd[i];
		cmdbuf[i] = 0;

		id = purple_cmd_register(cmdbuf, "s", PURPLE_CMD_P_PRPL, PURPLE_CMD_FLAG_CHAT | PURPLE_CMD_FLAG_IM | PURPLE_CMD_FLAG_PRPL_ONLY | PURPLE_CMD_FLAG_ALLOW_WRONG_ARGS,
				SLACK_PLUGIN_ID, send_cmd, cmd, NULL);
		commands = g_slist_prepend(commands, GUINT_TO_POINTER(id));
		cmdp++;
	}

	id = purple_cmd_register("edit", "s", PURPLE_CMD_P_PRPL, PURPLE_CMD_FLAG_IM | PURPLE_CMD_FLAG_CHAT | PURPLE_CMD_FLAG_PRPL_ONLY,
			SLACK_PLUGIN_ID, cmd_edit, "edit [new message]: edit your last message to be new message", NULL);
	commands = g_slist_prepend(commands, GUINT_TO_POINTER(id));

	id = purple_cmd_register("delete", "", PURPLE_CMD_P_PRPL, PURPLE_CMD_FLAG_IM | PURPLE_CMD_FLAG_CHAT | PURPLE_CMD_FLAG_PRPL_ONLY,
			SLACK_PLUGIN_ID, cmd_delete, "delete: remove your last message", NULL);
	commands = g_slist_prepend(commands, GUINT_TO_POINTER(id));

	// Make sure the number of %s matches the printfs further down.
	const char *thread_fmt = "%s [thread-timestamp [message]]:  Post messages in threads.\n"
			"This command can be used in several ways. One can either switch focus to a thread, causing all messages to be "
			"posted to that thread, or post single messages to a thread. See these usage examples:\n"
			"- /%s thread-timestamp:  Switch to thread with given timestamp.\n"
			"- /%s /:  Switch back to channel.\n"
			"- /%s:  Switch to the thread of the latest message. Switches to main channel if the latest reply was not threaded.\n"
			"- /%s thread-timestamp message:  Post single message to the thread with the given timestamp.\n"
			"- /%s / message:  Post single message to main channel.";
	GString *thread_help = g_string_new(NULL);

	g_string_printf(thread_help, thread_fmt, "th", "th", "th", "th", "th", "th", "th");
	id = purple_cmd_register("th", "s", PURPLE_CMD_P_PRPL, PURPLE_CMD_FLAG_IM | PURPLE_CMD_FLAG_CHAT | PURPLE_CMD_FLAG_PRPL_ONLY |
			PURPLE_CMD_FLAG_ALLOW_WRONG_ARGS, SLACK_PLUGIN_ID, cmd_thread, thread_help->str, NULL);
	commands = g_slist_prepend(commands, GUINT_TO_POINTER(id));

	g_string_printf(thread_help, thread_fmt, "thread", "thread", "thread", "thread", "thread", "thread", "thread");
	id = purple_cmd_register("thread", "s", PURPLE_CMD_P_PRPL, PURPLE_CMD_FLAG_IM | PURPLE_CMD_FLAG_CHAT | PURPLE_CMD_FLAG_PRPL_ONLY |
			PURPLE_CMD_FLAG_ALLOW_WRONG_ARGS, SLACK_PLUGIN_ID, cmd_thread, thread_help->str, NULL);
	commands = g_slist_prepend(commands, GUINT_TO_POINTER(id));

	g_string_free(thread_help, TRUE);

	id = purple_cmd_register("getthread", "s", PURPLE_CMD_P_PRPL, PURPLE_CMD_FLAG_IM | PURPLE_CMD_FLAG_CHAT | PURPLE_CMD_FLAG_PRPL_ONLY,
			SLACK_PLUGIN_ID, cmd_getthread, "getthread [thread-timestamp]: Fetch given thread", NULL);
	commands = g_slist_prepend(commands, GUINT_TO_POINTER(id));

	id = purple_cmd_register("gt", "s", PURPLE_CMD_P_PRPL, PURPLE_CMD_FLAG_IM | PURPLE_CMD_FLAG_CHAT | PURPLE_CMD_FLAG_PRPL_ONLY,
			SLACK_PLUGIN_ID, cmd_getthread, "gt [thread-timestamp]: Fetch given thread", NULL);
	commands = g_slist_prepend(commands, GUINT_TO_POINTER(id));
}

void slack_cmd_unregister() {
	while (commands) {
		purple_cmd_unregister(GPOINTER_TO_UINT(commands->data));
		commands = g_slist_delete_link(commands, commands);
	}
}
