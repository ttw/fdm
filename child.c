/* $Id$ */

/*
 * Copyright (c) 2006 Nicholas Marriott <nicm@users.sourceforge.net>
 *
 * Permission to use, copy, modify, and distribute this software for any
 * purpose with or without fee is hereby granted, provided that the above
 * copyright notice and this permission notice appear in all copies.
 *
 * THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES
 * WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF
 * MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR
 * ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
 * WHATSOEVER RESULTING FROM LOSS OF MIND, USE, DATA OR PROFITS, WHETHER
 * IN AN ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING
 * OUT OF OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
 */

#include <sys/types.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/wait.h>

#include <fcntl.h>
#include <fnmatch.h>
#include <limits.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "fdm.h"
#include "deliver.h"
#include "fetch.h"
#include "match.h"

int	poll_account(struct io *, struct account *);
int	fetch_account(struct io *, struct account *, double);
int	fetch_got(struct io *, struct account *, struct mail *, const char **);
int	do_expr(struct rule *, struct match_ctx *);
int	do_deliver(struct rule *, struct match_ctx *);
int	do_action(struct rule *, struct match_ctx *, struct action *);
int	do_rules(struct match_ctx *, struct rules *, const char **);

void	child_sighandler(int);

void
child_sighandler(int sig)
{
	switch (sig) {
	case SIGTERM:
		cleanup_purge();
		_exit(1);
	}
}

int
child_fork(void)
{
	pid_t		 pid;
	struct sigaction act;

	switch (pid = fork()) {
	case -1:
		fatal("fork");
	case 0:
		cleanup_flush();

		sigemptyset(&act.sa_mask);
		sigaddset(&act.sa_mask, SIGINT);
		sigaddset(&act.sa_mask, SIGTERM);
		act.sa_flags = SA_RESTART;

		act.sa_handler = SIG_IGN;
		if (sigaction(SIGINT, &act, NULL) < 0)
			fatal("sigaction");

		act.sa_handler = child_sighandler;
		if (sigaction(SIGTERM, &act, NULL) < 0)
			fatal("sigaction");

		return (0);
	default:
		return (pid);
	}
}

__dead void
child_exit(int status)
{
	cleanup_check();
	_exit(status);
}

int
do_child(int fd, enum fdmop op, struct account *a)
{
	struct io	*io;
	struct msg	 msg;
	int		 error = 1;
	double		 tim;

#ifdef DEBUG
	xmalloc_clear();
	COUNTFDS(a->name);
#endif

	io = io_create(fd, NULL, IO_LF, INFTIM);
	log_debug("%s: started, pid %ld", a->name, (long) getpid());

	if (geteuid() != 0) {
		log_debug("%s: not root. not dropping privileges", a->name);
	} else {
		log_debug("%s: changing to user %lu", a->name,
		    (u_long) conf.child_uid);
		if (dropto(conf.child_uid) != 0)
			fatal("dropto");
        }
#ifndef NO_SETPROCTITLE
	setproctitle("child: %s", a->name);
#endif

	if (op == FDMOP_POLL && a->fetch->poll == NULL) {
		log_info("%s: polling not supported", a->name);
		goto out;
	} else if (op == FDMOP_FETCH && a->fetch->fetch == NULL) {
		log_info("%s: fetching not supported", a->name);
		goto out;
	}
	tim = get_time();

	/* start fetch */
	if (a->fetch->start != NULL && a->fetch->start(a) != FETCH_SUCCESS) {
		log_debug("%s: start error. aborting", a->name);
		goto out;
	}

	/* process fetch */
	log_debug("%s: started processing", a->name);
	switch (op) {
	case FDMOP_POLL:
		error = poll_account(io, a);
		break;
	case FDMOP_FETCH:
		error = fetch_account(io, a, tim);
		break;
	default:
		fatalx("child: unexpected command");
	}
	log_debug("%s: finished processing. exiting", a->name);

out:
	/* finish fetch */
	if (a->fetch->finish != NULL && a->fetch->finish(a) != FETCH_SUCCESS)
		error = 1;

	memset(&msg, 0, sizeof msg);
	msg.type = MSG_EXIT;
	log_debug3("%s: sending exit message to parent", a->name);
	if (privsep_send(io, &msg, NULL, 0) != 0)
		fatalx("child: privsep_send error");
	log_debug3("%s: waiting for exit message from parent", a->name);
	if (privsep_recv(io, &msg, NULL, 0) != 0)
		fatalx("child: privsep_recv error");
	if (msg.type != MSG_EXIT)
		fatalx("child: unexpected message");

	io_close(io);
	io_free(io);

#ifdef DEBUG
	COUNTFDS(a->name);
	xmalloc_report(a->name);
#endif

	return (error);
}

int
poll_account(unused struct io *io, struct account *a)
{
	u_int	n;

	log_debug("%s: polling", a->name);

	if (a->fetch->poll(a, &n) == FETCH_ERROR) {
		log_warnx("%s: polling error. aborted", a->name);
		return (1);
	}

	log_info("%s: %u messages found", a->name, n);

	return (0);
}

int
fetch_account(struct io *io, struct account *a, double tim)
{
	struct mail	 m;
	u_int	 	 n, dropped, kept;
	int		 error;
 	const char	*cause = NULL;

	log_debug("%s: fetching", a->name);

	n = dropped = kept = 0;
        for (;;) {
		memset(&m, 0, sizeof m);
		m.body = -1;
		m.decision = DECISION_DROP;

		/* fetch a message */
		error = a->fetch->fetch(a, &m);
		switch (error) {
		case FETCH_ERROR:
			cause = "fetching";
			goto out;
		case FETCH_OVERSIZE:
			log_warnx("%s: message too big: %zu bytes", a->name,
			    m.size);
			if (conf.del_big)
				goto done;
			cause = "fetching";
			goto out;
		case FETCH_COMPLETE:
			goto out;
		}

		trim_from(&m);
		if (m.size == 0) {
			mail_destroy(&m);
			log_warnx("%s: got empty message. ignored", a->name);
			continue;
		}

		/* handle match/delivery */
		if (fetch_got(io, a, &m, &cause) != FETCH_SUCCESS)
			goto out;

	done:
		/* finished with the message */
		if (a->fetch->done != NULL) {
			switch (m.decision) {
			case DECISION_DROP:
				log_debug("%s: deleting message", a->name);
				cause = "deleting";
				dropped++;
				break;
			case DECISION_KEEP:
				log_debug("%s: keeping message", a->name);
				cause = "keeping";
				kept++;
				break;
			default:
				fatalx("invalid decision");
			}
			if (a->fetch->done(a, m.decision) != FETCH_SUCCESS)
				goto out;
			cause = NULL;
		}

		if (conf.purge_after > 0 && a->fetch->purge != NULL) {
			n++;
			if (n >= conf.purge_after) {
				log_debug("%s: %u mails, purging", a->name, n);
				if (a->fetch->purge(a) != FETCH_SUCCESS) {
					cause = "purging";
					goto out;
				}
				n = 0;
			}
		}

 		mail_destroy(&m);
	}

out:
	mail_destroy(&m);
	if (cause != NULL)
		log_warnx("%s: %s error. aborted", a->name, cause);

	tim = get_time() - tim;
	n = dropped + kept;
	if (n > 0) {
		log_info("%s: %u messages processed (%u kept) in %.3f seconds "
		    "(average %.3f)", a->name, n, kept, tim, tim / n);
	} else {
	        log_info("%s: %u messages processed in %.3f seconds",
		    a->name, n, tim);
	}

	return (cause != NULL);
}

int
fetch_got(struct io *io, struct account *a, struct mail *m, const char **cause)
{
	struct match_ctx mctx;
	char		*hdr, rtm[64], *rnm;
	size_t		 len;
	int		 error;
	u_int		 lines;
	
	memset(&mctx, 0, sizeof mctx);
	mctx.io = io;
	mctx.account = a;
	mctx.mail = m;

	log_debug("%s: got message: size %zu, body %zd", a->name,
	    m->size, m->body);

	hdr = find_header(m, "message-id", &len, 1);
	if (hdr == NULL || len == 0 || len > INT_MAX)
		log_debug("%s: message-id not found", a->name);
	else {
		log_debug("%s: message-id is: %.*s", a->name, (int) len, hdr);
		add_tag(&m->tags, "message_id", "%.*s", (int) len, hdr);
	}

	/*
	 * Insert received header.
	 *
	 * No header line must exceed 998 bytes. Limiting the user-supplied
	 * stuff to 900 bytes gives plenty of space for the other stuff, and if
	 * it gets truncated, who cares?
	 */
	if (!conf.no_received) {
		error = 1;
		if (rfc822_time(time(NULL), rtm, sizeof rtm) != NULL) {
			rnm = conf.info.fqdn;
			if (rnm == NULL)
				rnm = conf.info.host;

			error = insert_header(m, "received",
			    "Received: by %.450s (%s " BUILD ", "
			    "account \"%.450s\");\n\t%s",
			    rnm, __progname, a->name, rtm);
		}
		if (error != 0) {
			log_debug("%s: failed to add received header",
			    a->name);
		}
	}

	/* fill wrapped line list */
	lines = fill_wrapped(m);
	log_debug2("%s: found %u wrapped lines", a->name, lines);

	/* handle rule evaluation and actions */
	mctx.matched = mctx.stopped = 0;
	if (do_rules(&mctx, &conf.rules, cause) != 0)
		return (FETCH_ERROR);
	if (mctx.stopped)
		goto done;

	/* reached end of ruleset. find implicit decision */
	switch (conf.impl_act) {
	case DECISION_NONE:
		log_warnx("%s: reached end of ruleset. no "
		    "unmatched-mail option; keeping mail",  a->name);
		m->decision = DECISION_KEEP;
		break;
	case DECISION_KEEP:
		log_debug("%s: reached end of ruleset. keeping mail",
		    a->name);
		m->decision = DECISION_KEEP;
		break;
	case DECISION_DROP:
		log_debug("%s: reached end of ruleset. dropping mail",
		    a->name);
		m->decision = DECISION_DROP;
		break;
	}

done:
	if (conf.keep_all || a->keep)
		m->decision = DECISION_KEEP;
	return (FETCH_SUCCESS);
}

int
do_rules(struct match_ctx *mctx, struct rules *rules, const char **cause)
{
	struct rule		*r;
	struct strings		*aa;
	u_int		 	 i;
	int		 	 error;
	char			*name, *tkey, *tvalue;
	struct account		*a = mctx->account;
	struct mail		*m = mctx->mail;

	TAILQ_FOREACH(r, rules, entry) {
		/* check if the rule is for the current account */
		aa = r->accounts;
		if (!ARRAY_EMPTY(aa)) {
			for (i = 0; i < ARRAY_LENGTH(aa); i++) {
				name = ARRAY_ITEM(aa, i, char *);
				if (name_match(name, a->name))
					break;
			}
			if (i == ARRAY_LENGTH(aa))
				continue;
		}

		/* match all the regexps */
		switch (r->type) {
		case RULE_EXPRESSION:
			/* combine wrapped lines */
			set_wrapped(m, ' ');

			/* perform the expression */
			if ((error = do_expr(r, mctx)) == -1) {
				*cause = "matching";
				return (1);
			}

			/* continue if no match */
			if (!error)
				continue;
			break;
		case RULE_ALL:
			break;
		}

		/* reset wrapped lines */
		set_wrapped(m, '\n');

		/* report rule number */
		if (TAILQ_EMPTY(&r->rules)) {
			log_debug("%s: matched message with rule %u", a->name,
			    r->idx);
		} else {
			log_debug("%s: matched message with rule %u (nested)",
			    a->name, r->idx);
		}

		/* tag mail if needed */
		if (r->key.str != NULL) {
			tkey = replacestr(&r->key, m->tags, m, &m->rml);
			tvalue = replacestr(&r->value, m->tags, m, &m->rml);

			if (tkey != NULL && *tkey != '\0' && tvalue != NULL) {
				log_debug2("%s: tagging message: %s (%s)", 
				    a->name, tkey, tvalue);
				add_tag(&m->tags, tkey, "%s", tvalue);
			}

			if (tkey != NULL)
				xfree(tkey);
			if (tvalue != NULL)
				xfree(tvalue);
		}

		/* handle delivery */
		if (r->actions != NULL) {
			log_debug2("%s: delivering message", a->name);
			mctx->matched = 1;
			if (do_deliver(r, mctx) != 0) {
				*cause = "delivery";
				return (1);
			}
		}

		/* deal with nested rules */
		if (!TAILQ_EMPTY(&r->rules)) {
			log_debug2("%s: entering nested rules", a->name);
			if (do_rules(mctx, &r->rules, cause) != 0)
				return (1);
			log_debug2("%s: exiting nested rules%s", a->name,
			    mctx->stopped ? ", and stopping" : "");
			/* if it didn't drop off the end of the nested rules,
			   stop now */
			if (mctx->stopped)
				return (0);
		}

		/* if this rule is marked as stop, stop checking now */
		if (r->stop) {
			mctx->stopped = 1;
			return (0);
		}
	}

	return (0);
}

int
do_expr(struct rule *r, struct match_ctx *mctx)
{
	int		 fres, cres;
	struct expritem	*ei;
	char		 desc[DESCBUFSIZE];

	fres = 0;
	TAILQ_FOREACH(ei, r->expr, entry) {
		cres = ei->match->match(mctx, ei);
		if (cres == MATCH_ERROR)
			return (-1);
		cres = cres == MATCH_TRUE;
		if (ei->inverted)
			cres = !cres;
		switch (ei->op) {
		case OP_NONE:
		case OP_OR:
			fres = fres || cres;
			break;
		case OP_AND:
			fres = fres && cres;
			break;
		}

		ei->match->desc(ei, desc, sizeof desc);
		log_debug2("%s: tried %s%s, got %d", mctx->account->name,
		    ei->inverted ? "not " : "", desc, cres);
	}

	return (fres);
}

int
do_deliver(struct rule *r, struct match_ctx *mctx)
{
	struct account	*a = mctx->account;
	struct mail	*m = mctx->mail;
 	struct action	*t;
	struct actions	*ta;
	u_int		 i, j;
	char		*s;
	struct replstr	*rs;

	if (r->actions == NULL)
		return (0);

	for (i = 0; i < ARRAY_LENGTH(r->actions); i++) {
		rs = &ARRAY_ITEM(r->actions, i, struct replstr);
		s = replacestr(rs, m->tags, m, &m->rml);

		log_debug2("%s: looking for actions matching: %s", a->name, s);
		ta = match_actions(s);
		if (ARRAY_EMPTY(ta)) {
			log_warnx("%s: no actions matching: %s (was %s)",
			    a->name, s, rs->str);
			xfree(s);
			ARRAY_FREEALL(ta);
			return (1);
		}
		xfree(s);

		log_debug2("%s: found %u actions", a->name, ARRAY_LENGTH(ta));
		for (j = 0; j < ARRAY_LENGTH(ta); j++) {
			t = ARRAY_ITEM(ta, j, struct action *);
			log_debug2("%s: action %s", a->name, t->name);
			if (do_action(r, mctx, t) != 0) {
				ARRAY_FREEALL(ta);
				return (1);
			}
		}

		ARRAY_FREEALL(ta);
	}

	return (0);
}

int
do_action(struct rule *r, struct match_ctx *mctx, struct action *t)
{
	struct account		*a = mctx->account;
	struct mail		*m = mctx->mail;
	struct msg	 	 msg;
	struct deliver_ctx	 dctx;
	u_int		 	 i, l;
	int		 	 find;
	struct strings	        *users;
	void			*buf;
	size_t			 len;

 	if (t->deliver->deliver == NULL)
		return (0);
	add_tag(&m->tags, "action", "%s", t->name);

	/* just deliver now for in-child delivery */
	if (t->deliver->type == DELIVER_INCHILD) {
		memset(&dctx, 0, sizeof dctx);
		dctx.account = a;
		dctx.mail = m;

		if (t->deliver->deliver(&dctx, t) != DELIVER_SUCCESS)
			return (1);
		return (0);
	}

	/* figure out the users to use */
	find = 0;
	users = NULL;
	if (r->find_uid) {		/* rule comes first */
		find = 1;
		users = find_users(m);
	} else if (r->users != NULL) {
		find = 0;
		users = r->users;
	} else if (t->find_uid) {	/* then action */
		find = 1;
		users = find_users(m);
	} else if (t->users != NULL) {
		find = 0;
		users = t->users;
	} else if (a->find_uid) {	/* then account */
		find = 1;
		users = find_users(m);
	} else if (a->users != NULL) {
		find = 0;
		users = a->users;
	}
	if (users == NULL) {
		find = 1;
		users = xmalloc(sizeof *users);
		ARRAY_INIT(users);
		ARRAY_ADD(users, conf.def_user, uid_t);
	}

	for (i = 0; i < ARRAY_LENGTH(users); i++) {
		memset(&msg, 0, sizeof msg);
		msg.type = MSG_ACTION;
		msg.data.account = a;
		msg.data.action = t;
		msg.data.uid = ARRAY_ITEM(users, i, uid_t);

		mail_send(m, &msg);

		if (privsep_send(mctx->io, &msg, m->tags, 
		    STRB_SIZE(m->tags)) != 0) 
			fatalx("child: privsep_send error");
		
		if (privsep_recv(mctx->io, &msg, &buf, &len) != 0)
			fatalx("child: privsep_recv error");
		if (msg.type != MSG_DONE)
			fatalx("child: unexpected message");

		if (buf == NULL || len == 0)
			fatalx("child: bad tags");
		strb_destroy(&m->tags);
		m->tags = buf;
		update_tags(&m->tags);

		if (msg.data.error != 0) {
			ARRAY_FREEALL(users);
			return (1);
		}

		if (t->deliver->type != DELIVER_WRBACK) {
			/* check everything that should be is the same */
			if (m->size != msg.data.mail.size ||
			    m->body != msg.data.mail.body)
				fatalx("child: corrupted message");
			continue;
		}

		mail_receive(m, &msg);
		log_debug("%s: received modified mail: size %zu, body %zd",
		    a->name, m->size, m->body);

		/* trim from line */
		trim_from(m);

		/* and recreate the wrapped array */
		l = fill_wrapped(m);
		log_debug2("%s: found %u wrapped lines", a->name, l);
	}

	if (find)
		ARRAY_FREEALL(users);

	return (0);
}
