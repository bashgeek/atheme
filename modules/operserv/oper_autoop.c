/*
 * SPDX-License-Identifier: ISC
 * SPDX-URL: https://spdx.org/licenses/ISC.html
 *
 * Gives IRC operators (umode +o) owner (where the ircd has it), op and voice
 * when they join any channel, registered or not.
 *
 * Enabled with "oper_autoop;" in the operserv {} block.
 */

#include <atheme.h>

static struct service *opersvs = NULL;
static bool oper_autoop_enabled = false;

static bool
oper_autoop_applies(struct user *const restrict u)
{
	return oper_autoop_enabled && u != NULL && (u->flags & UF_IRCOP) && ! is_internal_client(u);
}

static void
oper_autoop_join_hook(struct hook_channel_joinpart *const restrict hdata)
{
	struct chanuser *const cu = hdata->cu;

	// A previous channel_join hook may have kicked the user
	if (cu == NULL || ! oper_autoop_applies(cu->user))
		return;

	struct user *const u = cu->user;
	struct channel *const c = cu->chan;

	/* Joins from a server that hasn't finished bursting are netjoins or a
	 * services restart; the ircd already carries the user's status there.
	 */
	if (u->server == NULL || ! (u->server->flags & SF_EOB))
		return;

	const char *const source = opersvs->me->nick;

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

	(void) add_bool_conf_item("OPER_AUTOOP", &opersvs->conf_table, 0, &oper_autoop_enabled, false);

	(void) hook_add_channel_join(&oper_autoop_join_hook);
	(void) hook_add_first_chanuser_sync(&oper_autoop_chanuser_sync_hook);
}

static void
mod_deinit(const enum module_unload_intent ATHEME_VATTR_UNUSED intent)
{
	(void) hook_del_chanuser_sync(&oper_autoop_chanuser_sync_hook);
	(void) hook_del_channel_join(&oper_autoop_join_hook);

	(void) del_conf_item("OPER_AUTOOP", &opersvs->conf_table);
}

SIMPLE_DECLARE_MODULE_V1("operserv/oper_autoop", MODULE_UNLOAD_CAPABILITY_OK)
