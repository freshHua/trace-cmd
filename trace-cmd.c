/*
 * Copyright (C) 2008,2009, Steven Rostedt <srostedt@redhat.com>
 *
 * ~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; version 2 of the License (not later!)
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
 *
 * ~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~
 */
#define _GNU_SOURCE
#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <pthread.h>
#include <fcntl.h>
#include <unistd.h>
#include <ctype.h>
#include <errno.h>

#include "trace-local.h"

#define _STR(x) #x
#define STR(x) _STR(x)
#define MAX_PATH 256

#define TRACE_CTRL	"tracing_on"
#define TRACE		"trace"
#define AVAILABLE	"available_tracers"
#define CURRENT		"current_tracer"
#define ITER_CTRL	"trace_options"
#define MAX_LATENCY	"tracing_max_latency"

unsigned int page_size;

static const char *output_file = "trace.dat";

static int latency;
static int sleep_time = 1000;
static int cpu_count;
static int *pids;

struct event_list {
	struct event_list *next;
	const char *event;
	int neg;
};

static struct event_list *event_selection;

struct events {
	struct events *sibling;
	struct events *children;
	struct events *next;
	char *name;
};

static struct tracecmd_recorder *recorder;

static char *get_temp_file(int cpu)
{
	char *file = NULL;
	int size;

	size = snprintf(file, 0, "%s.cpu%d", output_file, cpu);
	file = malloc_or_die(size + 1);
	sprintf(file, "%s.cpu%d", output_file, cpu);

	return file;
}

static void put_temp_file(char *file)
{
	free(file);
}

static void delete_temp_file(int cpu)
{
	char file[MAX_PATH];

	snprintf(file, MAX_PATH, "%s.cpu%d", output_file, cpu);
	unlink(file);
}

static void kill_threads(void)
{
	int i;

	if (!cpu_count)
		return;

	for (i = 0; i < cpu_count; i++) {
		if (pids[i] > 0) {
			kill(pids[i], SIGKILL);
			delete_temp_file(i);
			pids[i] = 0;
		}
	}
}

static void delete_thread_data(void)
{
	int i;

	if (!cpu_count)
		return;

	for (i = 0; i < cpu_count; i++) {
		if (pids[i]) {
			delete_temp_file(i);
			if (pids[i] < 0)
				pids[i] = 0;
		}
	}
}

static void stop_threads(void)
{
	int i;

	if (!cpu_count)
		return;

	for (i = 0; i < cpu_count; i++) {
		if (pids[i] > 0) {
			kill(pids[i], SIGINT);
			waitpid(pids[i], NULL, 0);
			pids[i] = -1;
		}
	}
}

static void flush_threads(void)
{
	int i;

	if (!cpu_count)
		return;

	for (i = 0; i < cpu_count; i++) {
		if (pids[i] > 0)
			kill(pids[i], SIGUSR1);
	}
}

void die(char *fmt, ...)
{
	va_list ap;
	int ret = errno;

	if (errno)
		perror("trace-cmd");
	else
		ret = -1;

	kill_threads();
	va_start(ap, fmt);
	fprintf(stderr, "  ");
	vfprintf(stderr, fmt, ap);
	va_end(ap);

	fprintf(stderr, "\n");
	exit(ret);
}

void warning(char *fmt, ...)
{
	va_list ap;

	if (errno)
		perror("trace-cmd");
	errno = 0;

	va_start(ap, fmt);
	fprintf(stderr, "  ");
	vfprintf(stderr, fmt, ap);
	va_end(ap);

	fprintf(stderr, "\n");
}

void *malloc_or_die(unsigned int size)
{
	void *data;

	data = malloc(size);
	if (!data)
		die("malloc");
	return data;
}

static int set_ftrace(int set)
{
	struct stat buf;
	char *path = "/proc/sys/kernel/ftrace_enabled";
	int fd;
	char *val = set ? "1" : "0";

	/* if ftace_enable does not exist, simply ignore it */
	fd = stat(path, &buf);
	if (fd < 0)
		return -ENODEV;

	fd = open(path, O_WRONLY);
	if (fd < 0)
		die ("Can't %s ftrace", set ? "enable" : "disable");

	write(fd, val, 1);
	close(fd);

	return 0;
}

void run_cmd(int argc, char **argv)
{
	int status;
	int pid;

	if ((pid = fork()) < 0)
		die("failed to fork");
	if (!pid) {
		/* child */
		if (execvp(argv[0], argv))
			exit(-1);
	}
	waitpid(pid, &status, 0);
}

static char *get_tracing_file(const char *name)
{
	static const char *tracing;
	char *file;

	if (!tracing) {
		tracing = tracecmd_find_tracing_dir();
		if (!tracing)
			die("Can't find tracing dir");
	}

	file = malloc_or_die(strlen(tracing) + strlen(name) + 2);
	if (!file)
		return NULL;

	sprintf(file, "%s/%s", tracing, name);
	return file;
}

static void put_tracing_file(char *file)
{
	free(file);
}

static void show_events(void)
{
	char buf[BUFSIZ];
	char *path;
	FILE *fp;
	size_t n;

	path = get_tracing_file("available_events");
	fp = fopen(path, "r");
	if (!fp)
		die("reading %s", path);
	put_tracing_file(path);

	do {
		n = fread(buf, 1, BUFSIZ, fp);
		if (n > 0)
			fwrite(buf, 1, n, stdout);
	} while (n > 0);
	fclose(fp);
}

static void show_plugins(void)
{
	char buf[BUFSIZ];
	char *path;
	FILE *fp;
	size_t n;

	path = get_tracing_file("available_tracers");
	fp = fopen(path, "r");
	if (!fp)
		die("reading %s", path);
	put_tracing_file(path);

	do {
		n = fread(buf, 1, BUFSIZ, fp);
		if (n > 0)
			fwrite(buf, 1, n, stdout);
	} while (n > 0);
	fclose(fp);
}

static void set_plugin(const char *name)
{
	FILE *fp;
	char *path;

	path = get_tracing_file("current_tracer");
	fp = fopen(path, "w");
	if (!fp)
		die("writing to '%s'", path);
	put_tracing_file(path);

	fwrite(name, 1, strlen(name), fp);
	fclose(fp);
}

static void show_options(void)
{
	char buf[BUFSIZ];
	char *path;
	FILE *fp;
	size_t n;

	path = get_tracing_file("trace_options");
	fp = fopen(path, "r");
	if (!fp)
		die("reading %s", path);
	put_tracing_file(path);

	do {
		n = fread(buf, 1, BUFSIZ, fp);
		if (n > 0)
			fwrite(buf, 1, n, stdout);
	} while (n > 0);
	fclose(fp);
}

static void set_option(const char *option)
{
	FILE *fp;
	char *path;

	path = get_tracing_file("trace_options");
	fp = fopen(path, "w");
	if (!fp)
		die("writing to '%s'", path);
	put_tracing_file(path);

	fwrite(option, 1, strlen(option), fp);
	fclose(fp);
}

static void enable_event(const char *name)
{
	struct stat st;
	FILE *fp;
	char *path;
	int ret;

	fprintf(stderr, "enable %s\n", name);
	if (strcmp(name, "all") == 0) {
		path = get_tracing_file("events/enable");

		ret = stat(path, &st);
		if (ret < 0) {
			put_tracing_file(path);
			/* need to use old way */
			path = get_tracing_file("set_event");
			fp = fopen(path, "w");
			if (!fp)
				die("writing to '%s'", path);
			put_tracing_file(path);
			fwrite("*:*\n", 4, 1, fp);
			fclose(fp);
			return;
		}

		fp = fopen(path, "w");
		if (!fp)
			die("writing to '%s'", path);
		put_tracing_file(path);
		ret = fwrite("1", 1, 1, fp);
		fclose(fp);
		if (ret < 0)
			die("writing to '%s'", path);
		return;
	}

	path = get_tracing_file("set_event");
	fp = fopen(path, "a");
	if (!fp)
		die("writing to '%s'", path);
	put_tracing_file(path);
	ret = fwrite(name, 1, strlen(name), fp);
	if (ret < 0)
		die("bad event '%s'", name);
	ret = fwrite("\n", 1, 1, fp);
	if (ret < 0)
		die("bad event '%s'", name);
	fclose(fp);
}

static void disable_event(const char *name)
{
	struct stat st;
	FILE *fp;
	char *path;
	int ret;

	if (strcmp(name, "all") == 0) {
		path = get_tracing_file("events/enable");

		ret = stat(path, &st);
		if (ret < 0) {
			put_tracing_file(path);
			/* need to use old way */
			path = get_tracing_file("set_event");
			fp = fopen(path, "w");
			if (!fp)
				die("writing to '%s'", path);
			put_tracing_file(path);
			fwrite("\n", 1, 1, fp);
			fclose(fp);
			return;
		}

		fp = fopen(path, "w");
		if (!fp)
			die("writing to '%s'", path);
		put_tracing_file(path);
		fwrite("0", 1, 1, fp);
		fclose(fp);
		return;
	}

	path = get_tracing_file("set_event");
	fp = fopen(path, "a");
	if (!fp)
		die("writing to '%s'", path);
	put_tracing_file(path);
	ret = fwrite("!", 1, 1, fp);
	if (ret < 0)
		die("can't write negative");
	ret = fwrite(name, 1, strlen(name), fp);
	if (ret < 0)
		die("bad event '%s'", name);
	ret = fwrite("\n", 1, 1, fp);
	if (ret < 0)
		die("bad event '%s'", name);
	fclose(fp);
}

static void enable_tracing(void)
{
	FILE *fp;
	char *path;

	/* reset the trace */
	path = get_tracing_file("tracing_on");
	fp = fopen(path, "w");
	if (!fp)
		die("writing to '%s'", path);
	put_tracing_file(path);
	fwrite("1", 1, 1, fp);
	fclose(fp);
}

static void disable_tracing(void)
{
	FILE *fp;
	char *path;

	/* reset the trace */
	path = get_tracing_file("tracing_on");
	fp = fopen(path, "w");
	if (!fp)
		die("writing to '%s'", path);
	put_tracing_file(path);
	fwrite("0", 1, 1, fp);
	fclose(fp);
}

static void disable_all(void)
{
	FILE *fp;
	char *path;

	disable_tracing();

	set_plugin("nop");
	disable_event("all");

	/* reset the trace */
	path = get_tracing_file("trace");
	fp = fopen(path, "w");
	if (!fp)
		die("writing to '%s'", path);
	put_tracing_file(path);
	fwrite("0", 1, 1, fp);
	fclose(fp);
}

static void reset_max_latency(void)
{
	FILE *fp;
	char *path;

	/* reset the trace */
	path = get_tracing_file("tracing_max_latency");
	fp = fopen(path, "w");
	if (!fp)
		die("writing to '%s'", path);
	put_tracing_file(path);
	fwrite("0", 1, 1, fp);
	fclose(fp);
}

static void enable_events(void)
{
	struct event_list *event;

	for (event = event_selection; event; event = event->next) {
		if (!event->neg)
			enable_event(event->event);
	}

	/* Now disable any events */
	for (event = event_selection; event; event = event->next) {
		if (event->neg)
			disable_event(event->event);
	}
}

static int count_cpus(void)
{
	FILE *fp;
	char buf[1024];
	int cpus = 0;
	char *pbuf;
	size_t *pn;
	size_t n;
	int r;

	n = 1024;
	pn = &n;
	pbuf = buf;

	fp = fopen("/proc/cpuinfo", "r");
	if (!fp)
		die("Can not read cpuinfo");

	while ((r = getline(&pbuf, pn, fp)) >= 0) {
		char *p;

		if (strncmp(buf, "processor", 9) != 0)
			continue;
		for (p = buf+9; isspace(*p); p++)
			;
		if (*p == ':')
			cpus++;
	}
	fclose(fp);

	return cpus;
}

static int finished;

static void finish(int sig)
{
	/* all done */
	if (recorder)
		tracecmd_stop_recording(recorder);
	finished = 1;
}

static void flush(int sig)
{
	if (recorder)
		tracecmd_stop_recording(recorder);
}

static int create_recorder(int cpu)
{
	char *file;
	int pid;

	pid = fork();
	if (pid < 0)
		die("fork");

	if (pid)
		return pid;

	signal(SIGINT, finish);
	signal(SIGUSR1, flush);

	/* do not kill tasks on error */
	cpu_count = 0;

	file = get_temp_file(cpu);

	recorder = tracecmd_create_recorder(file, cpu);
	put_temp_file(file);

	if (!recorder)
		die ("can't create recorder");
	while (!finished)
		tracecmd_start_recording(recorder, sleep_time);
	tracecmd_free_recorder(recorder);

	exit(0);
}

static void start_threads(void)
{
	int i;

	cpu_count = count_cpus();

	/* make a thread for every CPU we have */
	pids = malloc_or_die(sizeof(*pids) * cpu_count);

	memset(pids, 0, sizeof(*pids) * cpu_count);


	for (i = 0; i < cpu_count; i++) {
		pids[i] = create_recorder(i);
	}
}

static void record_data(void)
{
	struct tracecmd_output *handle;
	char **temp_files;
	int i;

	if (latency)
		handle = tracecmd_create_file_latency(output_file, cpu_count);
	else {
		if (!cpu_count)
			return;

		temp_files = malloc_or_die(sizeof(*temp_files) * cpu_count);

		for (i = 0; i < cpu_count; i++)
			temp_files[i] = get_temp_file(i);

		handle = tracecmd_create_file(output_file, cpu_count, temp_files);

		for (i = 0; i < cpu_count; i++)
			put_temp_file(temp_files[i]);
		free(temp_files);
	}
	if (!handle)
		die("could not write to file");
	tracecmd_output_close(handle);
}

static int trace_empty(void)
{
	char *path;
	FILE *fp;
	char *line = NULL;
	size_t size;
	ssize_t n;
	int ret;
	
	/*
	 * Test if the trace file is empty.
	 *
	 * Yes, this is a heck of a hack. What is done here
	 * is to read the trace file and ignore the
	 * lines starting with '#', and if we get a line
	 * that is without a '#' the trace is not empty.
	 * Otherwise it is.
	 */
	path = get_tracing_file("trace");
	fp = fopen(path, "r");
	if (!fp)
		die("reading '%s'", path);

	do {
		n = getline(&line, &size, fp);
		if (!line)
			ret = 1;
		else if (line[0] != '#')
			ret = 0;
		if (n < 0)
			ret = 1;
	} while (line && n > 0);

	put_tracing_file(path);

	fclose(fp);

	return ret;
}

void usage(char **argv)
{
	char *arg = argv[0];
	char *p = arg+strlen(arg);

	while (p >= arg && *p != '/')
		p--;
	p++;

	printf("\n"
	       "%s version %s\n\n"
	       "usage:\n"
	       " %s record [-v][-e event][-p plugin][-d][-o file][-s usecs][-O option ] [command ...]\n"
	       "          -e run command with event enabled\n"
	       "          -p run command with plugin enabled\n"
	       "          -v will negate all -e after it (disable those events)\n"
	       "          -d disable function tracer when running\n"
	       "          -o data output file [default trace.dat]\n"
	       "          -O option to enable (or disable)\n"
	       "          -s sleep interval between recording (in usecs) [default: 1000]\n"
	       "\n"
	       " %s start [-e event][-p plugin] [-d] [-O option ]\n"
	       "          Uses same options as record, but does not run a command.\n"
	       "          It only enables the tracing and exits\n"
	       "\n"
	       " %s extract [-p plugin][-O option][-o file]\n"
	       "          Uses same options as record, but only reads an existing trace.\n"
	       "\n"
	       " %s stop\n"
	       "          Stops the tracer from recording more data.\n"
	       "          Used in conjunction with start\n"
	       "\n"
	       " %s reset\n"
	       "          Disables the tracer (may reset trace file)\n"
	       "          Used in conjunction with start\n"
	       "\n"
	       " %s report [-i file] [--cpu cpu] [-e][-f][-l][-P][-E]\n"
	       "          -i input file [default trace.dat]\n"
	       "          -e show file endianess\n"
	       "          -f show function list\n"
	       "          -P show printk list\n"
	       "          -E show event files stored\n"
	       "          -l show latency format (default with latency tracers)\n"
	       "\n"
	       " %s list [-e][-p]\n"
	       "          -e list available events\n"
	       "          -p list available plugins\n"
	       "          -o list available options\n"
	       "\n", p, TRACECMD_VERSION, p, p, p, p, p, p, p);
	exit(-1);
}

int main (int argc, char **argv)
{
	const char *plugin = NULL;
	const char *output = NULL;
	const char *option;
	struct event_list *event;
	struct trace_seq s;
	int disable = 0;
	int plug = 0;
	int events = 0;
	int options = 0;
	int record = 0;
	int extract = 0;
	int run_command = 0;
	int neg_event = 0;
	int fset;
	int cpu;

	int c;

	errno = 0;

	if (argc < 2)
		usage(argv);

	if (strcmp(argv[1], "report") == 0) {
		trace_report(argc, argv);
		exit(0);
	} else if (strcmp(argv[1], "view") == 0) {
		trace_view(argc, argv);
		exit(0);
	} else if ((record = (strcmp(argv[1], "record") == 0)) ||
		   (strcmp(argv[1], "start") == 0) ||
		   ((extract = strcmp(argv[1], "extract") == 0))) {

		while ((c = getopt(argc-1, argv+1, "+he:p:do:O:s:v")) >= 0) {
			switch (c) {
			case 'h':
				usage(argv);
				break;
			case 'e':
				if (extract)
					usage(argv);
				events = 1;
				event = malloc_or_die(sizeof(*event));
				event->event = optarg;
				event->next = event_selection;
				event->neg = neg_event;
				event_selection = event;
				break;
			case 'v':
				if (extract)
					usage(argv);
				neg_event = 1;
				break;
			case 'p':
				if (plugin)
					die("only one plugin allowed");
				plugin = optarg;
				fprintf(stderr, "  plugin %s\n", plugin);
				break;
			case 'd':
				if (extract)
					usage(argv);
				disable = 1;
				break;
			case 'o':
				if (!record && !extract)
					die("start does not take output\n"
					    "Did you mean 'record'?");
				if (output)
					die("only one output file allowed");
				output = optarg;
				break;
			case 'O':
				option = optarg;
				set_option(option);
				break;
			case 's':
				if (extract)
					usage(argv);
				sleep_time = atoi(optarg);
				break;
			}
		}

	} else if (strcmp(argv[1], "stop") == 0) {
		disable_tracing();
		exit(0);

	} else if (strcmp(argv[1], "reset") == 0) {
		disable_all();
		exit(0);

	} else if (strcmp(argv[1], "list") == 0) {

		while ((c = getopt(argc-1, argv+1, "+hepo")) >= 0) {
			switch (c) {
			case 'h':
				usage(argv);
				break;
			case 'e':
				events = 1;
				break;
			case 'p':
				plug = 1;
				break;
			case 'o':
				options = 1;
				break;
			default:
				usage(argv);
			}
		}

		if (events)
			show_events();

		if (plug)
			show_plugins();

		if (options)
			show_options();

		if (!events && !plug && !options) {
			printf("events:\n");
			show_events();
			printf("\nplugins:\n");
			show_plugins();
			printf("\noptions:\n");
			show_options();
		}

		exit(0);

	} else {
		fprintf(stderr, "unknown command: %s\n", argv[1]);
		usage(argv);
	}

	if ((argc - optind) >= 2) {
		if (!record)
			die("Command start does not take any commands\n"
			    "Did you mean 'record'?");
		if (extract)
			die("Command extract does not take any commands\n"
			    "Did you mean 'record'?");
		run_command = 1;
	}

	if (!events && !plugin && !extract)
		die("no event or plugin was specified... aborting");

	if (output)
		output_file = output;

	if (!extract) {
		fset = set_ftrace(!disable);
		disable_all();

		if (events)
			enable_events();
	}

	if (plugin) {
		/*
		 * Latency tracers just save the trace and kill
		 * the threads.
		 */
		if (strcmp(plugin, "irqsoff") == 0 ||
		    strcmp(plugin, "preemptoff") == 0 ||
		    strcmp(plugin, "preemptirqsoff") == 0 ||
		    strcmp(plugin, "wakeup") == 0 ||
		    strcmp(plugin, "wakeup_rt") == 0) {
			latency = 1;
		}
		if (fset < 0 && (strcmp(plugin, "function") == 0 ||
				 strcmp(plugin, "function_graph") == 0))
			die("function tracing not configured on this kernel");
		if (!extract)
			set_plugin(plugin);
	}

	if (record || extract) {
		if (!latency)
			start_threads();
		signal(SIGINT, finish);
	}

	if (extract) {
		while (!finished && !trace_empty()) {
			flush_threads();
			sleep(1);
		}
	} else {
		enable_tracing();
		if (latency)
			reset_max_latency();

		if (!record)
			exit(0);

		if (run_command)
			run_cmd((argc - optind) - 1, &argv[optind + 1]);
		else {
			/* sleep till we are woken with Ctrl^C */
			printf("Hit Ctrl^C to stop recording\n");
			while (!finished)
				sleep(10);
		}

		disable_tracing();
	}

	stop_threads();

	record_data();
	delete_thread_data();

	printf("Buffer statistics:\n\n");
	for (cpu = 0; cpu < cpu_count; cpu++) {
		trace_seq_init(&s);
		trace_seq_printf(&s, "CPU: %d\n", cpu);
		tracecmd_stat_cpu(&s, cpu);
		trace_seq_do_printf(&s);
		printf("\n");
	}

	exit(0);

	return 0;
}

