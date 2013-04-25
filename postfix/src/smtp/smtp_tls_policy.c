/*++
/* NAME
/*	smtp_tls_policy 3
/* SUMMARY
/*	SMTP_TLS_POLICY structure management
/* SYNOPSIS
/*	#include "smtp.h"
/*
/*	void    smtp_tls_list_init()
/*
/*	int	smtp_tls_policy_cache_query(why, tls, iter)
/*	DSN_BUF	*why;
/*	SMTP_TLS_POLICY *tls;
/*	SMTP_ITERATOR *iter;
/*
/*	void	smtp_tls_policy_dummy(tls)
/*	SMTP_TLS_POLICY *tls;
/*
/*	void	smtp_tls_policy_cache_flush()
/* DESCRIPTION
/*	smtp_tls_list_init() initializes lookup tables used by the TLS
/*	policy engine.
/*
/*	smtp_tls_policy_cache_query() returns a shallow copy of the
/*	cached SMTP_TLS_POLICY structure for the iterator's
/*	destination, host, port and DNSSEC validation status.
/*	This copy is guaranteed to be valid until the next
/*	smtp_tls_policy_cache_query() or smtp_tls_policy_cache_flush()
/*	call.  The caller can override the TLS security level without
/*	corrupting the policy cache.
/*	When any required table or DNS lookups fail, the TLS level
/*	is set to TLS_LEV_INVALID, the "why" argument is updated
/*	with the error reason and the result value is zero (false).
/*
/*	smtp_tls_policy_dummy() initializes a trivial, non-cached,
/*	policy with TLS disabled.
/*
/*	smtp_tls_policy_cache_flush() destroys the TLS policy cache
/*	and contents.
/*
/*	Arguments:
/* .IP why
/*	A pointer to a DSN_BUF which holds error status information when
/*	the TLS policy lookup fails.
/* .IP tls
/*	Pointer to TLS policy storage.
/* .IP iter
/*	The literal next-hop or fall-back destination including
/*	the optional [] and including the :port or :service;
/*	the name of the remote host after MX and CNAME expansions
/*	(see smtp_cname_overrides_servername for the handling
/*	of hostnames that resolve to a CNAME record);
/*	the printable address of the remote host;
/*	the remote port in network byte order;
/*	the DNSSEC validation status of the host name lookup after
/*	MX and CNAME expansions.
/* LICENSE
/* .ad
/* .fi
/*	This software is free. You can do with it whatever you want.
/*	The original author kindly requests that you acknowledge
/*	the use of his software.
/* AUTHOR(S)
/*	TLS support originally by:
/*	Lutz Jaenicke
/*	BTU Cottbus
/*	Allgemeine Elektrotechnik
/*	Universitaetsplatz 3-4
/*	D-03044 Cottbus, Germany
/*
/*	Updated by:
/*	Wietse Venema
/*	IBM T.J. Watson Research
/*	P.O. Box 704
/*	Yorktown Heights, NY 10598, USA
/*
/*	Viktor Dukhovni
/*--*/

/* System library. */

#include <sys_defs.h>

#ifdef USE_TLS

#include <netinet/in.h>			/* ntohs() for Solaris or BSD */
#include <arpa/inet.h>			/* ntohs() for Linux or BSD */
#include <stdlib.h>
#include <string.h>

#ifdef STRCASECMP_IN_STRINGS_H
#include <strings.h>
#endif

/* Utility library. */

#include <msg.h>
#include <mymalloc.h>
#include <vstring.h>
#include <stringops.h>
#include <valid_hostname.h>
#include <ctable.h>

/* Global library. */

#include <mail_params.h>
#include <maps.h>

/* DNS library. */

#include <dns.h>

/* Application-specific. */

#include "smtp.h"

/* XXX Cache size should scale with [sl]mtp_mx_address_limit. */
#define CACHE_SIZE 20
static CTABLE *policy_cache;

static int global_tls_level(void);
static void dane_init(SMTP_TLS_POLICY *, SMTP_ITERATOR *);

static MAPS *tls_policy;		/* lookup table(s) */
static MAPS *tls_per_site;		/* lookup table(s) */

/* smtp_tls_list_init - initialize per-site policy lists */

void    smtp_tls_list_init(void)
{
    if (*var_smtp_tls_policy) {
	tls_policy = maps_create(SMTP_X(TLS_POLICY), var_smtp_tls_policy,
				 DICT_FLAG_LOCK | DICT_FLAG_FOLD_FIX);
	if (*var_smtp_tls_per_site)
	    msg_warn("%s ignored when %s is not empty.",
		     SMTP_X(TLS_PER_SITE), SMTP_X(TLS_POLICY));
	return;
    }
    if (*var_smtp_tls_per_site) {
	tls_per_site = maps_create(SMTP_X(TLS_PER_SITE), var_smtp_tls_per_site,
				   DICT_FLAG_LOCK | DICT_FLAG_FOLD_FIX);
    }
}

/* policy_name - printable tls policy level */

static const char *policy_name(int tls_level)
{
    const char *name = str_tls_level(tls_level);

    if (name == 0)
	name = "unknown";
    return name;
}

#define MARK_INVALID(why, levelp) do { \
	    dsb_simple((why), "4.7.5", "client TLS configuration problem"); \
	    *(levelp) = TLS_LEV_INVALID; } while (0)

/* tls_site_lookup - look up per-site TLS security level */

static void tls_site_lookup(SMTP_TLS_POLICY *tls, int *site_level,
		              const char *site_name, const char *site_class)
{
    const char *lookup;

    /*
     * Look up a non-default policy. In case of multiple lookup results, the
     * precedence order is a permutation of the TLS enforcement level order:
     * VERIFY, ENCRYPT, NONE, MAY, NOTFOUND. I.e. we override MAY with a more
     * specific policy including NONE, otherwise we choose the stronger
     * enforcement level.
     */
    if ((lookup = maps_find(tls_per_site, site_name, 0)) != 0) {
	if (!strcasecmp(lookup, "NONE")) {
	    /* NONE overrides MAY or NOTFOUND. */
	    if (*site_level <= TLS_LEV_MAY)
		*site_level = TLS_LEV_NONE;
	} else if (!strcasecmp(lookup, "MAY")) {
	    /* MAY overrides NOTFOUND but not NONE. */
	    if (*site_level < TLS_LEV_NONE)
		*site_level = TLS_LEV_MAY;
	} else if (!strcasecmp(lookup, "MUST_NOPEERMATCH")) {
	    if (*site_level < TLS_LEV_ENCRYPT)
		*site_level = TLS_LEV_ENCRYPT;
	} else if (!strcasecmp(lookup, "MUST")) {
	    if (*site_level < TLS_LEV_VERIFY)
		*site_level = TLS_LEV_VERIFY;
	} else {
	    msg_warn("%s: unknown TLS policy '%s' for %s %s",
		     tls_per_site->title, lookup, site_class, site_name);
	    MARK_INVALID(tls->why, site_level);
	    return;
	}
    } else if (tls_per_site->error) {
	msg_warn("%s: %s \"%s\": per-site table lookup error",
		 tls_per_site->title, site_class, site_name);
	dsb_simple(tls->why, "4.3.0", "Temporary lookup error");
	*site_level = TLS_LEV_INVALID;
	return;
    }
    return;
}

/* tls_policy_lookup_one - look up destination TLS policy */

static void tls_policy_lookup_one(SMTP_TLS_POLICY *tls, int *site_level,
				          const char *site_name,
				          const char *site_class)
{
    const char *lookup;
    char   *policy;
    char   *saved_policy;
    char   *tok;
    const char *err;
    char   *name;
    char   *val;
    static VSTRING *cbuf;

#undef FREE_RETURN
#define FREE_RETURN do { myfree(saved_policy); return; } while (0)

#define INVALID_RETURN(why, levelp) do { \
	    MARK_INVALID((why), (levelp)); FREE_RETURN; } while (0)

#define WHERE \
    STR(vstring_sprintf(cbuf, "%s, %s \"%s\"", \
		tls_policy->title, site_class, site_name))

    if (cbuf == 0)
	cbuf = vstring_alloc(10);

    if ((lookup = maps_find(tls_policy, site_name, 0)) == 0) {
	if (tls_policy->error) {
	    msg_warn("%s: policy table lookup error", WHERE);
	    MARK_INVALID(tls->why, site_level);
	}
	return;
    }
    saved_policy = policy = mystrdup(lookup);

    if ((tok = mystrtok(&policy, "\t\n\r ,")) == 0) {
	msg_warn("%s: invalid empty policy", WHERE);
	INVALID_RETURN(tls->why, site_level);
    }
    *site_level = tls_level_lookup(tok);
    if (*site_level == TLS_LEV_INVALID) {
	/* tls_level_lookup() logs no warning. */
	msg_warn("%s: invalid security level \"%s\"", WHERE, tok);
	INVALID_RETURN(tls->why, site_level);
    }

    /*
     * Warn about ignored attributes when TLS is disabled.
     */
    if (*site_level < TLS_LEV_MAY) {
	while ((tok = mystrtok(&policy, "\t\n\r ,")) != 0)
	    msg_warn("%s: ignoring attribute \"%s\" with TLS disabled",
		     WHERE, tok);
	FREE_RETURN;
    }

    /*
     * Levels that check certificates require or support explicit TA or EE
     * certificate digest match lists.
     */
    if (TLS_MUST_MATCH(*site_level))
	tls->dane = tls_dane_alloc(TLS_DANE_FLAG_MIXED);

    /*
     * Errors in attributes may have security consequences, don't ignore
     * errors that can degrade security.
     */
    while ((tok = mystrtok(&policy, "\t\n\r ,")) != 0) {
	if ((err = split_nameval(tok, &name, &val)) != 0) {
	    msg_warn("%s: malformed attribute/value pair \"%s\": %s",
		     WHERE, tok, err);
	    INVALID_RETURN(tls->why, site_level);
	}
	/* Only one instance per policy. */
	if (!strcasecmp(name, "ciphers")) {
	    if (*val == 0) {
		msg_warn("%s: attribute \"%s\" has empty value", WHERE, name);
		INVALID_RETURN(tls->why, site_level);
	    }
	    if (tls->grade) {
		msg_warn("%s: attribute \"%s\" is specified multiple times",
			 WHERE, name);
		INVALID_RETURN(tls->why, site_level);
	    }
	    tls->grade = mystrdup(val);
	    continue;
	}
	/* Only one instance per policy. */
	if (!strcasecmp(name, "protocols")) {
	    if (tls->protocols) {
		msg_warn("%s: attribute \"%s\" is specified multiple times",
			 WHERE, name);
		INVALID_RETURN(tls->why, site_level);
	    }
	    tls->protocols = mystrdup(val);
	    continue;
	}
	/* Multiple instances per policy. */
	if (!strcasecmp(name, "match")) {
	    if (*site_level <= TLS_LEV_ENCRYPT) {
		msg_warn("%s: attribute \"%s\" invalid at security level "
			 "\"%s\"", WHERE, name, policy_name(*site_level));
		INVALID_RETURN(tls->why, site_level);
	    }
	    if (*val == 0) {
		msg_warn("%s: attribute \"%s\" has empty value", WHERE, name);
		INVALID_RETURN(tls->why, site_level);
	    }
	    switch (*site_level) {
	    case TLS_LEV_DANE:
		if (tls->matchargv == 0)
		    tls->matchargv = argv_alloc(1);
		argv_add(tls->matchargv, val, ARGV_END);
		break;
	    case TLS_LEV_FPRINT:
		tls_dane_split(tls->dane, TLS_DANE_EE, TLS_DANE_PKEY,
			       var_smtp_tls_fpt_dgst, val, "|");
		break;
	    case TLS_LEV_VERIFY:
	    case TLS_LEV_SECURE:
		if (tls->matchargv == 0)
		    tls->matchargv = argv_split(val, ":");
		else
		    argv_split_append(tls->matchargv, val, ":");
		break;
	    }
	    continue;
	}
	/* Only one instance per policy. */
	if (!strcasecmp(name, "exclude")) {
	    if (tls->exclusions) {
		msg_warn("%s: attribute \"%s\" is specified multiple times",
			 WHERE, name);
		INVALID_RETURN(tls->why, site_level);
	    }
	    tls->exclusions = vstring_strcpy(vstring_alloc(10), val);
	    continue;
	}
	/* Multiple instances per policy. */
	if (!strcasecmp(name, "tafile")) {
	    /* Only makes sense if we're using CA-based trust */
	    if (!TLS_MUST_TRUST(*site_level)) {
		msg_warn("%s: attribute \"%s\" invalid at security level"
			 " \"%s\"", WHERE, name, policy_name(*site_level));
		INVALID_RETURN(tls->why, site_level);
	    }
	    if (*val == 0) {
		msg_warn("%s: attribute \"%s\" has empty value", WHERE, name);
		INVALID_RETURN(tls->why, site_level);
	    }
	    if (!tls_dane_load_trustfile(tls->dane, val)) {
		INVALID_RETURN(tls->why, site_level);
	    }
	    continue;
	}
	/* Only one instance per policy. */
	if (!strcasecmp(name, "notfound_tlsa_level")) {
	    if (*site_level != TLS_LEV_DANE) {
		msg_warn("%s: attribute \"%s\" invalid at security level"
			 " \"%s\"", WHERE, name, policy_name(*site_level));
		INVALID_RETURN(tls->why, site_level);
	    }
	    if (tls->dane_no_lev != TLS_LEV_NOTFOUND) {
		msg_warn("%s: attribute \"%s\" is specified multiple times",
			 WHERE, name);
		INVALID_RETURN(tls->why, site_level);
	    }
	    switch (tls->dane_no_lev = tls_level_lookup(val)) {
	    case TLS_LEV_INVALID:
	    case TLS_LEV_DANE:
		msg_warn("%s: invalid \"%s\" attribute value: \"%s\"",
			 WHERE, name, val);
		INVALID_RETURN(tls->why, site_level);
	    }
	    continue;
	}
	/* Only one instance per policy. */
	if (!strcasecmp(name, "unusable_tlsa_level")) {
	    if (*site_level != TLS_LEV_DANE) {
		msg_warn("%s: attribute \"%s\" invalid at security level"
			 " \"%s\"", WHERE, name, policy_name(*site_level));
		INVALID_RETURN(tls->why, site_level);
	    }
	    if (tls->dane_un_lev != TLS_LEV_NOTFOUND) {
		msg_warn("%s: attribute \"%s\" is specified multiple times",
			 WHERE, name);
		INVALID_RETURN(tls->why, site_level);
	    }
	    switch (tls->dane_un_lev = tls_level_lookup(val)) {
	    case TLS_LEV_INVALID:
	    case TLS_LEV_DANE:
		msg_warn("%s: invalid \"%s\" attribute value: \"%s\"",
			 WHERE, name, val);
		INVALID_RETURN(tls->why, site_level);
	    }
	    continue;
	}
	msg_warn("%s: invalid attribute name: \"%s\"", WHERE, name);
	INVALID_RETURN(tls->why, site_level);
    }

    FREE_RETURN;
}

/* tls_policy_lookup - look up destination TLS policy */

static void tls_policy_lookup(SMTP_TLS_POLICY *tls, int *site_level,
			              const char *site_name,
			              const char *site_class)
{

    /*
     * Only one lookup with [nexthop]:port, [nexthop] or nexthop:port These
     * are never the domain part of localpart@domain, rather they are
     * explicit nexthops from transport:nexthop, and match only the
     * corresponding policy. Parent domain matching (below) applies only to
     * sub-domains of the recipient domain.
     * 
     * XXX UNIX-domain connections query with the pathname as destination.
     */
    if (!valid_hostname(site_name, DONT_GRIPE)) {
	tls_policy_lookup_one(tls, site_level, site_name, site_class);
	return;
    }
    do {
	tls_policy_lookup_one(tls, site_level, site_name, site_class);
    } while (*site_level == TLS_LEV_NOTFOUND
	     && (site_name = strchr(site_name + 1, '.')) != 0);
}

/* load_tas - load one or more ta files */

static int load_tas(TLS_DANE *dane, const char *files)
{
    int     ret = 0;
    char   *save = mystrdup(files);
    char   *buf = save;
    char   *file;

    do {
	if ((file = mystrtok(&buf, "\t\n\r ,")) != 0)
	    ret = tls_dane_load_trustfile(dane, file);
    } while (file && ret);

    myfree(save);
    return (ret);
}

/* set_cipher_grade - Set cipher grade and exclusions */

static void set_cipher_grade(SMTP_TLS_POLICY *tls)
{
    const char *mand_exclude = "";
    const char *also_exclude = "";

    /*
     * Use main.cf cipher level if no per-destination value specified. With
     * mandatory encryption at least encrypt, and with mandatory verification
     * at least authenticate!
     */
    switch (tls->level) {
    case TLS_LEV_INVALID:
    case TLS_LEV_NONE:
	return;

    case TLS_LEV_MAY:
	if (tls->grade == 0)
	    tls->grade = mystrdup(var_smtp_tls_ciph);
	break;

    case TLS_LEV_ENCRYPT:
	if (tls->grade == 0)
	    tls->grade = mystrdup(var_smtp_tls_mand_ciph);
	mand_exclude = var_smtp_tls_mand_excl;
	also_exclude = "eNULL";
	break;

    case TLS_LEV_DANE:
    case TLS_LEV_FPRINT:
    case TLS_LEV_VERIFY:
    case TLS_LEV_SECURE:
	if (tls->grade == 0)
	    tls->grade = mystrdup(var_smtp_tls_mand_ciph);
	mand_exclude = var_smtp_tls_mand_excl;
	also_exclude = "aNULL";
	break;
    }

#define ADD_EXCLUDE(vstr, str) \
    do { \
	if (*(str)) \
	    vstring_sprintf_append((vstr), "%s%s", \
				   VSTRING_LEN(vstr) ? " " : "", (str)); \
    } while (0)

    /*
     * The "exclude" policy table attribute overrides main.cf exclusion
     * lists.
     */
    if (tls->exclusions == 0) {
	tls->exclusions = vstring_alloc(10);
	ADD_EXCLUDE(tls->exclusions, var_smtp_tls_excl_ciph);
	ADD_EXCLUDE(tls->exclusions, mand_exclude);
    }
    ADD_EXCLUDE(tls->exclusions, also_exclude);
}

/* policy_create - create SMTP TLS policy cache object (ctable call-back) */

static void *policy_create(const char *unused_key, void *context)
{
    SMTP_ITERATOR *iter = (SMTP_ITERATOR *) context;
    int     site_level;
    const char *dest = STR(iter->dest);
    const char *host = STR(iter->host);

    /*
     * Prepare a pristine policy object.
     */
    SMTP_TLS_POLICY *tls = (SMTP_TLS_POLICY *) mymalloc(sizeof(*tls));

    smtp_tls_policy_init(tls, dsb_create());

    /*
     * Compute the per-site TLS enforcement level. For compatibility with the
     * original TLS patch, this algorithm is gives equal precedence to host
     * and next-hop policies.
     */
    tls->level = global_tls_level();
    site_level = TLS_LEV_NOTFOUND;

    if (tls_policy) {
	tls_policy_lookup(tls, &site_level, dest, "next-hop destination");
    } else if (tls_per_site) {
	tls_site_lookup(tls, &site_level, dest, "next-hop destination");
	if (site_level != TLS_LEV_INVALID
	    && strcasecmp(dest, host) != 0)
	    tls_site_lookup(tls, &site_level, host, "server hostname");

	/*
	 * Override a wild-card per-site policy with a more specific global
	 * policy.
	 * 
	 * With the original TLS patch, 1) a per-site ENCRYPT could not override
	 * a global VERIFY, and 2) a combined per-site (NONE+MAY) policy
	 * produced inconsistent results: it changed a global VERIFY into
	 * NONE, while producing MAY with all weaker global policy settings.
	 * 
	 * With the current implementation, a combined per-site (NONE+MAY)
	 * consistently overrides global policy with NONE, and global policy
	 * can override only a per-site MAY wildcard. That is, specific
	 * policies consistently override wildcard policies, and
	 * (non-wildcard) per-site policies consistently override global
	 * policies.
	 */
	if (site_level == TLS_LEV_MAY && tls->level > TLS_LEV_MAY)
	    site_level = tls->level;
    }
    switch (site_level) {
    default:
	tls->level = site_level;
    case TLS_LEV_NOTFOUND:
	break;
    case TLS_LEV_INVALID:
	return ((void *) tls);
    }

    /*
     * DANE initialization may change the security level to something else,
     * so do this early, so that we use the right level below.
     */
    if (tls->level == TLS_LEV_DANE)
	dane_init(tls, iter);
    if (tls->level == TLS_LEV_INVALID)
	return ((void *) tls);

    /*
     * Use main.cf protocols setting if not set in per-destination table.
     */
    if (tls->level > TLS_LEV_NONE && tls->protocols == 0)
	tls->protocols =
	    mystrdup((tls->level == TLS_LEV_MAY) ?
		     var_smtp_tls_proto : var_smtp_tls_mand_proto);

    /*
     * Compute cipher grade (if set in per-destination table, else
     * set_cipher() uses main.cf settings) and security level dependent
     * cipher exclusion list.
     */
    set_cipher_grade(tls);

    /*
     * Use main.cf cert_match setting if not set in per-destination table.
     */
    switch (tls->level) {
    case TLS_LEV_INVALID:
    case TLS_LEV_NONE:
    case TLS_LEV_MAY:
    case TLS_LEV_ENCRYPT:
    case TLS_LEV_DANE:
	break;
    case TLS_LEV_FPRINT:
	if (tls->dane == 0)
	    tls->dane = tls_dane_alloc(TLS_DANE_FLAG_MIXED);
	if (!TLS_DANE_HASEE(tls->dane)) {
	    tls_dane_split(tls->dane, TLS_DANE_EE, TLS_DANE_PKEY,
			   var_smtp_tls_fpt_dgst, var_smtp_tls_fpt_cmatch,
			   "\t\n\r, ");
	    if (!TLS_DANE_HASEE(tls->dane)) {
		msg_warn("nexthop domain %s: configured at fingerprint "
		       "security level, but with no fingerprints to match.",
			 dest);
		MARK_INVALID(tls->why, &tls->level);
		return ((void *) tls);
	    }
	}
	tls_dane_final(tls->dane);
	break;
    case TLS_LEV_VERIFY:
    case TLS_LEV_SECURE:
	if (tls->matchargv == 0)
	    tls->matchargv =
		argv_split(tls->level == TLS_LEV_VERIFY ?
			   var_smtp_tls_vfy_cmatch : var_smtp_tls_sec_cmatch,
			   "\t\n\r, :");
	if (*var_smtp_tls_tafile) {
	    if (tls->dane == 0)
		tls->dane = tls_dane_alloc(TLS_DANE_FLAG_MIXED);
	    if (!TLS_DANE_HASTA(tls->dane)) {
		if (load_tas(tls->dane, var_smtp_tls_tafile))
		    tls_dane_final(tls->dane);
		else {
		    MARK_INVALID(tls->why, &tls->level);
		    return ((void *) tls);
		}
	    }
	}
	break;
    default:
	msg_panic("unexpected TLS security level: %d", tls->level);
    }

    if (msg_verbose && tls->level != global_tls_level())
	msg_info("%s TLS level: %s", "effective", policy_name(tls->level));

    return ((void *) tls);
}

/* policy_delete - free no longer cached policy (ctable call-back) */

static void policy_delete(void *item, void *unused_context)
{
    SMTP_TLS_POLICY *tls = (SMTP_TLS_POLICY *) item;

    if (tls->protocols)
	myfree(tls->protocols);
    if (tls->grade)
	myfree(tls->grade);
    if (tls->exclusions)
	vstring_free(tls->exclusions);
    if (tls->matchargv)
	argv_free(tls->matchargv);
    if (tls->dane)
	tls_dane_free(tls->dane);
    dsb_free(tls->why);

    myfree((char *) tls);
}

/* smtp_tls_policy_cache_query - cached lookup of TLS policy */

int     smtp_tls_policy_cache_query(DSN_BUF *why, SMTP_TLS_POLICY *tls,
				            SMTP_ITERATOR *iter)
{
    VSTRING *key;
    int     valid = iter->rr && iter->rr->dnssec_valid;

    /*
     * Create an empty TLS Policy cache on the fly.
     */
    if (policy_cache == 0)
	policy_cache =
	    ctable_create(CACHE_SIZE, policy_create, policy_delete, (void *) 0);

    /*
     * Query the TLS Policy cache, with a search key that reflects our shared
     * values that also appear in other cache and table search keys.
     */
    key = vstring_alloc(100);
    smtp_key_prefix(key, ":", iter, SMTP_KEY_FLAG_NEXTHOP
		    | SMTP_KEY_FLAG_HOSTNAME
		    | SMTP_KEY_FLAG_PORT);
    vstring_sprintf_append(key, "%d", !!valid);
    ctable_newcontext(policy_cache, (void *) iter);
    *tls = *(SMTP_TLS_POLICY *) ctable_locate(policy_cache, STR(key));
    vstring_free(key);

    /*
     * Report errors. Both error and non-error results are cached. We must
     * therefore copy the cached DSN buffer content to the caller's buffer.
     */
    if (tls->level == TLS_LEV_INVALID) {
	/* XXX Simplify this by implementing a "copy" primitive. */
	dsb_update(why,
		   STR(tls->why->status), STR(tls->why->action),
		   STR(tls->why->mtype), STR(tls->why->mname),
		   STR(tls->why->dtype), STR(tls->why->dtext),
		   "%s", STR(tls->why->reason));
	return (0);
    } else {
	return (1);
    }
}

/* smtp_tls_policy_cache_flush - flush TLS policy cache */

void    smtp_tls_policy_cache_flush(void)
{
    if (policy_cache != 0) {
	ctable_free(policy_cache);
	policy_cache = 0;
    }
}

/* dane_no_level - parse and cache var_smtp_tls_dane_no_lev */

static int dane_no_level(void)
{
    static int l = TLS_LEV_NOTFOUND;

    if (l != TLS_LEV_NOTFOUND)
	return l;

    l = tls_level_lookup(var_smtp_tls_dane_no_lev);
    if (l == TLS_LEV_INVALID || l == TLS_LEV_DANE)
	msg_fatal("invalid %s: \"%s\"", SMTP_X(TLS_DANE_NO_LEV),
		  var_smtp_tls_dane_no_lev);

    if (msg_verbose)
	msg_info("%s TLS level: %s", "DANE no_tlsa", policy_name(l));

    return l;
}

/* dane_un_level - parse and cache var_smtp_tls_dane_un_lev */

static int dane_un_level(void)
{
    static int l = TLS_LEV_NOTFOUND;

    if (l != TLS_LEV_NOTFOUND)
	return l;

    l = tls_level_lookup(var_smtp_tls_dane_un_lev);
    if (l == TLS_LEV_INVALID || l == TLS_LEV_DANE)
	msg_fatal("invalid %s: \"%s\"", SMTP_X(TLS_DANE_UN_LEV),
		  var_smtp_tls_dane_un_lev);

    if (msg_verbose)
	msg_info("%s TLS level: %s", "DANE unusable_tlsa", policy_name(l));

    return l;
}

/* global_tls_level - parse and cache var_smtp_tls_level */

static int global_tls_level(void)
{
    static int l = TLS_LEV_NOTFOUND;

    if (l != TLS_LEV_NOTFOUND)
	return l;

    /*
     * Compute the global TLS policy. This is the default policy level when
     * no per-site policy exists. It also is used to override a wild-card
     * per-site policy.
     * 
     * We require that the global level is valid on startup.
     */
    if (*var_smtp_tls_level) {
	if ((l = tls_level_lookup(var_smtp_tls_level)) == TLS_LEV_INVALID)
	    msg_fatal("invalid tls security level: \"%s\"", var_smtp_tls_level);
    } else if (var_smtp_enforce_tls)
	l = var_smtp_tls_enforce_peername ? TLS_LEV_VERIFY : TLS_LEV_ENCRYPT;
    else
	l = var_smtp_use_tls ? TLS_LEV_MAY : TLS_LEV_NONE;

    if (msg_verbose)
	msg_info("%s TLS level: %s", "global", policy_name(l));

    return l;
}

/* dane_init - special initialization for "dane" security level */

static void dane_init(SMTP_TLS_POLICY *tls, SMTP_ITERATOR *iter)
{
    TLS_DANE *dane;
    int     valid = iter->rr && iter->rr->dnssec_valid;

    if (!iter->port) {
	msg_warn("%s: the \"dane\" security level is invalid for delivery via"
		 " unix-domain sockets", STR(iter->dest));
	MARK_INVALID(tls->why, &tls->level);
	return;
    }
    /*-
     * Some senders will want (destination-dependent) mandatory TLS with key
     * management via DANE.  When TLSA lookups are not possible, use the
     * locally specified "degraded" levels.
     *
     * This is supported via the policy table:
     *
     * tls_policy:
     *
     *     # If TLSA records vanish, force TLS without verification
     *     example.com dane notfound_tlsa_level=encrypt
     *
     *     # If TLSA records vanish, resort to public CA trust
     *     example.net dane notfound_tlsa_level=secure
     *		unusable_tlsa_level=secure match=example.net
     *
     * or in isolated cases via global settings:
     *
     * main.cf:
     *     relayhost = [secure.example.com]
     *     smtp_tls_security_level = dane
     *     smtp_tls_dane_tlsa_notfound_level = secure
     *     smtp_tls_dane_tlsa_unusable_level = secure
     *     smtp_tls_CAfile = /etc/ssl/certs/someCA.pem
     */
    if (tls->dane_no_lev == TLS_LEV_NOTFOUND)
	tls->dane_no_lev = dane_no_level();
    if (tls->dane_un_lev == TLS_LEV_NOTFOUND)
	tls->dane_un_lev = dane_un_level();

    /*
     * To use DANE security the requisite features must be provided by the
     * OpenSSL (runtime) and DNS resolver (compile-time) libraries. Also,
     * "smtp_host_lookup = dns" and "smtp_dns_support_level = dnssec" are
     * required.
     * 
     * If DANE is not available we log a warning and fall back to the security
     * level for the TLSA records not found case.
     * 
     * XXX: We could get the administrator's attention by generating a lookup
     * failure.  In that case all mail, or mail to some sites will tempfail
     * and stay in the queue.  This will make it harder to accidentally
     * deploy insecure configurations, but easier to have no mail going
     * anywhere, security or ease of use, pick one.
     * 
     * XXX: Should there be a parameter controlling this choice?
     */
#define DANECARP(reason, iter, tls) \
	msg_warn("%s: %s, the security level for delivery via %s:%u will " \
		 "degrade to \"%s\"", STR((iter)->dest), reason, \
		 STR((iter)->host), ntohs((iter)->port), \
		 policy_name((tls)->level))
    if (!tls_dane_avail()) {
	tls->level = tls->dane_no_lev;
	DANECARP("DANE not available", iter, tls);
	return;
    }
    if (!(smtp_host_lookup_mask & SMTP_HOST_FLAG_DNS)
	|| smtp_dns_support != SMTP_DNS_DNSSEC) {
	tls->level = tls->dane_no_lev;
	DANECARP("DNSSEC hostname lookups not enabled", iter, tls);
	return;
    }
    if (!valid) {
	tls->level = tls->dane_no_lev;
	if (msg_verbose)
	    DANECARP("non-DNSSEC destination", iter, tls);
	return;
    }
    /* When TLSA lookups fail, we defer the message */
    if ((dane = tls_dane_resolve(STR(iter->host), "tcp", iter->port)) == 0) {
	tls->level = TLS_LEV_INVALID;
	dsb_simple(tls->why, "4.7.5", "TLSA lookup error for %s:%u",
		   STR(iter->host), ntohs(iter->port));
	return;
    }
    if (tls_dane_notfound(dane)) {
	tls->level = tls->dane_no_lev;
	if (msg_verbose)
	    DANECARP("no TLSA records found", iter, tls);
	return;
    }

    /*
     * Some TLSA records found, but none usable, per
     * 
     * https://tools.ietf.org/html/draft-ietf-dane-srv-02#section-4
     * 
     * we MUST use TLS, and SHALL use full PKIX certificate checks.  The latter
     * would be unwise for SMTP: no human present to "click ok" and risk of
     * non-delivery in most cases exceeds risk of interception.
     * 
     * We also have a form of Goedel's incompleteness theorem in play: any list
     * of public root CA certs is either incomplete or inconsistent (for any
     * given verifier some of the CAs are surely not trustworthy).
     */
    if (tls_dane_unusable(dane)) {
	tls->level = tls->dane_un_lev;
	DANECARP("all TLSA records unusable", iter, tls);
	return;
    }
    /* Going with DANE. Free up dane-like settings for fallback levels. */
    if (tls->dane)
	tls_dane_free(tls->dane);

    /*
     * With DANE trust anchors, peername matching is not configurable.
     */
    if (tls->matchargv)
	argv_free(tls->matchargv);
    if (TLS_DANE_HASTA(dane)) {
	tls->matchargv = argv_alloc(2);
	argv_add(tls->matchargv, "hostname", "nexthop", ARGV_END);
    } else
	tls->matchargv = 0;
    tls->dane = dane;
    return;
}

#endif
