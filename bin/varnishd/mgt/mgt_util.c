/*-
 * Copyright (c) 2006 Verdens Gang AS
 * Copyright (c) 2006-2011 Varnish Software AS
 * All rights reserved.
 *
 * Author: Poul-Henning Kamp <phk@phk.freebsd.dk>
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY THE AUTHOR AND CONTRIBUTORS ``AS IS'' AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED.  IN NO EVENT SHALL AUTHOR OR CONTRIBUTORS BE LIABLE
 * FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS
 * OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 * HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY
 * OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 *
 * The management process, various utility functions
 */

#include "config.h"

#include <sys/utsname.h>

#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include <syslog.h>

#include "mgt/mgt.h"


/*--------------------------------------------------------------------*/

static void
mgt_sltm(const char *tag, const char *sdesc, const char *ldesc)
{
	int i;

	assert(sdesc != NULL && ldesc != NULL);
	assert(*sdesc != '\0' || *ldesc != '\0');
	printf("\n%s\n", tag);
	i = strlen(tag);
	printf("%*.*s\n\n", i, i, "------------------------------------");
	if (*ldesc != '\0')
		printf("%s\n", ldesc);
	else if (*sdesc != '\0')
		printf("%s\n", sdesc);
}

/*lint -e{506} constant value boolean */
void
mgt_DumpRstVsl(void)
{

	printf(
	    "\n.. The following is autogenerated output from "
	    "varnishd -x dumprstvsl\n\n");

#define SLTM(tag, flags, sdesc, ldesc) mgt_sltm(#tag, sdesc, ldesc);
#include "tbl/vsl_tags.h"
}

/*--------------------------------------------------------------------*/

struct vsb *
mgt_BuildVident(void)
{
	struct utsname uts;
	struct vsb *vsb;

	vsb = VSB_new_auto();
	AN(vsb);
	if (!uname(&uts)) {
		VSB_printf(vsb, ",%s", uts.sysname);
		VSB_printf(vsb, ",%s", uts.release);
		VSB_printf(vsb, ",%s", uts.machine);
	}
	return (vsb);
}

/*--------------------------------------------------------------------
 * 'Ello, I wish to register a complaint...
 */

#ifndef LOG_AUTHPRIV
#  define LOG_AUTHPRIV 0
#endif

const char C_ERR[] = "Error:";
const char C_INFO[] = "Info:";
const char C_DEBUG[] = "Debug:";
const char C_SECURITY[] = "Security:";
const char C_CLI[] = "Cli:";

void
MGT_Complain(const char *loud, const char *fmt, ...)
{
	va_list ap;
	struct vsb *vsb;
	int sf;

	if (loud == C_CLI && !mgt_param.syslog_cli_traffic)
		return;
	vsb = VSB_new_auto();
	AN(vsb);
	va_start(ap, fmt);
	VSB_vprintf(vsb, fmt, ap);
	va_end(ap);
	AZ(VSB_finish(vsb));

	if (loud == C_ERR)
		sf = LOG_ERR;
	else if (loud == C_INFO)
		sf = LOG_INFO;
	else if (loud == C_DEBUG)
		sf = LOG_DEBUG;
	else if (loud == C_SECURITY)
		sf = LOG_WARNING | LOG_AUTHPRIV;
	else if (loud == C_CLI)
		sf = LOG_INFO;
	else
		WRONG("Wrong complaint loudness");

	if (loud != C_CLI)
		fprintf(stderr, "%s %s\n", loud, VSB_data(vsb));

	if (!MGT_DO_DEBUG(DBG_VTC_MODE))
		syslog(sf, "%s", VSB_data(vsb));
	VSB_destroy(&vsb);
}

/*--------------------------------------------------------------------*/

const void *
MGT_Pick(const struct choice *cp, const char *which, const char *kind)
{

	for(; cp->name != NULL; cp++) {
		if (!strcmp(cp->name, which))
			return (cp->ptr);
	}
	ARGV_ERR("Unknown %s method \"%s\"\n", kind, which);
}

/*--------------------------------------------------------------------
 * All praise POSIX!  Thanks to our glorious standards there are no
 * standard way to get a back-trace of the stack, and even if we hack
 * that together from spit and pieces of string, there is no way no
 * standard way to translate a pointer to a symbol, which returns anything
 * usable.  (See for instance FreeBSD PR-134391).
 *
 * Attempt to run nm(1) on our binary during startup, hoping it will
 * give us a usable list of symbols.
 */

struct symbols {
	uintptr_t		a;
	uintptr_t		l;
	char			*n;
	VTAILQ_ENTRY(symbols)	list;
};

static VTAILQ_HEAD(,symbols) symbols = VTAILQ_HEAD_INITIALIZER(symbols);

int
Symbol_Lookup(struct vsb *vsb, void *ptr)
{
	struct symbols *s, *s0;
	uintptr_t pp;

	pp = (uintptr_t)ptr;
	s0 = NULL;
	VTAILQ_FOREACH(s, &symbols, list) {
		if (s->a > pp || s->a + s->l <= pp)
			continue;
		if (s0 == NULL || s->l < s0->l)
			s0 = s;
	}
	if (s0 == NULL)
		return (-1);
	VSB_printf(vsb, "%p: %s", ptr, s0->n);
	if ((uintmax_t)pp != s0->a)
		VSB_printf(vsb, "+0x%jx", (uintmax_t)pp - s0->a);
	return (0);
}

void
mgt_SymbolHack(const char *a0)
{
	char buf[BUFSIZ];
	FILE *fi;
	struct symbols *s;
	uintmax_t aa, ll;
	char type[10];
	char name[100];
	int i;

	bprintf(buf, "nm -t x -n -P %s 2>/dev/null", a0);
	fi = popen(buf, "r");
	if (fi == NULL)
		return;
	while (fgets(buf, sizeof buf, fi)) {
		i = sscanf(buf, "%99s\t%9s\t%jx\t%jx\n", name, type, &aa, &ll);
		if (i != 4)
			continue;
		s = malloc(sizeof *s + strlen(name) + 1);
		AN(s);
		s->a = aa;
		s->l = ll;
		s->n = (void*)(s + 1);
		strcpy(s->n, name);
		VTAILQ_INSERT_TAIL(&symbols, s, list);
	}
	(void)pclose(fi);
}