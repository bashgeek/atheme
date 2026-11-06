/*
 * SPDX-License-Identifier: ISC
 * SPDX-URL: https://spdx.org/licenses/ISC.html
 *
 * Gives IRC operators (umode +o) owner (where the ircd has it), op and voice
 * when they join any channel, registered or not, and in every channel they
 * are already in when they become an IRC operator.
 *
 * Enabled with "oper_autoop;" in the operserv {} block. The modes are set by
 * a dedicated services client, configured in the operautoop {} block.
 */

#include <atheme.h>

static struct service *opersvs = NULL;
static struct service *autoopsvs = NULL;
static bool oper_autoop_enabled = false;

static bool
oper_autoop_applies(struct user *const restrict u)
{
	return oper_autoop_enabled && u != NULL && (u->flags & UF_IRCOP) && ! is_internal_client(u);
}

// The client has no commands; answer CTCPs and explain what it does
static void
oper_autoop_handler(struct sourceinfo *const restrict si, const int parc, char **const restrict parv)
{
	char *const msg = parv[parc - 1];

	if (*msg == '\001')
	{
		char *const cmd = strtok(msg, " ");
		char *const text = strtok(NULL, "");

		if (cmd != NULL)
			(void) handle_ctcp_common(si, cmd, text);

		return;
	}

	(void) command_success_nodata(si, _("I give IRC operators channel status in the channels they are in. "
	                                    "I have no commands."));
}

static void
oper_autoop_give_status(struct chanuser *const restrict cu)
{
	struct user *const u = cu->user;
	struct channel *const c = cu->chan;
	const char *const source = autoopsvs->me->nick;

	if (ircd->uses_owner && ircd->owner_mode && ! (cu->modes & ircd->owner_mode))
	{
		(void) modestack_mode_param(source, c, MTYPE_ADD, ircd->owner_mchar[1], CLIENT_NAME(u));
		cu->modes |= ircd->owner_mode;
	}

	if (! (cu->modes & CSTATUS_OP))
	{
		(void) modestack_mode_param(source, c, MTYPE_ADD, 'o', CLIENT_NAME(u));
		cu->modes |= CSTATUS_OP;
	}

	if (! (cu->modes & CSTATUS_VOICE))
	{
		(void) modestack_mode_param(source, c, MTYPE_ADD, 'v', CLIENT_NAME(u));
		cu->modes |= CSTATUS_VOICE;
	}
}

/* Whether status may be given to this user now. Joins and mode changes from
 * a server that hasn't finished bursting are netjoins or a services restart;
 * the ircd already carries the user's status there.
 */
static bool
oper_autoop_can_give(struct user *const restrict u)
{
	if (! oper_autoop_applies(u))
		return false;

	if (u->server == NULL || ! (u->server->flags & SF_EOB))
		return false;

	// Not introduced to the network (yet)
	if (autoopsvs->me == NULL)
		return false;

	return true;
}

static void
oper_autoop_join_hook(struct hook_channel_joinpart *const restrict hdata)
{
	struct chanuser *const cu = hdata->cu;

	// A previous channel_join hook may have kicked the user
	if (cu == NULL || ! oper_autoop_can_give(cu->user))
		return;

	(void) oper_autoop_give_status(cu);
}

// A user who is already in channels became an IRC operator
static void
oper_autoop_user_oper_hook(struct user *const restrict u)
{
	if (! oper_autoop_can_give(u))
		return;

	mowgli_node_t *n;

	MOWGLI_ITER_FOREACH(n, u->channels.head)
		(void) oper_autoop_give_status(n->data);
}

/* In SECURE (and freshly recreated) registered channels, ChanServ strips
 * status from users without channel access. Keep it from undoing ours.
 */
static void
oper_autoop_chanuser_sync_hook(struct hook_chanuser_sync *const restrict hdata)
{
	if (hdata->cu != NULL && oper_autoop_applies(hdata->cu->user))
		hdata->take_prefixes = false;
}

static void
mod_init(struct module *const restrict m)
{
	MODULE_TRY_REQUEST_DEPENDENCY(m, "operserv/main")

	if (! (opersvs = service_find("operserv")))
	{
		(void) slog(LG_ERROR, "%s: service_find() for OperServ failed! (BUG)", m->name);

		m->mflags |= MODFLAG_FAIL;
		return;
	}

	if (! (autoopsvs = service_add("operautoop", &oper_autoop_handler)))
	{
		(void) slog(LG_ERROR, "%s: service_add() failed", m->name);

		m->mflags |= MODFLAG_FAIL;
		return;
	}

	(void) add_bool_conf_item("OPER_AUTOOP", &opersvs->conf_table, 0, &oper_autoop_enabled, false);

	(void) hook_add_channel_join(&oper_autoop_join_hook);
	(void) hook_add_user_oper(&oper_autoop_user_oper_hook);
	(void) hook_add_first_chanuser_sync(&oper_autoop_chanuser_sync_hook);
}

static void
mod_deinit(const enum module_unload_intent ATHEME_VATTR_UNUSED intent)
{
	(void) hook_del_chanuser_sync(&oper_autoop_chanuser_sync_hook);
	(void) hook_del_user_oper(&oper_autoop_user_oper_hook);
	(void) hook_del_channel_join(&oper_autoop_join_hook);

	(void) del_conf_item("OPER_AUTOOP", &opersvs->conf_table);

	(void) service_delete(autoopsvs);
}

SIMPLE_DECLARE_MODULE_V1("operserv/oper_autoop", MODULE_UNLOAD_CAPABILITY_OK)
