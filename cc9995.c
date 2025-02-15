/*
 *	It's easiest to think of what cc does as a sequence of four
 *	conversions. Each conversion produces the inputs to the next step
 *	and the number of types is reduced. If the step is the final
 *	step for the conversion then the file is generated with the expected
 *	name but if it will be consumed by a later stage it is a temporary
 *	scratch file.
 *
 *	Stage 1: (-c -o overrides object name)
 *
 *	Ending			Action
 *	$1.S			preprocessor - may make $1.s
 *	$1.s			nothing
 *	$1.c			preprocessor, no-op or /dev/tty
 *	$1.o			nothing
 *	$1.a			nothing (library)
 *
 *	Stage 2: (not -E)
 *
 *	Ending			Action
 *	$1.s			nothing
 *	$1.%			cc - make $1.s
 *	$1.o			nothing
 *	$1.a			nothing (library)
 *
 *	Stage 3: (not -E or -S)
 *
 *	Ending			Action
 *	$1.s			assembler - makes $1.o
 *	$1.o			nothing
 *	$1.a			nothing (library)
 *
 *	Stage 4: (run if no -c -E -S)
 *
 *	ld [each .o|.a in order] [each -l lib in order] -lc
 *	(with -b -o $1 etc)
 *
 *	TODO:
 *
 *	Platform specifics
 *	Search library paths for libraries (or pass to ld and make ld do it)
 *	Turn on temp removal once confident
 */

#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <signal.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/wait.h>

#define DEBUG

#define BINPATH	"/opt/cc9995/bin/"
#define LIBPATH	"/opt/cc9995/lib/"
#define INCPATH "/opt/cc9995/include/"

#define LIBPATH_TI	LIBPATH"target-ti994a/"
#define INCPATH_TI	INCPATH"target-ti994a/"
#define CRT0_TI		LIBPATH_TI"crt0-ti994a.o"
#define CMD_TIBIN	LIBPATH_TI"tibin"

#define LIBPATH_MDOS	LIBPATH"target-mdos/"
#define INCPATH_MDOS	INCPATH"target-mdos/"
#define CRT0_MDOS	LIBPATH_TI"crt0-mdos.o"
#define CMD_MDOSBIN	LIBPATH_TI"mdosbin"

#define CMD_AS		BINPATH"as9900"
#define CMD_CC		LIBPATH"tms9995-cpp"
#define CMD_CCOM	LIBPATH"tms9995-ccom"
#define CMD_LD		BINPATH"ld9900"
#define CRT0		LIBPATH"crt0.o"
#define LIBC		LIBPATH"libc.a"
#define LIB9995		LIBPATH"lib9995.a"

struct obj {
	struct obj *next;
	char *name;
	uint8_t type;
#define TYPE_S			1
#define TYPE_C			2
#define TYPE_s			3
#define TYPE_C_pp		4
#define TYPE_O			5
#define TYPE_A			6
	uint8_t used;
};

struct objhead {
	struct obj *head;
	struct obj *tail;
};

struct objhead objlist;
struct objhead liblist;
struct objhead inclist;
struct objhead deflist;
struct objhead libpathlist;
struct objhead ccargs;		/* Arguments to pass on to the compiler */

int keep_temp;
int last_phase = 4;
int only_one_input;
char *target;
int strip;
int c_files;
int standalone;
int cpu = 9995;
int mapfile;
int targetos;
int discardable;
#define OS_NONE		0
#define OS_FUZIX	1
#define OS_TI994A	2
#define OS_MDOS		3
int fuzixsub;

#define MAXARG	512

int arginfd, argoutfd;
char *arglist[MAXARG];
char **argptr;
char *rmlist[MAXARG];
char **rmptr = rmlist;

static void remove_temporaries(void)
{
	char **p = rmlist;
	while (p < rmptr) {
		if (keep_temp == 0)
			unlink(*p);
		free(*p++);
	}
	rmptr = rmlist;
}

static void fatal(void)
{
	remove_temporaries();
	exit(1);
}

static void memory(void)
{
	fprintf(stderr, "cc: out of memory.\n");
	fatal();
}

static char *xstrdup(char *p, int extra)
{
	char *n = malloc(strlen(p) + extra + 1);
	if (n == NULL)
		memory();
	strcpy(n, p);
	return n;
}

static void append_obj(struct objhead *h, char *p, uint8_t type)
{
	struct obj *o = malloc(sizeof(struct obj));
	if (o == NULL)
		memory();
	o->name = p;
	o->next = NULL;
	o->used = 0;
	o->type = type;
	if (h->tail)
		h->tail->next = o;
	else
		h->head = o;
	h->tail = o;
}

static char *pathmod(char *p, char *f, char *t, int rmif)
{
	char *x = strrchr(p, '.');
	if (x == NULL) {
		fprintf(stderr, "cc: no extension on '%s'.\n", p);
		fatal();
	}
//	if (strcmp(x, f)) {
//		fprintf(stderr, "cc: internal got '%s' expected '%s'.\n",
//			p, t);
//		fatal();
//	}
	strcpy(x, t);
	if (last_phase > rmif) {
		*rmptr++ = xstrdup(p, 0);
	}
	return p;
}

static void add_argument(char *p)
{
	if (argptr == &arglist[MAXARG]) {
		fprintf(stderr, "cc: too many arguments to command.\n");
		fatal();
	}
	*argptr++ = p;
}

static void add_argument_list(char *header, struct objhead *h)
{
	struct obj *i = h->head;
	while (i) {
		if (header)
			add_argument(header);
		add_argument(i->name);
		i->used = 1;
		i = i->next;
	}
}

static char *resolve_library(char *p)
{
	static char buf[512];
	struct obj *o = libpathlist.head;
	if (strchr(p, '/') || strchr(p, '.'))
		return p;
	while(o) {
		snprintf(buf, 512, "%s/lib%s.a", o->name, p);
		if (access(buf, 0) == 0)
			return buf;
		o = o->next;
	}
	return NULL;
}

/* This turns -L/opt/cc9995/lib  -lfoo -lbar into resolved names like
   /opt/cc9995/lib/libfoo.a */
static void resolve_libraries(void)
{
	struct obj *o = liblist.head;
	while(o != NULL) {
		char *p = resolve_library(o->name);
		if (p == NULL) {
			fprintf(stderr, "cc: unable to find library '%s'.\n", o->name);
			exit(1);
		}
		add_argument(p);
		o = o->next;
	}
}

static void run_command(void)
{
	pid_t pid, p;
	int status;

	fflush(stdout);

	*argptr = NULL;

	pid = fork();
	if (pid == -1) {
		perror("fork");
		fatal();
	}
	if (pid == 0) {
#ifdef DEBUG
		{
			char **p = arglist;
			printf("[");
			while(*p)
				printf("%s ", *p++);
			printf("]\n");
		}
#endif
		fflush(stdout);
		if (arginfd != -1) {
			dup2(arginfd, 0);
			close(arginfd);
		}
		if (argoutfd != -1) {
			dup2(argoutfd, 1);
			close(argoutfd);
		}
		execv(arglist[0], arglist);
		perror("execv");
		exit(255);
	}
	if (arginfd)
		close(arginfd);
	if (argoutfd)
		close(argoutfd);
	while ((p = waitpid(pid, &status, 0)) != pid) {
		if (p == -1) {
			perror("waitpid");
			fatal();
		}
	}
	if (WIFSIGNALED(status) || WEXITSTATUS(status)) {
		printf("cc: %s failed.\n", arglist[0]);
		fatal();
	}
}

static void redirect_in(const char *p)
{
#ifdef DEBUG
	printf("[stdin to %s]\n", p);
#endif
	arginfd = open(p, O_RDONLY);
	if (arginfd == -1) {
		perror(p);
		fatal();
	}
}

static void redirect_out(const char *p)
{
#ifdef DEBUG
	printf("[stdout to %s]\n", p);
#endif
	argoutfd = open(p, O_WRONLY | O_CREAT | O_TRUNC, 0666);
	if (argoutfd == -1) {
		perror(p);
		fatal();
	}
}

static void build_arglist(char *p)
{
	arginfd = -1;
	argoutfd = -1;
	argptr = arglist;
	add_argument(p);
}

void convert_s_to_o(char *path)
{
	build_arglist(CMD_AS);
	add_argument(path);
	run_command();
	pathmod(path, ".s", ".o", 5);
}

static char *optimize[] = {
	"tailcall",
	"ssa",
	"temps",
	"deljumps",
	"dce",
	"ccp",
	"scp",
	NULL
};

void convert_c_to_s(char *path)
{
	char *t;
	char **p = optimize;

	build_arglist(CMD_CCOM);
	
	add_argument_list(NULL, &ccargs);
	while(*p) {
		add_argument("-x");
		add_argument(*p++);
	}
	if (cpu == 9900)
		add_argument("-mno-divs");
	if (discardable)
		add_argument("-mdiscard");
	t = xstrdup(path, 0);
	redirect_in(t);
	redirect_out(pathmod(path, ".%", ".s", 2));
	run_command();
	free(t);
}

void convert_S_to_s(char *path)
{
	build_arglist(CMD_CC);
	add_argument("-E");
	redirect_in(path);
	redirect_out(pathmod(path, ".S", ".s", 1));
	run_command();
}

void preprocess_c(char *path)
{
	build_arglist(CMD_CC);

	add_argument_list("-I", &inclist);
	add_argument_list("-D", &deflist);
	add_argument(xstrdup(path, 0));
	redirect_out(pathmod(path, ".c", ".%", 0));
	run_command();
}

void link_phase(void)
{
	build_arglist(CMD_LD);
	/* Force word alignment of segments */
	add_argument("-A");
	add_argument("2");
	switch (targetos) {
		case OS_FUZIX:
			switch(fuzixsub) {
			case 0:
				break;
			case 1:
				add_argument("-b");
				add_argument("-C");
				add_argument("256");
				add_argument("-Z");
				add_argument("0");
				break;
			case 2:
				add_argument("-b");
				add_argument("-C");
				add_argument("512");
				add_argument("-Z");
				add_argument("2");
				break;
			}
			break;
		case OS_TI994A:
			/* Link at 0xA000 */
			add_argument("-b");
			add_argument("-C");
			add_argument("40960");
			break;
		case OS_MDOS:
			/* Link at 0x0400 but with a 6 byte header in the crt  */
			add_argument("-b");
			add_argument("-C");
			add_argument("1018");
			break;
		case OS_NONE:
		default:
			add_argument("-b");
			add_argument("-C");
			add_argument("256");
			break;
	}
	if (strip)
		add_argument("-s");
	add_argument("-o");
	add_argument(target);
	if (mapfile) {
		/* For now output a map file. One day we'll have debug symbols
		   nailed to the binary */
		char *n = malloc(strlen(target) + 5);
		sprintf(n, "%s.map", target);
		add_argument("-m");
		add_argument(n);
	}
	if (!standalone) {
		/* Start with crt0.o, end with libc.a and support libraries */
		/* For now - we will want one per target */
		switch(targetos) {
		case OS_TI994A:
			add_argument(CRT0_TI);
			append_obj(&libpathlist, LIBPATH_TI, 0);
			append_obj(&liblist, LIBC, TYPE_A);
			break;
		case OS_FUZIX:
		case OS_NONE:
			add_argument(CRT0);
			append_obj(&libpathlist, LIBPATH, 0);
			append_obj(&liblist, LIBC, TYPE_A);
			break;
		}
	}
	append_obj(&liblist, LIB9995, TYPE_A);
	add_argument_list(NULL, &objlist);
	resolve_libraries();
	run_command();
	switch(targetos) {
	case OS_TI994A:
		/* TI 99/4A */
		build_arglist(CMD_TIBIN);
		add_argument(target);
		run_command();
		break;
	case OS_MDOS:
		/* MDOS */
		build_arglist(CMD_MDOSBIN);
		add_argument(target);
		run_command();
		break;
	}
}

void sequence(struct obj *i)
{
#ifdef DEBUG
	printf("Last Phase %d\n", last_phase);
	printf("1:Processing %s %d\n", i->name, i->type);
#endif
	if (i->type == TYPE_S) {
		convert_S_to_s(i->name);
		i->type = TYPE_s;
		i->used = 1;
	}
	if (i->type == TYPE_C) {
		preprocess_c(i->name);
		i->type = TYPE_C_pp;
		i->used = 1;
	}
	if (last_phase == 1)
		return;
#ifdef DEBUG
	printf("2:Processing %s %d\n", i->name, i->type);
#endif
	if (i->type == TYPE_C_pp) {
		convert_c_to_s(i->name);
		i->type = TYPE_s;
		i->used = 1;
	}
	if (last_phase == 2)
		return;
#ifdef DEBUG
	printf("3:Processing %s %d\n", i->name, i->type);
#endif
	if (i->type == TYPE_s) {
		convert_s_to_o(i->name);
		i->type = TYPE_O;
		i->used = 1;
	}
}

void processing_loop(void)
{
	struct obj *i = objlist.head;
	while (i) {
		sequence(i);
		remove_temporaries();
		i = i->next;
	}
	if (last_phase < 4)
		return;
	link_phase();
	/* And clean up anything we couldn't wipe earlier */
	last_phase = 255;
	remove_temporaries();
}

void unused_files(void)
{
	struct obj *i = objlist.head;
	while (i) {
		if (!i->used)
			fprintf(stderr, "cc: warning file %s unused.\n",
				i->name);
		i = i->next;
	}
}

void usage(void)
{
	fprintf(stderr, "usage...\n");
	fatal();
}

char **add_macro(char **p)
{
	if ((*p)[2])
		append_obj(&deflist, *p + 2, 0);
	else
		append_obj(&deflist, *++p, 0);
	return p;
}

char **add_library(char **p)
{
	if ((*p)[2])
		append_obj(&liblist, *p + 2, TYPE_A);
	else
		append_obj(&liblist, *++p, TYPE_A);
	return p;
}

char **add_library_path(char **p)
{
	if ((*p)[2])
		append_obj(&libpathlist, *p + 2, 0);
	else
		append_obj(&libpathlist, *++p, 0);
	return p;
}


char **add_includes(char **p)
{
	if ((*p)[2])
		append_obj(&inclist, *p + 2, 0);
	else
		append_obj(&inclist, *++p, 0);
	return p;
}

void add_system_include(char *p)
{
	append_obj(&inclist, p, 0);
}

void dunno(const char *p)
{
	fprintf(stderr, "cc: don't know what to do with '%s'.\n", p);
	fatal();
}

void add_file(char *p)
{
	char *x = strrchr(p, '.');
	if (x == NULL)
		dunno(p);
	switch (x[1]) {
	case 'a':
		append_obj(&objlist, p, TYPE_A);
		break;
	case 's':
		append_obj(&objlist, p, TYPE_s);
		break;
	case 'S':
		append_obj(&objlist, p, TYPE_S);
		break;
	case 'c':
		append_obj(&objlist, p, TYPE_C);
		c_files++;
		break;
	case 'o':
		append_obj(&objlist, p, TYPE_O);
		break;
	default:
		dunno(p);
	}
}

void one_input(void)
{
	fprintf(stderr, "cc: too many files for -E\n");
	fatal();
}

void uniopt(char *p)
{
	if (p[2])
		usage();
}

static char *passopts[] = {
	"*bss-name",
	" check-stack",
	"*code-name",
	"*data-name",
	" debug",
	" inline-stdfuncs",
	"*register-space",
	" register-vars",
	"*rodata-name",
	" signed-char",
	"*standard",
	" verbose",
	" writable-strings",
	NULL
};
	
char **longopt(char **ap)
{
	char *p = *ap + 2;
	char **x = passopts;
	while(*x) {
		char *t = *x++;
		if (strcmp(t + 1, p) == 0) {
			append_obj(&ccargs, p - 2, 0);
			if (*t == '*') {
				p = *++ap;
				if (p == NULL)
					usage();
				append_obj(&ccargs, p, 0);
			}
			return ap;
		}
	}
	usage();
	exit(1);
}
	
int main(int argc, char *argv[])
{
	char **p = argv;
	signal(SIGCHLD, SIG_DFL);

	append_obj(&deflist, "__CC9995__", 0);
	append_obj(&deflist, "__tms9995__", 0);

	while (*++p) {
		/* filename or option ? */
		if (**p != '-') {
			add_file(*p);
			continue;
		}
		switch ((*p)[1]) {
		case '-':
			p = longopt(p);
			break;
			/* Don't link */
		case 'c':
			uniopt(*p);
			last_phase = 3;
			break;
			/* Don't assemble */
		case 'd':
			/* Build for discard */
			discardable = 1;
			break;
		case 'S':
			uniopt(*p);
			last_phase = 2;
			break;
			/* Only pre-process */
		case 'E':
			uniopt(*p);
			last_phase = 1;
			only_one_input = 1;
			break;
		case 'l':
			p = add_library(p);
			break;
		case 'I':
			p = add_includes(p);
			break;
		case 'L':
			p = add_library_path(p);
			break;
		case 'D':
			p = add_macro(p);
			break;
		case 'i':
/*                    split_id();*/
			uniopt(*p);
			break;
		case 'o':
			if (target != NULL) {
				fprintf(stderr,
					"cc: -o can only be used once.\n");
				fatal();
			}
			if ((*p)[2])
				target = *p + 2;
			else if (*p)
				target = *++p;
			else {
				fprintf(stderr, "cc: no target given.\n");
				fatal();
			}
			break;
		case 's':	/* FIXME: for now - switch to getopt */
			standalone = 1;
			break;
		case 'X':
			uniopt(*p);
			keep_temp = 1;
			break;
		case 'm':
			cpu = atoi(*p + 2);
			if (cpu != 9995 && cpu != 9900) {
				fprintf(stderr, "cc: only 9900 and 9995 supported.\n");
				fatal();
			}
			break;	
		case 'M':
			mapfile = 1;
			break;
		case 't':
			if (strcmp(*p + 2, "ti994a") == 0) {
				targetos = OS_TI994A;
				cpu = 9900;
			} else if (strcmp(*p + 2, "mdos") == 0) {
				targetos = OS_MDOS;
				cpu = 9995;
			} else if (strcmp(*p + 2, "fuzix") == 0) {
				targetos = OS_FUZIX;
				fuzixsub = 0;
			} else if (strcmp(*p + 2, "fuzixrel1") == 0) {
				targetos = OS_FUZIX;
				fuzixsub = 1;
			} else if (strcmp(*p + 2, "fuzixrel2") == 0) {
				targetos = OS_FUZIX;
				fuzixsub = 2;
			} else {
				fprintf(stderr, "cc: only fuzix. mdos and ti994a target types are known.\n");
				fatal();
			}
			break;
		default:
			usage();
		}
	}

	if (!standalone) {
		switch (targetos) {
		case OS_TI994A:
                       add_system_include(INCPATH_TI);
			break;
		case OS_MDOS:
                       add_system_include(INCPATH_MDOS);
			break;
		case OS_FUZIX:
			break;
		}
		add_system_include(INCPATH);
	}

	if (target == NULL)
		target = "a.out";
	if (only_one_input && c_files > 1)
		one_input();
	processing_loop();
	unused_files();
	return 0;
}
