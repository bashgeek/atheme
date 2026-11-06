/*
 * SPDX-License-Identifier: ISC
 * SPDX-URL: https://spdx.org/licenses/ISC.html
 *
 * Copyright (C) 2005-2006 William Pitcock, et al.
 *
 * Jupiters a server.
 *
 * Jupes are kept in the services database and are reintroduced:
 *   - when the uplink finishes bursting (services restart, relink),
 *   - when the network SQUITs one of them,
 *   - when a real server shows up with a juped name,
 *   - periodically on P10, where jupes are ircd-side and expire.
 * The list held here is authoritative; use JUPE DEL to remove a jupe.
 */

#include <atheme.h>

#define JUPE_DBENTRY            "JUPE"

// Minimum time between two reintroductions of the same jupe (loop guard)
#define JUPE_REASSERT_MIN       10

// P10 jupes are sent with a 1 day expiry (see p10_jupe()); refresh well before that
#define JUPE_P10_REFRESH        (SECONDS_PER_HOUR * 12)

struct jupe_entry
{
	char *          server;
	char *          reason;
	char *          setby;
	time_t          ts;
	time_t          last_assert;
	bool            pending;
};

static mowgli_patricia_t *os_jupe_cmds = NULL;
static mowgli_patricia_t *jupe_list = NULL;
static mowgli_eventloop_timer_t *jupe_sync_timer = NULL;
static mowgli_eventloop_timer_t *jupe_p10_timer = NULL;

static void jupe_sync_schedule(time_t delay);

static struct jupe_entry *
jupe_entry_find(const char *const restrict server)
{
	return mowgli_patricia_retrieve(jupe_list, server);
}

static struct jupe_entry *
jupe_entry_add(const char *const restrict server, const char *const restrict reason,
               const char *const restrict setby, const time_t ts)
{
	struct jupe_entry *const j = smalloc(sizeof *j);

	j->server = sstrdup(server);
	j->reason = sstrdup(reason);
	j->setby  = sstrdup(setby);
	j->ts     = ts;

	(void) mowgli_patricia_add(jupe_list, j->server, j);

	return j;
}

static void
jupe_entry_free(struct jupe_entry *const restrict j)
{
	(void) sfree(j->server);
	(void) sfree(j->reason);
	(void) sfree(j->setby);
	(void) sfree(j);
}

static void
jupe_entry_destroy_cb(const char ATHEME_VATTR_UNUSED *const restrict key, void *const restrict data,
                      void ATHEME_VATTR_UNUSED *const restrict privdata)
{
	(void) jupe_entry_free(data);
}

static void
jupe_entry_delete(struct jupe_entry *const restrict j)
{
	(void) mowgli_patricia_delete(jupe_list, j->server);
	(void) jupe_entry_free(j);
}

// Our uplink has finished sending its burst
static bool
uplink_synced(void)
{
	if (! me.connected || me.actual == NULL)
		return false;

	const struct server *const s = server_find(me.actual);

	return s != NULL && (s->flags & SF_EOB);
}

static void
jupe_assert(struct jupe_entry *const restrict j)
{
	char reasonbuf[BUFSIZE];

	j->pending = false;
	j->last_assert = CURRTIME;

	if (! irccasecmp(j->server, me.name) || (me.actual != NULL && ! irccasecmp(j->server, me.actual)))
	{
		(void) slog(LG_ERROR, "JUPE: not introducing jupe for \2%s\2: it is the services server or "
		                      "the current uplink", j->server);
		return;
	}

	(void) snprintf(reasonbuf, sizeof reasonbuf, "[%s] %s", j->setby, j->reason);
	(void) jupe(j->server, reasonbuf);
}

static void
jupe_mark_all_pending(const bool reset_ratelimit)
{
	mowgli_patricia_iteration_state_t state;
	struct jupe_entry *j;

	MOWGLI_PATRICIA_FOREACH(j, &state, jupe_list)
	{
		j->pending = true;

		if (reset_ratelimit)
			j->last_assert = 0;
	}
}

static void
jupe_sync_cb(void ATHEME_VATTR_UNUSED *const restrict arg)
{
	mowgli_patricia_iteration_state_t state;
	struct jupe_entry *j;
	bool retry = false;

	jupe_sync_timer = NULL;

	// The uplink's end of burst will mark everything pending again
	if (! me.connected)
		return;

	MOWGLI_PATRICIA_FOREACH(j, &state, jupe_list)
	{
		if (! j->pending)
			continue;

		if (j->last_assert && (CURRTIME - j->last_assert) < JUPE_REASSERT_MIN)
		{
			retry = true;
			continue;
		}

		(void) jupe_assert(j);
	}

	if (retry)
		(void) jupe_sync_schedule(JUPE_REASSERT_MIN);
}

static void
jupe_sync_schedule(const time_t delay)
{
	if (jupe_sync_timer)
	{
		if (delay != 0)
			return;

		// An immediate sync supersedes a delayed one
		(void) mowgli_timer_destroy(base_eventloop, jupe_sync_timer);
	}

	jupe_sync_timer = mowgli_timer_add_once(base_eventloop, "jupe_sync", &jupe_sync_cb, NULL, delay);
}

static void
jupe_p10_refresh_cb(void ATHEME_VATTR_UNUSED *const restrict arg)
{
	if (! (ircd != NULL && ircd->uses_p10 && uplink_synced()))
		return;

	(void) jupe_mark_all_pending(true);
	(void) jupe_sync_schedule(0);
}

static void
jupe_server_eob_hook(struct server *const restrict s)
{
	// Only the uplink's EOB matters: it means the whole network has been bursted to us
	if (s == NULL || s->uplink != me.me)
		return;

	if (! mowgli_patricia_size(jupe_list))
		return;

	(void) slog(LG_DEBUG, "%s: uplink %s finished bursting, reintroducing %u jupe(s)", MOWGLI_FUNC_NAME,
	                      s->name, mowgli_patricia_size(jupe_list));

	(void) jupe_mark_all_pending(true);
	(void) jupe_sync_schedule(0);
}

static void
jupe_server_add_hook(struct server *const restrict s)
{
	if (s == NULL || s->name == NULL)
		return;

	struct jupe_entry *const j = jupe_entry_find(s->name);

	if (j == NULL)
		return;

	// Still receiving the uplink's burst; its EOB will reintroduce every jupe
	if (! uplink_synced())
		return;

	(void) slog(LG_INFO, "JUPE: juped server \2%s\2 linked (via \2%s\2), reintroducing jupe", s->name,
	                     s->uplink != NULL ? s->uplink->name : "<none>");

	// Hooks may not destroy the server, so do it from an event
	j->pending = true;
	(void) jupe_sync_schedule(0);
}

static void
jupe_server_squit_unknown_hook(struct hook_server_squit_unknown *const restrict hdata)
{
	if (hdata == NULL || hdata->name == NULL)
		return;

	struct jupe_entry *const j = jupe_entry_find(hdata->name);

	if (j == NULL)
		return;

	(void) slog(LG_INFO, "JUPE: \2%s\2 was SQUIT by \2%s\2 (%s), reintroducing jupe", j->server,
	                     hdata->si != NULL ? get_source_name(hdata->si) : "<unknown>",
	                     hdata->reason != NULL ? hdata->reason : "<none>");

	j->pending = true;
	(void) jupe_sync_schedule(0);
}

static void
jupe_db_write_hook(struct database_handle *const restrict db)
{
	mowgli_patricia_iteration_state_t state;
	struct jupe_entry *j;

	MOWGLI_PATRICIA_FOREACH(j, &state, jupe_list)
	{
		(void) db_start_row(db, JUPE_DBENTRY);
		(void) db_write_word(db, j->server);
		(void) db_write_word(db, j->setby);
		(void) db_write_time(db, j->ts);
		(void) db_write_str(db, j->reason);
		(void) db_commit_row(db);
	}
}

static void
jupe_db_read_hook(struct database_handle *const restrict db, const char ATHEME_VATTR_UNUSED *const restrict type)
{
	const char *const server = db_sread_word(db);
	const char *const setby = db_sread_word(db);
	const time_t ts = db_sread_time(db);
	const char *const reason = db_sread_str(db);

	if (jupe_entry_find(server))
	{
		(void) slog(LG_ERROR, "%s: duplicate jupe for %s in database, ignoring", MOWGLI_FUNC_NAME, server);
		return;
	}

	(void) jupe_entry_add(server, reason, setby, ts);
}

static bool
jupe_valid_server(struct sourceinfo *const restrict si, const char *const restrict server)
{
	// Disallow wildcards in a jupe as a minimal sanity check; it makes it hard to squit safely.
	if (! strchr(server, '.') || strchr(server, '*') || strchr(server, '?') || strlen(server) > HOSTLEN)
	{
		(void) command_fail(si, fault_badparams, _("\2%s\2 is not a valid server name."), server);
		return false;
	}

	if (! irccasecmp(server, me.name))
	{
		(void) command_fail(si, fault_noprivs, _("\2%s\2 is the services server; it cannot be "
		                                         "jupitered!"), server);
		return false;
	}

	if (me.actual != NULL && ! irccasecmp(server, me.actual))
	{
		(void) command_fail(si, fault_noprivs, _("\2%s\2 is the current uplink; it cannot be "
		                                         "jupitered!"), server);
		return false;
	}

	return true;
}

static void
jupe_do_add(struct sourceinfo *const restrict si, const char *const restrict server,
            const char *const restrict reason)
{
	if (! jupe_valid_server(si, server))
		return;

	struct jupe_entry *j = jupe_entry_find(server);
	const bool updated = (j != NULL);

	if (j)
	{
		(void) sfree(j->reason);
		(void) sfree(j->setby);

		j->reason = sstrdup(reason);
		j->setby  = sstrdup(get_storage_oper_name(si));
		j->ts     = CURRTIME;
	}
	else
		j = jupe_entry_add(server, reason, get_storage_oper_name(si), CURRTIME);

	(void) logcommand(si, CMDLOG_ADMIN, "JUPE:ADD: \2%s\2 (reason: \2%s\2)", j->server, reason);
	(void) wallops("\2%s\2 jupitered server \2%s\2 (%s).", get_oper_name(si), j->server, reason);

	// If we're not linked, the uplink's end of burst will introduce it
	if (me.connected)
		(void) jupe_assert(j);

	if (updated)
		(void) command_success_nodata(si, _("Jupe for \2%s\2 has been updated."), j->server);
	else
		(void) command_success_nodata(si, _("\2%s\2 has been jupitered."), j->server);
}

static void
os_cmd_jupe(struct sourceinfo *const restrict si, const int parc, char **const restrict parv)
{
	if (parc < 1)
	{
		(void) command_fail(si, fault_needmoreparams, STR_INSUFFICIENT_PARAMS, "JUPE");
		(void) command_fail(si, fault_needmoreparams, _("Syntax: JUPE ADD|DEL|LIST|SYNC"));
		return;
	}

	// Backwards compatibility: JUPE <server> <reason>. Subcommand names never contain a '.'
	if (strchr(parv[0], '.'))
	{
		char reason[BUFSIZE];

		if (parc < 2)
		{
			(void) command_fail(si, fault_needmoreparams, STR_INSUFFICIENT_PARAMS, "JUPE");
			(void) command_fail(si, fault_needmoreparams, _("Syntax: JUPE ADD <server> <reason>"));
			return;
		}

		if (parc > 2)
			(void) snprintf(reason, sizeof reason, "%s %s", parv[1], parv[2]);
		else
			(void) mowgli_strlcpy(reason, parv[1], sizeof reason);

		(void) jupe_do_add(si, parv[0], reason);
		return;
	}

	(void) subcommand_dispatch_simple(si->service, si, parc, parv, os_jupe_cmds, "JUPE");
}

static void
os_cmd_jupe_add(struct sourceinfo *const restrict si, const int ATHEME_VATTR_UNUSED parc,
                char **const restrict parv)
{
	const char *const server = parv[0];
	const char *const reason = parv[1];

	if (! (server && reason))
	{
		(void) command_fail(si, fault_needmoreparams, STR_INSUFFICIENT_PARAMS, "JUPE ADD");
		(void) command_fail(si, fault_needmoreparams, _("Syntax: JUPE ADD <server> <reason>"));
		return;
	}

	(void) jupe_do_add(si, server, reason);
}

static void
os_cmd_jupe_del(struct sourceinfo *const restrict si, const int ATHEME_VATTR_UNUSED parc,
                char **const restrict parv)
{
	const char *const server = parv[0];

	if (! server)
	{
		(void) command_fail(si, fault_needmoreparams, STR_INSUFFICIENT_PARAMS, "JUPE DEL");
		(void) command_fail(si, fault_needmoreparams, _("Syntax: JUPE DEL <server>"));
		return;
	}

	struct jupe_entry *const j = jupe_entry_find(server);

	if (! j)
	{
		(void) command_fail(si, fault_nosuch_target, _("\2%s\2 is not jupitered."), server);
		return;
	}

	(void) logcommand(si, CMDLOG_ADMIN, "JUPE:DEL: \2%s\2", j->server);
	(void) wallops("\2%s\2 removed the jupe on server \2%s\2.", get_oper_name(si), j->server);

	struct server *const s = server_find(j->server);

	if (s != NULL)
	{
		/* A real server by that name is linked: our jupe isn't on the network
		 * (yet). Make sure the core doesn't introduce it once that server goes.
		 */
		s->flags &= ~SF_JUPE_PENDING;
	}
	else if (me.connected)
	{
		char reasonbuf[BUFSIZE];

		(void) snprintf(reasonbuf, sizeof reasonbuf, "Jupe removed by %s", get_oper_name(si));
		(void) unjupe(j->server, reasonbuf);
	}

	(void) command_success_nodata(si, _("Jupe for \2%s\2 has been removed."), j->server);
	(void) jupe_entry_delete(j);
}

static void
os_cmd_jupe_list(struct sourceinfo *const restrict si, const int parc, char **const restrict parv)
{
	const char *const mask = (parc > 0) ? parv[0] : NULL;
	mowgli_patricia_iteration_state_t state;
	struct jupe_entry *j;
	unsigned int count = 0;

	(void) command_success_nodata(si, _("Jupe list:"));

	MOWGLI_PATRICIA_FOREACH(j, &state, jupe_list)
	{
		if (mask && match(mask, j->server))
			continue;

		count++;

		(void) command_success_nodata(si, _("%u: %s (set by %s, %s ago) - %s"), count, j->server, j->setby,
		                              time_ago(j->ts), j->reason);
	}

	(void) command_success_nodata(si, ngettext(N_("Total of %u jupe."), N_("Total of %u jupes."), count),
	                              count);
	(void) logcommand(si, CMDLOG_GET, "JUPE:LIST%s%s", mask ? ": " : "", mask ? mask : "");
}

static void
os_cmd_jupe_sync(struct sourceinfo *const restrict si, const int ATHEME_VATTR_UNUSED parc,
                 char ATHEME_VATTR_UNUSED **const restrict parv)
{
	if (! me.connected)
	{
		(void) command_fail(si, fault_nochange, _("Services is not connected to the network."));
		return;
	}

	(void) logcommand(si, CMDLOG_ADMIN, "JUPE:SYNC");

	(void) jupe_mark_all_pending(true);
	(void) jupe_sync_schedule(0);

	(void) command_success_nodata(si, _("Reintroducing %u jupe(s)."), mowgli_patricia_size(jupe_list));
}

static struct command os_jupe = {
	.name           = "JUPE",
	.desc           = N_("Jupiters a server."),
	.access         = PRIV_JUPE,
	.maxparc        = 3,
	.cmd            = &os_cmd_jupe,
	.help           = { .path = "oservice/jupe" },
};

static struct command os_jupe_add = {
	.name           = "ADD",
	.desc           = N_("Adds a persistent jupe."),
	.access         = AC_NONE,
	.maxparc        = 2,
	.cmd            = &os_cmd_jupe_add,
	.help           = { .path = "" },
};

static struct command os_jupe_del = {
	.name           = "DEL",
	.desc           = N_("Removes a jupe."),
	.access         = AC_NONE,
	.maxparc        = 1,
	.cmd            = &os_cmd_jupe_del,
	.help           = { .path = "" },
};

static struct command os_jupe_list = {
	.name           = "LIST",
	.desc           = N_("Lists jupes."),
	.access         = AC_NONE,
	.maxparc        = 1,
	.cmd            = &os_cmd_jupe_list,
	.help           = { .path = "" },
};

static struct command os_jupe_sync = {
	.name           = "SYNC",
	.desc           = N_("Reintroduces all jupes."),
	.access         = AC_NONE,
	.maxparc        = 0,
	.cmd            = &os_cmd_jupe_sync,
	.help           = { .path = "" },
};

static void
mod_init(struct module *const restrict m)
{
	MODULE_TRY_REQUEST_DEPENDENCY(m, "operserv/main")

	if (! (os_jupe_cmds = mowgli_patricia_create(&strcasecanon)))
	{
		(void) slog(LG_ERROR, "%s: mowgli_patricia_create() failed", m->name);

		m->mflags |= MODFLAG_FAIL;
		return;
	}

	if (! (jupe_list = mowgli_patricia_create(&irccasecanon)))
	{
		(void) slog(LG_ERROR, "%s: mowgli_patricia_create() failed", m->name);
		(void) mowgli_patricia_destroy(os_jupe_cmds, NULL, NULL);

		m->mflags |= MODFLAG_FAIL;
		return;
	}

	if (! (jupe_p10_timer = mowgli_timer_add(base_eventloop, "jupe_p10_refresh", &jupe_p10_refresh_cb, NULL,
	                                         JUPE_P10_REFRESH)))
	{
		(void) slog(LG_ERROR, "%s: mowgli_timer_add() failed", m->name);
		(void) mowgli_patricia_destroy(jupe_list, NULL, NULL);
		(void) mowgli_patricia_destroy(os_jupe_cmds, NULL, NULL);

		m->mflags |= MODFLAG_FAIL;
		return;
	}

	(void) command_add(&os_jupe_add, os_jupe_cmds);
	(void) command_add(&os_jupe_del, os_jupe_cmds);
	(void) command_add(&os_jupe_list, os_jupe_cmds);
	(void) command_add(&os_jupe_sync, os_jupe_cmds);

	(void) hook_add_db_write(&jupe_db_write_hook);
	(void) db_register_type_handler(JUPE_DBENTRY, &jupe_db_read_hook);

	(void) hook_add_server_eob(&jupe_server_eob_hook);
	(void) hook_add_server_add(&jupe_server_add_hook);
	(void) hook_add_server_squit_unknown(&jupe_server_squit_unknown_hook);

	(void) service_named_bind_command("operserv", &os_jupe);
}

static void
mod_deinit(const enum module_unload_intent ATHEME_VATTR_UNUSED intent)
{
	(void) service_named_unbind_command("operserv", &os_jupe);

	(void) hook_del_server_squit_unknown(&jupe_server_squit_unknown_hook);
	(void) hook_del_server_add(&jupe_server_add_hook);
	(void) hook_del_server_eob(&jupe_server_eob_hook);

	(void) hook_del_db_write(&jupe_db_write_hook);
	(void) db_unregister_type_handler(JUPE_DBENTRY);

	if (jupe_sync_timer)
		(void) mowgli_timer_destroy(base_eventloop, jupe_sync_timer);

	(void) mowgli_timer_destroy(base_eventloop, jupe_p10_timer);

	(void) mowgli_patricia_destroy(jupe_list, &jupe_entry_destroy_cb, NULL);
	(void) mowgli_patricia_destroy(os_jupe_cmds, NULL, NULL);
}

// Never unload: the jupe list is only read from the database at startup
SIMPLE_DECLARE_MODULE_V1("operserv/jupe", MODULE_UNLOAD_CAPABILITY_NEVER)
