#include "system.h"

#if HAVE_SYS_SYSTEMCFG_H
#include <sys/systemcfg.h>
#else
#define __power_pc() 0
#endif

#include "rpmlib.h"
#include "rpmmacro.h"

#include "misc.h"
#include "intl.h"

struct MacroContext globalMacroContext;

struct machCacheEntry {
    char * name;
    int count;
    char ** equivs;
    int visited;
};

struct machCache {
    struct machCacheEntry * cache;
    int size;
};

struct machEquivInfo {
    char * name;
    int score;
};

struct machEquivTable {
    int count;
    struct machEquivInfo * list;
};

struct rpmvarValue {
    char * value;
    /* eventually, this arch will be replaced with a generic condition */
    char * arch;
    struct rpmvarValue * next;
};

struct rpmOption {
    char * name;
    int var;
    int archSpecific, required, macroize, localize;
    struct rpmOptionValue * value;
};

struct defaultEntry {
    char *name;
    char *defName;
};

struct canonEntry {
    char *name;
    char *short_name;
    short num;
};

/* tags are 'key'canon, 'key'translate, 'key'compat

   for giggles, 'key'_canon, 'key'_compat, and 'key'_canon will also work */
struct tableType {
    char * key;
    int hasCanon, hasTranslate;
    struct machEquivTable equiv;
    struct machCache cache;
    struct defaultEntry * defaults;
    struct canonEntry * canons;
    int defaultsLength;
    int canonsLength;
};

static struct tableType tables[RPM_MACHTABLE_COUNT] = {
    { "arch", 1, 0 },
    { "os", 1, 0 },
    { "buildarch", 0, 1 },
    { "buildos", 0, 1 }
};

/* this *must* be kept in alphabetical order */
static struct rpmOption optionTable[] = {
    { "arch",			RPMVAR_ARCH,			0, 1,	1, 2 },
    { "builddir",		RPMVAR_BUILDDIR,		0, 0,	1, 1 },
    { "buildplatform",		RPMVAR_BUILDPLATFORM,           0, 1,	1, 2 },
    { "buildroot",              RPMVAR_BUILDROOT,               0, 0,	1, 0 },
    { "buildshell",             RPMVAR_BUILDSHELL,              0, 0,	1, 0 },
    { "bzip2bin",		RPMVAR_BZIP2BIN,		0, 1,	1, 2 },
    { "dbpath",			RPMVAR_DBPATH,			0, 1,	1, 2 },
    { "defaultdocdir",		RPMVAR_DEFAULTDOCDIR,		0, 0,	1, 1 },
    { "distribution",		RPMVAR_DISTRIBUTION,		0, 0,	1, 0 },
    { "excludedocs",	        RPMVAR_EXCLUDEDOCS,             0, 0,	1, 0 },
    { "fixperms",		RPMVAR_FIXPERMS,		0, 1,	1, 2 },
    { "ftpport",		RPMVAR_FTPPORT,			0, 0,	1, 0 },
    { "ftpproxy",		RPMVAR_FTPPROXY,		0, 0,	1, 0 },
    { "gzipbin",		RPMVAR_GZIPBIN,			0, 1,	1, 2 },
    { "include",		RPMVAR_INCLUDE,			0, 1,	1, 2 },
    { "instchangelog",		RPMVAR_INSTCHANGELOG,		0, 0,	0, 0 },
    { "langpatt",               RPMVAR_LANGPATT,                0, 0,	1, 0 },
    { "messagelevel",		RPMVAR_MESSAGELEVEL,		0, 0,	1, 0 },
    { "netsharedpath",		RPMVAR_NETSHAREDPATH,		0, 0,	1, 0 },
    { "optflags",		RPMVAR_OPTFLAGS,		1, 0,	1, 0 },
    { "os",			RPMVAR_OS,                      0, 1,	1, 2 },
    { "packager",               RPMVAR_PACKAGER,                0, 0,	1, 0 },
    { "pgp_name",               RPMVAR_PGP_NAME,                0, 0,	1, 0 },
    { "pgp_path",               RPMVAR_PGP_PATH,                0, 0,	1, 0 },
    { "provides",               RPMVAR_PROVIDES,                0, 0,	1, 0 },
    { "require_distribution",	RPMVAR_REQUIREDISTRIBUTION,	0, 0,	1, 0 },
    { "require_icon",		RPMVAR_REQUIREICON,		0, 0,	1, 0 },
    { "require_vendor",		RPMVAR_REQUIREVENDOR,		0, 0,	1, 0 },
/* root is obsolete - use buildroot instead                             */
/*    { "root",			RPMVAR_ROOT,			0, 0,	1, 0 }, */
    { "rpmdir",			RPMVAR_RPMDIR,			0, 0,	1, 1 },
    { "rpmfilename",		RPMVAR_RPMFILENAME,		0, 1,	1, 2 },
    { "signature",		RPMVAR_SIGTYPE,			0, 0,	1, 0 },
    { "sourcedir",		RPMVAR_SOURCEDIR,		0, 0,	1, 1 },
    { "specdir",		RPMVAR_SPECDIR,			0, 0,	1, 1 },
    { "srcrpmdir",		RPMVAR_SRPMDIR,			0, 0,	1, 1 },
    { "timecheck",		RPMVAR_TIMECHECK,		0, 0,	1, 0 },
    { "tmppath",		RPMVAR_TMPPATH,			0, 1,	1, 2 },
    { "topdir",			RPMVAR_TOPDIR,			0, 0,	1, 1 },
    { "vendor",			RPMVAR_VENDOR,			0, 0,	1, 1 },
};
static int optionTableSize = sizeof(optionTable) / sizeof(*optionTable);

#define OS	0
#define ARCH	1

static char * current[2];
static int currTables[2] = { RPM_MACHTABLE_INSTOS, RPM_MACHTABLE_INSTARCH };
static struct rpmvarValue values[RPMVAR_NUM];

/* prototypes */
static void defaultMachine(char ** arch, char ** os);
static int doReadRC(int fd, char * filename);
static int optionCompare(const void * a, const void * b);
static int addCanon(struct canonEntry **table, int *tableLen, char *line,
		   char *fn, int lineNum);
static int addDefault(struct defaultEntry **table, int *tableLen, char *line,
		      char *fn, int lineNum);
static void freeRpmVar(struct rpmvarValue * orig);
static void rpmSetVarArch(int var, char * val, char * arch);
static struct canonEntry *lookupInCanonTable(char *name,
					   struct canonEntry *table,
					   int tableLen);
static char *lookupInDefaultTable(char *name,
				  struct defaultEntry *table,
				  int tableLen);
static void setDefaults(void);
static void setPathDefault(int var, char * macroname, char * subdir);
static void rebuildCompatTables(int type, char * name);

/* compatiblity tables */
static int machCompatCacheAdd(char * name, char * fn, int linenum,
				struct machCache * cache);
static struct machCacheEntry * machCacheFindEntry(struct machCache * cache,
						  char * key);
static struct machEquivInfo * machEquivSearch(
		struct machEquivTable * table, char * name);
static void machAddEquiv(struct machEquivTable * table, char * name,
			   int distance);
static void machCacheEntryVisit(struct machCache * cache,
				  struct machEquivTable * table,
				  char * name,
	  			  int distance);
static void machFindEquivs(struct machCache * cache,
			     struct machEquivTable * table,
			     char * key);

static int optionCompare(const void * a, const void * b) {
    return strcasecmp(((struct rpmOption *) a)->name,
		      ((struct rpmOption *) b)->name);
}



static struct machCacheEntry * machCacheFindEntry(struct machCache * cache,
						  char * key) {
    int i;

    for (i = 0; i < cache->size; i++)
	if (!strcmp(cache->cache[i].name, key)) return cache->cache + i;

    return NULL;
}

static int machCompatCacheAdd(char * name, char * fn, int linenum,
				struct machCache * cache) {
    char * chptr, * equivs;
    int delEntry = 0;
    int i;
    struct machCacheEntry * entry = NULL;

    while (*name && isspace(*name)) name++;

    chptr = name;
    while (*chptr && *chptr != ':') chptr++;
    if (!*chptr) {
	rpmError(RPMERR_RPMRC, _("missing second ':' at %s:%d"), fn, linenum);
	return 1;
    } else if (chptr == name) {
	rpmError(RPMERR_RPMRC, _("missing architecture name at %s:%d"), fn,
			     linenum);
	return 1;
    }

    while (*chptr == ':' || isspace(*chptr)) chptr--;
    *(++chptr) = '\0';
    equivs = chptr + 1;
    while (*equivs && isspace(*equivs)) equivs++;
    if (!*equivs) {
	delEntry = 1;
    }

    if (cache->size) {
	entry = machCacheFindEntry(cache, name);
	if (entry) {
	    for (i = 0; i < entry->count; i++)
		free(entry->equivs[i]);
	    if (entry->count) free(entry->equivs);
	    entry->count = 0;
	}
    }

    if (!entry) {
	cache->cache = realloc(cache->cache,
			       (cache->size + 1) * sizeof(*cache->cache));
	entry = cache->cache + cache->size++;
	entry->name = strdup(name);
	entry->count = 0;
	entry->visited = 0;
    }

    if (delEntry) return 0;

    chptr = strtok(equivs, " ");
    while (chptr) {
	if (strlen(chptr)) {		/* does strtok() return "" ever?? */
	    if (entry->count)
		entry->equivs = realloc(entry->equivs, sizeof(*entry->equivs)
					* (entry->count + 1));
	    else
		entry->equivs = malloc(sizeof(*entry->equivs));

	    entry->equivs[entry->count] = strdup(chptr);
	    entry->count++;
	}

	chptr = strtok(NULL, " ");
    }

    return 0;
}

static struct machEquivInfo * machEquivSearch(
		struct machEquivTable * table, char * name) {
    int i;

    for (i = 0; i < table->count; i++)
	if (!strcmp(table->list[i].name, name))
	    return table->list + i;

    return NULL;
}

static void machAddEquiv(struct machEquivTable * table, char * name,
			   int distance) {
    struct machEquivInfo * equiv;

    equiv = machEquivSearch(table, name);
    if (!equiv) {
	if (table->count)
	    table->list = realloc(table->list, (table->count + 1)
				    * sizeof(*table->list));
	else
	    table->list = malloc(sizeof(*table->list));

	table->list[table->count].name = strdup(name);
	table->list[table->count++].score = distance;
    }
}

static void machCacheEntryVisit(struct machCache * cache,
				  struct machEquivTable * table,
				  char * name,
	  			  int distance) {
    struct machCacheEntry * entry;
    int i;

    entry = machCacheFindEntry(cache, name);
    if (!entry || entry->visited) return;

    entry->visited = 1;

    for (i = 0; i < entry->count; i++) {
	machAddEquiv(table, entry->equivs[i], distance);
    }

    for (i = 0; i < entry->count; i++) {
	machCacheEntryVisit(cache, table, entry->equivs[i], distance + 1);
    }
}

static void machFindEquivs(struct machCache * cache,
			     struct machEquivTable * table,
			     char * key) {
    int i;

    for (i = 0; i < cache->size; i++)
	cache->cache[i].visited = 0;

    table->count = 0;

    /* We have a general graph built using strings instead of pointers.
       Yuck. We have to start at a point at traverse it, remembering how
       far away everything is. */
    machAddEquiv(table, key, 1);
    machCacheEntryVisit(cache, table, key, 2);
}

static int addCanon(struct canonEntry **table, int *tableLen, char *line,
		    char *fn, int lineNum) {
    struct canonEntry *t;
    char *s, *s1;

    if (! *tableLen) {
	*tableLen = 2;
	*table = malloc(2 * sizeof(struct canonEntry));
    } else {
	(*tableLen) += 2;
	*table = realloc(*table, sizeof(struct canonEntry) * (*tableLen));
    }
    t = & ((*table)[*tableLen - 2]);

    t->name = strtok(line, ": \t");
    t->short_name = strtok(NULL, " \t");
    s = strtok(NULL, " \t");
    if (! (t->name && t->short_name && s)) {
	rpmError(RPMERR_RPMRC, _("Incomplete data line at %s:%d"), fn, lineNum);
	return RPMERR_RPMRC;
    }
    if (strtok(NULL, " \t")) {
	rpmError(RPMERR_RPMRC, _("Too many args in data line at %s:%d"),
	      fn, lineNum);
	return RPMERR_RPMRC;
    }

    t->num = strtoul(s, &s1, 10);
    if ((*s1) || (s1 == s) || (t->num == ULONG_MAX)) {
	rpmError(RPMERR_RPMRC, _("Bad arch/os number: %s (%s:%d)"), s,
	      fn, lineNum);
	return(RPMERR_RPMRC);
    }

    t->name = strdup(t->name);
    t->short_name = strdup(t->short_name);

    /* From A B C entry */
    /* Add  B B C entry */
    t[1].name = strdup(t->short_name);
    t[1].short_name = strdup(t->short_name);
    t[1].num = t->num;

    return 0;
}

static int addDefault(struct defaultEntry **table, int *tableLen, char *line,
		      char *fn, int lineNum) {
    struct defaultEntry *t;

    if (! *tableLen) {
	*tableLen = 1;
	*table = malloc(sizeof(struct defaultEntry));
    } else {
	(*tableLen)++;
	*table = realloc(*table, sizeof(struct defaultEntry) * (*tableLen));
    }
    t = & ((*table)[*tableLen - 1]);

    t->name = strtok(line, ": \t");
    t->defName = strtok(NULL, " \t");
    if (! (t->name && t->defName)) {
	rpmError(RPMERR_RPMRC, _("Incomplete default line at %s:%d"),
		 fn, lineNum);
	return RPMERR_RPMRC;
    }
    if (strtok(NULL, " \t")) {
	rpmError(RPMERR_RPMRC, _("Too many args in default line at %s:%d"),
	      fn, lineNum);
	return RPMERR_RPMRC;
    }

    t->name = strdup(t->name);
    t->defName = strdup(t->defName);

    return 0;
}

static struct canonEntry *lookupInCanonTable(char *name,
					     struct canonEntry *table,
					     int tableLen) {
    while (tableLen) {
	tableLen--;
	if (!strcmp(name, table[tableLen].name)) {
	    return &(table[tableLen]);
	}
    }

    return NULL;
}

static char *lookupInDefaultTable(char *name, struct defaultEntry *table,
				  int tableLen) {
    while (tableLen) {
	tableLen--;
	if (!strcmp(name, table[tableLen].name)) {
	    return table[tableLen].defName;
	}
    }

    return name;
}

int rpmReadConfigFiles(char * file, char * arch, char * os, int building) {

    rpmSetMachine(arch, os);

    if (rpmReadRC(file)) return -1;

    if (building)
	rpmSetTables(RPM_MACHTABLE_BUILDARCH, RPM_MACHTABLE_BUILDOS);

    /* XXX WTFO?: Presumably, this is *the* place to set arch/os ??? */
    rpmSetMachine(arch, os);
  {	char *canonarch, *canonos;
	char buf[BUFSIZ];
	int x;
	rpmGetArchInfo(&canonarch, NULL);
	addMacro(&globalMacroContext, "_buildarch", NULL, canonarch, -1);

	/* XXX is this necessary? */
	for (x = 0; canonarch[x]; x++)
		buf[x] = tolower(canonarch[x]);
	addMacro(&globalMacroContext, "_buildarch_lc", NULL, buf, -1);

	rpmGetOsInfo(&canonos, NULL);
	addMacro(&globalMacroContext, "_buildos", NULL, canonos, -1);

	/* XXX is this necessary? */
	for (x = 0; canonos[x]; x++)
		buf[x] = tolower(canonos[x]);
	addMacro(&globalMacroContext, "_buildos_lc", NULL, buf, -1);

  }

    return 0;
}

static void setPathDefault(int var, char *macroname, char *subdir) {
    char * topdir;
    char * fn;

    if (rpmGetVar(var)) return;

    topdir = rpmGetVar(RPMVAR_TOPDIR);
    if (!topdir) topdir = rpmGetVar(RPMVAR_TMPPATH);

    fn = alloca(strlen(topdir) + strlen(subdir) + 2);
    strcpy(fn, topdir);
    if (fn[strlen(topdir) - 1] != '/')
	strcat(fn, "/");
    strcat(fn, subdir);

    rpmSetVar(var, fn);

    if (macroname != NULL) {
#define	_TOPDIRMACRO	"%{_topdir}/"
	char *body = alloca(sizeof(_TOPDIRMACRO) + strlen(subdir) + 2);
	strcpy(body, _TOPDIRMACRO);
	strcat(body, subdir);
	addMacro(&globalMacroContext, macroname, NULL, body, -1);
#undef _TOPDIRMACRO
    }
}

static void setDefaults(void) {
    char * arch, * os;

    initMacros(&globalMacroContext, MACROFILE);

    rpmSetVar(RPMVAR_OPTFLAGS, "-O2");
    rpmSetVar(RPMVAR_SIGTYPE, "none");
    rpmSetVar(RPMVAR_DEFAULTDOCDIR, "/usr/doc");
    rpmSetVar(RPMVAR_TOPDIR, "/usr/src/redhat");
    rpmSetVar(RPMVAR_BUILDSHELL, "/bin/sh");
    defaultMachine(&arch, &os);
    rpmSetVar(RPMVAR_ARCH,arch);
    rpmSetVar(RPMVAR_OS,os);


}

int rpmReadRC(char * file) {
    int fd;
    char * fn;
    char * home;
    int rc = 0;
    static int first = 1;

    if (first) {
	setDefaults();
	first = 0;
    }

    fd = open(LIBRPMRC_FILENAME, O_RDONLY);
    if (fd >= 0) {
	rc = doReadRC(fd, LIBRPMRC_FILENAME);
	close(fd);
	if (rc) return rc;
    } else {
	rpmError(RPMERR_RPMRC, _("Unable to open %s for reading: %s."),
		 LIBRPMRC_FILENAME, strerror(errno));
	return 1;
    }

    if (file)
	fn = file;
    else
	fn = "/etc/rpmrc";

    fd = open(fn, O_RDONLY);
    if (fd >= 0) {
	rc = doReadRC(fd, fn);
	close(fd);
	if (rc) return rc;
    } else if (file) {
	rpmError(RPMERR_RPMRC, _("Unable to open %s for reading: %s."), file,
		 strerror(errno));
	return 1;
    }

    if (!file) {
	home = getenv("HOME");
	if (home) {
	    fn = alloca(strlen(home) + 8);
	    strcpy(fn, home);
	    strcat(fn, "/.rpmrc");
	    fd = open(fn, O_RDONLY);
	    if (fd >= 0) {
		rc |= doReadRC(fd, fn);
		close(fd);
		if (rc) return rc;
	    }
	}
    }

    rpmSetMachine(NULL, NULL);	/* XXX WTFO? Why bother? */

    setPathDefault(RPMVAR_BUILDDIR, "_builddir", "BUILD");
    setPathDefault(RPMVAR_RPMDIR, "_rpmdir", "RPMS");
    setPathDefault(RPMVAR_SRPMDIR, "_srcrpmdir", "SRPMS");
    setPathDefault(RPMVAR_SOURCEDIR, "_sourcedir", "SOURCES");
    setPathDefault(RPMVAR_SPECDIR, "_specdir", "SPECS");

    return 0;
}

static int doReadRC(int fd, char * filename) {
    char buf[BUFSIZ];
    char * start, * chptr, * next, * rest;
    int linenum = 0;
    struct rpmOption searchOption, * option;
    int i;
    int gotit;
    int rc;

  { struct stat sb;
    fstat(fd, &sb);
    next = alloca(sb.st_size + 2);
    if (read(fd, next, sb.st_size) != sb.st_size) {
	rpmError(RPMERR_RPMRC, _("Failed to read %s: %s."), filename,
		 strerror(errno));
	return 1;
    }
    next[sb.st_size] = '\n';
    next[sb.st_size + 1] = '\0';
  }

    while (*next) {
	linenum++;

	chptr = start = next;
	while (*chptr != '\n') chptr++;

	*chptr = '\0';
	next = chptr + 1;

	while (isspace(*start)) start++;

	/* we used to allow comments to begin anywhere, but not anymore */
	if (*start == '#' || !*start) continue;

	chptr = start;
	while (!isspace(*chptr) && *chptr != ':' && *chptr) chptr++;

	if (isspace(*chptr)) {
	    *chptr++ = '\0';
	    while (isspace(*chptr) && *chptr != ':' && *chptr) chptr++;
	}

	if (*chptr != ':') {
	    rpmError(RPMERR_RPMRC, _("missing ':' at %s:%d"),
		     filename, linenum);
	    return 1;
	}

	*chptr++ = '\0';

	searchOption.name = start;
	option = bsearch(&searchOption, optionTable, optionTableSize,
			 sizeof(struct rpmOption), optionCompare);

	if (option) {
	    start = chptr;
	    while (isspace(*start) && *start) start++;

	    if (! *start) {
		rpmError(RPMERR_RPMRC, _("missing argument for %s at %s:%d"),
		      option->name, filename, linenum);
		return 1;
	    }

	    switch (option->var) {
	    case RPMVAR_INCLUDE:
	      {	int fdinc;

		strcpy(buf, start);
		if (expandMacros(NULL, &globalMacroContext, buf, sizeof(buf))) {
		    rpmError(RPMERR_RPMRC, _("expansion failed at %s:d \"%s\""),
			filename, linenum, start);
		    return 1;
		}

		if ((fdinc = open(buf, O_RDONLY)) < 0) {
		    rpmError(RPMERR_RPMRC, _("cannot open %s at %s:%d"),
			buf, filename, linenum);
			return 1;
		}
		rc = doReadRC(fdinc, buf);
		close(fdinc);
		if (rc) return rc;
	      }	break;
	    default:
		break;
	    }

	    chptr = start;
	    if (option->archSpecific) {
		while (!isspace(*chptr) && *chptr) chptr++;

		if (!*chptr) {
		    rpmError(RPMERR_RPMRC,
				_("missing architecture for %s at %s:%d"),
			  	option->name, filename, linenum);
		    return 1;
		}

		*chptr++ = '\0';

		while (isspace(*chptr) && *chptr) chptr++;
		if (!*chptr) {
		    rpmError(RPMERR_RPMRC,
				_("missing argument for %s at %s:%d"),
			  	option->name, filename, linenum);
		    return 1;
		}
		if (option->macroize && strcmp(start, current[ARCH])) {
		    char *s = buf;
		    *s++ = '_';
		    if (option->localize)
			*s++ = '_';
		    strcpy(s, option->name);
		    addMacro(&globalMacroContext, buf, NULL, chptr, -1);
		}
	    } else {
		start = NULL;	/* no arch */
		/* XXX for now only non-arch values can get macroized */
		if (option->macroize) {
		    char *s = buf;
		    *s++ = '_';
		    if (option->localize)
			*s++ = '_';
		    strcpy(s, option->name);
		    addMacro(&globalMacroContext, buf, NULL, chptr, -1);
		}
	    }
	    rpmSetVarArch(option->var, chptr, start);
	} else {
	    gotit = 0;

	    for (i = 0; i < RPM_MACHTABLE_COUNT; i++) {
		if (!strncmp(tables[i].key, start, strlen(tables[i].key)))
		    break;
	    }

	    if (i < RPM_MACHTABLE_COUNT) {
		rest = start + strlen(tables[i].key);
		if (*rest == '_') rest++;

		if (!strcmp(rest, "compat")) {
		    if (machCompatCacheAdd(chptr, filename, linenum,
						&tables[i].cache))
			return 1;
		    gotit = 1;
		} else if (tables[i].hasTranslate &&
			   !strcmp(rest, "translate")) {
		    if (addDefault(&tables[i].defaults,
				   &tables[i].defaultsLength,
				   chptr, filename, linenum))
			return 1;
		    gotit = 1;
		} else if (tables[i].hasCanon &&
			   !strcmp(rest, "canon")) {
		    if (addCanon(&tables[i].canons, &tables[i].canonsLength,
				 chptr, filename, linenum))
			return 1;
		    gotit = 1;
		}
	    }

	    if (!gotit) {
		rpmError(RPMERR_RPMRC, _("bad option '%s' at %s:%d"),
			    start, filename, linenum);
	    }
	}
    }

    return 0;
}

static void defaultMachine(char ** arch, char ** os) {
    static struct utsname un;
    static int gotDefaults = 0;
    char * chptr;
    struct canonEntry * canon;

    if (!gotDefaults) {
	uname(&un);
	if (!strcmp(un.sysname, "AIX")) {
	    strcpy(un.machine, __power_pc() ? "ppc" : "rs6000");
	} else if (!strncmp(un.sysname, "IP", 2)) {
	    un.sysname[2] = '\0';
	}

	/* get rid of the hyphens in the sysname */
	chptr = un.machine;
	while (*chptr++)
	    if (*chptr == '/') *chptr = '-';

#	if defined(__MIPSEL__) || defined(__MIPSEL) || defined(_MIPSEL)
	    /* little endian */
	    strcpy(un.machine, "mipsel");
#	elif defined(__MIPSEB__) || defined(__MIPSEB) || defined(_MIPSEB)
	   /* big endian */
		strcpy(un.machine, "mipseb");
#	endif

	#if defined(__hpux) && defined(_SC_CPU_VERSION)
	{
	    int cpu_version = sysconf(_SC_CPU_VERSION);

#	    if defined(CPU_HP_MC68020)
		if (cpu_version == CPU_HP_MC68020)
		    strcpy(un.machine, "m68k");
#	    endif
#	    if defined(CPU_HP_MC68030)
		if (cpu_version == CPU_HP_MC68030)
		    strcpy(un.machine, "m68k");
#	    endif
#	    if defined(CPU_HP_MC68040)
		if (cpu_version == CPU_HP_MC68040)
		    strcpy(un.machine, "m68k");
#	    endif

#	    if defined(CPU_PA_RISC1_0)
		if (cpu_version == CPU_PA_RISC1_0)
		    strcpy(un.machine, "parisc");
#	    endif
#	    if defined(CPU_PA_RISC1_1)
		if (cpu_version == CPU_PA_RISC1_1)
		    strcpy(un.machine, "parisc");
#	    endif
#	    if defined(CPU_PA_RISC1_2)
		if (cpu_version == CPU_PA_RISC1_2)
		    strcpy(un.machine, "parisc");
#	    endif
	}
#	endif

	/* the uname() result goes through the arch_canon table */
	canon = lookupInCanonTable(un.machine,
				   tables[RPM_MACHTABLE_INSTARCH].canons,
				   tables[RPM_MACHTABLE_INSTARCH].canonsLength);
	if (canon)
	    strcpy(un.machine, canon->short_name);

	canon = lookupInCanonTable(un.sysname,
				   tables[RPM_MACHTABLE_INSTOS].canons,
				   tables[RPM_MACHTABLE_INSTOS].canonsLength);
	if (canon)
	    strcpy(un.sysname, canon->short_name);
    }

    if (arch) *arch = un.machine;
    if (os) *os = un.sysname;
}

static char * rpmGetVarArch(int var, char * arch) {
    struct rpmvarValue * next;

    if (!arch) arch = current[ARCH];

    if (arch) {
	next = &values[var];
	while (next) {
	    if (next->arch && !strcmp(next->arch, arch)) return next->value;
	    next = next->next;
	}
    }

    next = values + var;
    while (next && next->arch) next = next->next;

    return next ? next->value : NULL;
}

char *rpmGetVar(int var)
{
    return rpmGetVarArch(var, NULL);
}

int rpmGetBooleanVar(int var) {
    char * val;
    int num;
    char * chptr;

    val = rpmGetVar(var);
    if (!val) return 0;

    if (val[0] == 'y' || val[0] == 'Y') return 1;

    num = strtol(val, &chptr, 0);
    if (chptr && *chptr == '\0') {
	return num != 0;
    }

    return 0;
}

void rpmSetVar(int var, char *val) {
    freeRpmVar(&values[var]);

    values[var].arch = NULL;
    values[var].next = NULL;

    if (val)
	values[var].value = strdup(val);
    else
	values[var].value = NULL;
}

/* this doesn't free the passed pointer! */
static void freeRpmVar(struct rpmvarValue * orig) {
    struct rpmvarValue * next, * var = orig;

    while (var) {
	next = var->next;
	if (var->arch) free(var->arch);
	if (var->value) free(var->value);

	if (var != orig) free(var);
	var = next;
    }
}

static void rpmSetVarArch(int var, char * val, char * arch) {
    struct rpmvarValue * next = values + var;

    if (next->value) {
	if (arch) {
	    while (next->next) {
		if (next->arch && !strcmp(next->arch, arch)) break;
		next = next->next;
	    }
	} else {
	    while (next->next) {
		if (!next->arch) break;
		next = next->next;
	    }
	}

	if (next->arch && arch && !strcmp(next->arch, arch)) {
	    if (next->value) free(next->value);
	    if (next->arch) free(next->arch);
	} else if (next->arch || arch) {
	    next->next = malloc(sizeof(*next->next));
	    next = next->next;
	    next->next = NULL;
	}
    }

    next->value = strdup(val);
    if (arch)
	next->arch = strdup(arch);
    else
	next->arch = NULL;
}

void rpmSetTables(int archTable, int osTable) {
    char * arch, * os;

    defaultMachine(&arch, &os);

    if (currTables[ARCH] != archTable) {
	currTables[ARCH] = archTable;
	rebuildCompatTables(ARCH, arch);
    }

    if (currTables[OS] != osTable) {
	currTables[OS] = osTable;
	rebuildCompatTables(OS, os);
    }
}

int rpmMachineScore(int type, char * name) {
    struct machEquivInfo * info;

    info = machEquivSearch(&tables[type].equiv, name);
    if (info)
	return info->score;
    else
	return 0;
}

void rpmGetMachine(char **arch, char **os)
{
    if (arch) {
	*arch = current[ARCH];
    }

    if (os) {
	*os = current[OS];
    }
}

void rpmSetMachine(char * arch, char * os) {
    int transOs = os == NULL;
    int transArch = arch == NULL;
    char * realArch, * realOs;

    defaultMachine(&realArch, &realOs);

    if (!arch)
	arch = realArch;
    if (!os)
	os = realOs;

    if (transArch && tables[currTables[ARCH]].hasTranslate)
	arch = lookupInDefaultTable(arch,
			    tables[currTables[ARCH]].defaults,
			    tables[currTables[ARCH]].defaultsLength);
    if (transOs && tables[currTables[OS]].hasTranslate)
	os = lookupInDefaultTable(os,
			    tables[currTables[OS]].defaults,
			    tables[currTables[OS]].defaultsLength);

    if (!current[ARCH] || strcmp(arch, current[ARCH])) {
	if (current[ARCH]) free(current[ARCH]);
	current[ARCH] = strdup(arch);
	rebuildCompatTables(ARCH, realArch);
    }

    if (!current[OS] || strcmp(os, current[OS])) {
	if (current[OS]) free(current[OS]);
	current[OS] = strdup(os);
	rebuildCompatTables(OS, realOs);
    }
}

static void rebuildCompatTables(int type, char * name) {
    machFindEquivs(&tables[currTables[type]].cache,
		   &tables[currTables[type]].equiv,
		   name);
}

static void getMachineInfo(int type, char ** name, int * num) {
    struct canonEntry * canon;
    int which = currTables[type];

    /* use the normal canon tables, even if we're looking up build stuff */
    if (which >= 2) which -= 2;

    canon = lookupInCanonTable(current[type],
			       tables[which].canons,
			       tables[which].canonsLength);

    if (canon) {
	if (num) *num = canon->num;
	if (name) *name = canon->short_name;
    } else {
	if (num) *num = 255;
	if (name) *name = current[type];

	if (tables[currTables[type]].hasCanon) {
	    rpmMessage(RPMMESS_WARNING, "Unknown system: %s\n", current[type]);
	    rpmMessage(RPMMESS_WARNING, "Please contact rpm-list@redhat.com\n");
	}
    }
}

void rpmGetArchInfo(char ** name, int * num) {
    getMachineInfo(ARCH, name, num);
}

void rpmGetOsInfo(char ** name, int * num) {
    getMachineInfo(OS, name, num);
}

int rpmShowRC(FILE *f)
{
    struct rpmOption *opt;
    int count = 0;
    char *s;
    int i;
    struct machEquivTable * equivTable;

    /* the caller may set the build arch which should be printed here */
    fprintf(f, "ARCHITECTURE AND OS:\n");
    fprintf(f, "build arch            : %s\n", current[ARCH]);

    fprintf(f, "compatible build archs:");
    equivTable = &tables[RPM_MACHTABLE_BUILDARCH].equiv;
    for (i = 0; i < equivTable->count; i++)
	fprintf(f," %s", equivTable->list[i].name);
    fprintf(f, "\n");

    fprintf(f, "build os              : %s\n", current[OS]);

    fprintf(f, "compatible build os's :");
    equivTable = &tables[RPM_MACHTABLE_BUILDOS].equiv;
    for (i = 0; i < equivTable->count; i++)
	fprintf(f," %s", equivTable->list[i].name);
    fprintf(f, "\n");

    rpmSetTables(RPM_MACHTABLE_INSTARCH, RPM_MACHTABLE_INSTOS);
    rpmSetMachine(NULL, NULL);	/* XXX WTFO? Why bother? */

    fprintf(f, "install arch          : %s\n", current[ARCH]);
    fprintf(f, "install os            : %s\n", current[OS]);

    fprintf(f, "compatible archs      :");
    equivTable = &tables[RPM_MACHTABLE_INSTARCH].equiv;
    for (i = 0; i < equivTable->count; i++)
	fprintf(f," %s", equivTable->list[i].name);
    fprintf(f, "\n");

    fprintf(f, "compatible os's       :");
    equivTable = &tables[RPM_MACHTABLE_INSTOS].equiv;
    for (i = 0; i < equivTable->count; i++)
	fprintf(f," %s", equivTable->list[i].name);
    fprintf(f, "\n");

    fprintf(f, "RPMRC VALUES:\n");
    opt = optionTable;
    while (count < optionTableSize) {
	s = rpmGetVar(opt->var);
	fprintf(f, "%-21s : %s\n", opt->name, s ? s : "(not set)");
	opt++;
	count++;
    }

    return 0;
}
